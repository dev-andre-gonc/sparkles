# CLAUDE.md

iPlug2 plugin project. This document describes choices in this project's structure that are not obvious from the iPlug2 framework docs.

> This file is duplicated by `../duplicate.py` along with the rest of the project, and the project-name string-rewrite pass runs over it — so `Sparkles.cpp` etc. resolve to the new project's filenames after duplication.

## UI: responsive IGraphics layout

The UI in `Sparkles.cpp` is a **resizable / responsive IGraphics** example. The pattern is the important thing to preserve when extending it.

Key pieces:

- `config.h` enables host resize and sets bounds: `PLUG_HOST_RESIZE 1`, `PLUG_MIN_WIDTH/HEIGHT`, `PLUG_MAX_WIDTH/HEIGHT`.
- `OnHostRequestingSupportedViewConfiguration` returns `true` so hosts know any size in range is acceptable.
- `mLayoutFunc` runs on both first attach AND every resize: in the initial-attach branch it calls `pGraphics->SetLayoutOnResize(true)` and `AttachCornerResizer(...)`, which together cause it to re-run on every resize.

### How `mLayoutFunc` is structured

The function computes every IRECT once at the top from the current `pGraphics->GetBounds()`, then splits:

1. **Resize branch (appears first in the code):** `if (pGraphics->NControls()) { ... return; }` — re-applies each tagged control's new rect via `SetTargetAndDrawRECTs(...)` (background uses `GetBackgroundControl()`), then returns early so the attach code below doesn't run again.
2. **Initial-attach branch (fall-through below the `if`):** one-time setup — `SetLayoutOnResize(true)`, `AttachCornerResizer`, `LoadFont`, `AttachPanelBackground`, then `AttachControl(...)` for each control with its tag.

The early `return` in the resize branch is load-bearing — without it, controls would be re-attached on every resize.

Because the IRECTs are computed once at the top, the same layout math feeds both branches.

### Background artwork drives the three-column layout

