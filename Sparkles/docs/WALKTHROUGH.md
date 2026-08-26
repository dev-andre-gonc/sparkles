# Sparkles — Codebase Walkthrough

This document explains everything in this plugin that goes beyond the stock iPlug2 template, at
three levels of depth. It complements `docs/SPEC.md` (the design spec, written in the intended-
behavior voice) and `CLAUDE.md` (UI layout mechanics) — this document instead explains how the
*implementation* is wired together, file by file.

Sparkles listens to an audio or MIDI input, detects a "trigger" (a note starting or stopping), and
in response fires a **sprinkle** — a burst of generated notes arranged into **rays** of **sparkles**
— either out as MIDI, rendered by the plugin's own built-in synth, or both.

---

## Summary 1 — High Level

Audio comes in through `Sparkles::ProcessBlock` (`Sparkles.cpp`). Two independent analyses run on
it every sample: a one-pole envelope follower (`mEnvelope`) tracks loudness, and a continuous
autocorrelation pitch tracker (`core/PitchTracker.h`, owned as `mPitchTracker`) tracks which note is
sounding. When the envelope crosses the user's Threshold in the configured direction (up/down/both),
that's a **trigger**. MIDI input can trigger the same way, independently, via
`Sparkles::HandleMidiTrigger` reading note-on/note-off messages queued by `ProcessMidiMsg`. Either
path resolves to a trigger *note* and funnels through the same place: `Sparkles::FireSprinkle`.

`FireSprinkle` hands the trigger note to `sparkle_core::SparkleGenerator::Generate`
(`core/SparkleGenerator.h`), which is the heart of the plugin's creative logic. It walks a note
eligibility grid (`sparkle_core::NoteMatrix`, `core/NoteMatrix.h` — a 12×12 table of which trigger
pitch classes may produce which sparkle pitch classes, editable by hand or generated from a musical
key/scale/chord) and produces a flat list of `SparkleEvent`s — each one a note, velocity, duration,
pan position, and synth envelope, timestamped as a sample offset from the trigger. Those events are
handed to two independent output paths depending on the user's Output Mode: `core/EventScheduler.h`
queues them as sample-accurate MIDI note-on/note-off pairs that `ProcessBlock` drains and sends out
via `SendMidiMsg`, and/or `core/SynthEngine.h` spawns polyphonic oscillator voices that render
directly into the plugin's own stereo output block, additively on top of the dry passthrough, then
through a soft-clip safety net.

Everything a user can turn a knob on lives in `params/ParamList.h`, a single X-macro table that
generates the `EParams` enum, every `IParam`'s init call, and (via `params/ParamSnapshot.h`) a
plain-data snapshot the framework-free `core/` DSP code consumes once per block — the DSP layer
never touches an iPlug2 `IParam` directly. The UI (`Sparkles.cpp`'s `mLayoutFunc`, following the
pattern documented in `CLAUDE.md`) is a single fixed-aspect-ratio canvas, organized into six tabs,
that renders those same params as knobs/dropdowns arranged from a hand-maintained layout table, plus
several hand-built custom `IControl`s (`ui/*.h`) for things a plain knob can't show: the note matrix
grid, live envelope/pitch meters, a per-note confidence bar strip, and a trigger-flash light — all
fed from the audio thread via iPlug2's `ISender` mechanism and drained once per idle tick.

---

## Summary 2 — Mid-Level Detail

### Detection: turning raw audio into a trigger note

Two things run continuously and independently, every sample, whenever Detection Mode allows audio
triggering (`Sparkles::ProcessBlock`, gated on `audioTriggerEnabled`):

- **Envelope follower** — `mEnvelope = mEnvelope*(1-reactiveness) + sqrt(|input|)*reactiveness`, a
  one-pole smoother. `ProcessBlock` compares consecutive samples' envelope value against Threshold
  to detect a genuine crossing (not just "currently above"), in the direction(s) the Trigger On
  param selects.
- **Pitch tracking** — `sparkle_core::PitchTracker` (`core/PitchTracker.h`) is pushed every sample
  into a 2048-sample ring buffer, and every 512 samples (~86 Hz) runs a normalized-autocorrelation
  scan over a table of candidate lags (one per MIDI note in the configured detect range). It layers
  three states on top of the raw per-hop winner: a **local-peak mask** and **octave-error guard**
  that keep a low-frequency "wall" of correlated energy from always winning at the range's top note;
  **hysteresis** (a challenger note must win 3 consecutive hops before it replaces the tracked note,
  killing vibrato flicker); and a **held/last-confident note**, remembered for 250 ms after
  confidence collapses, which is what a down-crossing (note fading) resolves against, since the
  actual audio at that instant is unreliable.

