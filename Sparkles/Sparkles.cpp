//build with Sparkles\scripts\build-vst3-win.bat

// # Configure (first time only, or after CMakeLists.txt changes)
// cmake --preset windows-vs2022

// # Build just the test binary
// cmake --build build/windows-vs2022 --config Release --target sparkle_tests

// # Run it
// ./build/windows-vs2022/Sparkles/Release/sparkle_tests.exe

// # Or via ctest, scoped to the Sparkles subdir (root ctest finds nothing —
// # would need enable_testing() in the shared root CMakeLists.txt too, which
// # I deliberately didn't touch since it's shared infra for other plugins)
// ctest --test-dir build/windows-vs2022/Sparkles -C Release --output-on-failure


#include "Sparkles.h"
#include "IPlug_include_in_plug_src.h"

#if IPLUG_EDITOR
#include "IControls.h"
#endif

#include "params/ParamSnapshot.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace
{
  // Below this pitch-tracker confidence the note display shows "--" instead of a note name --
  // purely presentational, independent of the trigger-accept Confidence param.
  constexpr double kMinDisplayConfidence = 0.3;

  // Scientific pitch notation (MIDI 60 = C4), for the note-detected UI display.
  void FormatNoteName(int midiNote, WDL_String& str)
  {
    static constexpr const char* kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int octave = midiNote / 12 - 1;
    const int pitchClass = ((midiNote % 12) + 12) % 12;
    str.SetFormatted(8, "%s%d", kNoteNames[pitchClass], octave);
  }

#if IPLUG_EDITOR
  // UI presentation table for every param in params/ParamList.h (except Key Root/Key Scale, which
  // are hand-placed beside the note matrix -- see mLayoutFunc), grouped/labelled/clustered for
  // mLayoutFunc.
  // This grouping is a presentation-layer decision independent of the DSP param table itself, so
  // it's hand-maintained here rather than macro-generated from ParamList.h -- same rationale as
  // params/ParamSnapshot.h's per-field mapping (see that file's header comment).
  //
  // Every §7 "chain" section in ParamList.h (structure/properties/timing/pitch) has a base param
  // plus one or two "_rm"/"_sm" multipliers (see docs/SPEC.md §7.3-7.5: `_rm` scales the value per
  // ray, `_sm` scales it per sparkle within a ray). Each base param and its multiplier(s) form one
  // visual cluster -- base control at normal size, its Rm/Sm as smaller controls immediately beside
  // it -- living in the same functional group as the base (not a separate "multipliers" group), so
  // each modifier's short label doesn't need to repeat which property it's modifying.
  //
  // Which of the 6 tabs (see mLayoutFunc) a group's controls show under -- see CLAUDE.md and
  // docs/SPEC.md §8 for why Detection and Note Matrix/Key/Scale stay separate from the other three
  // (the Presets button below only reaches General/PitchTiming/Synth). QuickGuide has no
  // ParamGroupDesc at all -- it's a single hand-placed placeholder image control.
  enum class EUITab { QuickGuide, General, Detection, PitchTiming, NoteMatrix, Synth };

  // Toggle is for 2-option enums that should alternate directly on click (IVSwitchControl) rather
  // than pop up a menu (IVMenuButtonControl) -- currently just the four Beats/ms *Unit params.
  enum class EParamCtrlKind { Knob, Dropdown, TimeKnob, Toggle };

  // Every cluster's position is a static (x, y) -- see the big comment above mLayoutFunc's
  // flatControls loop for why: with bounds locked to a single fixed logical size (see CLAUDE.md's
  // "Aspect ratio is locked"), there's no variable-width layout problem for a flow/wrap algorithm
  // to solve, so positions are just hand-placed numbers instead. x/y are relative to the owning
  // group's own (x, y) -- see ParamGroupDesc below -- so moving a whole group is a one-line edit
  // and every cluster's number stays a small, local offset instead of an absolute canvas coordinate.
  struct ParamClusterDesc
  {
    int paramIdx;
    const char* label;
    EParamCtrlKind kind;
    float x, y;                                     // static position, relative to the group's own (x, y)
    int unitParamIdx = -1;                          // TimeKnob only: sibling *Unit enum param
    int rmParamIdx = -1;                             // Rm/Sm are always numeric multipliers, shown
    int smParamIdx = -1;                             // as a ModifierValueControl chip -- see that file's header comment
  };

  struct ParamGroupDesc
  {
    const char* name;
    const ParamClusterDesc* clusters;
    int numClusters;
    EUITab tab;
    float x, y; // static origin, relative to paramsArea's own top-left -- see mLayoutFunc
  };

  // Output Mode + Passthrough only -- Detect/Trigger On/Threshold/Min Velocity live in
  // kDetectionControls below now (General vs. Detection tabs, see EUITab).
  constexpr ParamClusterDesc kGeneralControls[] = {
    { kParamOutputMode,     "Output",       EParamCtrlKind::Dropdown, 0.f,  18.f },
    { kParamGain,           "Passthrough",  EParamCtrlKind::Knob,     90.f, 18.f },
  };

  constexpr ParamClusterDesc kDetectionControls[] = {
    { kParamDetectionMode,  "Detect",       EParamCtrlKind::Dropdown, 0.f,   18.f },
    { kParamTriggerOn,      "Trigger On",   EParamCtrlKind::Dropdown, 90.f,  18.f },
    { kParamThreshold,      "Threshold",    EParamCtrlKind::Knob,     180.f, 18.f },
    { kParamVelocityDetect, "Min Velocity", EParamCtrlKind::Knob,     270.f, 18.f },
  };

  constexpr ParamClusterDesc kTechnicalControls[] = {
    { kParamReactiveness,   "Env Reactiveness", EParamCtrlKind::Knob, 0.f,   18.f },
    { kParamConfidence,     "Note Confidence",  EParamCtrlKind::Knob, 90.f,  18.f },
    { kParamMinNote,        "Min Note",         EParamCtrlKind::Knob, 180.f, 18.f },
    { kParamMaxNote,        "Max Note",         EParamCtrlKind::Knob, 0.f,   80.f },
    { kParamTriggerCooloff, "Trigger Cooloff",  EParamCtrlKind::Knob, 90.f,  80.f },
  };

  // Rays + Sparkles/Ray only -- Range Min/Max + Wrap Mode moved to kRangeControls (Pitch and
  // Timing tab, per "output note range and wrap mode" belonging there instead of General).
  constexpr ParamClusterDesc kSprinkleStructureControls[] = {
    { kParamNRays,           "Rays",         EParamCtrlKind::Knob, 0.f,  18.f },
    { kParamNSparklesPerRay, "Sparkles/Ray", EParamCtrlKind::Knob, 90.f, 18.f, -1, kParamNSparklesPerRayRm },
  };

  constexpr ParamClusterDesc kRangeControls[] = {
    { kParamRangeMin, "Range Min", EParamCtrlKind::Knob,     0.f,   18.f },
    { kParamRangeMax, "Range Max", EParamCtrlKind::Knob,     90.f,  18.f },
    { kParamWrapMode, "Wrap",      EParamCtrlKind::Dropdown, 180.f, 18.f },
  };

  constexpr ParamClusterDesc kOffsetControls[] = {
    { kParamPreDelayUnit, "Delay Unit",   EParamCtrlKind::Toggle,   0.f,   18.f },
    { kParamPreDelay,     "Pre Delay",    EParamCtrlKind::TimeKnob, 90.f,  18.f, kParamPreDelayUnit },
    { kParamPreInterval,  "Pre Interval", EParamCtrlKind::Knob,     180.f, 18.f },
  };

  constexpr ParamClusterDesc kSparklePropertyControls[] = {
    { kParamVelocity,     "Velocity",      EParamCtrlKind::Knob,     0.f,  18.f, -1, kParamLoudnessRm, kParamLoudnessSm },
    { kParamDurationUnit, "Duration Unit", EParamCtrlKind::Toggle,   0.f,  80.f },
    { kParamDuration,     "Duration",      EParamCtrlKind::TimeKnob, 90.f, 80.f, kParamDurationUnit, kParamDurationRm, kParamDurationSm },
  };

  constexpr ParamClusterDesc kTimingControls[] = {
    { kParamRayDelayUnit, "Ray Delay Unit", EParamCtrlKind::Toggle,   0.f,  18.f },
    { kParamRayDelay,     "Ray Delay",      EParamCtrlKind::TimeKnob, 90.f, 18.f, kParamRayDelayUnit, kParamRayDelayRm },
    { kParamDelayUnit,    "Delay Unit",     EParamCtrlKind::Toggle,   0.f,  80.f },
    { kParamDelay,        "Delay",          EParamCtrlKind::TimeKnob, 90.f, 80.f, kParamDelayUnit, kParamDelayRm, kParamDelaySm },
  };

  constexpr ParamClusterDesc kPitchControls[] = {
    { kParamRayInterval, "Ray Interval", EParamCtrlKind::Knob, 0.f,   18.f, -1, kParamRayIntervalRm },
    { kParamInterval,    "Interval",     EParamCtrlKind::Knob, 160.f, 18.f, -1, kParamIntervalRm, kParamIntervalSm },
  };

  // Ray Rotation's Rm ("Keep"/"Invert") renders through the same numeric ModifierValueControl
  // chip as every other Rm now, formatted as "x1"/"x-1" -- see that file's header comment.
  constexpr ParamClusterDesc kStereoControls[] = {
    { kParamPanning,     "Panning",      EParamCtrlKind::Dropdown, 0.f,   18.f },
    { kParamWidth,       "Width",        EParamCtrlKind::Knob,     0.f,   80.f, -1, kParamWidthRm, kParamWidthSm },
    { kParamPhase,       "Phase",        EParamCtrlKind::Knob,     0.f,   142.f, -1, kParamPhaseRm, kParamPhaseSm },
    { kParamRayRotation, "Ray Rotation", EParamCtrlKind::Dropdown, 0.f,   204.f, -1, kParamRayRotationRm },
    { kParamSeed,        "Seed",         EParamCtrlKind::Knob,     175.f, 204.f },
  };

  constexpr ParamClusterDesc kSynthControls[] = {
    { kParamWaveShape, "Wave Shape", EParamCtrlKind::Knob, 0.f,  18.f },
    { kParamAttack,    "Attack",     EParamCtrlKind::Knob, 90.f, 18.f,  -1, kParamAttackRm,  kParamAttackSm },
    { kParamDecay,     "Decay",      EParamCtrlKind::Knob, 0.f,  80.f,  -1, kParamDecayRm,   kParamDecaySm },
    { kParamSustain,   "Sustain",    EParamCtrlKind::Knob, 0.f,  142.f, -1, kParamSustainRm, kParamSustainSm },
    { kParamRelease,   "Release",    EParamCtrlKind::Knob, 0.f,  204.f, -1, kParamReleaseRm, kParamReleaseSm },
  };

  constexpr ParamGroupDesc kParamGroups[] = {
    { "General",               kGeneralControls,          (int) std::size(kGeneralControls),          EUITab::General,     0.f,   0.f },
    { "Sprinkle Structure",    kSprinkleStructureControls, (int) std::size(kSprinkleStructureControls), EUITab::General,    182.f, 0.f },
    { "Sparkle Properties",    kSparklePropertyControls,   (int) std::size(kSparklePropertyControls),   EUITab::General,    0.f,   82.f },
    { "Detection",             kDetectionControls,         (int) std::size(kDetectionControls),         EUITab::Detection,  0.f,   0.f },
    { "Technical",             kTechnicalControls,         (int) std::size(kTechnicalControls),         EUITab::Detection,  0.f,   82.f },
    { "Trigger Offset",        kOffsetControls,            (int) std::size(kOffsetControls),            EUITab::PitchTiming, 0.f,  0.f },
    { "Timing",                kTimingControls,            (int) std::size(kTimingControls),            EUITab::PitchTiming, 0.f,  82.f },
    { "Pitch",                 kPitchControls,             (int) std::size(kPitchControls),             EUITab::PitchTiming, 0.f,  226.f },
    { "Output Range",          kRangeControls,             (int) std::size(kRangeControls),             EUITab::PitchTiming, 0.f,  308.f },
    { "Synth",                 kSynthControls,             (int) std::size(kSynthControls),             EUITab::Synth,      0.f,   0.f },
    // Stereo sits snugly right of Synth -- both have clusters as wide as 246px (a knob + two
    // modifier chips), and there's only ~492px of paramsArea width to split between two such
    // columns, so this runs a few px past paramsArea's right edge into the card's own padding. If
    // that ends up visibly clipping against the right column, either nudge this left a little more
    // (it'll then overlap Synth's own Wave Shape/Attack row instead) or shrink kChipW/kClusterGap.
    { "Stereo (Audio Output)", kStereoControls,            (int) std::size(kStereoControls),            EUITab::Synth,      252.f, 0.f },
  };

  constexpr int kNumParamGroups = (int) std::size(kParamGroups);

  // Per-control cell size used when attaching each FlatCtrl in mLayoutFunc. Knob cells are taller
  // than dropdown cells since a knob needs room for its widget plus label plus value text above
  // and below it.
  constexpr float kCellW = 72.f;
  constexpr float kKnobCellH = 54.f;
  // Switch/dropdown cells show a caption line plus a value line just like a knob does (see
  // EParamCtrlKind::Toggle/Dropdown in the attach loop below, both IVSwitchControl) -- tall enough
  // for both without cropping, though still shorter than a knob cell since there's no circular
  // widget to make room for.
  constexpr float kDropdownCellH = 46.f;
  // Toggle/Dropdown-kind clusters (the option-cycling pill buttons -- Wrap, Detect, Trigger On,
  // Output Mode, Panning, Ray Rotation, the Delay/Duration Unit toggles) render smaller than their
  // full cell -- see the FlatCtrl loop below, which scales clusterRect down to
  // kOptionButtonWScale/kOptionButtonHScale x its own size, centred in the same static (x, y) slot.
  // Height gets a separate (taller) scale from width: the value text inside the pill is pinned to
  // EVAlign::Middle (kActionButtonStyle's valueText, reused for these buttons -- see the
  // EParamCtrlKind::Toggle/Dropdown case in the attach loop further down) -- with a fully
  // proportional 80% shrink that centering reads as too cramped/strong, so height gets 10% more
  // room than width to loosen it back up. If it still looks off, this pair -- not the VAlign::Middle
  // itself -- is the place to fine-tune.
  constexpr float kOptionButtonWScale = 0.8f;
  constexpr float kOptionButtonHScale = kOptionButtonWScale * 1.1f;
  // Every Rm/Sm modifier renders as a condensed text chip ("x1.20 p/ray") beside its base control
  // instead of a small knob -- see ui/ModifierValueControl.h and the attach loop below.
  constexpr float kChipW = 92.f;
  constexpr float kChipH = 16.f;
  // Gap between a cluster's Rm chip and its Sm chip (see modX below) -- small on purpose, so the
  // two read as one continuous "x1.20 p/ray x0.90 p/sparkle" sentence rather than two separate
  // floating labels.
  constexpr float kClusterGap = 3.f;
  // IVKnobControl's actual circular widget ends up much narrower than kCellW once IVectorBase's
  // label/value bands are carved out of the cell (see IVKnobControl::GetRadius, which sizes the
  // circle off the *shorter* of the remaining widget width/height) -- anchoring a Knob cluster's
  // modifier chip off the full kCellW left a visible gap between the small circle and its chip.
  // TimeMagnitudeControl (TimeKnob) is itself an IVKnobControl now (see that file's header
  // comment) and ends up the exact same size, so Knob/TimeKnob-kind clusters share this same
  // offset; Dropdown-kind clusters (no circular widget to clear) keep using the full kCellW.
  // Measured against kCompactStyle's 10px label/value text at kKnobCellH -- re-measure if either
  // changes (a too-small value re-creates the modifier-chip-on-top-of-the-knob bug this fixed).
  // Padded a few extra px past that measurement on purpose -- err on the side of a hair too far
  // rather than risk clipping the knob again.
  constexpr float kModifierOffsetKnobW = 56.f;
  // Height reserved above a group's first row of clusters for its own name heading -- see
  // kGroupLabelText and the y offsets above (every group's first-row clusters start at y=18).
  constexpr float kGroupLabelH = 18.f;

  // IVStyle's default label/value text (~19px/14px) is sized for the framework's normal control
  // sizes -- at these compact cell sizes it doesn't just look oversized, IVectorBase::MakeRects
  // carves the label/value bands out of the control's own rect *before* sizing the actual
  // clickable/draggable widget, so an unshrunk label can consume the whole cell (or overflow it,
  // producing a negative-size widget rect that IRECT::Contains() can never hit -- an invisible,
  // permanently unclickable control). kCompactStyle keeps that carve-out small enough to leave a
  // real widget behind.
  const IVStyle kCompactStyle = DEFAULT_STYLE
    .WithLabelText(DEFAULT_LABEL_TEXT.WithSize(10.f).WithFGColor(sparkle_palette::kLinesOuter).WithFont(sparkle_palette::kFontFredokaMedium))
    .WithValueText(DEFAULT_VALUE_TEXT.WithSize(10.f).WithFGColor(sparkle_palette::kLinesInterior).WithFont(sparkle_palette::kFontFredokaRegular))
    .WithColor(kFG, sparkle_palette::kPearlFrost)
    .WithColor(kPR, sparkle_palette::kPeriwinkleWarmed)
    .WithColor(kFR, sparkle_palette::kLinesInterior)
    .WithColor(kHL, sparkle_palette::kAquaChrome.WithOpacity(0.4f))
    .WithColor(kSH, sparkle_palette::kLinesInterior.WithOpacity(0.2f))
    .WithColor(kX1, sparkle_palette::kCobaltSheen)
    .WithRoundness(0.45f)
    .WithFrameThickness(sparkle_palette::kLineThickness)
    // Flat, not embossed -- see MakeTabStyle's own WithDrawShadows(false) for the tab pills' matching
    // reasoning. The tab pills' and Presets button's inset shadow (InsetShadowButtonControl) is a
    // separate hand-painted effect unaffected by this flag -- see that class's comment.
    .WithDrawShadows(false);

  // One IVStyle per left-column tab pill, built from sparkle_palette::kTabColors -- solid fill
  // (kFG), a darker press-flash of the same hue (kPR), small rounded corners (not a full pill --
  // see kActionButtonStyle below for the same corner/edge treatment shared with Shut Up), no drop
  // shadow (the reference art reads flat/matte, not embossed). Value text is disabled and the
  // label alone is centered dead in the middle of the button -- these are plain click-through
  // action buttons with nothing to display but their own caption.
  IVStyle MakeTabStyle(int tabIndex)
  {
    using namespace sparkle_palette;
    const TabColor& tc = kTabColors[tabIndex % kNumTabColors];
    return DEFAULT_STYLE
      .WithShowValue(false)
      .WithLabelText(DEFAULT_LABEL_TEXT.WithSize(13.f).WithFGColor(tc.text).WithFont(kFontFredokaMedium).WithVAlign(EVAlign::Middle))
      .WithColor(kFG, tc.fill)
      .WithColor(kPR, tc.fill.WithContrast(-0.18f))
      .WithColor(kFR, kLinesOuter.WithOpacity(0.7f))
      .WithColor(kHL, kPearlFrost.WithOpacity(0.35f))
      .WithDrawShadows(false)
      .WithRoundness(0.5f)
      .WithFrameThickness(sparkle_palette::kLineThickness);
  }

  // A tab pill with a small inset shadow hugging all four edges, on top of its normal fill/frame.
  // IVStyle's own shadow options don't offer this look: WithDrawShadows draws an offset drop
  // shadow *behind* the shape (outside it, not internal), and WithEmboss draws a two-tone diagonal
  // bevel (a lighter kPR ring top-left, a darker kSH ring bottom-right) rather than one uniform
  // tone on every side. IGraphics has a real blurred box-gradient shadow (DrawFastDropShadow, via
  // NanoVG's nvgBoxGradient), but it's hardcoded to a solid-center/fading-edge shape for casting a
  // shadow *behind* something -- exactly backwards from an inset shadow (transparent center, dark
  // edge), and there's no portable way to swap its inner/outer colors without reaching into
  // backend-specific (NanoVG-only) code. Faked instead with many concentric inset strokes, spaced
  // closer together than their own thickness so consecutive strokes overlap with no gap between
  // them -- the first attempt (4 strokes, spacing wider than thickness) left visible bands, which
  // read as distinct lines instead of a blur; overlap is what actually sells it as soft.
  //
  // When pActiveTab/myTabIndex are given, the active tab is marked instead by a flat, subtle white
  // wash (replacing the inset shadow entirely, rather than layering on top of it) plus an
  // underline beneath its label. Needs live access to mActiveTab since this control is constructed
  // once and only repositioned/hidden afterward on a tab switch, never recreated -- "am I
  // selected" can't be baked in at construction time.
  class InsetShadowButtonControl : public IVButtonControl
  {
  public:
    InsetShadowButtonControl(const IRECT& bounds, IActionFunction af, const char* label, const IVStyle& style,
                              const int* pActiveTab = nullptr, int myTabIndex = -1)
    : IVButtonControl(bounds, af, label, style)
    , mActiveTabPtr(pActiveTab)
    , mMyTabIndex(myTabIndex)
    {
    }

    bool IsSelected() const { return mActiveTabPtr && *mActiveTabPtr == mMyTabIndex; }

    void DrawWidget(IGraphics& g) override
    {
      // Selected tab draws its frame at kLineThicknessSelected (sparkle_palette) -- makes the
      // active pill read as clearly bolder than its neighbors, on top of the flat wash/underline
      // below. mStyle is this control's own copy (each MakeTabStyle(...) call makes a fresh IVStyle),
      // so overwriting it here on every draw can't bleed into any other control.
      mStyle.frameThickness = IsSelected() ? sparkle_palette::kLineThicknessSelected : sparkle_palette::kLineThickness;
      IVButtonControl::DrawWidget(g);
      const IRECT wb = GetWidgetBounds();
      const float cr = GetRoundedCornerRadius(wb);

      if (IsSelected())
      {
        g.FillRoundRect(sparkle_palette::kPearlFrost.WithOpacity(0.1f), wb, cr);
        return;
      }

      // Eased (quadratic) falloff -- denser near the edge, tapering off gradually -- reads more
      // like a real shadow than a linear ramp. kStrokeThickness > the ~1px step between
      // consecutive insets is what keeps every ring overlapping its neighbors (see class comment).
      constexpr int kRings = 7;
      constexpr float kMaxInset = 7.f;
      constexpr float kStrokeThickness = 1.6f;
      for (int i = 0; i < kRings; i++) {
        const float t = (float) i / (float) (kRings - 1);
        const float inset = 0.8f + t * kMaxInset;
        const float alpha = 0.2f * (1.f - t) * (1.f - t);
        g.DrawRoundRect(sparkle_palette::kLinesOuter.WithOpacity(alpha), wb.GetPadded(-inset), std::max(0.f, cr - inset), nullptr, kStrokeThickness);
      }
    }

    // Selected tab gets an underline under its (otherwise unchanged) label.
    void DrawLabel(IGraphics& g) override
    {
      IVButtonControl::DrawLabel(g);
      if (!IsSelected() || !GetLabelBounds().H() || !mStyle.showLabel) return;

      IRECT textRect;
      g.MeasureText(mStyle.labelText, mLabelStr.Get(), textRect);
      const IBlend blend = GetBlend();
      const float cx = GetLabelBounds().MW();
      const float underlineY = GetLabelBounds().MH() + textRect.H() * 0.42f;
      g.DrawLine(mStyle.labelText.mFGColor, cx - textRect.W() * 0.5f, underlineY, cx + textRect.W() * 0.5f, underlineY, &blend, 1.5f);
    }

  private:
    const int* mActiveTabPtr;
    int mMyTabIndex;
  };

  // Shared by Shut Up and the Note Matrix tab's Root/Scale/Mode quick-fill buttons (see
  // mLayoutFunc) -- same small-radius/thick-edge/centered-single-line treatment as the tab pills
  // above, just without committing to any one fill color (each call site tints kFG itself).
  // Shut Up shows only its label (no bound param, nothing to display as a value); Root/Scale/Mode
  // show only their value (the selected option) and skip the static "Root"/"Scale"/"Mode" caption
  // entirely, for the same single-centered-line look.
  const IVStyle kActionButtonStyle = kCompactStyle
    .WithLabelText(kCompactStyle.labelText.WithSize(12.f).WithVAlign(EVAlign::Middle))
    .WithValueText(kCompactStyle.labelText.WithSize(12.f).WithVAlign(EVAlign::Middle))
    .WithRoundness(0.5f);

  // A param group's own heading -- just its name, no bordered box around the cluster of knobs
  // beneath it (a plain ITextControl, not IVGroupControl -- see the attach loop below). Larger
  // than kCompactStyle's per-knob caption since a group name is a section heading (read once, from
  // a distance) rather than a label sized to fit inside a ~72px cell. DEFAULT_LABEL_TEXT only sets
  // EVAlign::Top, not EAlign -- that defaults to EAlign::Center (see IText's ctor), which centered
  // the name across the whole 260px-wide groupLabelBounds instead of anchoring it at the group's
  // own (x, y). Explicit EAlign::Near here is what actually puts the name flush at that corner, so
  // a group's static x/y in kParamGroups matches where its heading visibly renders.
  const IText kGroupLabelText = kCompactStyle.labelText.WithSize(13.f).WithFGColor(sparkle_palette::kLinesOuter).WithFont(sparkle_palette::kFontFredokaSemiBold).WithAlign(EAlign::Near);

  // Recolors kCompactStyle per-tab so each tab's knobs/switches pick up that tab's own accent
  // color (sparkle_palette::kTabColors), the same way its selector pill is colored -- "colorful
  // like the tabs" rather than one flat neutral style everywhere. EUITab's own values line up 1:1
  // with kTabColors' first 6 entries (QuickGuide..Synth; the 7th, Presets, isn't a tab).
  IVStyle MakeParamStyle(EUITab tab)
  {
    using namespace sparkle_palette;
    const IColor& fill = kTabColors[(int) tab % kNumTabColors].fill;
    return kCompactStyle
      .WithColor(kFG, fill)
      .WithColor(kPR, fill.WithContrast(-0.15f))
      .WithColor(kX1, fill.WithContrast(-0.4f))
      .WithColor(kFR, kLinesOuter.WithOpacity(0.6f));
  }

  float ClusterHeight(const ParamClusterDesc& c)
  {
    return (c.kind == EParamCtrlKind::Dropdown || c.kind == EParamCtrlKind::Toggle) ? kDropdownCellH : kKnobCellH;
  }

  // Fixed factory preset list for the Presets button (docs/SPEC.md §8) -- scoped to exactly the
  // General/Pitch and Timing/Synth tab params, plus Key Mode; Key Root/Scale and the rest of the
  // Note Matrix tab (and Detection) are deliberately left untouched, see that section. Key Mode is
  // included despite living on the Note Matrix tab because, unlike Root/Scale, it doesn't pin the
  // matrix to a particular key -- it's a chord-shape choice (§5.1) that's as much a part of a
  // preset's "sound" as e.g. Wrap Mode, and applying it still regenerates the matrix (via
  // Sparkles::OnParamChange, triggered by ApplyPreset's SetParameterValue call below exactly like a
  // manual dropdown change would) rather than leaving a stale one from whatever preset came before.
  // Listed here in General-tab-params, then Pitch and Timing-tab-params, then Synth-tab-params, then
  // Key Mode order; each PresetDesc::values entry below is positional against this same order (not a
  // {id, value} pair list), so adding/removing a scoped param means editing both together. Values
  // are the real (non-normalized) param values, same units ParamList.h declares them in -- e.g. the
  // four Beats/ms magnitudes here are in Beats, since every preset below also pins their *Unit param
  // to Beats (0) alongside them.
  // Output Mode is deliberately NOT in this list even though it lives on the General tab -- it's
  // output routing (MIDI vs. Audio vs. Both), not part of the sprinkle "sound" a preset describes,
  // and silently flipping it (e.g. to MIDI-only with nothing downstream to receive it) made presets
  // seem to do nothing at all. Same reasoning as excluding Detection: it's a setting the performer
  // controls independently of which preset is loaded.
  constexpr int kScopedParamIds[] = {
    // General
    kParamGain, kParamNRays, kParamNSparklesPerRay, kParamNSparklesPerRayRm,
    kParamVelocity, kParamLoudnessRm, kParamLoudnessSm, kParamDuration, kParamDurationUnit,
    kParamDurationRm, kParamDurationSm,
    // Pitch and Timing
    kParamPreDelay, kParamPreDelayUnit, kParamPreInterval, kParamRayDelay, kParamRayDelayUnit,
    kParamRayDelayRm, kParamDelay, kParamDelayUnit, kParamDelayRm, kParamDelaySm, kParamRayInterval,
    kParamRayIntervalRm, kParamInterval, kParamIntervalRm, kParamIntervalSm, kParamRangeMin,
    kParamRangeMax, kParamWrapMode,
    // Synth
    kParamWaveShape, kParamAttack, kParamAttackRm, kParamAttackSm, kParamDecay, kParamDecayRm,
    kParamDecaySm, kParamSustain, kParamSustainRm, kParamSustainSm, kParamRelease, kParamReleaseRm,
    kParamReleaseSm, kParamPanning, kParamWidth, kParamWidthRm, kParamWidthSm, kParamPhase,
    kParamPhaseRm, kParamPhaseSm, kParamRayRotation, kParamRayRotationRm, kParamSeed,
    // Key Mode (§5.1) -- Key Root/Scale stay excluded, see the comment above.
    kParamKeyMode,
  };
  constexpr int kNumScopedParams = (int) std::size(kScopedParamIds);

  struct PresetDesc
  {
    const char* name;
    double values[kNumScopedParams]; // positional against kScopedParamIds, see above
  };

  // "Default" (first entry) is kept deliberately in sync with params/ParamList.h's own defaults for
  // every scoped param, so it represents exactly the sound a fresh plugin instance already makes --
  // update both together.
  constexpr PresetDesc kPresets[] = {
    { "Default", {
      0., 3, 3, 1., 63., 1., 1., 0.0625, 0, 1., 1.,
      0., 0, 12, 0.25, 0, 1., 0.0625, 0, 1., 1., 2, 1., 1, 1., 1., 48, 96, 0,
      0.5, 5., 1., 1., 5., 1., 1., 0.6, 1., 1., 15., 1., 1., 2, 1., 1., 1., 0., 1., 1., 0, 1, 0,
      0, // Key Mode: Full Scale
    } },
    { "Gentle Cascade", {
      0., 5, 6, 1.0, 55., 0.85, 0.9, 0.5, 0, 1.0, 0.95,
      0., 0, 0, 0.5, 0, 1.15, 0.25, 0, 1.1, 1.05, -2, 1.0, -1, 1.0, 1.0, 55, 91, 0,
      0.5, 40., 1.1, 1.05, 200., 1.1, 1.0, 0.5, 1., 1., 300., 1.1, 1.05, 2, 0.8, 1., 1., 0., 0.4, 0.15, 0, 0, 12,
      5, // Key Mode: Triad
    } },
    { "Rapid Fire", {
      20., 10, 12, 0.6, 100., 0.6, 0.5, 40., 1, 0.8, 0.7,
      0., 1, 0, 15., 1, 0.7, 8., 1, 0.7, 0.6, 3, 0.7, 2, 0.8, 0.7, 40, 100, 2,
      2.0, 2., 0.8, 0.7, 30., 0.7, 0.6, 0.2, 0.8, 0.7, 40., 0.7, 0.6, 1, 1.4, 1., 1., 0., 0.6, 0.3, 1, 1, 777,
      2, // Key Mode: Power Chord
    } },
    { "Wide Arpeggio", {
      0., 4, 8, 1.3, 70., 0.75, 0.85, 0.25, 0, 1., 1.,
      0., 0, 0, 0.25, 0, 1.0, 0.125, 0, 1.0, 1.0, 4, 1.0, 3, 1.0, 1.0, 48, 108, 1,
      1.0, 10., 1., 1., 100., 1., 1., 0.5, 1., 1., 150., 1., 1., 3, 2.0, 1., 1., 0., 0.7, 0.35, 1, 1, 42,
      5, // Key Mode: Triad
    } },
    { "Slow Bloom", {
      0., 2, 5, 1.5, 45., 1.0, 1.1, 2., 0, 1.2, 1.1,
      0., 0, 0, 1., 0, 1.3, 1., 0, 1.2, 1.15, 7, 1.1, 5, 1.1, 1.05, 36, 84, 0,
      0.0, 800., 1.2, 1.1, 600., 1.1, 1.05, 0.8, 1., 1., 1500., 1.2, 1.1, 2, 0.6, 1., 1., 0., 0.3, 0.1, 0, 0, 101,
      7, // Key Mode: Ninth
    } },
    { "Tight Pluck", {
      0., 2, 3, 1.0, 90., 0.9, 0.9, 0.125, 0, 1.0, 1.0,
      0., 0, 0, 0.0625, 0, 1.0, 0.03125, 0, 1.0, 1.0, 0, 1.0, 0, 1.0, 1.0, 60, 84, 2,
      2.5, 1., 1., 1., 20., 0.9, 0.9, 0.1, 1., 1., 25., 0.9, 0.9, 0, 0.5, 1., 1., 0., 0.5, 0.2, 0, 1, 5,
      1, // Key Mode: Root
    } },
    { "Ambient Wash", {
      0., 8, 10, 1.4, 40., 1.1, 1.2, 1.5, 0, 1.2, 1.15,
      0., 0, 0, 0.5, 0, 1.2, 0.25, 0, 1.15, 1.1, 5, 1.3, 4, 1.2, 1.1, 48, 96, 1,
      0.3, 600., 1.2, 1.1, 500., 1.15, 1.1, 0.7, 1., 1., 1200., 1.25, 1.15, 4, 1.8, 1.05, 1.05, 0., 0.6, 0.25, 1, 0, 2024,
      9, // Key Mode: Thirteenth
    } },
    { "Chiptune Burst", {
      0., 6, 6, 1.0, 110., 0.65, 0.6, 60., 1, 0.85, 0.8,
      0., 1, 0, 25., 1, 0.8, 15., 1, 0.8, 0.75, 5, 0.9, 3, 0.9, 0.85, 48, 96, 2,
      2.0, 1., 1., 1., 50., 0.85, 0.8, 0.3, 1., 1., 60., 0.85, 0.8, 5, 1.2, 1., 1., 0., 0.5, 0.2, 1, 1, 8,
      2, // Key Mode: Power Chord
    } },
    { "Deep Bell", {
      0., 3, 4, 1.1, 70., 0.9, 1.0, 1., 0, 1.1, 1.05,
      0., 0, -12, 0.75, 0, 1.1, 0.5, 0, 1.05, 1.0, 7, 1.0, 4, 1.0, 1.0, 24, 72, 0,
      0.0, 5., 1., 1., 1200., 1.2, 1.1, 0.4, 1., 1., 1800., 1.25, 1.15, 0, 0.7, 1., 1., 0., 0.4, 0.15, 0, 0, 333,
      6, // Key Mode: Seventh
    } },
    { "Glass Rain", {
      0., 9, 14, 1.6, 50., 0.8, 0.7, 0.75, 0, 1.1, 1.0,
      0., 0, 12, 0.125, 0, 1.15, 0.0625, 0, 1.1, 1.05, 4, 1.2, 3, 1.1, 1.05, 72, 120, 1,
      1.0, 3., 1., 1., 250., 1.1, 1.0, 0.35, 1., 1., 400., 1.15, 1.05, 3, 1.9, 1.05, 1.05, 0., 0.65, 0.3, 1, 1, 2468,
      8, // Key Mode: Eleventh
    } },
  };
  // Named kNumFactoryPresets (not kNumPresets) to avoid confusion with Sparkles.h's kNumPresets,
  // which is an unrelated iPlug2 concept (MakeConfig's host-visible preset-bank slot count).
  constexpr int kNumFactoryPresets = (int) std::size(kPresets);
#endif
}

