# Sparkles — Spec

> Sparkles: rays of sunshine.

Status: **design spec, not yet implemented**. This document describes the intended behavior of the finished plugin. A minimal prototype already exists in `Sparkles.cpp`/`.h` (threshold-crossing trigger + autocorrelation pitch detection, no ray/sparkle structure yet) — a few names below (`threshold`, detection note range) are already present there and are called out where relevant.

## 1. Concept

Audio comes into the plugin. **Output Mode** (`Audio` / `MIDI` / `Both`) selects where a fired sprinkle's notes go: out as MIDI (the plugin's original, still-default behavior), rendered directly by the plugin's own built-in synth (§7.7) into its audio output, or both at once. Two analyses run continuously on the input signal:

- **Pitch detection** determines which note is currently being played (the **trigger note**).
- **Envelope tracking** follows the input level. When the envelope crosses a **threshold** in the configured direction, it fires a **trigger** event carrying the current trigger note.

Each trigger launches a **sprinkle**: a burst of MIDI notes ("sparkles") arranged into **rays**. A ray is a short run of notes that climbs the (user-defined, in-key) note range while losing intensity — like a tuned, dreamy chime run. A sprinkle is a group of rays fired together from one trigger.

Hierarchy: **sprinkle → ray → sparkle** (sparkle = a single emitted MIDI note).

Indices used throughout:
- `ray_n` — index of a ray within its sprinkle, starting at 0 (`ray_n = 1` is the second ray).
- `sparkle_n` — index of a sparkle within its ray, starting at 0 (`sparkle_n = 1` is the second sparkle in that ray).

## 2. Detection stage

| Param | Description |
|---|---|
| `trigger_type` | `up` / `down` / `both` — fire when the envelope crosses `threshold` upward, downward, or either direction. `both` treats each direction as an independent trigger event. |
| `threshold` | Envelope level that must be crossed (in the direction(s) selected by `trigger_type`) to fire a sprinkle. *(Already implemented as `kParamThreshold` in the prototype.)* |
| `reactiveness` | Single knob controlling the envelope follower's responsiveness (internally shapes attack/release smoothing). Higher = tracks the input level faster; lower = smoother/slower tracking, fewer spurious triggers. *(The current prototype compares the raw instantaneous sample to `threshold` with no smoothing — `reactiveness` replaces that with a proper envelope follower.)* |
| `confidence` | Minimum pitch-detection confidence (normalized autocorrelation score, 0–1) a trigger needs before it fires. A trigger whose note can't be identified at least this confidently is **dropped silently** — never fired with a guessed/fallback note. See the tracker notes below for how each crossing direction resolves its note. *(Implemented as `kParamConfidence`, consumed by `core/PitchTracker.h`.)* |
| `detect_note_min`, `detect_note_max` | MIDI note range the pitch detector searches for the trigger note. *(Already implemented as `kParamMinNote`/`kParamMaxNote` in the prototype.)* **This range is distinct from `range_min`/`range_max` below**, which bound the generated sparkle notes, not the detected input pitch — see the naming note in §3. |

### 2.1 Continuous pitch tracking and trigger-note resolution

Pitch detection runs **continuously** (analysis every few hundred samples), independent of triggering — not once per trigger. The tracker maintains a hysteresis-debounced *stable note*, plus a *held note*: the last stable note whose score cleared `confidence`, remembered for ~250 ms after confidence collapses. The UI's note display always shows the live stable note and its confidence (e.g. `A3 87%`, or `--` when nothing is being tracked), trigger or no trigger.

Each crossing direction resolves its trigger note differently, because the audio available differs:

- **Up-crossing** (note starting — the analysis window is mid-transient): the trigger arms and fires as soon as a tracker update clears `confidence`, up to a ~50 ms ceiling; if confidence is never reached, the trigger is dropped. A note already confidently sounding when the envelope crosses (e.g. a swell) fires immediately.
- **Down-crossing** (note fading or gone): resolves immediately from the held note — what was confidently playing just before the crossing. Nothing confident within the hold window ⇒ dropped.

Implemented in `core/PitchTracker.h` (framework-free, covered by `sparkle_tests`).

## 3. Note range vs. detection range — naming clarification