An up-crossing doesn't fire immediately — it *arms* (`mTriggerPending`), and `ProcessBlock` polls the
tracker each subsequent sample until either a confident hop lands (fires then, up to a 50 ms
ceiling) or the ceiling passes (drops silently). A down-crossing fires immediately from the tracker's
held note, or drops if nothing confident is held. MIDI triggering bypasses all of this — the note is
already known exactly — but goes through the same velocity-gating and Up/Down/Both routing logic in
`HandleMidiTrigger`, and both paths converge on `FireSprinkle`, which additionally enforces a
**trigger cooloff** window (drops a trigger arriving too soon after the last accepted one) and a cap
on simultaneously-sounding sprinkles (`kMaxSimultaneousSprinkles`).

### Generation: trigger note → a burst of sparkle events

`sparkle_core::SparkleGenerator::Generate` (`core/SparkleGenerator.h`) is a pure function: given the
note matrix, a `SparkleParams` snapshot, and a trigger note, it produces a `std::vector<SparkleEvent>`
capped at 1024 events. It loops rays 0..nRays, and for each ray, sparkles 0..numSparkles(ray) (itself
computed from `n_sparkles_per_ray * n_sparkles_per_ray_rm^ray`). Every per-sparkle property —
loudness, duration, width, phase, attack/decay/sustain/release — is one of two shapes: a **direct**
exponential formula evaluated at `(ray, sparkle)` (e.g. `loudness * loudnessRm^ray * loudnessSm^sparkle`),
or a **cumulative sum** for anything describing a gap between consecutive events (delay/ray_delay,
interval/ray_interval) — because "the delay between two consecutive notes" isn't reproduced by a
single exponential term once the `_rm`/`_sm` multipliers differ from 1. Pitch is resolved by rounding
the cumulative pitch-step offset to an integer and walking that many positions through
`NoteMatrix::Walk`'s eligible-note list (every MIDI note in `[range_min, range_max]` whose pitch
class the matrix allows for this trigger's column), applying `wrap_mode` (mirror/around/stop) at the
list's boundaries. Panning is a static per-sparkle lookup — either a waveform (`sine`/`triangle`/
`square`/`saw`) sampled at the sparkle's own phase, or a stateless hash of the trigger's project-
timeline position + note + ray/sparkle index (`random` mode) that reproduces the same "random" pan
on repeated playthroughs of the same project.

### Output: two independent, simultaneously-usable rendering paths

`FireSprinkle` schedules every generated event into whichever of two engines Output Mode selects:

- **MIDI** — `core/EventScheduler.h`, a fixed-capacity, allocation-free priority queue of note-
  on/off pairs sorted by absolute sample position. `ProcessBlock` calls `FlushBlock` each block,
  which hands back every event due in that block's sample range (note-offs enqueued lazily, only
  once their paired note-on actually fires) and turns them into real `IMidiMsg`s via `SendMidiMsg`.
- **Audio** — `core/SynthEngine.h`, a fixed pool of up to 64 oscillator voices (quietest-then-oldest
  stolen once full). Each voice free-runs a phase accumulator through a continuous Sine→Triangle→
  Square→Saw morph (`wave_shape`), shaped by a linear ADSR whose release begins the instant the
  sparkle's gate (its `duration`) elapses, continuing seamlessly from wherever attack/decay had
  reached. `SynthEngine::Render` adds its output directly into `ProcessBlock`'s output buffer, on top
  of the dry passthrough, then the whole mix passes through `sparkle_core::SoftClip` (identity below
  90%, asymptotic above) as overs protection.

A separate, purely cosmetic `mFlashScheduler` (same `EventScheduler` type, repurposed) schedules a
note-matrix cell flash at the exact sample each event actually sounds, regardless of which output
path(s) are active, so the UI's note matrix lights up in sync with what's audible.

### Parameters, presets, and the UI