Sparkles::Sparkles(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  // Generated from params/ParamList.h -- see that file's header comment for the macro contract.
#define SPARKLE_PARAM_DOUBLE(id, name, defaultVal, minVal, maxVal, step, label) \
  GetParam(id)->InitDouble(name, defaultVal, minVal, maxVal, step, label);
#define SPARKLE_PARAM_DOUBLE_CURVE(id, name, defaultVal, minVal, maxVal, step, label, curve) \
  GetParam(id)->InitDouble(name, defaultVal, minVal, maxVal, step, label, 0, "", IParam::ShapePowCurve(curve));
#define SPARKLE_PARAM_INT(id, name, defaultVal, minVal, maxVal, label) \
  GetParam(id)->InitInt(name, defaultVal, minVal, maxVal, label);
#define SPARKLE_PARAM_ENUM(id, name, defaultIdx, ...) \
  GetParam(id)->InitEnum(name, defaultIdx, { __VA_ARGS__ });
#include "params/ParamList.h"

  // Note-name display (e.g. "C5") for every knob whose value is an absolute MIDI note number,
  // instead of the raw 0-127 integer -- the detection range (§2) and the §7.1 output range. Set
  // here rather than threaded through the X-macro above -- SetDisplayFunc is a one-off per param,
  // not worth a fifth macro shape for four call sites. Deliberately NOT applied to Pre
  // Interval/Ray Interval/Interval -- those are relative semitone offsets, not absolute notes, so a
  // note name would misrepresent them.
  GetParam(kParamMinNote)->SetDisplayFunc([](double value, WDL_String& str) {
    FormatNoteName(static_cast<int>(std::lround(value)), str);
  });
  GetParam(kParamMaxNote)->SetDisplayFunc([](double value, WDL_String& str) {
    FormatNoteName(static_cast<int>(std::lround(value)), str);
  });
  GetParam(kParamRangeMin)->SetDisplayFunc([](double value, WDL_String& str) {
    FormatNoteName(static_cast<int>(std::lround(value)), str);
  });
  GetParam(kParamRangeMax)->SetDisplayFunc([](double value, WDL_String& str) {
    FormatNoteName(static_cast<int>(std::lround(value)), str);
  });

  // Names the current Sine/Triangle/Square/Saw blend (§7.7) instead of showing the raw 0-3 morph
  // value, including in-between transitions (e.g. "Sine -> Tri 40%").
  GetParam(kParamWaveShape)->SetDisplayFunc([](double value, WDL_String& str) {
    static constexpr const char* kNames[4] = { "Sine", "Triangle", "Square", "Saw" };
    const double clamped = std::clamp(value, 0.0, 3.0);
    int segment = static_cast<int>(std::floor(clamped));
    segment = std::clamp(segment, 0, 2);
    const double blend = clamped - segment;
    if (blend < 0.005)
      str.Set(kNames[segment]);
    else if (blend > 0.995)
      str.Set(kNames[segment + 1]);
    else
      str.SetFormatted(32, "%s -> %s %d%%", kNames[segment], kNames[segment + 1], static_cast<int>(std::lround(blend * 100.0)));
  });

#if IPLUG_EDITOR // http://bit.ly/2S64BDd
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS);
  };


  mLayoutFunc = [&](IGraphics* pGraphics) {
    const IRECT bounds = pGraphics->GetBounds();
    const EUITab activeTab = (EUITab) mActiveTab;

    // Fractions measured directly off resources/img/background.png's own painted border and
    // divider lines (a 1497x828 source image): the border sits ~0.8%/1.3% in from each edge, the
    // left divider (end of the baked-in logo column) at x=323.5px, the right divider at x=1358.5px,
    // and the "Rays of Sunshine" tagline's last row of pixels at y=434px. Expressed as fractions of
    // bounds.W()/bounds.H() (not fixed pixel counts) so the three columns below stay flush with the
    // art at every window size -- see ui/BackgroundImageControl.h, which stretches that same image
    // non-uniformly to fill bounds every resize.
    constexpr float kBorderPadXFrac = 0.013f;
    constexpr float kBorderPadYFrac = 0.024f;
    constexpr float kLeftColFrac = 0.226f;   // x-boundary between the left (tabs) and middle column
    constexpr float kRightColFrac = 0.908f;  // x-boundary between the middle and right column
    constexpr float kLogoBottomFrac = 0.525f; // y, below which the left column is free of baked art
    constexpr float kColumnGap = 6.f;

    const IRECT innerBounds = bounds.GetPadded(-bounds.W() * kBorderPadXFrac, -bounds.H() * kBorderPadYFrac,
                                                -bounds.W() * kBorderPadXFrac, -bounds.H() * kBorderPadYFrac);
    const float leftColX = bounds.L + bounds.W() * kLeftColFrac;
    const float rightColX = bounds.L + bounds.W() * kRightColFrac;

    // Three columns, left to right: tab selectors (under the baked-in logo), tab content, and the
    // always-visible indicator strip -- see CLAUDE.md. All three span innerBounds' full height;
    // nothing lives in a separate top strip anymore now that the logo/title are part of the
    // background artwork itself.
    const IRECT leftColumnBounds(innerBounds.L, innerBounds.T, leftColX - kColumnGap * 0.5f, innerBounds.B);
    const IRECT middleColumnBounds(leftColX + kColumnGap * 0.5f, innerBounds.T, rightColX - kColumnGap * 0.5f, innerBounds.B);
    const IRECT rightColumnBounds(rightColX + kColumnGap * 0.5f, innerBounds.T, innerBounds.R, innerBounds.B);

    // Detection tab: note-detection "piano" confidence strip (see ui/NoteBarsControl.h), inset a
    // couple pixels inside the two divider lines rather than spanning the full plugin width, since
    // it now belongs to just this tab instead of sitting behind every tab as a background texture.
    // Bottom edge sits kNoteBarsBottomPad above bounds.B (a fixed pixel offset, not a fraction --
    // consistent with kColumnGap/kNoteBarsDividerPad above -- safe since the aspect-locked canvas
    // means this logical bounds.B never changes, only the draw scale does) so the tiles' base lines
    // up with the background art's baked-in outer border instead of running under/past it.
    constexpr float kNoteBarsDividerPad = 7.f;
    constexpr float kNoteBarsBottomPad = 10.f;
    const IRECT noteBarsBounds(leftColX + kNoteBarsDividerPad, bounds.T, rightColX - kNoteBarsDividerPad, bounds.B - kNoteBarsBottomPad);

    // Always-visible left column: the 6 tab selectors plus the Presets button (not a tab, see
    // ApplyPreset) -- everything in the middle column belongs to exactly one tab and is shown/
    // hidden as a whole by the Hide() calls further down. Colored pills (kTabColors), stacked
    // below the logo/tagline that's baked into the background image.
    const float tabBarTop = bounds.T + bounds.H() * kLogoBottomFrac;
    const IRECT tabBarBounds(leftColumnBounds.L, std::max(tabBarTop, leftColumnBounds.T), leftColumnBounds.R, leftColumnBounds.B);
    // Padding shrunk from -3 -> -1.5: since each cell is a fixed 1/7 slice of tabBarBounds
    // regardless of padding, a smaller pad eats less into each pill's own height, both tightening
    // the gap between adjacent pills and (for cell 0) the gap below the logo -- same knob controls
    // both, since tabBarBounds itself starts flush against the logo already (see tabBarTop above).
    constexpr float kTabCellPad = -0.5f;
    const IRECT tabQuickGuideBounds  = tabBarBounds.GetGridCell(0, 7, 1).GetPadded(kTabCellPad);
    const IRECT tabGeneralBounds     = tabBarBounds.GetGridCell(1, 7, 1).GetPadded(kTabCellPad);
    const IRECT tabDetectionBounds   = tabBarBounds.GetGridCell(2, 7, 1).GetPadded(kTabCellPad);
    const IRECT tabPitchTimingBounds = tabBarBounds.GetGridCell(3, 7, 1).GetPadded(kTabCellPad);
    const IRECT tabNoteMatrixBounds  = tabBarBounds.GetGridCell(4, 7, 1).GetPadded(kTabCellPad);
    const IRECT tabSynthBounds       = tabBarBounds.GetGridCell(5, 7, 1).GetPadded(kTabCellPad);
    const IRECT presetsBounds        = tabBarBounds.GetGridCell(6, 7, 1).GetPadded(kTabCellPad);

    // Middle column: every tab's own content lives here now.
    const IRECT tabContentArea = middleColumnBounds.GetPadded(-10.f);
    const IRECT paramsArea = tabContentArea;

    // Right column: everything that used to live in a persistent top strip (envelope meter, note/
    // confidence + sprinkle-count readouts, trigger light, Shut Up) -- never Hide()'d regardless of
    // mActiveTab, same reasoning as before (see the removed persistentStripBounds comment this
    // replaced): Shut Up living outside the tab system entirely means a tab switch mid-click-
    // animation can never leave it looking stuck. Top to bottom: Shut Up, sprinkle count, detected
    // note/confidence, then the envelope meter -- the one element that benefits from more room --
    // takes whatever's left above the trigger light, pinned at the very bottom. Fixed-height items
    // are carved off top/bottom in turn.
    // +1px: this column's fixed -9 padding (below) reads slightly left-of-centre against the
    // border/divider baked into the background art, which don't fall on exact integer fractions of
    // it -- shifts every element below without changing rightColumnBounds itself (only referenced
    // here, so nothing else needs to know).
    IRECT rightCursor = rightColumnBounds.GetPadded(-9.f).GetTranslated(1.f, 0.f);
    // Widened a bit past rightCursor's own padding (still short of rightColumnBounds' edges) --
    // "Shut Up" was clipping against the button's edges at rightCursor's narrower width. The
    // further +15%-per-side on top of that is expressed as a fraction of its own width (rather
    // than another fixed pixel add-on) so it stays proportional at any window size. Height (34,
    // down from an original 40) is ~15% shorter -- keep the two 34.f's below in sync if this
    // changes again, since the second one is what advances rightCursor past the button.
    //
    // That widen alone now reads oversized now that DrawShadows is off (the shadow used to eat
    // into the pill's apparent edge, masking how big the fill itself was) -- shrunk back down 15%
    // in both directions afterward, via padding (not a straight GetHPadded/GetVPadded pair) so the
    // reduction is proportional to the box's own W/H rather than a fixed px amount, and centred
    // rather than sliding the button off its slot.
    const IRECT shutUpBoundsBase = rightCursor.GetFromTop(34.f).GetHPadded(6.f);
    const IRECT shutUpBoundsWide = shutUpBoundsBase.GetHPadded(shutUpBoundsBase.W() * 0.15f);
    const IRECT shutUpBounds = shutUpBoundsWide.GetPadded(-shutUpBoundsWide.W() * 0.075f, -shutUpBoundsWide.H() * 0.075f,
                                                            -shutUpBoundsWide.W() * 0.075f, -shutUpBoundsWide.H() * 0.075f);
    rightCursor = rightCursor.GetReducedFromTop(34.f + 6.f);
    // Widened past rightCursor's own width the same way shutUpBounds is above -- there's room for
    // it inside rightColumnBounds (see the sprinkle-count IText's own comment for the width/font
    // math this and that were tuned together against).
    const IRECT sprinkleCountBoundsBase = rightCursor.GetFromTop(24.f);
    const IRECT sprinkleCountBounds = sprinkleCountBoundsBase.GetHPadded(6.f);
    rightCursor = rightCursor.GetReducedFromTop(24.f + 6.f);
    const IRECT noteBounds = rightCursor.GetFromTop(32.f);
    rightCursor = rightCursor.GetReducedFromTop(32.f + 6.f);
    const IRECT triggerLightBounds = rightCursor.GetFromBottom(24.f).GetCentredInside(15.f);
    rightCursor = rightCursor.GetReducedFromBottom(24.f + 6.f);
    const IRECT meterBounds = rightCursor; // remaining flexible space

    // Note Matrix tab: §5.1 Key/Scale quick-fill trio, stacked vertically down the tab's left edge
    // (small, out of the way) so the §5 note-eligibility matrix (12x12 grid + column/row toggles,
    // plus its own 12-button trigger row -- see ui/NoteMatrixControl.h and mNoteMatrix in
    // Sparkles.h) can sit larger and further right, leaving room to actually play it like a
    // keyboard. Hand-placed here (rather than in the flowing param-group grid) since Root/Scale/
    // Mode act on the matrix directly, not via kParamGroups.
    constexpr float kKeyColumnW = 105.f; // 70 * 1.5
    constexpr float kKeyColumnGap = 7.f; // half the original 14 -- matrix sits closer to the column
    const IRECT keyColumnBounds = tabContentArea.GetFromLeft(kKeyColumnW).GetFromTop(42.f * 3.f);
    const IRECT keyRootBounds = keyColumnBounds.GetGridCell(0, 3, 1).GetPadded(-6.f);
    const IRECT keyScaleDropdownBounds = keyColumnBounds.GetGridCell(1, 3, 1).GetPadded(-6.f);
    const IRECT keyModeBounds = keyColumnBounds.GetGridCell(2, 3, 1).GetPadded(-6.f);
    // Square (matrix grid + header) plus NoteMatrixControl::kTriggerRowSize of extra height on top
    // for its trigger row, centred (both axes) in the space right of keyColumnBounds. Capped at
    // kNoteMatrixMaxSquare (rather than always filling noteMatrixArea) so the grid doesn't grow
    // arbitrarily large on wide/tall layouts; GetCentredInside still keeps it centred in
    // noteMatrixArea either way.
    constexpr float kNoteMatrixMaxSquare = 340.f;
    const IRECT noteMatrixArea = tabContentArea.GetReducedFromLeft(kKeyColumnW + kKeyColumnGap);
    const float noteMatrixSquare = std::min({noteMatrixArea.W(), noteMatrixArea.H() - NoteMatrixControl::kTriggerRowSize, kNoteMatrixMaxSquare});
    const IRECT noteMatrixBounds =
      noteMatrixArea.GetCentredInside(noteMatrixSquare, noteMatrixSquare + NoteMatrixControl::kTriggerRowSize);

    // Quick Guide tab: a single placeholder image control until the real guide PNG exists.
    const IRECT quickGuideBounds = tabContentArea;

    // Every control's position is statically defined -- see ParamClusterDesc::x/y and
    // ParamGroupDesc::x/y above -- rather than computed by a row-packing/flow algorithm. There's no
    // variable-width layout problem here for such an algorithm to solve: bounds (and so
    // paramsArea) is always the exact same fixed 748x414 logical size regardless of window scale
    // (see CLAUDE.md's "Aspect ratio is locked" -- the corner resizer changes draw scale, never
    // bounds itself), so a layout that reflows for different available widths was solving a
    // problem that never actually occurs. To move something, just edit its x/y in the tables above
    // -- a cluster's (x, y) is relative to its own group's (x, y), which is itself relative to
    // paramsArea's own top-left.
    //
    // One entry per allocated param control (base, then Rm, then Sm -- in kParamGroups' group/
    // cluster order), computed once here so both the resize branch and the initial-attach branch
    // below stay fed by the same layout math -- see CLAUDE.md's "How mLayoutFunc is structured".
    // Every group still gets a flatControls entry regardless of tab, in the same fixed order on
    // every single call -- control tags (kCtrlTagFirstParamControl + index) must stay stable
    // across tab switches, which only holds if that enumeration order never depends on mActiveTab.
    // Only each entry's rect (real regardless of whether its group's tab is active -- harmless,
    // since Hide() below keeps an inactive-tab control off-screen and un-hit-testable) and .tab
    // (used by that same Hide() call) depend on anything per-call.
    struct FlatCtrl
    {
      IRECT rect;
      int paramIdx;
      const char* label;
      EParamCtrlKind kind;
      int unitParamIdx; // TimeKnob only, else -1
      bool isModifier = false; // Rm/Sm control -- numeric ones become a ModifierValueControl chip
      EUITab tab = EUITab::General;
    };

    // One label rect per group, top-left anchored at the group's own static (x, y) -- see the
    // attach loop further down, which draws each group's name there with no bordered box around it.
    std::vector<IRECT> groupLabelBounds(kNumParamGroups);

    std::vector<FlatCtrl> flatControls;
    flatControls.reserve(96);
    for (int g = 0; g < kNumParamGroups; g++) {
      const ParamGroupDesc& group = kParamGroups[g];
      const float groupOriginX = paramsArea.L + group.x, groupOriginY = paramsArea.T + group.y;
      groupLabelBounds[g] = IRECT(groupOriginX, groupOriginY, groupOriginX + 260.f, groupOriginY + kGroupLabelH);

      for (int c = 0; c < group.numClusters; c++) {
        const ParamClusterDesc& cluster = group.clusters[c];
        const float cx = groupOriginX + cluster.x, cy = groupOriginY + cluster.y;
        const float clusterH = ClusterHeight(cluster);

        const IRECT clusterRect(cx, cy, cx + kCellW, cy + clusterH);
        // See kOptionButtonWScale/kOptionButtonHScale's own comment above for why Toggle/Dropdown
        // clusters get their own (non-uniform, width < height) shrink here, centred in the same
        // static (x, y) slot. Knob/TimeKnob-kind clusters are untouched.
        const bool isOptionButton = cluster.kind == EParamCtrlKind::Dropdown || cluster.kind == EParamCtrlKind::Toggle;
        const IRECT clusterButtonRect = isOptionButton
          ? clusterRect.GetCentredInside(clusterRect.W() * kOptionButtonWScale, clusterRect.H() * kOptionButtonHScale)
          : clusterRect;
        flatControls.push_back(FlatCtrl{ clusterButtonRect.GetPadded(-2.f), cluster.paramIdx, cluster.label, cluster.kind, cluster.unitParamIdx, false, group.tab });

        // Every Rm/Sm gets the condensed "x1.20 p/ray" text chip (see ui/ModifierValueControl.h),
        // including Ray Rotation's enum-valued Rm (shown as "x1"/"x-1" -- see that file). Anchored
        // off kModifierOffsetKnobW rather than the full kCellW for Knob/TimeKnob-kind bases -- see
        // that constant's comment for why (Dropdown-kind bases keep the full kCellW).
        const bool clusterIsKnobLike = cluster.kind == EParamCtrlKind::Knob || cluster.kind == EParamCtrlKind::TimeKnob;
        float modX = cx + (clusterIsKnobLike ? kModifierOffsetKnobW : kCellW) + kClusterGap;
        if (cluster.rmParamIdx >= 0) {
          const IRECT modRect(modX, clusterRect.MH() - kChipH * 0.5f, modX + kChipW, clusterRect.MH() + kChipH * 0.5f);
          flatControls.push_back(FlatCtrl{ modRect.GetPadded(-1.f), cluster.rmParamIdx, "p/ray", EParamCtrlKind::Knob, -1, true, group.tab });
          modX += kChipW + kClusterGap;
        }
        if (cluster.smParamIdx >= 0) {
          const IRECT modRect(modX, clusterRect.MH() - kChipH * 0.5f, modX + kChipW, clusterRect.MH() + kChipH * 0.5f);
          flatControls.push_back(FlatCtrl{ modRect.GetPadded(-1.f), cluster.smParamIdx, "p/sparkle", EParamCtrlKind::Knob, -1, true, group.tab });
        }
      }
    }

    if (pGraphics->NControls()) {
      pGraphics->GetBackgroundControl()->SetTargetAndDrawRECTs(bounds);

      pGraphics->GetControlWithTag(kCtrlTagTabQuickGuide)->SetTargetAndDrawRECTs(tabQuickGuideBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabGeneral)->SetTargetAndDrawRECTs(tabGeneralBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabDetection)->SetTargetAndDrawRECTs(tabDetectionBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabPitchTiming)->SetTargetAndDrawRECTs(tabPitchTimingBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabNoteMatrix)->SetTargetAndDrawRECTs(tabNoteMatrixBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabSynth)->SetTargetAndDrawRECTs(tabSynthBounds);
      pGraphics->GetControlWithTag(kCtrlTagPresets)->SetTargetAndDrawRECTs(presetsBounds);

      // Always-visible, never Hide()'d -- see the big comment above rightCursor. Order here follows
      // the same top-to-bottom visual order as rightCursor's own layout above.
      pGraphics->GetControlWithTag(kCtrlTagShutUp)->SetTargetAndDrawRECTs(shutUpBounds);
      pGraphics->GetControlWithTag(kCtrlTagSprinkleCount)->SetTargetAndDrawRECTs(sprinkleCountBounds);
      pGraphics->GetControlWithTag(kCtrlTagNoteDisplay)->SetTargetAndDrawRECTs(noteBounds);
      pGraphics->GetControlWithTag(kCtrlTagEnvelopeMeter)->SetTargetAndDrawRECTs(meterBounds);
      pGraphics->GetControlWithTag(kCtrlTagTriggerLight)->SetTargetAndDrawRECTs(triggerLightBounds);

      // Reposition + Hide every tab-scoped hand-placed control, based on whether its own tab is
      // the active one -- this is what actually makes tab-switching visible, on top of the
      // reposition every one of these already needed on a plain resize.
      auto setTabbed = [&](int tag, const IRECT& rect, EUITab tab) {
        IControl* pControl = pGraphics->GetControlWithTag(tag);
        pControl->SetTargetAndDrawRECTs(rect);
        pControl->Hide(tab != activeTab);
      };
      setTabbed(kCtrlTagKeyRoot, keyRootBounds, EUITab::NoteMatrix);
      setTabbed(kCtrlTagKeyScale, keyScaleDropdownBounds, EUITab::NoteMatrix);
      setTabbed(kCtrlTagKeyMode, keyModeBounds, EUITab::NoteMatrix);
      setTabbed(kCtrlTagNoteMatrix, noteMatrixBounds, EUITab::NoteMatrix);
      setTabbed(kCtrlTagQuickGuideImage, quickGuideBounds, EUITab::QuickGuide);
      setTabbed(kCtrlTagNoteBars, noteBarsBounds, EUITab::Detection);

      for (int i = 0; i < (int) flatControls.size(); i++)
        setTabbed(kCtrlTagFirstParamControl + i, flatControls[i].rect, flatControls[i].tab);
      for (int g = 0; g < kNumParamGroups; g++)
        setTabbed(kCtrlTagFirstParamControl + (int) flatControls.size() + g, groupLabelBounds[g], kParamGroups[g].tab);
      return;
    }

    pGraphics->SetLayoutOnResize(true);
    // Scale (not Size): dragging the corner handle re-renders the whole fixed-logical-size layout
    // at a new draw scale instead of recomputing it at a new width/height -- bounds.W()/H() (and so
    // every fraction-based rect mLayoutFunc computes) never change, which is what keeps the aspect
    // ratio locked and makes every knob/label/font scale together for free. PLUG_HOST_RESIZE is 0
    // (config.h) so this corner handle is the only way to resize at all -- no host-driven resize
    // path exists here to fight with it.
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, true);
    pGraphics->LoadFont(sparkle_palette::kFontRoboto, ROBOTO_FN);
    pGraphics->LoadFont(sparkle_palette::kFontBungee, BUNGEE_FN);
    pGraphics->LoadFont(sparkle_palette::kFontRighteous, RIGHTEOUS_FN);
    pGraphics->LoadFont(sparkle_palette::kFontFredokaLight, FREDOKA_LIGHT_FN);
    pGraphics->LoadFont(sparkle_palette::kFontFredokaRegular, FREDOKA_REGULAR_FN);
    pGraphics->LoadFont(sparkle_palette::kFontFredokaMedium, FREDOKA_MEDIUM_FN);
    pGraphics->LoadFont(sparkle_palette::kFontFredokaSemiBold, FREDOKA_SEMIBOLD_FN);
    pGraphics->LoadFont(sparkle_palette::kFontFredokaBold, FREDOKA_BOLD_FN);

    // Background artwork (logo/tagline/border/divider lines) -- see ui/BackgroundImageControl.h
    // for why this isn't IGraphics::AttachBackground(). Attached first (index 0), same slot
    // GetBackgroundControl() / the resize branch above already assumed AttachPanelBackground would
    // occupy.
    const IBitmap backgroundBitmap = pGraphics->LoadBitmap(BACKGROUND_FN);
    pGraphics->AttachControl(new BackgroundImageControl(bounds, backgroundBitmap));

    // Attached next so the translucent bars render behind every real control -- draw order follows
    // attach order. Detection-tab-scoped (see setTabbed above); bounds passed here are just the
    // initial attach rect, replaced by noteBarsBounds via setTabbed on the very next mLayoutFunc
    // call this same attach pass ends with.
    pGraphics->AttachControl(new NoteBarsControl(bounds), kCtrlTagNoteBars);

    // No separate title control -- "SPARKLES" and the tagline are baked into the background
    // artwork itself (see the fraction comments above). No build-info string either -- that "built
    // on <date>" line was dev-only clutter with no use to an end user.

    // Tab selectors + Presets button -- plain IVButtonControls for now (see the big comment above
    // tabBarBounds); each tab button just flips mActiveTab and re-invokes mLayoutFunc so the resize
    // branch above reapplies both layout and Hide() state, exactly like a real resize would.
    //
    // Every one of these action functions calls SplashClickActionFunc(pCaller) itself, same as the
    // Shut Up button below -- IVButtonControl's own OnMouseDown (IButtonControlBase::OnMouseDown)
    // sets its value to 1 (which is what makes DrawWidget render it "pressed"/highlighted) and
    // relies on a splash-click animation completing (OnEndAnimation) to reset it back to 0.
    // SplashClickActionFunc is what sets that animation up; passing a custom action function (as
    // every button here needs to, to actually do something) replaces that default and skips it
    // entirely unless called explicitly -- omitting it is what left these buttons stuck looking
    // permanently pressed after the first click.
    //
    // InsetShadowButtonControl (not plain IVButtonControl) for the small inset shadow around each
    // pill -- see that class's comment. The Presets button below uses it too (it isn't a tab, so
    // pActiveTab/myTabIndex are left at their defaults -- IsSelected() is always false, which is
    // exactly what's wanted: the inset shadow always shows, never the selected-tab flat wash).
    pGraphics->AttachControl(new InsetShadowButtonControl(tabQuickGuideBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::QuickGuide; mLayoutFunc(pCaller->GetUI());
    }, "Guide", MakeTabStyle(0), &mActiveTab, (int) EUITab::QuickGuide), kCtrlTagTabQuickGuide);
    pGraphics->AttachControl(new InsetShadowButtonControl(tabGeneralBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::General; mLayoutFunc(pCaller->GetUI());
    }, "General", MakeTabStyle(1), &mActiveTab, (int) EUITab::General), kCtrlTagTabGeneral);
    pGraphics->AttachControl(new InsetShadowButtonControl(tabDetectionBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::Detection; mLayoutFunc(pCaller->GetUI());
    }, "Detection", MakeTabStyle(2), &mActiveTab, (int) EUITab::Detection), kCtrlTagTabDetection);
    pGraphics->AttachControl(new InsetShadowButtonControl(tabPitchTimingBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::PitchTiming; mLayoutFunc(pCaller->GetUI());
    }, "Pitch/Timing", MakeTabStyle(3), &mActiveTab, (int) EUITab::PitchTiming), kCtrlTagTabPitchTiming);
    pGraphics->AttachControl(new InsetShadowButtonControl(tabNoteMatrixBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::NoteMatrix; mLayoutFunc(pCaller->GetUI());
    }, "Note Matrix", MakeTabStyle(4), &mActiveTab, (int) EUITab::NoteMatrix), kCtrlTagTabNoteMatrix);
    pGraphics->AttachControl(new InsetShadowButtonControl(tabSynthBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::Synth; mLayoutFunc(pCaller->GetUI());
    }, "Synth", MakeTabStyle(5), &mActiveTab, (int) EUITab::Synth), kCtrlTagTabSynth);
    // Click-through, not a dropdown: each click advances to the next factory preset (wrapping) and
    // applies it immediately, updating the button's own label to the newly-active preset's name so
    // there's always visible feedback for which one is loaded. Left-aligned (unlike every other
    // tab pill) since "Preset: <name>" reads more naturally hugging the left edge than centered.
    {
      // Leading spaces (rather than relying on the button's own edge padding) keep the text clear
      // of the pill's left edge, matching how every other tab pill's centered text never touches
      // its own edge.
      WDL_String presetLabel;
      presetLabel.SetFormatted(64, "   Preset: %s", kPresets[0].name);
      const IVStyle presetStyle = MakeTabStyle(6).WithLabelText(MakeTabStyle(6).labelText.WithAlign(EAlign::Near));
      pGraphics->AttachControl(new InsetShadowButtonControl(presetsBounds, [&](IControl* pCaller) {
        SplashClickActionFunc(pCaller);
        mPresetIndex = (mPresetIndex + 1) % kNumFactoryPresets;
        ApplyPreset(mPresetIndex);
        WDL_String label;
        label.SetFormatted(64, "   Preset: %s", kPresets[mPresetIndex].name);
        pCaller->As<IVectorBase>()->SetLabelStr(label.Get());
      }, presetLabel.Get(), presetStyle), kCtrlTagPresets);
    }

    pGraphics->AttachControl(new ITextControl(quickGuideBounds, "Quick Guide -- image pending",
      IText(16.f, sparkle_palette::kLinesOuter, sparkle_palette::kFontFredokaMedium)), kCtrlTagQuickGuideImage);

    pGraphics->AttachControl(new EnvelopeMeterControl(meterBounds, kParamThreshold), kCtrlTagEnvelopeMeter);
    pGraphics->AttachControl(new ValueDisplayControl<2>(noteBounds, "--",
      IText(13.f, sparkle_palette::kLinesOuter, sparkle_palette::kFontFredokaSemiBold), [](const std::array<float, 2>& vals, WDL_String& str) {
      const int note = static_cast<int>(std::lround(vals[0]));
      if (note < 0 || vals[1] < kMinDisplayConfidence) {
        str.Set("--");
        return;
      }
      WDL_String noteName;
      FormatNoteName(note, noteName);
      str.SetFormatted(16, "%s %d%%", noteName.Get(), static_cast<int>(std::lround(vals[1] * 100.f)));
    }), kCtrlTagNoteDisplay);
    pGraphics->AttachControl(new ValueDisplayControl<1>(sprinkleCountBounds, "0 sprinkles",
      // At the column's real width, "N sprinkles" overflows the control's own rect and gets
      // clipped by IGraphics::DrawControl's per-control scissor (bounds.Intersect(controlBounds),
      // padded only 0.75px) -- cropped instead of just overlapping neighbors like text usually can,
      // because that scissor is set from the control's own mRECT. sprinkleCountBounds' own +6px
      // widen (above) buys back some of that room -- 11px is the largest size that still keeps the
      // worst case ("999 sprinkles") inside the widened rect uncropped, verified by temporarily
      // hardcoding that string and a screenshot, not just estimated.
      IText(11.f, sparkle_palette::kLinesInterior, sparkle_palette::kFontFredokaRegular), [](const std::array<float, 1>& vals, WDL_String& str) {
      str.SetFormatted(32, "%d sprinkles", static_cast<int>(std::lround(vals[0])));
    }), kCtrlTagSprinkleCount);
    pGraphics->AttachControl(new TriggerLightControl(triggerLightBounds), kCtrlTagTriggerLight);
    pGraphics->AttachControl(new IVButtonControl(shutUpBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mShutUpRequested.store(true, std::memory_order_release);
    }, "Shut Up", kActionButtonStyle
      .WithColor(kFG, sparkle_palette::kCustomRed)
      .WithColor(kPR, sparkle_palette::kCustomRed.WithContrast(-0.18f))
      .WithShowValue(false)
      // A little bolder than kActionButtonStyle's own Medium (shared with Root/Scale/Mode, which
      // stay as-is) -- overridden here rather than on kActionButtonStyle itself.
      .WithLabelText(kActionButtonStyle.labelText.WithFGColor(sparkle_palette::kPearlFrost).WithFont(sparkle_palette::kFontFredokaSemiBold))), kCtrlTagShutUp);
    // Root/Scale/Mode: same small-radius/thick-edge/single-centered-line look as Shut Up (see
    // kActionButtonStyle), tinted with the Note Matrix tab's own accent color instead of Shut Up's
    // fuchsia. Caption ("Root"/"Scale"/"Mode") is hidden -- only the selected option (the value)
    // shows, centered, matching Shut Up's one-line label.
    {
      const IVStyle keyStyle = kActionButtonStyle.WithShowLabel(false).WithColor(kFG, sparkle_palette::kAquaChrome).WithColor(kPR, sparkle_palette::kAquaChrome.WithContrast(-0.15f));
      pGraphics->AttachControl(new IVMenuButtonControl(keyRootBounds, kParamKeyRoot, "Root", keyStyle), kCtrlTagKeyRoot);
      pGraphics->AttachControl(new IVMenuButtonControl(keyScaleDropdownBounds, kParamKeyScale, "Scale", keyStyle), kCtrlTagKeyScale);
      pGraphics->AttachControl(new IVMenuButtonControl(keyModeBounds, kParamKeyMode, "Mode", keyStyle), kCtrlTagKeyMode);
    }
    pGraphics->AttachControl(new NoteMatrixControl(noteMatrixBounds, &mNoteMatrix,
      [this](int pitchClass) { PushManualTrigger(pitchClass); }), kCtrlTagNoteMatrix);

    for (int i = 0; i < (int) flatControls.size(); i++) {
      const FlatCtrl& ctrl = flatControls[i];
      const int tag = kCtrlTagFirstParamControl + i;
      const IVStyle style = MakeParamStyle(ctrl.tab);
      switch (ctrl.kind) {
        case EParamCtrlKind::Knob:
          if (ctrl.isModifier)
            pGraphics->AttachControl(new ModifierValueControl(ctrl.rect, ctrl.paramIdx, ctrl.label), tag);
          else
            pGraphics->AttachControl(new IVKnobControl(ctrl.rect, ctrl.paramIdx, ctrl.label, style), tag);
          break;
        case EParamCtrlKind::TimeKnob:
          // Same `style` every plain Knob-kind cluster on this tab uses -- see TimeMagnitudeControl.h's
          // header comment for why this now renders identically to a regular IVKnobControl.
          pGraphics->AttachControl(new TimeMagnitudeControl(ctrl.rect, ctrl.paramIdx, ctrl.unitParamIdx, ctrl.label, style), tag);
          break;
        case EParamCtrlKind::Toggle:
        case EParamCtrlKind::Dropdown:
        default:
          // Both Beats/ms (Toggle) and every other enum param (Dropdown) click straight through
          // their states (IVSwitchControl) rather than popping up a menu -- see CLAUDE.md. Key
          // Root/Scale/Mode above are the one deliberate exception, kept as real dropdowns since
          // their option lists are long enough that cycling through every one would be tedious.
          //
          // Value text (the selected option, drawn inside the button pill -- valueInButton
          // defaults true) borrows kActionButtonStyle's valueText's font/color/alignment --
          // Fredoka-Medium in kLinesOuter, vertically centered -- rather than
          // style.valueText's own Fredoka-Regular in kLinesInterior (still fine for a knob's
          // value caption sitting in its own thin band below the widget, just too quiet in a
          // pill where the value is the primary text), same as the Root/Scale/Mode quick-fill
          // buttons. Sized back down to 10px here rather than kActionButtonStyle's 12px, to
          // match these pills' own smaller (20%-shrunk, see the FlatCtrl loop above) size --
          // kActionButtonStyle itself stays untouched so Root/Scale/Mode/Shut Up don't shrink.
          // The caption line above the pill (ctrl.label, e.g. "Wrap") is untouched -- still
          // style's own labelText -- only the value inside the pill changes.
          pGraphics->AttachControl(new IVSwitchControl(ctrl.rect, ctrl.paramIdx, ctrl.label,
            style.WithValueText(kActionButtonStyle.valueText.WithSize(10.f))), tag);
          break;
      }
    }
    // Just the group's name, no bordered box around its knobs -- see kGroupLabelText's comment.
    for (int g = 0; g < kNumParamGroups; g++)
      pGraphics->AttachControl(new ITextControl(groupLabelBounds[g], kParamGroups[g].name, kGroupLabelText), kCtrlTagFirstParamControl + (int) flatControls.size() + g);

    // Everything above attaches with whichever tab happened to be active at construction time as
    // its rect/Hide() truth -- re-enter through the resize branch once now (NControls() is nonzero
    // as of the attach calls just above, so this bounces exactly once) so the initial Hide() state
    // actually matches mActiveTab instead of leaving every tab's controls visible on first paint.
    mLayoutFunc(pGraphics);
  };