`resources/img/background.png` is not a plain fill -- it bakes in the "SPARKLES" logo/tagline
(top-left), the outer border + corner-resizer dots, and two vertical divider lines marking where
the left tab column and right indicator column should sit. `ui/BackgroundImageControl.h` stretches
it to `bounds` every resize via `DrawFittedBitmap` (plain `IGraphics::AttachBackground()` draws at
native size instead, which would leave gaps/crop on resize -- see that file's header comment).

`mLayoutFunc`'s three columns (`leftColumnBounds`/`middleColumnBounds`/`rightColumnBounds`) are cut
at `kLeftColFrac`/`kRightColFrac` -- fractions of `bounds.W()`, not fixed pixel widths -- measured
directly off that PNG's divider lines. If the background art is ever replaced, re-measure those
fractions (and `kLogoBottomFrac`, `kBorderPadXFrac`/`kBorderPadYFrac`) against the new image rather
than eyeballing them. `ui/Palette.h` (`sparkle_palette` namespace) holds the named colors/fonts the
art was built from -- every control that draws its own colors (`NoteBarsControl`,
`NoteMatrixControl`, `EnvelopeMeterControl`, etc., plus `kCompactStyle`/`MakeParamStyle`/
`MakeTabStyle`/`kActionButtonStyle`/`kGroupStyle` in Sparkles.cpp) pulls from there rather than
hardcoding hex values, so a palette change only has to happen in one place. No separate backdrop
sits behind the middle/right columns -- the background art itself is pale enough for text to stay
legible directly on it.

### Aspect ratio is locked, and that's what makes text scale with the window

`config.h`'s `PLUG_WIDTH`/`PLUG_HEIGHT` (748x414) is exactly half of `background.png`'s own pixel
size (1497x828). `PLUG_HOST_RESIZE` is `0` and `PLUG_MIN/MAX_WIDTH/HEIGHT` are both pinned to
`PLUG_WIDTH`/`PLUG_HEIGHT` -- between them, no host ever gets to resize this logical size to
anything else (a host-driven resize path exists per format, e.g. VST3's `checkSizeConstraint`/
`onSize`, entirely independent of anything IGraphics does; disabling it there is simpler and more
reliable than trying to aspect-constrain a live host-driven drag after the fact). The *only* way to
resize at all is the in-UI corner-resizer, attached with `EUIResizerMode::Scale` rather than `Size`
(see mLayoutFunc). In Scale mode, dragging the handle calls `Resize(Width(), Height(), newScale)` --
`bounds.W()`/`H()` literally never change after construction, only the draw scale does. That's
*why* the aspect ratio can never change, and *why* every knob, label, and pill font scales smoothly
with the window for free: the whole canvas -- vector shapes and text alike -- is rendered at a
uniform zoom via NanoVG's transform, rather than mLayoutFunc recomputing a new layout at a new
logical size (which is what `Size` mode does, and what this project used before this aspect-lock
pass). `mLayoutFunc` still re-runs on every zoom step (`SetLayoutOnResize(true)` + `mLayoutOnResize`),
but since `bounds` never changes, it always recomputes the exact same rects -- the resize/attach-
branch split described above still matters, just not for reflow anymore.

### Tabs reuse the same mechanism

The UI is organized into 6 tabs (`EUITab` in `Sparkles.cpp`'s anonymous namespace) plus a Presets
button that isn't a tab. `mActiveTab` (a plain `int`, see `Sparkles.h`) says which one is showing.
`mLayoutFunc` is deliberately reentrant: every tab button's action function just sets `mActiveTab`
and calls `mLayoutFunc(pCaller->GetUI())` again directly, rather than relying on an actual host
resize. Since `pGraphics->NControls()` is already nonzero by then, this re-enters the resize branch,
which both repositions every control *and* calls `Hide(tab != activeTab)` on every tab-scoped one —
so a tab click reuses exactly the same repositioning code path a real resize does, just triggered
manually. The initial-attach branch ends by calling `mLayoutFunc(pGraphics)` once more itself, for
the same reason: attaching happens with whichever tab is active, but the Hide() state needs applying
too, and re-entering the resize branch is simpler than duplicating that logic.

Only reference locals owned by the *call* that's still live when a callback fires (a control's own
`pCaller`/`pCaller->GetUI()`, or `this`/member variables) inside stored action functions — never a
captured `pGraphics` local from the `mLayoutFunc` invocation that created the callback, since that
stack frame is gone by the time the control is actually clicked.

## Adding a new control — checklist

To keep the responsive layout working, every new control needs all of these:

1. Add a tag to `ECtrlTags` in the plugin header.
2. In `mLayoutFunc`, compute its `IRECT` at the top from `innerBounds` (or `bounds`) using IRECT helpers like `GetFromLeft`, `GetMidVPadded`, `GetCentredInside`, `GetGridCell`, etc. — never hard-code pixel coordinates that don't derive from the current bounds.
3. In the **resize branch** (inside `if (pGraphics->NControls())`, before the `return`), add `pGraphics->GetControlWithTag(kYourTag)->SetTargetAndDrawRECTs(yourRect);`.
4. In the **initial-attach branch** (the fall-through below the `if`), `AttachControl(new IVWhatever(rect, ...), kYourTag);`.

Forgetting step 3 is the common mistake: the control will appear correctly at startup but stay frozen at its original rect when the window is resized.

Controls that are not tagged (e.g. the panel background, corner resizer) are handled via their dedicated accessors or don't need repositioning.

This checklist is for hand-placed controls (the note matrix, Key Root/Scale, the visual-indicator
panel, Shut Up, etc. — each with its own named `ECtrlTag`). A new **param** control usually doesn't
need any of this: add it to the right group's array in `Sparkles.cpp`'s `kParamGroups` (as a
`ParamClusterDesc` — set `rmParamIdx`/`smParamIdx` too if it has `_Rm`/`_Sm` multipliers, which
render as a condensed `ui/ModifierValueControl.h` text chip beside the base control automatically,
formatted as a decimal multiplier or, for an enum-valued Rm like Ray Rotation's Keep/Invert, as
`x1`/`x-1`) and `mLayoutFunc`'s flow-layout
assigns it a tag from `kCtrlTagFirstParamControl` at runtime, in both branches, for free. Every
`ParamGroupDesc` also declares which `EUITab` it belongs to — a tab-scoped hand-placed control
instead calls the `setTabbed` helper in the resize branch. A control meant to be visible on every
tab (Title/Version, the visual-indicator panel, Shut Up, the tab selectors themselves) just
uses a plain `SetTargetAndDrawRECTs` with no `Hide()` call at all, same as before tabs existed —
`setTabbed` (and Hide in general) is only for controls that belong to exactly one tab. The
visual-indicator panel and Shut Up used to be tab-scoped to Detection/General respectively; they
were moved back to always-visible after that caused Shut Up to be hideable mid-click-animation,
leaving it looking stuck when you switched tabs right after clicking it.

## Files of interest

- `Sparkles.cpp` / `.h` — plugin class, parameters (`EParams`), control tags (`ECtrlTags`), `ProcessBlock`, `mLayoutFunc`. `Sparkles.cpp`'s anonymous namespace also holds the tab table (`EUITab`/`kParamGroups`) and the fixed factory preset list (`kScopedParamIds`/`kPresets`, applied by `Sparkles::ApplyPreset` — see docs/SPEC.md §8 for what's in/out of scope).
- `ui/ModifierValueControl.h` — the condensed "x1.20 p/ray" Rm/Sm text control (no knob graphic, drag/scroll to change value), modeled on `ui/TimeMagnitudeControl.h`'s plain-`IControl` style.
- `config.h` — plugin metadata, channel I/O, size constraints, format-specific IDs (`PLUG_UNIQUE_ID`, `PLUG_MFR_ID`, `AAX_TYPE_IDS`, etc.).
- `projects/` — per-format IDE projects (Xcode, VS, WAM makefiles).
- `resources/` — Info.plists, icons, fonts, images. Fonts referenced via `*_FN` macros in `config.h` (e.g. `ROBOTO_FN`).
- `CMakeLists.txt` — alternative CMake build (see `build-cmake` skill).
- `build-mac/`, `build-win/`, `installer/`, `manual/`, `scripts/` — build, packaging, and docs tooling.