The spec originally used "min/max detection notes" for the input-pitch range and `range_min`/`range_max` for the output-sparkle range, both called "note" ranges. To avoid confusion they're named distinctly:

- `detect_note_min` / `detect_note_max` — constrains what the pitch detector listens for on the **input**.
- `range_min` / `range_max` — constrains what MIDI notes sparkles can be generated at on the **output**.

## 4. Time base

Four time parameters — **`pre_delay`, `duration`, `ray_delay`, `delay`** (and their `_rm`/`_sm` multipliers) — each independently support either **project/tempo-relative time** (beats) or **absolute time** (ms or seconds), selectable per parameter.

`pre_interval`, `ray_interval`, and `interval` are **not** time parameters — despite the naming similarity to `delay`, they are pitch-domain parameters (counted in eligible-note steps, see §5), and always have integer-step semantics regardless of time base. This distinction (`delay` family = time, `interval` family = pitch-step) is easy to miss since "interval" colloquially can suggest time — flagging it explicitly.

## 5. Note eligibility matrix

A 12×12 grid of on/off buttons defines which sparkle pitch classes are reachable from which trigger pitch classes:

- Columns = trigger pitch class, **X axis, A → G#** (12 chromatic pitch classes, in that fixed order: A, A#, B, C, C#, D, D#, E, F, F#, G, G#).
- Rows = sparkle pitch class, **Y axis, A → G#**, same order.
- Cell `(column, row)` ON means: when the trigger note's pitch class is `column`, sparkles are allowed to land on pitch class `row`.
- A per-column toggle button sits above the grid, one per trigger pitch class: clicking it inspects that column's 12 individual cells — if any are ON, it sets all 12 OFF; otherwise it sets all 12 ON (`NoteMatrix::ToggleColumn`). This is a direct, destructive overwrite of the cells themselves, not a separate gate — clicking it off then on again does not restore whatever pattern was there before.
- A per-row toggle button sits to the left of the grid, one per sparkle pitch class, with the same click behavior applied down that row across every trigger column (`NoteMatrix::ToggleRow`).
- `NoteMatrix` also exposes an independent `SetColumnEnabled`/`SetRowEnabled` gate that ANDs with the cells without touching them (non-destructive) — kept for testability, but nothing in the UI currently drives it; the toggle buttons above use the destructive cell-overwrite instead.

**Resolving eligible notes:** given a trigger note, take its pitch class (`trigger_note mod 12`) to select the column. The eligible sparkle pitch classes are the rows where the cell is ON *and* the row's `SetRowEnabled` gate is ON (true by default, see above). The eligible sparkle **notes** are then every MIDI note in `[range_min, range_max]` whose pitch class is eligible, sorted ascending — this sorted list is what `interval`/`ray_interval` step counts walk across (see §7), and what `wrap_mode` (§6) operates on. Interval parameters count **positions in this eligible-note list**, not chromatic semitones — e.g. an interval of 2 skips over one eligible note, however many semitones that spans.

If the trigger note's own column has zero eligible rows (every cell off, whether via the toggle button or hand-editing), no sprinkle fires for that trigger.