#endif
}

#if IPLUG_EDITOR
void Sparkles::ApplyPreset(int idx)
{
  if (idx < 0 || idx >= kNumFactoryPresets)
    return;

  // Same silence-everything request the Shut Up button makes (see mShutUpRequested's comment in
  // Sparkles.h) -- otherwise a sprinkle triggered under the old preset could keep ringing out with
  // a mix of old and newly-applied params once its later rays/sparkles fire.
  mShutUpRequested.store(true, std::memory_order_release);

  const PresetDesc& preset = kPresets[idx];
  mApplyingPreset = true;
  for (int i = 0; i < kNumScopedParams; i++)
    SetParameterValue(kScopedParamIds[i], GetParam(kScopedParamIds[i])->ToNormalized(preset.values[i]));
  mApplyingPreset = false;

  // SetParameterValue above updates the param and notifies the host, but doesn't resync an
  // already-attached control's own cached display value -- do that explicitly so the knobs jump to
  // the new values immediately instead of only on the next redraw.
  if (IGraphics* pGraphics = GetUI()) {
    for (int i = 0; i < kNumScopedParams; i++) {
      pGraphics->ForControlWithParam(kScopedParamIds[i], [](IControl* pControl) {
        pControl->SetValueFromDelegate(pControl->GetParam()->GetNormalized());
      });
    }
  }
}

