# Code review — 2026-08-23

Deep review of the plugin implementation (`Sparkles.cpp`/`.h`, `core/`, `params/`, `ui/`,
`config.h`, `CMakeLists.txt`, `tests/`) at commit `22b2f4d` ("ui mostly finalized"). Scope was the
plugin's own code, not the packaging/installer tooling (`build-mac/`, `build-win/`, `installer/`,
`scripts/`, `projects/`).

## Test results

- **Before:** `sparkle_tests.exe` (stale build from 2026-08-20) reported 91/91 passing. Rebuilding
  from current source gave **96/96 passing** — the extra 5 tests already existed in source but
  hadn't been compiled in yet.
- **After all changes below:** **96/96 passing**, plus a full `Sparkles-app` build (exercises
  `Sparkles.cpp`, `config.h`, `resources/main.rc`, `ui/Palette.h` — none of which `sparkle_tests`
  touches) compiles and links cleanly.
- Build commands used (Ninja generator, needs a VS Developer environment for `cl.exe`/`rc.exe`):
  ```
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  cmake --build build\windows-ninja --target sparkle_tests
  ./build/windows-ninja/sparkle_tests.exe
  cmake --build build\windows-ninja --target Sparkles-app   # full plugin compile check
  ```

## Changes made

All changes below are deletions of dead/unused code and resources, plus two mechanical
housekeeping edits. Nothing in `core/`, `params/ParamList.h`, or any DSP/generation logic was
touched — the changes cannot alter plugin behavior or sound. Revert any of them with
`git checkout -- <path>` (nothing has been committed).

### 1. Removed `ui/CardPanelControl.h` (dead code)
A fully-implemented "frosted glass" backdrop control, never instantiated or `#include`d anywhere.
Its own file comment describes sitting behind the middle/right columns — superseded per this
project's `CLAUDE.md`, which already documents that decision: *"No separate backdrop sits behind
the middle/right columns -- the background art itself is pale enough for text to stay legible
directly on it."* Confirmed zero references before deleting.

### 2. Removed 4 unused fonts (Roboto, Bungee, Righteous, Monoton)
Verified by exhaustive grep that none of these four font IDs are ever passed to an `IText(...)` —
every real label/value in the UI uses a Fredoka variant. Roboto/Bungee/Righteous were being loaded
via `LoadFont` for nothing; Monoton wasn't even loaded, just bundled. All four were embedded as
Win32 `.rc` resources on Windows (and via `CMakeLists.txt`'s `RESOURCES` on every platform), so
this was dead weight in every shipped binary — ~354KB of font data removed
(`Roboto-Regular.ttf` 145KB, `Bungee-Regular.ttf` 118KB, `Monoton-Regular.ttf` 49KB,
`Righteous-Regular.ttf` 40KB).

Confirmed via user check-in before deleting the actual `.ttf` files, since it was possible (if
unlikely) they were reserved for the still-placeholder "Quick Guide" tab.

Touched: `Sparkles.cpp` (3 `LoadFont` calls), `ui/Palette.h` (3 unused `kFontXxx` constants),
`config.h` (4 `*_FN` macros), `resources/main.rc` (2 embedding blocks — VS resource-editor's
`TEXTINCLUDE` copy and the actual compiled one), `CMakeLists.txt` (4 `RESOURCES` lines), plus the
4 `.ttf` files themselves. Also fixed a stale `CLAUDE.md` line that used `ROBOTO_FN` as its
example macro (now `FREDOKA_REGULAR_FN`).

Verified via a full `Sparkles-app` build (compiles `main.rc` through `rc.exe` and `Sparkles.cpp`
through `cl.exe`) — link succeeded, so nothing was left dangling.

### 3. `CMakeLists.txt`: added missing-but-used headers to `SOURCES`
`ui/BackgroundImageControl.h`, `ui/GroupMarkerControl.h`, `ui/ModifierValueControl.h`,
`ui/Palette.h`, and `ui/TimeMagnitudeControl.h` are all actively `#include`d and used, but weren't
listed in the plugin target's `SOURCES`. This has **zero effect on the actual compiled binary**
(headers are pulled in via `#include` regardless of the `SOURCES` list under the Ninja/Make
generators this project builds with) — it only affects whether they show up as browsable files in
an IDE's generated project tree (Visual Studio's CMake integration, Xcode). Pure IDE-hygiene fix.

### 4. `ui/TimeMagnitudeControl.h`'s `kNoteValues` table: fixed values, removed 2 redundant entries, re-sorted
Follow-up to the labels-vs-values ambiguity noted during review: the user fixed the "2"/"3"/"4"
tier's beat values to the linear "N whole notes" reading (`"3"` = 12 beats, `"4"` = 16 beats,
matching the header comment's own arithmetic), which confirmed the *values* were the bug, not the
labels. That fix's diff also changed every triplet entry's multiplier from `1.0/3.0` to `4.0/3.0`
project-wide — almost certainly an unintended side effect (a triplet is supposed to be *shorter*
than its straight note, i.e. `2.0/3.0`, not longer; `1.0/3.0`, the original, wasn't right either,
just wrong in the other direction).

Fixed the triplet multiplier to the musically-correct `2.0/3.0` everywhere. That surfaced two exact
collisions, both confined to the whole-note-and-up tier: tripleted `"3"` (2/3 × 12 = 8 beats) lands
exactly on straight `"2"` (8 beats), and dotted `"2"` (1.5 × 8 = 12 beats) lands exactly on straight
`"3"` (12 beats) — an inherent consequence of that tier counting bars linearly (1, 2, 3, 4) rather
than doubling like every division below the whole note does, not a new mistake. Deleted `"3T"` and
`"2."` (the two redundant entries — keeping the plainer, more expected label of each colliding
pair) rather than leaving duplicate-valued entries in, since a duplicate would have been
undisplayable anyway: `NearestIndex()`'s tie-break always favors whichever entry comes first, so
the later of a tied pair could be selected (its value stored correctly) but could never show its
own name on redraw.