Implemented as a single composite `IControl` (`ui/NoteMatrixControl.h`) rather than 144+24 separate `IVButtonControl`s, since every cell reads/writes the same shared `sparkle_core::NoteMatrix` directly (it isn't `IParam`-backed) and one control keeps resize/tag bookkeeping simple.

### 5.1 Key + scale quick-fill

Two additional controls act as a shortcut for filling in the matrix from music theory instead of hand-toggling 144 cells:

- `key_root` — root pitch class, A – G# (12 options), plus a 13th option, **Trigger Note**: instead of one fixed root shared by every column, each column uses its own trigger pitch class as that column's root (see below).
- `key_scale` — scale relative to `key_root`. Selectable scales: the 7 modes (Ionian/Major, Dorian, Phrygian, Lydian, Mixolydian, Aeolian/Minor, Locrian), plus Harmonic Minor, Melodic Minor, Major Pentatonic, Minor Pentatonic, Blues, and Chromatic (all 12 pitch classes).
- `key_mode` (labelled **Mode** in the UI) — further restricts each in-scale column's eligible rows to a chord stacked in thirds from that column's own scale degree, rather than every scale tone. Options: **Full Scale** (default — every scale tone, no restriction), **Root** (just the trigger note's own pitch class), **Power Chord** (root + 5th), **Sus2** (root + 2nd + 5th), **Sus4** (root + 4th + 5th), **Triad** (root + 3rd + 5th), **Seventh** (triad + 7th), **Ninth** (7th chord + 9th), **Eleventh** (9th chord + 11th), **Thirteenth** (11th chord + 13th). Every mode past Root is built by walking the *scale's own* degrees from the column's degree, not fixed chromatic intervals — e.g. with `key_root` C and `key_scale` Ionian, selecting Triad makes the D column eligible for D, F, A (whatever triad fits into C major starting on D), and Locrian's built-in diminished 5th carries through the same way rather than being corrected to a perfect 5th.

Changing any of the three controls **regenerates the whole matrix from scratch**: with a fixed `key_root`, it computes the scale's pitch-class set relative to `key_root`; with `key_mode` at Full Scale, cell `(column, row)` is ON where both `column` and `row` are in that set (OFF otherwise), same as before `key_mode` existed. For any other `key_mode`, each in-scale column instead gets only the rows belonging to the chord built on that column's own scale degree (walking `key_scale`'s degree list, wrapping for scales with fewer than the 7 degrees a chord extension implies) — an out-of-scale column still gets zero eligible rows regardless of `key_mode`. With `key_root` set to **Trigger Note**, there's no single shared root — each column instead uses its own trigger pitch class as the root, and `key_mode` applies the same chord-stacking relative to that per-column root (every column is trivially "in scale" here, since a scale always includes its own root). This is a one-time overwrite, not a standing constraint — after generating, individual cells/columns/rows can still be hand-edited directly in the matrix (§5), and those manual edits stick until `key_root`/`key_scale`/`key_mode` is changed again, which regenerates (and so discards) them.

This deliberately drives only the cells, never the independent `SetColumnEnabled`/`SetRowEnabled` gate mentioned in §5: that gate ANDs with the cells without touching them, and short-circuits before the cells are ever consulted. If the quick-fill used it to blank out-of-scale pitch classes, a manual cell edit afterward (e.g. re-enabling one black-key pair after picking C major) would silently have no effect, since the gate would keep killing that column/row regardless of what the cell said. Driving cells only keeps eligibility exactly "whatever the cells currently say" everywhere, matching how the column/row toggle buttons and individual cell clicks already work.

## 6. wrap_mode

Governs what happens when the next note an ascending/descending ray walk would land on falls outside `[range_min, range_max]`'s eligible-note list:

- **mirror** — the ray reverses direction (without repeating the boundary note it just landed on) and continues stepping the other way. If the reversed walk also runs past the *opposite* boundary before the ray's sparkles are exhausted, it reflects again, bouncing back and forth between the two ends of the range for as many sparkles as the ray needs.
- **around** — the walk jumps to the opposite end of the eligible-note list and continues in the same direction (circular wrap).
- **stop** — the ray stops emitting further sparkles once it would go out of range. Only that ray is affected; other rays in the same sprinkle are unaffected and continue normally.

## 7. Parameters and formulas

### 7.1 Structure

| Param | Description |
|---|---|
| `output_mode` | `Audio` / `MIDI` / `Both` — where a fired sprinkle's notes go (§1). Default `MIDI`, matching the plugin's original (pre-synth) behavior. Independent of Detection Mode (§2), which governs the *input* trigger source rather than the output path. |
| `n_rays` | Number of rays per sprinkle. |
| `n_sparkles_per_ray` | Base number of sparkles per ray (fixed a typo from `n_sparkles_pray` in the original draft). |
| `n_sparkles_per_ray_rm` | Ray multiplier on sparkle count: `n_sparkles(ray_n) = round(n_sparkles_per_ray * n_sparkles_per_ray_rm^ray_n)`, floored at 0 (a ray computing to 0 or fewer sparkles simply doesn't fire). |
| `range_min`, `range_max` | Output note range for sparkles (see §3). |
| `wrap_mode` | `mirror` / `around` / `stop` — see §6. |

### 7.2 Trigger-to-sprinkle offset

Applied once, ahead of the ray/sparkle chains below:

- `pre_delay` — time delay between the trigger and the start of the sprinkle. Combines with ray 0's own `ray_delay` step (see §7.4) — it does not replace it.
- `pre_interval` — pitch-step offset between the trigger note and the sprinkle's first sparkle. Combines with ray 0's own `ray_interval` step (see §7.5) the same way.

### 7.3 Base per-sparkle properties (evaluated directly, not cumulative)

These describe an absolute property of one sparkle and are evaluated directly at `(ray_n, sparkle_n)` — no summation involved:

- `loudness`, `loudness_rm`, `loudness_sm`:
  `loudness(ray_n, sparkle_n) = loudness * loudness_rm^ray_n * loudness_sm^sparkle_n`
  (MIDI velocity, 1–127.)
- `duration`, `duration_rm`, `duration_sm`:
  `duration(ray_n, sparkle_n) = duration * duration_rm^ray_n * duration_sm^sparkle_n`

### 7.4 Timing chain (cumulative)

`delay`-family parameters describe the **gap between two consecutive events**, so a sparkle's absolute time is the running sum of those gaps, not a single formula plugged in at `sparkle_n`/`ray_n` — the original draft wrote it as a single term, which doesn't actually reproduce "delay between two consecutive X" once `_rm`/`_sm` differ from 1. This spec uses cumulative sums instead:

**Ray start time** (time from trigger to the first sparkle of ray `ray_n`):

```
RayStart(ray_n) = pre_delay + Σ (ray_delay * ray_delay_rm^i)   for i = 0..ray_n
```

(Ray 0's own `i=0` term is the "first ray's `ray_delay`" that `pre_delay` explicitly adds up with.)

**Sparkle time within its ray** (offset from `RayStart(ray_n)`; sparkle 0 of a ray sits exactly at `RayStart` — there's no equivalent "entry delay" before a ray's first sparkle):

```
WithinRay(ray_n, sparkle_n) = Σ (delay * delay_rm^ray_n * delay_sm^(k-1))   for k = 1..sparkle_n
```

(Empty sum = 0 when `sparkle_n = 0`.)

**Absolute sparkle time:**

```
Time(ray_n, sparkle_n) = trigger_time + RayStart(ray_n) + WithinRay(ray_n, sparkle_n)
```

### 7.5 Pitch chain (cumulative)

Same cumulative-sum treatment, and for the same reason, applies to `interval`-family parameters:

**Ray's first-note pitch-step offset from the trigger note:**

```
RayIntervalOffset(ray_n) = pre_interval + Σ (ray_interval * ray_interval_rm^i)   for i = 0..ray_n
```

**Sparkle's additional pitch-step offset within its ray** (sparkle 0 of a ray uses `RayIntervalOffset` alone):

```
WithinRayInterval(ray_n, sparkle_n) = Σ (interval * interval_rm^ray_n * interval_sm^(k-1))   for k = 1..sparkle_n
```

**Total raw step offset:**

```
RawSteps(ray_n, sparkle_n) = RayIntervalOffset(ray_n) + WithinRayInterval(ray_n, sparkle_n)
```

Round `RawSteps` to the nearest integer. The resulting signed integer is the number of positions to walk (up if positive, down if negative, staying put if zero) from the trigger note's position in the eligible-note list (§5), applying `wrap_mode` at the list boundaries, to land on the sparkle's actual MIDI note — a zero offset lands back on the trigger note itself.

### 7.6 Panning

| Param | Description |
|---|---|
| `panning` | `mono` / `random` / `sine` / `triangle` / `square` / `saw`. `mono` ignores all panning params below. `random` uses only the `width` and `seed` params. |
| `seed` | User-picked integer, used only by `random`. Combined with the trigger's absolute position in the DAW project timeline (not wall-clock time, and not any plugin-internal counter) so the same trigger, at the same point in the same project, reproduces the same "random" pan across playthroughs — while still varying per ray/sparkle/note within one trigger. A plain saved value rather than a "reseed" action: dialing the same number back in recovers a pattern the user liked. |
| `width`, `width_rm`, `width_sm` | Stereo spread, evaluated directly (same pattern as §7.3): `width(ray_n, sparkle_n) = width * width_rm^ray_n * width_sm^sparkle_n`. |
| `phase`, `phase_rm`, `phase_sm` | Position along the pan waveform's cycle, evaluated directly: `phase(ray_n, sparkle_n) = phase * phase_rm^ray_n * phase_sm^sparkle_n`. Used only by `sine`/`triangle`/`square`/`saw`. |
| `ray_rotation` | `L` / `R` — base sign applied to ray 0's computed pan position (see below). |
| `ray_rotation_rm` | `Keep` / `Invert` — whether each subsequent ray's sign matches the previous ray's, or flips it. Applied progressively per ray, not as a continuous exponential multiplier like the other `_rm` params — flagged here since it reuses the `_rm` suffix for a different kind of behavior. |

**Pan position formula.** For `sine`/`triangle`/`square`/`saw`, a sparkle's pan position is a static lookup — the sparkle's own `phase` and `width` completely determine it, there's no continuous sweep over time:

```
sign(ray_n) = (ray_rotation == L ? 1 : -1) * (ray_rotation_rm == Keep ? 1 : -1)^ray_n

pan(ray_n, sparkle_n) = clamp( sign(ray_n) * width(ray_n, sparkle_n) * Wave(panning, phase(ray_n, sparkle_n)), -1, 1 )
```

`1 = 100% L`, `-1 = 100% R`. `ray_rotation` flips the sign of the *final computed position* for the whole ray (not the phase going into the wave) — e.g. with `panning = sine`, `width = 0.5`, `phase = 0.3`: `Wave = sin(0.3 * 2π)`, and the ray's sign multiplies that result. `ray_rotation_rm` then decides whether each following ray keeps that sign or flips it again, alternating per ray when set to `Invert`. The `clamp` matters because `width` can exceed 1 via `width_rm`/`width_sm` growth, which would otherwise push `pan` outside the valid ±1 range.

`Wave(panning, p)` — all four are periodic in `p` with period 1 (so any phase value, including ones pushed outside `[0,1)` by `phase_rm`/`phase_sm`, wraps naturally):

- `sine`: `sin(2π p)`
- `triangle`: ramps linearly `-1 → 1` over `p ∈ [0, 0.5]`, then `1 → -1` over `p ∈ [0.5, 1]`.
- `square`: `-1` over `p ∈ [0, 0.5)`, `+1` over `p ∈ [0.5, 1)`.
- `saw`: ramps linearly `-1 → 1` over `p ∈ [0, 1)`, then jumps back to `-1`.

Real per-sparkle stereo panning is only possible when `output_mode` includes `Audio` — MIDI has no true per-note pan (CC10 is a per-channel control, not per-voice), so in `MIDI`-only mode the pan values computed above are simply never applied to anything audible. The formulas themselves always run regardless of `output_mode`, same as every other per-sparkle property.

**`random`'s pan position.** Unlike the waveform modes above, `random` has no `Wave()` lookup — instead a sparkle's pan is a stateless hash of everything that identifies "this exact trigger, this exact sparkle":

```
unit(seed, timeline_sample, trigger_note, ray_n, sparkle_n) = Hash(...) -> uniform [0, 1)

pan(ray_n, sparkle_n) = clamp( width(ray_n, sparkle_n) * (unit * 2 - 1), -1, 1 )
```

`timeline_sample` is the trigger's absolute position in the project timeline (the host's transport position when the trigger fired, not a plugin-internal free-running counter — see `Sparkles::ProcessBlock`'s `GetSamplePos()` usage), so replaying the same project from the start reproduces the same "random" pan for every trigger, while a trigger a millisecond earlier or later (a genuinely different timeline position) gets a different, still-reproducible pan. `ray_rotation`/`ray_rotation_rm`/`phase` have no effect on `random` — only `width` shapes its output, same as before this hash replaced a plain RNG stream.

### 7.7 Synth (Audio Output Mode)

When `output_mode` includes `Audio`, each generated sparkle also spawns one voice in the plugin's own built-in synth, rendered directly into its stereo audio output (mixed additively with whatever dry input passthrough is already there) instead of — or alongside — being sent out as MIDI.

| Param | Description |
|---|---|
| `wave_shape` | Continuous morph across the four classic waveforms: `0` = pure Sine, `1` = pure Triangle, `2` = pure Square, `3` = pure Saw, linearly crossfading the two neighboring shapes at fractional values (e.g. `1.5` = half Triangle/half Square). A single global control — unlike every other property in this section, it is **not** resolved per `(ray_n, sparkle_n)` and has no `_rm`/`_sm` modifiers. |
| `attack`, `attack_rm`, `attack_sm` | Time (seconds) for a voice's amplitude to ramp linearly from 0 to its post-decay peak. Resolved per sparkle exactly like §7.3's `loudness`/`duration`: `attack(ray_n, sparkle_n) = attack * attack_rm^ray_n * attack_sm^sparkle_n`. |
| `decay`, `decay_rm`, `decay_sm` | Time (seconds) for the amplitude to ramp linearly from its peak down to `sustain`'s level. Same resolution pattern as `attack`. |
| `sustain`, `sustain_rm`, `sustain_sm` | Level (0–1) held for as long as the sparkle's gate (its own `duration`, §7.3) remains open. Resolved the same way, then clamped to `[0, 1]` (its `_rm`/`_sm` multipliers can otherwise push it outside that range). |
| `release`, `release_rm`, `release_sm` | Time (seconds) for the amplitude to ramp linearly from wherever it was at gate-close down to 0. Same resolution pattern. |

Unlike §7.4's `delay`/`ray_delay`/etc., attack/decay/release are plain seconds rather than a beats/ms/s `TimeParam` toggle — a deliberate simplification, since these describe a short percussive envelope shape rather than a tempo-locked timing chain.

**Envelope shape.** A voice's envelope is entirely determined by its gate length (the sparkle's `duration`) and its resolved attack/decay/sustain/release — there's no separate "note off" the way MIDI has one, since a sparkle's lifetime is fixed at generation time:

- While the gate is open: ramps 0→peak over `attack`, then peak→`sustain` over `decay`, then holds at `sustain` for whatever's left of the gate.
- The instant the gate closes: release begins from *whatever level the envelope had actually reached* (continuous, no jump) and ramps to 0 over `release` — so a gate shorter than `attack + decay` releases early without a click, rather than waiting for attack/decay to finish first.

**Panning.** §7.6's `panning`/`width`/`phase`/`ray_rotation` formulas apply directly to a voice's stereo position (constant-power pan law), since Audio Output Mode is exactly the case those params were designed for.

**Polyphony.** Voices are drawn from a fixed-size pool (independent of MIDI, which has no such limit). Once the pool is full, spawning a new voice steals whichever currently-sounding voice is quietest at that instant (ties broken by oldest) — a deliberate, bounded-cost tradeoff to keep CPU load predictable regardless of how many sparkles are in flight, rather than growing polyphony without limit.

**Output safety.** The final mixed output (dry passthrough + all synth voices) passes through a soft-clip safety net: identity at normal levels, asymptotically approaching ±1 only once the signal would otherwise clip — protecting against overs when many simultaneous sparkles' voices stack up, without audibly coloring normal-level signal. Not a user-facing param.

## 8. Presets

The in-plugin Presets button is a fixed factory list, not a general save/recall mechanism — the DAW's own preset browser already handles saving/recalling the plugin's full state (every param, the note matrix, everything), independent of this button. This button's own scope is deliberately narrower: it only covers the structure/timing/pitch-step/loudness/duration and synth/panning params (the UI's General, Pitch and Timing, and Synth tabs), plus `key_mode` (§5.1). It does **not** touch detection params (§2 — Detection Mode, Trigger On, Threshold, Min Velocity, Envelope Reactiveness, Note Confidence, Min/Max Detection Note), the note eligibility matrix (§5), its `key_root`/`key_scale` quick-fill controls (§5.1), or Output Mode. Loading a different preset changes the sprinkle/ray/sparkle "sound" (now including the chord shape `key_mode` quick-fills onto the matrix) but leaves however the performer currently has detection tuned, whatever key/scale they're playing in, and how they've routed output, untouched.

This is deliberate: detection tuning and `key_root`/`key_scale` both represent how the plugin is currently listening/what key the performer is currently playing in, independent of which sprinkle "sound" is loaded — switching presets mid-performance shouldn't retune detection or yank the plugin out of the current key. `key_mode`, by contrast, doesn't pin the matrix to a key at all — it's a chord-shape choice (root only, power chord, triad, up through 13th) layered on top of whatever key/scale is already dialed in, so it's treated as part of the "sound" the same way Wrap Mode is. Applying a preset still regenerates the note matrix from the (unchanged) `key_root`/`key_scale` and (preset-set) `key_mode` — via the same `Sparkles::OnParamChange`/`sparkle_core::ApplyKeyScale` path a manual Mode-dropdown change goes through — so the matrix never goes stale after a preset load. Output Mode is excluded for the same reason as detection: it's where the performer has chosen to route sound (MIDI to an external instrument, the built-in synth, or both), not part of the "sound" a preset describes — silently flipping it (e.g. to MIDI-only with nothing downstream listening) makes a preset seem to do nothing at all. All of it is still part of the plugin's normal saved state (DAW project save/reload, copying the plugin instance, or the DAW's own preset browser) — excluded from the in-plugin Presets button only means that specific button doesn't touch it, not that it's ephemeral.

## 9. Assumptions and interpretations applied in this spec

Resolved by you during review (recommended options, all accepted):

1. **Cumulative vs. direct formulas** for `delay`/`ray_delay` and `interval`/`ray_interval`: cumulative sum (§7.4, §7.5), not a single direct term.
2. **`wrap_mode: stop` scope**: kills only the current ray, not the whole sprinkle.
3. **`wrap_mode: mirror` at a second boundary**: keeps bouncing back and forth for as many sparkles as the ray needs, rather than reflecting once.
4. **`reactiveness`**: a single combined knob, not separate attack/release parameters.
5. **`ray_rotation`** (§7.6, added after you supplied the `width * Wave(phase)` pan formula): flips the sign of the ray's *computed output pan position*, not the phase fed into the waveform.
6. **Key/scale quick-fill vs. manual matrix** (§5.1): the two dropdowns generate/overwrite the matrix, but the underlying per-cell/column/row toggles from §5 still exist for hand-tweaking afterward, rather than the dropdowns being the only way to set the matrix.
7. **Scale list** (§5.1): used the standard set — 7 modes, Harmonic Minor, Melodic Minor, Major/Minor Pentatonic, Blues, Chromatic — rather than a custom list.

`saw` is back in the `panning` list (§7.6) per your edit — the original draft's "saw wouldn't make sense" reasoning no longer applies now that panning is a static per-sparkle `width * Wave(phase)` lookup rather than a continuous sweep.

Other small fixes/clarifications made while writing this up (typos and gaps in the original draft, not design decisions):

- `n_sparkles_pray` → `n_sparkles_per_ray` (typo).
- `ray_interval_rm^(ran_n)` → `ray_interval_rm^(ray_n)` (typo, `ran_n` isn't defined anywhere else).
- Added the `threshold` parameter, which the trigger description depends on (`trigger_type` needs something to cross) but which wasn't listed as a named param in the original draft. It already exists in the prototype as `kParamThreshold`.
- Named the previously-unnamed "min max detection notes" params as `detect_note_min`/`detect_note_max`, and explicitly distinguished them from `range_min`/`range_max` (§3) — both are "note range" concepts but govern different things (input detection vs. output sparkle range), and the original wording risked conflating them.
- Moved `pre_delay`/`pre_interval` into their own subsection (§7.2) ahead of the base ray/sparkle params, since they only affect the trigger→ray0 handoff rather than being peers of `ray_delay`/`ray_interval`/`delay`/`interval`.
- Made explicit that `loudness` is MIDI velocity (matches the prototype, which already sends velocity 127) — the original draft didn't state units.
- Clarified `delay`-family params are time-domain (support the beats/absolute-time toggle) while `interval`-family params are pitch-step-domain integers, never time — the original listed them alongside each other without saying so explicitly.

None of these change the creative intent described in your draft; they fill in gaps needed to make the formulas and the eligible-note matrix implementable and internally consistent.