#endif

#if IPLUG_DSP
void Sparkles::OnReset()
{
  mPitchTracker.Reset();
  ConfigurePitchTracker();
  mSynthEngine.Reset();
  ShutUp();
  mBlockStartSample = 0;
  mLastTriggerSample = kNoLastTrigger; // blockStart resets to 0 too -- see this sentinel's comment
  mEnvelope = 0.0;
  mMidiQueue.Resize(GetBlockSize());
  mMidiQueue.Clear();
  mHeldNoteVelocity.fill(-1);
}

void Sparkles::ProcessMidiMsg(const IMidiMsg& msg)
{
  // Queued here (called before ProcessBlock, per IPlugProcessor's contract) and drained
  // sample-by-sample from inside ProcessBlock's main loop -- see HandleMidiTrigger.
  mMidiQueue.Add(msg);
}

void Sparkles::HandleMidiTrigger(const IMidiMsg& msg, int64_t triggerSample, int64_t timelineSample,
                                  const sparkle_core::DetectionParams& detection,
                                  const sparkle_core::SparkleParams& sparkleParams,
                                  sparkle_core::OutputMode outputMode, double bpm, double sampleRate)
{
  const int note = msg.NoteNumber();
  if (note < 0 || note >= (int) mHeldNoteVelocity.size())
    return;

  // A Note On with velocity 0 is conventionally a Note Off (running-status optimization) -- treat
  // it as one rather than as a zero-velocity trigger.
  const bool isNoteOn = msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() > 0;
  const bool isNoteOff = msg.StatusMsg() == IMidiMsg::kNoteOff ||
                          (msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() == 0);
  if (!isNoteOn && !isNoteOff)
    return;

  const bool midiListening = detection.detectionMode == sparkle_core::DetectionMode::Midi ||
                              detection.detectionMode == sparkle_core::DetectionMode::Both;

  // Keep held-note bookkeeping current even while not listening for MIDI triggers, so switching
  // Detection Mode to MIDI/Both mid-performance doesn't inherit a stale velocity for a note that
  // was already held before the switch.
  if (isNoteOn)
    mHeldNoteVelocity[note] = msg.Velocity();

  if (!midiListening) {
    if (isNoteOff)
      mHeldNoteVelocity[note] = -1;
    return;
  }

  const sparkle_core::TriggerType triggerType = detection.triggerType;

  if (isNoteOn) {
    const bool fireUp = triggerType == sparkle_core::TriggerType::Up || triggerType == sparkle_core::TriggerType::Both;
    if (fireUp && msg.Velocity() >= detection.minVelocity)
      FireSprinkle(note, triggerSample, timelineSample, sparkleParams, outputMode, bpm, sampleRate, detection.triggerCooloffMs);
  }
  else { // isNoteOff -- gate on the velocity the note was struck with, not the note-off's own byte.
    const int heldVelocity = mHeldNoteVelocity[note];
    mHeldNoteVelocity[note] = -1;
    const bool fireDown = triggerType == sparkle_core::TriggerType::Down || triggerType == sparkle_core::TriggerType::Both;
    if (fireDown && heldVelocity >= detection.minVelocity)
      FireSprinkle(note, triggerSample, timelineSample, sparkleParams, outputMode, bpm, sampleRate, detection.triggerCooloffMs);
  }
}