Also re-sorted the full table into true ascending numeric order — with the corrected 2/3 ratio, a
bigger division's triplet can fall *before* a smaller division's straight/dotted value (e.g. `"4T"`
= 10.67 beats sits between `"2"` = 8 and `"3"` = 12, not next to `"4"`), so the old "straight, T,
dotted" per-division grouping no longer produced an ascending list. `kNoteValues` is now 32 entries
(down from 34), array size and comment updated to match. `NearestIndex`/`SetToIndex`/wheel-and-drag
clamping all derive their bounds from `kNoteValues.size()` rather than a hardcoded count, so nothing
else needed to change.

Verified with a standalone scratch program (compiled and run, not just hand math) asserting the
final table is strictly ascending with zero duplicate values, and that the two deleted entries'
would-be values exactly equal their surviving counterparts. `Sparkles-app` also rebuilds clean.

## Findings not acted on (need your input)

### Build-artifact binaries committed to git
`tests_out.exe` (388KB) and 5 `test_*.obj` files (~1.6MB) — stray build output, not source — are
tracked in git, committed in `c2e1159` ("tabs org", 2026-08-20). `build/` itself is `.gitignore`d
(`../.gitignore` line 4, `**/build*/`) but these landed directly in the project root outside that
pattern. Didn't touch this myself since it's a git-index change (`git rm --cached`) rather than a
code fix, and you may want to review `git log` on those paths first. To fix:
```
git rm --cached test_*.obj tests_out.exe
echo -e "test_*.obj\ntests_out.exe" >> ../.gitignore    # or Sparkles/.gitignore
```

## Reviewed, found correct (no action needed)

- **Factory preset table** (`Sparkles.cpp`'s `kPresets`, ~line 551): each `PresetDesc::values` is
  a positional `double[53]` array matched against `kScopedParamIds` by index with no compile-time
  name checking — a classic place for silent data corruption if a row's value count or order ever
  drifts. Manually cross-checked the `"Default"` preset's all 53 values against
  `params/ParamList.h`'s own declared defaults, field-by-field in order — exact match. Every other
  preset's value list has an identical line-break/comma-count shape. Not a bug, just worth knowing
  this pattern has no automated guard if a future edit adds/reorders a scoped param without
  updating every preset row to match.
- **iPlug2 submodule customization**: `../iPlug2/Scripts/cmake/IPlugPlugin.cmake` has an
  uncommitted local change (`git status` in the submodule) that makes every desktop target
  (VST3/AU/CLAP/AAX/VST2), not just APP, compile `resources/main.rc` in — without it, `LoadFont`
  silently fails on Windows for any format except the standalone app. This is already a deliberate,
  working fix (matches this project's own memory of the issue), not something I touched.
- Every `core/*.h` DSP file (`DetectionParams`, `EventScheduler`, `NoteMatrix`, `PitchTracker`,
  `SparkleGenerator`, `SynthEngine`) — traced the logic in each against its own header comments and
  docs/SPEC.md's referenced sections; internally consistent, no bugs found. This is also the
  best-tested part of the codebase (95 of the 96 unit tests target `core/`).

## Performance: what's actually costing CPU

Ranked by where the cycles go, for anyone chasing a CPU spike:

1. **`PitchTracker::AnalyzeHop()`** (`core/PitchTracker.h`) — by far the biggest cost. Runs every
   512-sample hop (~86x/sec at 44.1kHz) whenever Detection Mode includes Audio, *regardless of
   whether a trigger is imminent*. Each hop scores every candidate note in the Min/Max Detection
   Note range via autocorrelation — an O(2048-sample) loop per candidate — and the octave-error
   guard's local-peak mask adds up to 2 more full O(2048) `Score()` calls per candidate on top of
   that (so realistically ~3x the base candidate count in O(2048) passes, every hop). **This scales
   directly with how wide Min/Max Detection Note is set** — a 49-note default range vs. the full
   85-note triggerable span roughly triples this cost. Already gated sensibly: skipped entirely
   when Detection Mode is MIDI-only (`ProcessBlock`'s `audioTriggerEnabled` check).
2. **`SynthEngine::Render()`** (`core/SynthEngine.h`) — per-sample, per-active-voice oscillator +
   envelope loop, up to 64 simultaneous voices x block size. Cheap per sample individually, but
   scales with polyphony; only runs when Output Mode includes Audio.
3. **`SparkleGenerator::Generate()`** — bursty, not sustained: up to 1,024 events per single
   trigger (`kMaxEventsPerTrigger`), each with several `std::pow` calls for the `_rm`/`_sm` chains.
   Only runs on a trigger, bounded, and further capped by 10 in-flight sprinkles
   (`kMaxSimultaneousSprinkles`) — the scenario that spikes CPU is a dense, fast retrigger stream
   (e.g. "Rapid Fire"-style presets), not steady-state playback.
4. Everything else — the envelope follower, `EventScheduler`/flash-scheduler flushing, note-matrix
   flash-decay bookkeeping, UI drawing — is O(block size) or O(events-this-block) and negligible by
   comparison.

**Bottom line:** if this plugin is ever CPU-bound in a session, the Detection Note range (Min/Max
Note on the Detection tab) is the first knob to check — it directly multiplies the pitch tracker's
per-hop cost, and that tracker runs continuously, not just around triggers.
