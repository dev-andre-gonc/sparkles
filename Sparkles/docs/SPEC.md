# Sparkles — Spec

> Sparkles: rays of sunshine.

Status: **design spec, not yet implemented**. This document describes the intended behavior of the finished plugin. A minimal prototype already exists in `Sparkles.cpp`/`.h` (threshold-crossing trigger + autocorrelation pitch detection, no ray/sparkle structure yet) — a few names below (`threshold`, detection note range) are already present there and are called out where relevant.

## 1. Concept

Audio comes into the plugin. No audio goes out — the plugin's only output is MIDI. Two analyses run continuously on the input signal:

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
| `detect_note_min`, `detect_note_max` | MIDI note range the pitch detector searches for the trigger note. *(Already implemented as `kParamMinNote`/`kParamMaxNote` in the prototype.)* **This range is distinct from `range_min`/`range_max` below**, which bound the generated sparkle notes, not the detected input pitch — see the naming note in §3. |

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
- A per-column toggle turns off/on that whole column at once: if off, that trigger pitch class is ignored entirely and no sprinkle is generated for it, regardless of individual cells.
- A per-row toggle turns off/on that sparkle pitch class **for every trigger column simultaneously**: if off, that pitch class can never be produced as a sparkle no matter which cells are ON.

**Resolving eligible notes:** given a trigger note, take its pitch class (`trigger_note mod 12`) to select the column. The eligible sparkle pitch classes are the rows where the cell is ON *and* the row toggle is ON. The eligible sparkle **notes** are then every MIDI note in `[range_min, range_max]` whose pitch class is eligible, sorted ascending — this sorted list is what `interval`/`ray_interval` step counts walk across (see §7), and what `wrap_mode` (§6) operates on. Interval parameters count **positions in this eligible-note list**, not chromatic semitones — e.g. an interval of 2 skips over one eligible note, however many semitones that spans.

If the trigger note's own column is off, or has zero eligible rows, no sprinkle fires for that trigger.

Implementation note (from the project's `CLAUDE.md`): start with `IVButtonControl`/similar IVControls for this matrix; a custom PNG-based control set is a later pass once the logic is verified.

### 5.1 Key + scale quick-fill

Two additional controls act as a shortcut for filling in the matrix from music theory instead of hand-toggling 144 cells:

- `key_root` — root pitch class, A – G# (12 options).
- `key_scale` — scale relative to `key_root`. Selectable scales: the 7 modes (Ionian/Major, Dorian, Phrygian, Lydian, Mixolydian, Aeolian/Minor, Locrian), plus Harmonic Minor, Melodic Minor, Major Pentatonic, Minor Pentatonic, Blues, and Chromatic (all 12 pitch classes).

Changing either control **regenerates the whole matrix from scratch**: it computes the scale's pitch-class set relative to `key_root`, sets the column and row master toggles ON for exactly those pitch classes (OFF for the rest), and sets every cell `(column, row)` ON where both `column` and `row` are in that set (OFF otherwise). This is a one-time overwrite, not a standing constraint — after generating, individual cells/columns/rows can still be hand-edited directly in the matrix (§5), and those manual edits stick until `key_root`/`key_scale` is changed again, which regenerates (and so discards) them.

## 6. wrap_mode

Governs what happens when the next note an ascending/descending ray walk would land on falls outside `[range_min, range_max]`'s eligible-note list:

- **mirror** — the ray reverses direction (without repeating the boundary note it just landed on) and continues stepping the other way. If the reversed walk also runs past the *opposite* boundary before the ray's sparkles are exhausted, it reflects again, bouncing back and forth between the two ends of the range for as many sparkles as the ray needs.
- **around** — the walk jumps to the opposite end of the eligible-note list and continues in the same direction (circular wrap).
- **stop** — the ray stops emitting further sparkles once it would go out of range. Only that ray is affected; other rays in the same sprinkle are unaffected and continue normally.

## 7. Parameters and formulas

### 7.1 Structure

| Param | Description |
|---|---|
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

Round `RawSteps` to the nearest integer; if it rounds to exactly 0, bump it away from zero to the nearest nonzero integer (±1) instead — a sparkle should never land back on the trigger note itself. The resulting signed integer is the number of positions to walk (up if positive, down if negative) from the trigger note's position in the eligible-note list (§5), applying `wrap_mode` at the list boundaries, to land on the sparkle's actual MIDI note.

### 7.6 Panning

| Param | Description |
|---|---|
| `panning` | `mono` / `random` / `sine` / `triangle` / `square` / `saw`. `mono` ignores all panning params below. `random` uses only the `width` params. |
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

## 8. Presets

Presets save and recall every parameter in this spec **except** the note eligibility matrix (§5) and its `key_root`/`key_scale` quick-fill controls (§5.1). Loading a different preset changes the sprinkle/ray/sparkle design — structure, timing, pitch-step, loudness, duration, panning, detection params — but leaves whatever key/scale/matrix the performer currently has dialed in untouched.

This is deliberate: the matrix represents the key the performer is currently playing in, which is independent of which sprinkle "sound" is loaded — switching presets mid-performance shouldn't yank the plugin out of the current key. The matrix and `key_root`/`key_scale` are still part of the plugin's normal saved state (DAW project save/reload, copying the plugin instance) — "excluded from presets" only means the preset browser/bank doesn't touch them, not that they're ephemeral.

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