void Sparkles::ShutUp()
{
  mTriggerPending = false;

  // Explicitly turn off every note we've actually sent a real Note On for and haven't turned off
  // yet -- mEventScheduler.StopAll() drains exactly those (its note-off pool), as immediate events,
  // and silently drops any note-ons that hadn't fired yet (nothing to turn off for those). Don't
  // rely solely on the All-Notes-Off CC below for this: it's a Channel Mode Message that not every
  // downstream synth/host honors, which was leaving notes stuck sounding forever when this method
  // used to send only that. Looped like ProcessBlock's own FlushBlock drain, since StopAll only
  // pops up to outCapacity per call.
  std::array<sparkle_core::SchedEvent, 64> schedEvents;
  size_t nSchedEvents;
  do {
    nSchedEvents = mEventScheduler.StopAll(schedEvents.data(), schedEvents.size());
    for (size_t i = 0; i < nSchedEvents; i++) {
      IMidiMsg msg;
      msg.MakeNoteOffMsg(schedEvents[i].note, schedEvents[i].offsetInBlock);
      SendMidiMsg(msg);
    }
  } while (nSchedEvents == schedEvents.size());

  // Backstop All-Notes-Off (CC 123) on top of the explicit note-offs above -- cheap insurance for
  // anything outside our own bookkeeping, and matches the original transport-stop/reset behavior.
  IMidiMsg allNotesOffMsg;
  allNotesOffMsg.MakeControlChangeMsg(IMidiMsg::kAllNotesOff, 0.0);
  SendMidiMsg(allNotesOffMsg);

  // Audio Output Mode's voices -- silenced immediately, matching the hard-stop spirit of the MIDI
  // side above rather than fading through a release tail.
  mSynthEngine.StopAll();

  // Drop every pending cell flash too -- notes that will now never sound shouldn't still light up
  // the matrix after the fact.
  mFlashScheduler.Reset();

  mNumActiveSprinkles = 0;
}