Every automatable value is declared once in `params/ParamList.h` via one of four macros
(`SPARKLE_PARAM_DOUBLE[_CURVE]/INT/ENUM`); including that file with different macros defined
generates the `EParams` enum (`Sparkles.h`), the constructor's `InitXxx()` calls (`Sparkles.cpp`),
and (hand-mapped, since the shapes don't line up mechanically) `params/ParamSnapshot.h`'s
`BuildParamSnapshot`, which `ProcessBlock` calls once per block to get a plain-data, framework-free
copy of every relevant param for `core/` to consume — the sample loop itself never re-reads a live
`IParam`. The note matrix is deliberately *not* an `IParam` at all (144 cells would be awkward to
automate and must survive preset changes untouched per the spec), living only in
`sparkle_core::NoteMatrix`, written directly by the UI thread and read directly by the audio thread
with no synchronization (acceptable since it's read once per trigger, not per sample, of plain
bools). A fixed factory preset list (`kPresets` in `Sparkles.cpp`, applied by `ApplyPreset`)
deliberately scopes itself to the "sound" params only — never Detection tuning, the note matrix, or
Output Mode — so switching presets mid-performance doesn't retune what the performer is currently
playing into.

The UI itself follows the responsive-canvas pattern documented in `CLAUDE.md`: a single
`mLayoutFunc` that both attaches controls once and repositions them on every resize/tab-switch, at a
fixed aspect ratio so the whole canvas (including text) scales via NanoVG zoom rather than reflowing.
Most param controls are generated automatically from a hand-maintained `kParamGroups` table
(`Sparkles.cpp`'s anonymous namespace) that groups/labels/positions each param into one of six tabs;
a handful of hand-placed controls (the note matrix, envelope meter, note-confidence bars, trigger
light, Shut Up) are custom `IControl` subclasses in `ui/*.h`, fed live DSP state via `ISender`s
pushed from `ProcessBlock` and drained once per `OnIdle` tick.

---

## Summary 3 — Deep Detail

### 1. Signal flow and threading model

The plugin is a single-instance, non-distributed iPlug2 build — the UI and DSP share the same
`Sparkles` object directly, which is why cross-thread state (`mNoteMatrix`, the manual-trigger
queue, `mShutUpRequested`) can be shared as plain members rather than routed through iPlug2's
message-passing plumbing, as several comments in `Sparkles.h` call out explicitly. Three kinds of
cross-thread communication appear:

- **UI → audio, "do this now"**: `mShutUpRequested` is a plain `std::atomic<bool>`, set by the Shut
  Up button's action function and consumed (exchanged back to false) at the very top of
  `ProcessBlock`, before that block's samples get a chance to re-arm a trigger.
- **UI → audio, "queue of events"**: `mManualTriggerQueue` is a fixed-size SPSC ring buffer (16
  slots) of MIDI note numbers, filled by `NoteMatrixControl`'s trigger-row clicks via
  `PushManualTrigger` (UI thread, producer, owns `mManualTriggerTail`) and drained by
  `PopManualTrigger` (audio thread, consumer, owns `mManualTriggerHead`) at the start of every block
  — classic lock-free SPSC, no mutex needed since each index is only ever written by one side.
- **audio → UI, "here's fresh state"**: six `ISender<N>` members (`mEnvelopeSender`,
  `mNoteSender`, `mNoteBarsSender`, `mTriggerSender`, `mSprinkleCountSender`,
  `mNoteMatrixFlashSender`) are pushed to from `ProcessBlock` and drained by `Sparkles::OnIdle`,
  which calls `TransmitData` on each — iPlug2's standard pattern for getting per-block DSP state to
  the UI without the UI ever touching audio-thread memory directly.
- **UI ↔ audio, shared mutable state with no synchronization at all**: `mNoteMatrix` (edited by
  `NoteMatrixControl` on the UI thread, read by `SparkleGenerator::Generate` on the audio thread) is
  the one deliberate exception — the header comment argues this is safe because `NoteMatrix::Walk`
  only runs once per trigger (not per sample), every field is a plain `bool`, and a torn read at
  worst corrupts a single in-flight sprinkle's eligible-note list, never crashes.

### 2. Detection stage internals

**Envelope follower.** `ProcessBlock`'s per-sample loop (only entered when `audioTriggerEnabled`,
i.e. Detection Mode includes Audio) takes the max absolute value across all connected input
channels, square-roots it (compressing dynamic range so quiet input still moves the follower
meaningfully), and smooths it: `mEnvelope = mEnvelope*(1-reactiveness) + inputLevel*reactiveness`.
Crossing detection compares `prevEnvelope`/`mEnvelope` against `threshold` (not just "currently
above") specifically so a sustained note sitting above threshold doesn't immediately re-fire the
instant the previous trigger resolves — it has to actually dip and re-cross.

**Pitch tracker (`core/PitchTracker.h`).** Deliberately framework-free (no iPlug2/IGraphics
dependency) so the same header is shared between the plugin and the standalone `sparkle_tests`
binary. Internals:

- A 2048-sample power-of-two ring buffer (`mBuffer`), fed one sample at a time via `Push()`.
  Roughly 46 ms at 44.1 kHz. Every 512 samples (`kHopSamples`, one "hop", ~86 updates/sec), it runs
  `AnalyzeHop()`.
- `Configure(sampleRate, noteMin, noteMax, holdSeconds)` builds a candidate table: one
  `(midiNote, lag)` pair per note in the detect range, where `lag = round(sampleRate / NoteToFreq(note))`.
  Notes whose lag falls outside the buffer are skipped.
- `AnalyzeHop()` first linearizes the ring buffer (oldest sample first) into a scratch array and
  removes its mean — without this, any DC offset inflates every lag's autocorrelation score
  simultaneously, which is half of why a naive implementation collapses onto short lags. It then
  computes a normalized autocorrelation `Score(lag)` for every candidate.
- A **local-peak mask** rejects any candidate that doesn't out-score its immediate semitone
  neighbors in lag-space (using virtual one-semitone-out neighbors at the table's edges). This
  exists because autocorrelation is structurally generous to short lags — a low fundamental (or
  broadband/sub-range rumble) produces a smooth ramp of high scores across the top octaves of the
  table with no interior local maximum, which a plain argmax would pick every time (the range's
  highest note), producing the "wall" the header comment describes. A true pitch is a local peak;
  the ramp isn't.
- Among peak candidates, an **octave-error guard** lets a smaller-lag (higher-note) candidate steal
  the win from the raw argmax only if its score is within 5% (`kOctaveTiebreak`) *and* its lag is a
  harmonic subdivision (within 3%, `kHarmonicTolerance`) of the argmax's lag — i.e. only a genuine
  octave/twelfth error gets corrected, not an unrelated near-tie.
- The winning note then goes through **hysteresis**: from an untracked state it's adopted
  immediately, but once a note is tracked, a different note must win 3 consecutive hops
  (`kHysteresisHops`) before it replaces the stable note — this kills flicker from vibrato, beating
  strings, or window-boundary artifacts.
- `mLastConfidentNote`/`mLastConfidentTime` record the stable note whenever its own score clears
  the caller-set `SetConfidenceThreshold` (the user's Confidence param) — distinct from
  `kMinTrackConfidence` (0.3), a much lower floor that gates whether tracking happens *at all*.
  `HasConfidentNote()` is true only while that record is within the configured hold window
  (`kNoteHoldSeconds` = 250 ms in `Sparkles.h`).
- `GetNoteConfidences()` exposes every candidate's raw last-hop score (not gated by the peak mask —
  deliberately, so the UI's note-bars strip shows what the input actually correlates to, while
  the mask only gates *selection*), which feeds `ui/NoteBarsControl.h`.

**Trigger resolution (`ProcessBlock`).** An up-crossing doesn't fire on the spot — the analysis
window is usually mid-transient right at note onset, so it *arms* (`mTriggerPending = true`,
`mTriggerArmTime = mPitchTracker.Now()`, `mTriggerDeadline = mTriggerArmTime + 50ms`). Every
subsequent sample while armed, `ProcessBlock` checks whether the tracker now has a confident note
whose confidence timestamp is at or after the arm time (with one hop of slack, so a note already
confidently sounding at the moment of crossing — e.g. a swell — fires immediately rather than
waiting a full hop) — if so it fires with that note; if the deadline passes first, it's dropped
silently. A down-crossing never arms anything — it resolves immediately against
`mPitchTracker.LastConfidentNote()`/`HasConfidentNote()`, since by the time the envelope falls the
note itself is fading or already gone, and "what was confidently playing a moment ago" is the
question that actually has an answer.

**MIDI trigger path (`HandleMidiTrigger`).** Runs independently, sample-accurately: `ProcessMidiMsg`
queues incoming messages into `mMidiQueue` (an `IMidiQueue`), and `ProcessBlock`'s sample loop drains
any message due at that exact sample offset before doing anything else that sample. Velocity-0
note-ons are treated as note-offs (MIDI running-status convention). A note-off's own velocity byte
is usually 0 on real hardware, so Min Velocity gating for a Down-trigger looks up the velocity the
note was originally struck with, tracked in `mHeldNoteVelocity[note]` (updated on every note-on
regardless of whether MIDI triggering is currently enabled, so switching Detection Mode to MIDI
mid-performance doesn't inherit a stale/missing velocity for an already-held note).

### 3. The note matrix and key/scale system (`core/NoteMatrix.h`)

`sparkle_core::NoteMatrix` is a 12×12 grid of booleans (`mCells[column][row]`), columns indexed by
trigger pitch class and rows by sparkle pitch class, in the spec's fixed order A, A#, B, C, ... G#
(**not** MIDI's C-first order — `PitchClassOf()` does the conversion: `(midiNote%12 + 3) % 12`).
Independent `mColumnEnabled`/`mRowEnabled` gates AND with the cells without touching them (kept for
testability; nothing in the UI drives them — the UI's column/row toggle buttons instead call
`ToggleColumn`/`ToggleRow`, which destructively overwrite all 12 cells in that column/row: if any
were on, turn all off, else turn all on).

`EligibleNotes(startNote, rangeMin, rangeMax)` returns every MIDI note in range whose pitch class is
an eligible row for `startNote`'s column, sorted ascending — this sorted list is the space
`Walk(...)` steps through. `Walk` takes a signed step count and a `WrapMode`:

- **Stop** — `target = startIdx + steps`; out of `[0, n)` returns `std::nullopt` (the caller, in
  `SparkleGenerator::Generate`, treats this as "this ray's sparkle-generation loop ends here").
- **Around** — `target = (startIdx + steps) mod n`, circular.
- **Mirror** — index space is folded as a triangle wave of period `2*(n-1)`, so a walk that runs
  past one boundary bounces back toward the other without ever repeating the boundary note itself,
  and keeps bouncing for as many steps as needed.

Two free functions regenerate the whole matrix from music theory, both driving `SetCell` only (never
the enabled-gates, since those short-circuit `EligibleNotes` *before* the cells are even consulted —
using them to blank out-of-scale cells would make a later hand-edit silently do nothing):

- `ApplyKeyScale(matrix, keyRoot, scale, chordMode)` — computes the scale's pitch-class set relative
  to a single fixed root. With `chordMode == FullScale`, cell `(col,row)` is on iff both pitch
  classes are in that set. For any other `ChordMode`, each in-scale column instead gets only the
  rows reachable by walking `detail::ChordDegreeSteps(chordMode)` (a list of scale-degree offsets —
  e.g. Triad is `{0,2,4}`, "every other degree" — indexed modulo the scale's own degree count, so a
  5-note pentatonic's "Thirteenth" just repeats earlier pitch classes rather than reading out of
  bounds) starting from that column's own scale degree.
- `ApplyKeyScalePerColumn(matrix, scale, chordMode)` — the "Trigger Note" root mode: every column
  uses its own pitch class as the root, so cell `(col,row)` is on iff `(row-col) mod 12` is one of
  the scale's own degree offsets (trivially always true for offset 0, so no `inScale[]` gate is
  needed the way the fixed-root version has one).

`Sparkles::OnParamChange` calls one of these two whenever `kParamKeyRoot`/`kParamKeyScale`/
`kParamKeyMode` changes (dispatching on whether `keyRoot == kNumPitchClasses`, the sentinel for
"Trigger Note"), which is a **one-time destructive overwrite** — subsequent hand-edits to individual
cells persist until the next key/scale/mode change.

### 4. Sparkle generation (`core/SparkleGenerator.h`)

`SparkleParams` mirrors `docs/SPEC.md` §7's param names field-for-field, including the `_rm`
(per-ray) / `_sm` (per-sparkle-within-ray) multiplier convention. `Generate()` is the single entry
point, and it's carefully bounded: `outEvents` is `reserve()`'d exactly once to
`MaxEventCount(params)` (itself capped at `kMaxEventsPerTrigger = 1024`), so the `push_back` calls
inside the nested ray/sparkle loops never trigger a reallocation — this is the function's only
allocation.

**Two accumulation shapes.** Loudness, duration, width, phase, and every synth-envelope stage
(attack/decay/sustain/release) are **direct** properties — evaluated straight from `(ray, sparkle)`
with no dependency on prior sparkles, e.g. `loudness(ray,sparkle) = loudness * loudnessRm^ray *
loudnessSm^sparkle`. Timing (`delay`/`ray_delay`) and pitch-step (`interval`/`ray_interval`) are
**cumulative** — because both describe "the gap between two consecutive events," which a single
direct exponential term can't reproduce once `_rm`/`_sm` differ from 1 (spec §7.4/§7.5 walks through
why). The code tracks four running accumulators through the nested loop:
`rayDelayAccumSamples`/`rayIntervalAccum` (summed once per ray, `i=0..rayN`) and
`withinRaySamplesAccum`/`withinRayIntervalAccum` (summed once per sparkle within the current ray,
reset at the top of each ray's inner loop, `k=1..sparkleN`).

**Pitch resolution.** `rawSteps = rayIntervalOffset + withinRayIntervalAccum` is rounded to the
nearest integer and handed to the two-argument `NoteMatrix::Walk(triggerNote, anchorNote, steps, ...)`
overload — `triggerNote` supplies which matrix *column* governs eligibility, while `anchorNote`
(`triggerNote + preInterval`) is where step-counting actually starts from; this split exists so Pre
Interval can chromatically transpose the sprinkle's starting point without changing which column's
eligibility rules apply to it.

**Panning (`Pan()`).** `Mono` is always 0. The four waveform modes (`sine`/`triangle`/`square`/`saw`)
look up `Wave(mode, phase)` (each periodic with period 1, so any phase pushed outside `[0,1)` by
additive `_rm`/`_sm` growth wraps for free) and multiply by `signForThisRay * width`, where
`signForThisRay` derives from `ray_rotation` (base L/R sign) and `ray_rotation_rm`
(Keep/Invert — flips the sign progressively per ray, tracked in a running `raySign` variable rather
than as a continuous exponent, since it's a binary flip-or-don't rather than a multiplier). `Random`
mode instead computes a stateless 64-bit hash (`PanRandomUnit`, SplitMix64-style avalanche via
`MixBits`/`HashCombine`) folding together `seed`, the trigger's absolute project-timeline sample
position, the trigger note, and `(ray, sparkle)` — so the same trigger at the same timeline position
always reproduces the same "random" pan across playthroughs of the same project, while still varying
per-note/per-sparkle within one trigger, with zero mutable RNG state to manage.

**Hard caps.** `NumSparklesForRay` clamps its result to `[0, kMaxEventsPerTrigger]` *before* the
`double→int` cast, specifically so a pathological `n_sparkles_per_ray_rm` (e.g. 3.0 with a dozen
rays) can't overflow or invoke UB on the cast. The outer ray loop and inner sparkle loop both check
`outEvents.size() >= kMaxEventsPerTrigger` and `break` — truncating gracefully, potentially
mid-ray, rather than growing the buffer past its one reservation.

### 5. Scheduling and output rendering

**`core/EventScheduler.h`** is a template class (`NoteOnCapacity`/`NoteOffCapacity`, both default
1024) holding two `std::array`-backed pools kept sorted by insertion (`InsertSorted`, scanning from
the back since new entries are almost always near the newest already-pending one — cheap for the
"append near the end" access pattern this workload actually has). `Schedule(note, velocity,
durationSamples, atSample)` only inserts into the note-*on* pool — the paired note-off is deferred
into the note-*off* pool lazily, only once `FlushBlock` actually pops that note-on, so a sprinkle
scheduled far in the future doesn't tie up note-off capacity prematurely. `FlushBlock(blockStart,
nBlockSamples, outEvents, outCapacity)` drains everything due before `blockEnd`, interleaved in
ascending time (ties broken note-off-before-note-on, so a note retriggered on the exact same sample
never appears to overlap itself), returning early if `outCapacity` is exhausted — callers (both
`ProcessBlock`'s MIDI flush and its flash flush) loop calling it until it returns fewer than
requested, so nothing is ever silently dropped for exceeding one call's capacity. `StopAll` is the
"kill everything now" variant used by `Sparkles::ShutUp` — pops every pending note-off as due
immediately (ignoring its real scheduled time) and discards every not-yet-fired note-on outright
(nothing to turn off for a note that never sounded).

`ProcessBlock` uses **two separate instances**: `mEventScheduler` for real MIDI, and `mFlashScheduler`
for a purely cosmetic per-cell UI flash. The flash instance repurposes `SchedEvent::note` to carry a
packed `column * 12 + row` pitch-class pair rather than an actual MIDI note (there's no matching
note-off semantics needed, so `durationSamples` is always scheduled as 0) — `ProcessBlock`'s flush
loop decodes it back with `/` and `%` before deduplicating within the block (`cellsHit[][]`, since
several sparkles can hit the same cell in one block and the sender behind it is only 64 deep) and
pushing to `mNoteMatrixFlashSender`.

**`core/SynthEngine.h`** is a fixed-capacity polyphonic voice pool (`MaxVoices=64`,
`MaxPendingSpawns=1024`), deliberately *not* built on top of `EventScheduler` even though both
schedule-by-absolute-sample — a synth voice needs pan and a full resolved ADSR that `SchedEvent`
has no fields for, and `SparkleEvent` already carries them, so reusing `EventScheduler` would mean
bolting audio-only fields onto a MIDI-shaped type. `ScheduleVoice` queues a `PendingSpawn`
(insertion-sorted, same back-scan rationale as `EventScheduler`); `Render(blockStart, nFrames,
sampleRate, waveShape, outputs, nOutChans)` first drains any spawns due this block
(`DrainDueSpawns`, which shifts remaining not-yet-due spawns down rather than reallocating), then
walks every active voice sample-by-sample: a voice spawned mid-future-block stays silent via
`samplesUntilStart` counting down first, then advances a phase accumulator through
`WaveformSample(waveShape, phase)` — a continuous crossfade across the four basic waveforms indexed
by `floor(waveShape)`/`waveShape - floor(waveShape)` — scaled by `EnvelopeLevel(voice)` and
`velocityGain`, and written into `outputs[0]/[1]` weighted by `gainL`/`gainR` (constant-power pan,
computed once at spawn time from the sparkle's `pan` via `cos/sin(quarterPi*(pan+1))`). Envelope
shape has no explicit "note off" the way MIDI does — the gate length *is* the sparkle's own
`duration`, so `EnvelopeLevel` ramps attack→decay→sustain while `elapsedSamples < gateSamples`, and
the instant the gate elapses, release begins from `PreReleaseLevel` at the gate boundary (continuous,
no jump — so a gate shorter than attack+decay releases early without a click rather than waiting for
those phases to finish). Voice stealing (`FindVoiceSlot`) prefers the currently-quietest voice
(`lastLevel`), tie-broken oldest-first — a bounded-cost tradeoff so CPU stays predictable regardless
of how many sparkles are in flight.

**`ProcessBlock`'s output stage**, in order: the dry passthrough is written first (`outputs[c][s] =
inputs[c][s] * gain`), `mSynthEngine.Render(...)` then *adds* onto it if Output Mode includes Audio,
and finally every output sample passes through `sparkle_core::SoftClip` (identity below 0.9,
`tanh`-shaped approach to ±1 above that) as cheap insurance against several simultaneous voices
stacking past unity — not a user-facing parameter.

### 6. Parameter plumbing

`params/ParamList.h` is an X-macro file containing *only* macro invocations (no `#pragma once` —
it's deliberately included multiple times per translation unit). Four macros
(`SPARKLE_PARAM_DOUBLE[_CURVE]/INT/ENUM`) are individually defined-or-not by each include site, so
one file drives: `enum EParams` generation (`Sparkles.h`, macros expand to just `id,`), the
constructor's `InitDouble/InitInt/InitEnum` calls (`Sparkles.cpp`), and (hand-written rather than
macro-generated, since the shapes don't line up 1:1) `params/ParamSnapshot.h`'s per-field reads. The
`_CURVE` variant applies `ShapePowCurve` so a knob's travel isn't wasted on one end of a
disproportionately wide range (the `_rm`/`_sm` multiplier knobs, and the millisecond-range
attack/decay/release knobs). `params/ParamRanges.h` holds the shared MIDI-note bounds
(`kMinTriggerableNote=24`/`kMaxTriggerableNote=108`) referenced by both `Sparkles.h`'s candidate
table sizing and `ParamList.h`'s Min/Max Note param ranges — kept in its own ordinary header rather
than inline in the X-macro file specifically because the X-macro file must contain nothing else.

`params/ParamSnapshot.h::BuildParamSnapshot(plugin)` is called once at the top of every
`ProcessBlock` and returns a `ParamSnapshot` — a `DetectionParams` plus a `SparkleParams` (both
plain-old-data, no iPlug2 dependency) plus a few loose fields (`outputMode`, `waveShape`, the
key/scale quick-fill selectors). `ReadTimeParam` collapses each of the four beats/ms-toggleable
params (Pre Delay, Duration, Ray Delay, Delay) — a magnitude `IParam` plus a sibling `*Unit` enum
`IParam` — into one `sparkle_core::TimeParam{value, unit}`. Every enum-valued param needs an
explicit `static_cast` to its matching `sparkle_core` enum, which is exactly the mechanical
awkwardness the header comment cites for why this mapping is hand-written rather than generated:
adding a param means touching `ParamList.h` *and* adding one matching line here.

**Beats/ms unit conversion.** `Sparkles::OnParamChange(idx, source, ...)` — the 3-argument override
— reacts to one of the four `*Unit` toggle params flipping, but only when `source == kUI`: iPlug2
also re-announces every param's current value through this same override on construction/preset
recall (`OnParamReset`), and reacting to those spurious calls the same way would rescale an
already-correct magnitude every time the plugin loads (dividing it toward zero, since the "new" unit
always matches the old one in that case). `ConvertTimeMagnitudeUnit` then rescales the sibling
magnitude so it represents the same real duration under the new unit at the current tempo — but it
*also* bails out early whenever `mApplyingPreset` is set, because `ApplyPreset` drives magnitude and
`*Unit` params back-to-back via the exact same `SetParameterValue` path a real UI toggle uses (so
its calls also arrive tagged `kUI`), and without the guard every preset load would rescale each of
the four magnitudes a second time immediately after setting it to the preset's intended value.

### 7. Presets

`kScopedParamIds` (`Sparkles.cpp`) is a flat, hand-ordered list of every param a factory preset
touches — General-tab params, then Pitch/Timing, then Synth, then Key Mode — deliberately excluding
Detection tuning, Output Mode, and Key Root/Scale (per `docs/SPEC.md` §8: those represent how the
performer is currently listening/what key they're playing in, independent of which sprinkle "sound"
is loaded). `kPresets` is an array of `PresetDesc{name, values[]}`, where `values` is strictly
*positional* against `kScopedParamIds` — reordering one array without the other silently corrupts
every preset, so the file's comments flag this explicitly. `ApplyPreset(idx)`: requests a Shut Up
first (so a sprinkle mid-flight under the old params doesn't finish ringing out with a mix of old and
new values), sets `mApplyingPreset = true` for the duration of the `SetParameterValue` loop (see
§6's guard), then does a second pass calling `SetValueFromDelegate` on every attached control bound
to one of those params — `SetParameterValue` alone updates the `IParam` and notifies the host, but
doesn't resync an already-attached control's own cached display value, so without this second pass
the knobs would visually freeze at their pre-preset positions until an unrelated redraw. Loading a
preset also regenerates the note matrix, but *not* via any preset-specific code path — Key Mode is
itself one of the scoped params, so setting it through the normal `SetParameterValue` call fires the
ordinary `OnParamChange` → `ApplyKeyScale` path exactly as a manual Mode-dropdown change would.

### 8. UI architecture

The full mechanics of the responsive layout (`mLayoutFunc`'s resize/attach-branch split, why the
aspect ratio is locked, how tabs reuse the resize path) are documented in `CLAUDE.md` and aren't
repeated here — the summary is that `bounds` never actually changes size after construction (only
draw *scale* does, via the corner resizer's `EUIResizerMode::Scale`), so `mLayoutFunc` always
recomputes the same rects, and a tab click just re-invokes it manually with a new `mActiveTab`.

What's specific to this plugin on top of that pattern:

- **`kParamGroups`** (`Sparkles.cpp` anonymous namespace) is a hand-maintained table of
  `ParamGroupDesc`s (name, tab, static origin) each holding an array of `ParamClusterDesc`s (param
  id, label, control kind, position relative to the group's origin, optional `*Unit`/`Rm`/`Sm`
  sibling ids). At construction, `mLayoutFunc` flattens every group's clusters (offsetting each
  cluster's local x/y by its group's origin, itself offset by the tab's `paramsArea`) into a single
  `flatControls` vector, and attaches one real `IControl` per entry — `IVKnobControl` for `Knob`,
  `ui/TimeMagnitudeControl.h` for `TimeKnob`, `IVSwitchControl` for `Toggle`/`Dropdown` (these click
  directly through their states rather than popping a menu — reserved for the handful of enums with
  only 2-3 options), and `ui/ModifierValueControl.h` (a bare-text draggable chip, no knob graphic) for
  any cluster's `Rm`/`Sm` sibling. Every control gets a tag of `kCtrlTagFirstParamControl + index`,
  and the **same index** is reused in the resize branch to call `SetTargetAndDrawRECTs` on the
  matching control — this is why the flatten order has to stay identical between the two branches
  (it does, since both read the same static table).
- **Tab visibility** — a cluster whose owning group's `tab` doesn't match `mActiveTab` gets
  `Hide(true)` in the resize branch (the `setTabbed` helper `CLAUDE.md` references); everything not
  routed through `kParamGroups` (Title, the note matrix, the visual-indicator panel, Shut Up, the
  tab pills themselves) is always visible and never calls `Hide` at all.
- **Custom controls** (`ui/*.h`), each a plain `IControl` subclass rather than a stock IVControl,
  because none of them are "one param, one widget":
  - `NoteMatrixControl` — the entire 12×12 grid, its 24 column/row toggle headers, and a 13th
    "trigger row" of 12 manual-play buttons, as one composite control (rather than 144+24 separate
    `IVButtonControl`s) since every cell reads/writes the same shared `NoteMatrix*` directly and
    isn't `IParam`-backed at all.
  - `EnvelopeMeterControl` — a read-only (`mIgnoreMouse = true`) vertical bar bound to
    `kParamThreshold` so its threshold line stays in sync automatically via iPlug2's normal
    param-binding, fed its live level via `ISender<1>`.
  - `NoteBarsControl` — one bar per note across the *full* triggerable span (not just the currently
    configured detect range, so the strip's geometry never shifts when Min/Max Note move), fed via
    `ISender<kNumTriggerableNotes>`.
  - `TriggerLightControl` — flashes via `IControl`'s own animation timer (`SetAnimation`) rather
    than a custom clock, triggered by an `ISender<1>` message each time `FireSprinkle` accepts a
    trigger.
  - `ModifierValueControl` / `TimeMagnitudeControl` — condensed, no-knob-graphic drag/scroll
    controls; the latter snaps to named rhythmic divisions (128th note up to whole note,
    straight/dotted/triplet) in Beats mode and free-runs continuously in ms mode, re-reading its
    sibling `*Unit` param on every interaction rather than caching which mode it's in.
  - `ValueDisplayControl<N>` — generic `ISender<N>`-fed text, parameterized by a caller-supplied
    formatting lambda; reused for both the note-name+confidence display (`N=2`) and the
    active-sprinkle-count display (`N=1`).
  - `BackgroundImageControl` / `GroupMarkerControl` — purely decorative (the stretched-to-fit
    background art, and the small dot-and-line marker beside each group heading).
  - `Palette.h` — no controls, just named `IColor`/`IVStyle` constants every other file above pulls
    from instead of hardcoding hex values, so a palette-wide change is one file.

### 9. Testing

`tests/` is a small, framework-free suite (`tests/test_framework.h`) exercising the `core/` headers
directly, with no iPlug2 dependency — the same design choice ("deliberately free of iPlug2/IGraphics
dependencies") called out in nearly every `core/*.h` header comment exists specifically so this is
possible. One test file per `core/` header: `test_pitch_tracker.cpp`, `test_note_matrix.cpp`,
`test_sparkle_generator.cpp`, `test_event_scheduler.cpp`, `test_synth_engine.cpp`. Built via CMake as
the `sparkle_tests` target (see the build instructions at the top of `Sparkles.cpp`), runnable
standalone or through `ctest`.