void Sparkles::PushManualTrigger(int pitchClass)
{
  if (pitchClass < 0 || pitchClass >= sparkle_core::kNumPitchClasses)
    return;

  // Fixed octave around middle C (MIDI 60 = C4, matrix column kC == 3) -- inverts
  // sparkle_core::PitchClassOf's (semitone + 3) % 12 mapping so e.g. pitchClass kA (0) lands on A4
  // (69), matching standard MIDI/concert-pitch numbering rather than an arbitrary octave.
  const int note = 60 + (pitchClass + 9) % sparkle_core::kNumPitchClasses;

  const int tail = mManualTriggerTail.load(std::memory_order_relaxed);
  const int nextTail = (tail + 1) % kManualTriggerQueueSize;
  if (nextTail == mManualTriggerHead.load(std::memory_order_acquire))
    return; // queue full -- drop rather than block the UI thread
  mManualTriggerQueue[tail].store(note, std::memory_order_relaxed);
  mManualTriggerTail.store(nextTail, std::memory_order_release);
}

int Sparkles::PopManualTrigger()
{
  const int head = mManualTriggerHead.load(std::memory_order_relaxed);
  if (head == mManualTriggerTail.load(std::memory_order_acquire))
    return -1;
  const int note = mManualTriggerQueue[head].load(std::memory_order_relaxed);
  mManualTriggerHead.store((head + 1) % kManualTriggerQueueSize, std::memory_order_release);
  return note;
}

void Sparkles::OnParamChange(int paramIdx)
{
  if (paramIdx == kParamMinNote || paramIdx == kParamMaxNote) {
    ConfigurePitchTracker();
  }
  else if (paramIdx == kParamKeyRoot || paramIdx == kParamKeyScale || paramIdx == kParamKeyMode) {
    // §5.1 quick-fill: regenerates the whole note matrix from scratch, discarding any hand-edits
    // made since the last key/scale/mode change (see mNoteMatrix's comment in Sparkles.h).
    const int keyRoot = GetParam(kParamKeyRoot)->Int();
    const auto keyScale = static_cast<sparkle_core::Scale>(GetParam(kParamKeyScale)->Int());
    const auto keyMode = static_cast<sparkle_core::ChordMode>(GetParam(kParamKeyMode)->Int());
    if (keyRoot == sparkle_core::kNumPitchClasses) // "Trigger Note", see params/ParamList.h
      sparkle_core::ApplyKeyScalePerColumn(mNoteMatrix, keyScale, keyMode);
    else
      sparkle_core::ApplyKeyScale(mNoteMatrix, keyRoot, keyScale, keyMode);
  }
}

void Sparkles::OnParamChange(int paramIdx, EParamSource source, int sampleOffset)
{
  OnParamChange(paramIdx);

  // The Beats/ms unit-conversion below must only run for a genuine UI-driven toggle. iPlug2 also
  // routes every param through here from OnParamReset (IPlugEditorDelegate.h) -- on construction,
  // preset recall, etc. -- re-announcing each param's CURRENT value with no actual change. Reacting
  // to those the same as a real toggle would rescale an already-correct magnitude on every plugin
  // load (dividing it down toward 0, since the "new" unit always matches the old one there).
  if (source != kUI)
    return;

  if (paramIdx == kParamPreDelayUnit)
    ConvertTimeMagnitudeUnit(kParamPreDelay, paramIdx);
  else if (paramIdx == kParamDurationUnit)
    ConvertTimeMagnitudeUnit(kParamDuration, paramIdx);
  else if (paramIdx == kParamRayDelayUnit)
    ConvertTimeMagnitudeUnit(kParamRayDelay, paramIdx);
  else if (paramIdx == kParamDelayUnit)
    ConvertTimeMagnitudeUnit(kParamDelay, paramIdx);
}

void Sparkles::ConfigurePitchTracker()
{
  mPitchTracker.Configure(
    GetSampleRate(), GetParam(kParamMinNote)->Int(), GetParam(kParamMaxNote)->Int(), kNoteHoldSeconds);
}

void Sparkles::ConvertTimeMagnitudeUnit(int magnitudeParamIdx, int unitParamIdx)
{
  // ApplyPreset sets each magnitude and its *Unit param back-to-back via SetParameterValue, which
  // always reports kUI -- without bailing out here, the *Unit param's own SetParameterValue call
  // would rescale the magnitude a second time right after ApplyPreset just set it to the preset's
  // intended value. See mApplyingPreset's comment.
  if (mApplyingPreset)
    return;

  // Beats/ms is a straight two-option toggle (see params/ParamList.h), so by the time this fires
  // the unit param already holds the new unit -- rescale the magnitude so it still represents the
  // same real duration under that unit at the current tempo, rather than reinterpreting the same
  // raw number under a different unit.
  const int newUnit = GetParam(unitParamIdx)->Int(); // 0 = Beats, 1 = ms
  const double msPerBeat = 60000.0 / GetTempo();
  const double oldValue = GetParam(magnitudeParamIdx)->Value();
  const double newValue = newUnit == 1 ? oldValue * msPerBeat : oldValue / msPerBeat;
  SetParameterValue(magnitudeParamIdx, GetParam(magnitudeParamIdx)->ToNormalized(newValue));
}

void Sparkles::FireSprinkle(int triggerNote, int64_t triggerSample, int64_t timelineSample, const sparkle_core::SparkleParams& params,
                            sparkle_core::OutputMode outputMode, double bpm, double sampleRate, double triggerCooloffMs)
{
  // §2 trigger cooloff: a trigger landing inside the cooloff window after the last accepted one
  // is dropped silently, before anything (including the trigger light) reacts to it -- same
  // "nothing happened" semantics as a confidence/deadline drop elsewhere in this file.
  const int64_t cooloffSamples = static_cast<int64_t>(std::llround(triggerCooloffMs * 0.001 * sampleRate));
  if (triggerSample - mLastTriggerSample < cooloffSamples)
    return;
  mLastTriggerSample = triggerSample;

  mTriggerSender.PushData(ISenderData<1>(kCtrlTagTriggerLight, std::array<float, 1>{ 1.f }));

  // Reap sprinkles that have finished sounding by now, so mNumActiveSprinkles reflects only
  // ones still actually in flight.
  ReapFinishedSprinkles(triggerSample);

  // At the cap -- drop this trigger's sprinkle entirely rather than truncating any one
  // sprinkle's own rays/sparkles.
  if (mNumActiveSprinkles >= kMaxSimultaneousSprinkles)
    return;

  sparkle_core::SparkleGenerator::Generate(mNoteMatrix, params, triggerNote, bpm, sampleRate, mScratchEvents, timelineSample);

  const bool sendMidi = outputMode != sparkle_core::OutputMode::Audio;
  const bool sendAudio = outputMode != sparkle_core::OutputMode::Midi;

  // Every event shares triggerNote's column; a trigger can still generate hundreds of events
  // (SparkleGenerator::kMaxEventsPerTrigger). Each one gets its own mFlashScheduler entry at its
  // own atSample -- regardless of sendMidi/sendAudio, since the flash should track whichever event
  // actually sounds -- rather than flashing every hit cell immediately here, so the note matrix
  // reflects notes as they're actually heard instead of all at once when the sprinkle is generated.
  const int triggerColumn = sparkle_core::PitchClassOf(triggerNote);

  // Column-header-only flash, pushed immediately (unlike the per-cell flashes below) so it reflects
  // the sparkle being created rather than any individual note actually sounding -- see
  // ui/NoteMatrixControl.h's OnMsgFromDelegate for the row < 0 header-only convention.
  mNoteMatrixFlashSender.PushData(ISenderData<2>(kCtrlTagNoteMatrix,
    std::array<float, 2>{ static_cast<float>(triggerColumn), -1.f }));

  int64_t sprinkleEndSample = triggerSample;
  for (const auto& event : mScratchEvents) {
    const int64_t atSample = triggerSample + event.timeOffsetSamples;

    if (sendMidi)
      mEventScheduler.Schedule(event.note, event.velocity, event.durationSamples, atSample);

    if (sendAudio) {
      mSynthEngine.ScheduleVoice(event.note, event.velocity, event.pan, event.attackSamples,
                                  event.decaySamples, event.sustainLevel, event.releaseSamples,
                                  event.durationSamples, atSample);
    }

    const int row = sparkle_core::PitchClassOf(event.note);
    mFlashScheduler.Schedule(triggerColumn * sparkle_core::kNumPitchClasses + row, 0, 0, atSample);

    sprinkleEndSample = std::max(sprinkleEndSample, atSample + event.durationSamples);
  }

  if (!mScratchEvents.empty())
    mActiveSprinkleEndSamples[mNumActiveSprinkles++] = sprinkleEndSample;
}

void Sparkles::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // Consumed here (audio thread) rather than where it's set (UI thread) -- see mShutUpRequested's
  // comment in Sparkles.h. Checked first so a shut-up request cancels an armed trigger before this
  // block's samples get a chance to resolve it into a fresh sprinkle.
  if (mShutUpRequested.exchange(false, std::memory_order_acq_rel))
    ShutUp();

  const int nChans = NOutChansConnected();
  const int nInChans = NInChansConnected();
  const double gain = GetParam(kParamGain)->Value() / 100.;
  const double threshold = GetParam(kParamThreshold)->Value() / 100.;

  // Snapshot every sparkle-generation param once per block -- never re-read the underlying
  // (host-automatable, potentially concurrently-written) IParams from inside the sample loop.
  const sparkle_params::ParamSnapshot snapshot = sparkle_params::BuildParamSnapshot(*this);
  const double bpm = GetTempo();
  const double sampleRate = GetSampleRate();
  const int64_t blockStart = mBlockStartSample;
  // Host transport position at this block's start, in samples since the project's own origin --
  // distinct from `blockStart` above, which only counts samples since this plugin instance's last
  // OnReset and has no relation to where the project's playhead actually is. -1 when the host
  // doesn't report one (e.g. no transport, some hosts/formats); see the per-sample fallback below.
  const double hostSamplePos = GetSamplePos();

  mPitchTracker.SetConfidenceThreshold(snapshot.detection.confidence);
  const int64_t triggerTimeoutSamples = static_cast<int64_t>(std::llround(kTriggerTimeoutSeconds * sampleRate));

  // Whether the audio envelope path below is allowed to arm/fire triggers this block -- Detection
  // Mode gates it the same way it gates HandleMidiTrigger's MIDI path (see that function).
  const bool audioTriggerEnabled = snapshot.detection.detectionMode != sparkle_core::DetectionMode::Midi;

  // Drain any note-matrix "play" buttons clicked since the last block (see PushManualTrigger and
  // ui/NoteMatrixControl.h's trigger row) -- each fires straight through FireSprinkle, the same
  // shared endpoint the audio and MIDI paths below funnel through, but deliberately skipping both
  // of their Detection Mode/velocity/trigger-type gating: a manual click is a deliberate
  // performance gesture, not a detected crossing, so it always sounds regardless of Input Type.
  // Fired at this block's very first sample -- a mouse click has no sub-block timing worth
  // preserving the way real MIDI does.
  const int64_t timelineSampleAtBlockStart = hostSamplePos >= 0.0
    ? static_cast<int64_t>(std::llround(hostSamplePos)) : blockStart;
  for (int note = PopManualTrigger(); note >= 0; note = PopManualTrigger())
    FireSprinkle(note, blockStart, timelineSampleAtBlockStart, snapshot.sparkle, snapshot.outputMode,
                 bpm, sampleRate, snapshot.detection.triggerCooloffMs);

  for (int s = 0; s < nFrames; s++) {
    // This sample's absolute project-timeline position, for PanMode::Random's seed (§7.6) --
    // falls back to the free-running block counter (today's behavior) when the host doesn't
    // report a valid transport position, which loses the cross-playthrough guarantee but is no
    // worse than before this feature existed.
    const int64_t timelineSample = hostSamplePos >= 0.0
      ? static_cast<int64_t>(std::llround(hostSamplePos)) + s
      : blockStart + s;

    // Drain every MIDI message queued (by ProcessMidiMsg) for this exact sample offset before
    // doing anything else this sample -- keeps MIDI-triggered sprinkles sample-accurate rather
    // than all landing at block start.
    while (!mMidiQueue.Empty()) {
      const IMidiMsg& msg = mMidiQueue.Peek();
      if (msg.mOffset > s)
        break;
      HandleMidiTrigger(msg, blockStart + s, timelineSample, snapshot.detection, snapshot.sparkle, snapshot.outputMode, bpm, sampleRate);
      mMidiQueue.Remove();
    }

    // Everything in here -- pitch tracker, envelope follower, threshold-crossing arming -- only
    // matters for the audio-triggered path, which Detection Mode disables entirely in MIDI-only
    // mode (audioTriggerEnabled above). Skipping it there avoids paying for autocorrelation-based
    // pitch tracking and envelope smoothing every sample when nothing ever consumes their output --
    // HandleMidiTrigger's MIDI path above doesn't read mPitchTracker/mEnvelope at all. Safe even
    // though mPitchTracker.Now() (its own internal clock, driven only by Push() below) stops
    // advancing while this is skipped: mTriggerArmTime/mTriggerDeadline are only ever set inside
    // the fireUp branch below, which is unreachable whenever audioTriggerEnabled is false, so
    // nothing is left reading a stale clock. The tracker naturally needs a moment to reacquire once
    // Detection Mode switches back to Audio/Both, same as it would right after plugin startup.
    if (audioTriggerEnabled) {
      const double pitchSample =
        nInChans > 1 ? (inputs[0][s] + inputs[1][s]) * 0.5 : (nInChans == 1 ? inputs[0][s] : 0.);
      mPitchTracker.Push(static_cast<float>(pitchSample));

      // One-pole envelope follower, updated every sample regardless of trigger state -- see
      // mEnvelope in Sparkles.h. Triggering below reads this, not the raw instantaneous sample.
      // Square-rooted before smoothing so the follower responds to quieter input more readily
      // (compresses the input's dynamic range going in, rather than the threshold comparison
      // needing a separate curve).
      double inputLevel = 0.;
      for (int c = 0; c < nInChans; c++) {
        inputLevel = std::max(inputLevel, std::abs(inputs[c][s]));
      }
      inputLevel = std::sqrt(inputLevel);

      const double prevEnvelope = mEnvelope;
      mEnvelope = mEnvelope * (1.0 - snapshot.detection.reactiveness) + inputLevel * snapshot.detection.reactiveness;

      if (mTriggerPending) {
        // Armed up-crossing: fire the moment the tracker has a confident hop at/after arming.
        // The one-hop slack means a note that was already confidently sounding when the envelope
        // crossed (e.g. a swell on a sustained note) fires immediately instead of waiting for the
        // next analysis hop.
        if (mPitchTracker.HasConfidentNote() &&
            mPitchTracker.LastConfidentTime() + sparkle_core::PitchTracker::kHopSamples >= mTriggerArmTime) {
          mTriggerPending = false;
          FireSprinkle(mPitchTracker.LastConfidentNote(), blockStart + s, timelineSample, snapshot.sparkle, snapshot.outputMode, bpm, sampleRate, snapshot.detection.triggerCooloffMs);
        }
        else if (mPitchTracker.Now() >= mTriggerDeadline) {
          mTriggerPending = false; // never got confident about the pitch -- drop the trigger silently
        }
      }
      else {
        // A true crossing -- the envelope must have been on the other side of threshold on the
        // previous sample -- not just "currently above it". Without this, a sustained note whose
        // envelope sits above threshold would re-arm and refire the instant the previous trigger
        // resolved (mTriggerPending -> false), rather than waiting for it to actually dip and
        // cross again.
        const bool crossedUp = prevEnvelope <= threshold && mEnvelope > threshold;
        const bool crossedDown = prevEnvelope >= threshold && mEnvelope < threshold;
        const auto triggerType = snapshot.detection.triggerType;
        const bool fireUp = crossedUp && (triggerType == sparkle_core::TriggerType::Up ||
                                          triggerType == sparkle_core::TriggerType::Both);
        const bool fireDown = crossedDown && (triggerType == sparkle_core::TriggerType::Down ||
                                              triggerType == sparkle_core::TriggerType::Both);

        if (fireDown) {
          // The note is fading or already gone -- resolve from the tracker's held note (what was
          // confidently playing just before the crossing) rather than analyzing post-crossing
          // audio. Nothing confident within the hold window means we don't know the note: drop.
          if (mPitchTracker.HasConfidentNote())
            FireSprinkle(mPitchTracker.LastConfidentNote(), blockStart + s, timelineSample, snapshot.sparkle, snapshot.outputMode, bpm, sampleRate, snapshot.detection.triggerCooloffMs);
        }
        else if (fireUp) {
          // The note is just starting -- the analysis buffer is mid-transient, so defer to the
          // adaptive wait above rather than trusting (or fabricating) a pitch right now.
          mTriggerPending = true;
          mTriggerArmTime = mPitchTracker.Now();
          mTriggerDeadline = mTriggerArmTime + triggerTimeoutSamples;
        }
      }
    }
    else {
      // Detection Mode is MIDI-only -- don't arm/fire from the envelope (which isn't being fed
      // above). Drop (rather than leave stranded) a trigger that was already armed before a mode
      // switch landed mid-block, and zero the meter so the always-visible envelope display reads
      // idle instead of freezing at whatever level it last saw.
      mTriggerPending = false;
      mEnvelope = 0.0;
    }

    for (int c = 0; c < nChans; c++) {
      outputs[c][s] = inputs[c][s] * gain;
    }
  }

  // Audio Output Mode (§7.7): render every voice FireSprinkle scheduled into mSynthEngine this
  // block, adding onto the dry passthrough already written above rather than replacing it. Skipped
  // entirely in MIDI-only mode -- there are never any pending voices to drain in that case anyway
  // (FireSprinkle never schedules them), but this also avoids the per-voice render loop's cost when
  // it can't possibly have anything to do.
  if (snapshot.outputMode != sparkle_core::OutputMode::Midi)
    mSynthEngine.Render(blockStart, nFrames, sampleRate, snapshot.waveShape, outputs, nChans);

  // Soft-clip safety net (see core/SynthEngine.h::SoftClip) on the final mix -- cheap insurance
  // against overs when several simultaneous sparkles' synth voices (plus the dry passthrough) sum
  // above unity, without audibly colouring normal-level signal.
  for (int c = 0; c < nChans; c++) {
    for (int s = 0; s < nFrames; s++) {
      outputs[c][s] = static_cast<sample>(sparkle_core::SoftClip(outputs[c][s]));
    }
  }

  // Flush every note-on/note-off due in this block into MIDI out. Looped because FlushBlock only
  // fills up to outCapacity per call, leaving any remainder pending for a follow-up call rather
  // than dropping it (see EventScheduler::FlushBlock) -- draining here keeps it all landing in
  // this same block instead of trickling into the next one.
  std::array<sparkle_core::SchedEvent, 64> schedEvents;
  size_t nSchedEvents;
  do {
    nSchedEvents = mEventScheduler.FlushBlock(blockStart, nFrames, schedEvents.data(), schedEvents.size());

    for (size_t i = 0; i < nSchedEvents; i++) {
      const sparkle_core::SchedEvent& event = schedEvents[i];
      IMidiMsg msg;

      if (event.type == sparkle_core::SchedEventType::NoteOn)
        msg.MakeNoteOnMsg(event.note, event.velocity, event.offsetInBlock);
      else
        msg.MakeNoteOffMsg(event.note, event.offsetInBlock);

      SendMidiMsg(msg);
    }
  } while (nSchedEvents == schedEvents.size());

  // Flush every flash "note-on" due in this block the same way, decoding mFlashScheduler's packed
  // column/row back out of SchedEvent::note (see FireSprinkle) and deduping within the block before
  // pushing to mNoteMatrixFlashSender -- several events can land on the same cell in one block, and
  // the queue behind the sender is only 64 deep (see ISender.h). Note-offs are meaningless here
  // (every flash event schedules with durationSamples 0 purely to get a paired one out of the pool)
  // so they're ignored.
  std::array<std::array<bool, sparkle_core::kNumPitchClasses>, sparkle_core::kNumPitchClasses> cellsHit{};
  size_t nFlashEvents;
  do {
    nFlashEvents = mFlashScheduler.FlushBlock(blockStart, nFrames, schedEvents.data(), schedEvents.size());

    for (size_t i = 0; i < nFlashEvents; i++) {
      const sparkle_core::SchedEvent& event = schedEvents[i];
      if (event.type != sparkle_core::SchedEventType::NoteOn)
        continue;

      const int col = event.note / sparkle_core::kNumPitchClasses;
      const int row = event.note % sparkle_core::kNumPitchClasses;
      cellsHit[col][row] = true;
    }
  } while (nFlashEvents == schedEvents.size());

  for (int col = 0; col < sparkle_core::kNumPitchClasses; col++) {
    for (int row = 0; row < sparkle_core::kNumPitchClasses; row++) {
      if (cellsHit[col][row]) {
        mNoteMatrixFlashSender.PushData(ISenderData<2>(kCtrlTagNoteMatrix,
          std::array<float, 2>{ static_cast<float>(col), static_cast<float>(row) }));
      }
    }
  }

  mBlockStartSample += nFrames;

  // Reap once more here (rather than only at the next trigger) so mSprinkleCountSender reports a
  // live count instead of one that lags until the next trigger happens to reap it.
  ReapFinishedSprinkles(mBlockStartSample);

  mEnvelopeSender.PushData(ISenderData<1>(kCtrlTagEnvelopeMeter, std::array<float, 1>{ static_cast<float>(mEnvelope) }));
  mNoteSender.PushData(ISenderData<2>(kCtrlTagNoteDisplay,
    std::array<float, 2>{ static_cast<float>(mPitchTracker.DisplayNote()),
                          static_cast<float>(mPitchTracker.DisplayConfidence()) }));

  ISenderData<sparkle_params::kNumTriggerableNotes> noteBarsData(kCtrlTagNoteBars,
    sparkle_params::kNumTriggerableNotes, 0);
  mPitchTracker.GetNoteConfidences(
    sparkle_params::kMinTriggerableNote, sparkle_params::kMaxTriggerableNote, noteBarsData.vals.data());
  mNoteBarsSender.PushData(noteBarsData);
  mSprinkleCountSender.PushData(ISenderData<1>(kCtrlTagSprinkleCount, std::array<float, 1>{ static_cast<float>(mNumActiveSprinkles) }));
}

void Sparkles::ReapFinishedSprinkles(int64_t nowSample)
{
  int writeIdx = 0;
  for (int i = 0; i < mNumActiveSprinkles; i++) {
    if (mActiveSprinkleEndSamples[i] > nowSample)
      mActiveSprinkleEndSamples[writeIdx++] = mActiveSprinkleEndSamples[i];
  }
  mNumActiveSprinkles = writeIdx;
}

void Sparkles::OnIdle()
{
  mEnvelopeSender.TransmitData(*this);
  mNoteSender.TransmitData(*this);
  mNoteBarsSender.TransmitData(*this);
  mTriggerSender.TransmitData(*this);
  mSprinkleCountSender.TransmitData(*this);
  mNoteMatrixFlashSender.TransmitData(*this);
}
#endif
