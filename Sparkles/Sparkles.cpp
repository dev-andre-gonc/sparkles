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

  enum class EParamCtrlKind { Knob, Dropdown, TimeKnob };

  struct ParamClusterDesc
  {
    int paramIdx;
    const char* label;
    EParamCtrlKind kind;
    int unitParamIdx = -1;                          // TimeKnob only: sibling *Unit enum param
    int rmParamIdx = -1;
    EParamCtrlKind rmKind = EParamCtrlKind::Knob;
    int smParamIdx = -1;                             // Sm is always the same kind as the base
  };

  struct ParamGroupDesc
  {
    const char* name;
    const ParamClusterDesc* clusters;
    int numClusters;
    EUITab tab;
    int preferredCols = 0; // 0 = auto (min(numClusters, 3)); set explicitly where pairing matters
  };

  // Output Mode + Passthrough only -- Detect/Trigger On/Threshold/Min Velocity live in
  // kDetectionControls below now (General vs. Detection tabs, see EUITab).
  constexpr ParamClusterDesc kGeneralControls[] = {
    { kParamOutputMode,     "Output",       EParamCtrlKind::Dropdown },
    { kParamGain,           "Passthrough",  EParamCtrlKind::Knob },
  };

  constexpr ParamClusterDesc kDetectionControls[] = {
    { kParamDetectionMode,  "Detect",       EParamCtrlKind::Dropdown },
    { kParamTriggerOn,      "Trigger On",   EParamCtrlKind::Dropdown },
    { kParamThreshold,      "Threshold",    EParamCtrlKind::Knob },
    { kParamVelocityDetect, "Min Velocity", EParamCtrlKind::Knob },
  };

  constexpr ParamClusterDesc kTechnicalControls[] = {
    { kParamReactiveness, "Env Reactiveness", EParamCtrlKind::Knob },
    { kParamConfidence,   "Note Confidence",  EParamCtrlKind::Knob },
    { kParamMinNote,      "Min Note",         EParamCtrlKind::Knob },
    { kParamMaxNote,      "Max Note",         EParamCtrlKind::Knob },
  };

  // Rays + Sparkles/Ray only -- Range Min/Max + Wrap Mode moved to kRangeControls (Pitch and
  // Timing tab, per "output note range and wrap mode" belonging there instead of General).
  constexpr ParamClusterDesc kSprinkleStructureControls[] = {
    { kParamNRays,           "Rays",         EParamCtrlKind::Knob },
    { kParamNSparklesPerRay, "Sparkles/Ray", EParamCtrlKind::Knob, -1, kParamNSparklesPerRayRm },
  };

  constexpr ParamClusterDesc kRangeControls[] = {
    { kParamRangeMin, "Range Min", EParamCtrlKind::Knob },
    { kParamRangeMax, "Range Max", EParamCtrlKind::Knob },
    { kParamWrapMode, "Wrap",      EParamCtrlKind::Dropdown },
  };

  constexpr ParamClusterDesc kOffsetControls[] = {
    { kParamPreDelay,     "Pre Delay",    EParamCtrlKind::TimeKnob, kParamPreDelayUnit },
    { kParamPreDelayUnit, "Delay Unit",   EParamCtrlKind::Dropdown },
    { kParamPreInterval,  "Pre Interval", EParamCtrlKind::Knob },
  };

  constexpr ParamClusterDesc kSparklePropertyControls[] = {
    { kParamVelocity,     "Velocity",      EParamCtrlKind::Knob, -1, kParamLoudnessRm, EParamCtrlKind::Knob, kParamLoudnessSm },
    { kParamDuration,     "Duration",      EParamCtrlKind::TimeKnob, kParamDurationUnit, kParamDurationRm, EParamCtrlKind::Knob, kParamDurationSm },
    { kParamDurationUnit, "Duration Unit", EParamCtrlKind::Dropdown },
  };

  constexpr ParamClusterDesc kTimingControls[] = {
    { kParamRayDelay,     "Ray Delay",      EParamCtrlKind::TimeKnob, kParamRayDelayUnit, kParamRayDelayRm },
    { kParamRayDelayUnit, "Ray Delay Unit", EParamCtrlKind::Dropdown },
    { kParamDelay,        "Delay",          EParamCtrlKind::TimeKnob, kParamDelayUnit, kParamDelayRm, EParamCtrlKind::Knob, kParamDelaySm },
    { kParamDelayUnit,    "Delay Unit",     EParamCtrlKind::Dropdown },
  };

  constexpr ParamClusterDesc kPitchControls[] = {
    { kParamRayInterval, "Ray Interval", EParamCtrlKind::Knob, -1, kParamRayIntervalRm },
    { kParamInterval,    "Interval",     EParamCtrlKind::Knob, -1, kParamIntervalRm, EParamCtrlKind::Knob, kParamIntervalSm },
  };

  constexpr ParamClusterDesc kStereoControls[] = {
    { kParamPanning,     "Panning",      EParamCtrlKind::Dropdown },
    { kParamWidth,       "Width",        EParamCtrlKind::Knob, -1, kParamWidthRm, EParamCtrlKind::Knob, kParamWidthSm },
    { kParamPhase,       "Phase",        EParamCtrlKind::Knob, -1, kParamPhaseRm, EParamCtrlKind::Knob, kParamPhaseSm },
    { kParamRayRotation, "Ray Rotation", EParamCtrlKind::Dropdown, -1, kParamRayRotationRm, EParamCtrlKind::Dropdown },
    { kParamSeed,        "Seed",         EParamCtrlKind::Knob },
  };

  constexpr ParamClusterDesc kSynthControls[] = {
    { kParamWaveShape, "Wave Shape", EParamCtrlKind::Knob },
    { kParamAttack,    "Attack",     EParamCtrlKind::Knob, -1, kParamAttackRm,  EParamCtrlKind::Knob, kParamAttackSm },
    { kParamDecay,     "Decay",      EParamCtrlKind::Knob, -1, kParamDecayRm,   EParamCtrlKind::Knob, kParamDecaySm },
    { kParamSustain,   "Sustain",    EParamCtrlKind::Knob, -1, kParamSustainRm, EParamCtrlKind::Knob, kParamSustainSm },
    { kParamRelease,   "Release",    EParamCtrlKind::Knob, -1, kParamReleaseRm, EParamCtrlKind::Knob, kParamReleaseSm },
  };

  constexpr ParamGroupDesc kParamGroups[] = {
    { "General",              kGeneralControls,           (int) std::size(kGeneralControls),          EUITab::General },
    { "Sprinkle Structure",   kSprinkleStructureControls,  (int) std::size(kSprinkleStructureControls), EUITab::General },
    { "Sparkle Properties",   kSparklePropertyControls,    (int) std::size(kSparklePropertyControls),   EUITab::General },
    { "Detection",            kDetectionControls,          (int) std::size(kDetectionControls),         EUITab::Detection },
    { "Technical",            kTechnicalControls,          (int) std::size(kTechnicalControls),         EUITab::Detection },
    { "Trigger Offset",       kOffsetControls,              (int) std::size(kOffsetControls),            EUITab::PitchTiming },
    { "Timing",               kTimingControls,              (int) std::size(kTimingControls),            EUITab::PitchTiming, 2 },
    { "Pitch",                kPitchControls,                (int) std::size(kPitchControls),             EUITab::PitchTiming },
    { "Output Range",         kRangeControls,                (int) std::size(kRangeControls),             EUITab::PitchTiming },
    { "Synth",                kSynthControls,                (int) std::size(kSynthControls),             EUITab::Synth },
    { "Stereo (Audio Output)", kStereoControls,              (int) std::size(kStereoControls),            EUITab::Synth },
  };

  constexpr int kNumParamGroups = (int) std::size(kParamGroups);

  // Per-control cell size used by mLayoutFunc's group-flow layout below. Knob cells are taller
  // than dropdown cells since a knob needs room for its widget plus label plus value text above
  // and below it -- cropping those was the whole reason the fixed-outer-grid layout got replaced
  // with this flow-packed one. Kept small on purpose -- each row is then stretched to fill any
  // leftover width (see kMaxExtraPerGroupW below), so these are a floor, not the final size.
  constexpr float kCellW = 72.f;
  constexpr float kKnobCellH = 54.f;
  constexpr float kDropdownCellH = 32.f;
  // A numeric (Knob-kind) Rm/Sm modifier renders as a condensed text chip ("x1.20 p/ray") beside
  // its base control instead of a small knob -- see ui/ModifierValueControl.h and the attach loop
  // below. kModW/kModH are kept only for the one remaining non-numeric case, an enum-valued Rm
  // (Ray Rotation's Rm is itself "Keep"/"Invert", not a multiplier -- see kStereoControls), which
  // still renders as a small IVMenuButtonControl dropdown.
  constexpr float kModW = 30.f;
  constexpr float kModH = 30.f;
  constexpr float kChipW = 92.f;
  constexpr float kChipH = 16.f;
  constexpr float kClusterGap = 3.f;
  // Cap on how much extra width a single group can be stretched by when justifying a row to fill
  // paramsArea -- without this, a row with only one or two small groups would balloon its knobs to
  // an ungainly size on a wide window.
  constexpr float kMaxExtraPerGroupW = 50.f;

  // IVStyle's default label/value text (~19px/14px) is sized for the framework's normal control
  // sizes -- at these compact cell sizes it doesn't just look oversized, IVectorBase::MakeRects
  // carves the label/value bands out of the control's own rect *before* sizing the actual
  // clickable/draggable widget, so an unshrunk label can consume the whole cell (or overflow it,
  // producing a negative-size widget rect that IRECT::Contains() can never hit -- an invisible,
  // permanently unclickable control). kCompactStyle keeps that carve-out small enough to leave a
  // real widget behind.
  const IVStyle kCompactStyle = DEFAULT_STYLE
    .WithLabelText(DEFAULT_LABEL_TEXT.WithSize(10.f))
    .WithValueText(DEFAULT_VALUE_TEXT.WithSize(10.f));

  float ClusterWidth(const ParamClusterDesc& c)
  {
    float w = kCellW;
    if (c.rmParamIdx >= 0) w += kClusterGap + (c.rmKind == EParamCtrlKind::Knob ? kChipW : kModW);
    if (c.smParamIdx >= 0) w += kClusterGap + kChipW; // Sm is always numeric, see ParamClusterDesc's comment
    return w;
  }

  float ClusterHeight(const ParamClusterDesc& c)
  {
    return c.kind == EParamCtrlKind::Dropdown ? kDropdownCellH : kKnobCellH;
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
    const IRECT innerBounds = bounds.GetPadded(-4.f);
    const EUITab activeTab = (EUITab) mActiveTab;

    const IRECT titleBounds = innerBounds.GetFromTop(18.f).GetFromLeft(110.f);
    const IRECT versionBounds = innerBounds.GetFromTop(13.f).GetFromRight(200.f);
    const IRECT contentBounds = innerBounds.GetReducedFromTop(20.f);

    // Always-visible left column: the 6 tab selectors plus the Presets button (not a tab, see
    // ApplyPreset) -- everything right of tabBarBounds belongs to exactly one tab and is shown/
    // hidden as a whole by the Hide() calls further down. Currently plain IVButtonControls in equal
    // grid cells; swap these for PNG bitmap-frame controls once art exists (see CLAUDE.md).
    const IRECT tabBarBounds = contentBounds.GetFromLeft(120.f);
    const IRECT mainArea = contentBounds.GetReducedFromLeft(126.f);
    const IRECT tabQuickGuideBounds  = tabBarBounds.GetGridCell(0, 7, 1).GetPadded(-2.f);
    const IRECT tabGeneralBounds     = tabBarBounds.GetGridCell(1, 7, 1).GetPadded(-2.f);
    const IRECT tabDetectionBounds   = tabBarBounds.GetGridCell(2, 7, 1).GetPadded(-2.f);
    const IRECT tabPitchTimingBounds = tabBarBounds.GetGridCell(3, 7, 1).GetPadded(-2.f);
    const IRECT tabNoteMatrixBounds  = tabBarBounds.GetGridCell(4, 7, 1).GetPadded(-2.f);
    const IRECT tabSynthBounds       = tabBarBounds.GetGridCell(5, 7, 1).GetPadded(-2.f);
    const IRECT presetsBounds        = tabBarBounds.GetGridCell(6, 7, 1).GetPadded(-2.f);

    // Always-visible strip (envelope meter beside a stacked note/confidence + sprinkle-count +
    // trigger-light readout, plus the Shut Up button) -- carved from the top of mainArea, same as
    // the tab selectors above: never hidden regardless of mActiveTab, so tabContentArea (every
    // tab's own content, below it) is uniformly reduced here rather than per-tab like an earlier
    // version of this did. That per-tab carve-out was also the root cause of a visual glitch: since
    // Shut Up used to live inside the General tab's Hide()-scoped content, switching away from
    // General mid-click-animation could hide it mid-flash and leave it looking stuck; living outside
    // the tab system entirely (like NoteBarsControl already did) means it's never hidden at all.
    const IRECT persistentStripBounds = mainArea.GetFromTop(90.f);
    const IRECT tabContentArea = mainArea.GetReducedFromTop(96.f);
    const IRECT paramsArea = tabContentArea;
    const IRECT shutUpBounds = persistentStripBounds.GetFromRight(90.f).GetPadded(-4.f);
    const IRECT meterBounds = persistentStripBounds.GetFromLeft(60.f).GetPadded(-4.f);
    const IRECT infoArea = persistentStripBounds.GetReducedFromLeft(64.f).GetReducedFromRight(96.f);
    const IRECT noteBounds = infoArea.GetGridCell(0, 1, 3).GetPadded(-3.f);
    const IRECT sprinkleCountBounds = infoArea.GetGridCell(1, 1, 3).GetPadded(-3.f);
    const IRECT triggerLightBounds = infoArea.GetGridCell(2, 1, 3).GetCentredInside(22.f);

    // Note Matrix tab: §5.1 Key/Scale quick-fill pair above the §5 note-eligibility matrix (12x12
    // grid + column/row toggles, see ui/NoteMatrixControl.h and mNoteMatrix in Sparkles.h) --
    // hand-placed here (rather than in the flowing param-group grid) since they act on the matrix
    // directly, not via kParamGroups.
    const IRECT keyHeaderBounds = tabContentArea.GetFromTop(26.f).GetFromLeft(360.f);
    const IRECT keyRootBounds = keyHeaderBounds.GetGridCell(0, 1, 3).GetPadded(-2.f);
    const IRECT keyScaleDropdownBounds = keyHeaderBounds.GetGridCell(1, 1, 3).GetPadded(-2.f);
    const IRECT keyModeBounds = keyHeaderBounds.GetGridCell(2, 1, 3).GetPadded(-2.f);
    const IRECT noteMatrixBounds = tabContentArea.GetReducedFromTop(32.f)
      .GetCentredInside(std::min(tabContentArea.W(), tabContentArea.H() - 32.f));

    // Quick Guide tab: a single placeholder image control until the real guide PNG exists.
    const IRECT quickGuideBounds = tabContentArea;

    // One entry per allocated param control (base, then Rm, then Sm -- in kParamGroups' group/
    // cluster order) and one IRECT per group's labelled frame, computed once here so both the
    // resize branch and the initial-attach branch below stay fed by the same layout math -- see
    // CLAUDE.md's "How mLayoutFunc is structured". Groups are shelf-packed left to right, wrapping
    // to a new row when they'd overflow paramsArea's width, rather than forced into a fixed outer
    // grid -- a uniform grid either wasted space on small groups or squeezed big ones down to where
    // their controls' labels got cropped. Within a group, clusters flow the same way (see the
    // per-group inner loop below), since a cluster's width varies with how many Rm/Sm modifiers it
    // has (see ClusterWidth).
    //
    // Only one tab's groups are ever visible at once, so the row-packing below (pass 1: bucket into
    // rows by nominal width; pass 2: stretch each row to fill paramsArea) only ever considers the
    // active tab's groups -- there's no reason to share width with groups that are about to be
    // hidden anyway. Every group still gets a flatControls entry further down (see Step B),
    // regardless of tab, in the same fixed order on every single call -- control tags
    // (kCtrlTagFirstParamControl + index) must stay stable across tab switches, which only holds if
    // that enumeration order never depends on mActiveTab. Only each entry's rect (real if its
    // group's tab is active, IRECT() otherwise -- harmless, since Hide() below keeps it off-screen
    // and un-hit-testable regardless) and .tab (used by that same Hide() call) depend on it.
    constexpr float kGroupGap = 6.f;

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

    std::vector<int> activeGroupIndices;
    for (int g = 0; g < kNumParamGroups; g++)
      if (kParamGroups[g].tab == activeTab)
        activeGroupIndices.push_back(g);

    std::vector<float> nominalW(kNumParamGroups, 0.f), nominalH(kNumParamGroups, 0.f);
    std::vector<std::vector<int>> rows;
    rows.reserve(activeGroupIndices.size());

    {
      std::vector<int> currentRow;
      float rowW = 0.f;
      for (int g : activeGroupIndices) {
        const ParamGroupDesc& group = kParamGroups[g];
        float maxClusterW = 0.f, maxClusterH = 0.f;
        for (int c = 0; c < group.numClusters; c++) {
          maxClusterW = std::max(maxClusterW, ClusterWidth(group.clusters[c]));
          maxClusterH = std::max(maxClusterH, ClusterHeight(group.clusters[c]));
        }

        const int cols = group.preferredCols > 0 ? group.preferredCols : std::min(group.numClusters, 6);
        const int gRows = (group.numClusters + cols - 1) / cols;
        nominalW[g] = cols * maxClusterW + (cols - 1) * kClusterGap + 16.f;
        nominalH[g] = gRows * maxClusterH + 26.f + 16.f; // + label offset + top/bottom padding

        if (!currentRow.empty() && rowW + kGroupGap + nominalW[g] > paramsArea.W()) {
          rows.push_back(currentRow);
          currentRow.clear();
          rowW = 0.f;
        }

        rowW += (currentRow.empty() ? 0.f : kGroupGap) + nominalW[g];
        currentRow.push_back(g);
      }
      if (!currentRow.empty())
        rows.push_back(currentRow);
    }

    std::vector<IRECT> groupBounds(kNumParamGroups); // IRECT() (zero rect) for inactive-tab groups
    float rowY = paramsArea.T;
    for (const std::vector<int>& rowGroups : rows) {
      float nominalRowW = 0.f;
      for (size_t i = 0; i < rowGroups.size(); i++)
        nominalRowW += (i == 0 ? 0.f : kGroupGap) + nominalW[rowGroups[i]];

      const float extra = std::max(0.f, paramsArea.W() - nominalRowW);
      const float extraPerGroup = std::min(extra / (float) rowGroups.size(), kMaxExtraPerGroupW);

      float rowH = 0.f;
      float x = paramsArea.L;
      for (int g : rowGroups) {
        const float w = nominalW[g] + extraPerGroup;
        const float h = nominalH[g];
        groupBounds[g] = IRECT(x, rowY, x + w, rowY + h);
        x += w + kGroupGap;
        rowH = std::max(rowH, h);
      }
      rowY += rowH + kGroupGap;
    }

    // Step B: flow every group's clusters (base control, then a smaller Rm/Sm beside it) using
    // groupBounds[g] from above -- see the big comment before Step A for why this walks all
    // kNumParamGroups in fixed order rather than just activeGroupIndices.
    std::vector<FlatCtrl> flatControls;
    flatControls.reserve(96);
    for (int g = 0; g < kNumParamGroups; g++) {
      const ParamGroupDesc& group = kParamGroups[g];
      const IRECT groupContent = groupBounds[g].GetPadded(-8.f, -22.f, -8.f, -8.f); // room for the group's own label

      float cx = groupContent.L, cy = groupContent.T, clusterRowH = 0.f;
      for (int c = 0; c < group.numClusters; c++) {
        const ParamClusterDesc& cluster = group.clusters[c];
        const float clusterW = ClusterWidth(cluster);
        const float clusterH = ClusterHeight(cluster);

        if (cx > groupContent.L && cx + clusterW > groupContent.R) {
          cy += clusterRowH + kClusterGap;
          cx = groupContent.L;
          clusterRowH = 0.f;
        }

        const IRECT clusterRect(cx, cy, cx + clusterW, cy + clusterH);
        flatControls.push_back(FlatCtrl{ clusterRect.GetFromLeft(kCellW).GetPadded(-2.f), cluster.paramIdx, cluster.label, cluster.kind, cluster.unitParamIdx, false, group.tab });

        // A numeric (Knob-kind) Rm/Sm gets the condensed "x1.20 p/ray" text chip (see
        // ui/ModifierValueControl.h); Ray Rotation's Rm is the one enum-valued exception and keeps
        // the small "Rm"-captioned dropdown instead, since it isn't a multiplier -- see ClusterWidth.
        float modX = cx + kCellW + kClusterGap;
        if (cluster.rmParamIdx >= 0) {
          const bool numeric = cluster.rmKind == EParamCtrlKind::Knob;
          const float w = numeric ? kChipW : kModW, h = numeric ? kChipH : kModH;
          const IRECT modRect(modX, clusterRect.MH() - h * 0.5f, modX + w, clusterRect.MH() + h * 0.5f);
          flatControls.push_back(FlatCtrl{ modRect.GetPadded(-1.f), cluster.rmParamIdx, numeric ? "p/ray" : "Rm", cluster.rmKind, -1, true, group.tab });
          modX += w + kClusterGap;
        }
        if (cluster.smParamIdx >= 0) { // Sm is always numeric, see ParamClusterDesc's comment
          const IRECT modRect(modX, clusterRect.MH() - kChipH * 0.5f, modX + kChipW, clusterRect.MH() + kChipH * 0.5f);
          flatControls.push_back(FlatCtrl{ modRect.GetPadded(-1.f), cluster.smParamIdx, "p/sparkle", EParamCtrlKind::Knob, -1, true, group.tab });
        }

        cx += clusterW + kClusterGap;
        clusterRowH = std::max(clusterRowH, clusterH);
      }
    }

    if (pGraphics->NControls()) {
      pGraphics->GetBackgroundControl()->SetTargetAndDrawRECTs(bounds);
      pGraphics->GetControlWithTag(kCtrlTagNoteBars)->SetTargetAndDrawRECTs(bounds);
      pGraphics->GetControlWithTag(kCtrlTagTitle)->SetTargetAndDrawRECTs(titleBounds);
      pGraphics->GetControlWithTag(kCtrlTagVersionNumber)->SetTargetAndDrawRECTs(versionBounds);

      pGraphics->GetControlWithTag(kCtrlTagTabQuickGuide)->SetTargetAndDrawRECTs(tabQuickGuideBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabGeneral)->SetTargetAndDrawRECTs(tabGeneralBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabDetection)->SetTargetAndDrawRECTs(tabDetectionBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabPitchTiming)->SetTargetAndDrawRECTs(tabPitchTimingBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabNoteMatrix)->SetTargetAndDrawRECTs(tabNoteMatrixBounds);
      pGraphics->GetControlWithTag(kCtrlTagTabSynth)->SetTargetAndDrawRECTs(tabSynthBounds);
      pGraphics->GetControlWithTag(kCtrlTagPresets)->SetTargetAndDrawRECTs(presetsBounds);

      // Always-visible, never Hide()'d -- see the big comment above persistentStripBounds.
      pGraphics->GetControlWithTag(kCtrlTagEnvelopeMeter)->SetTargetAndDrawRECTs(meterBounds);
      pGraphics->GetControlWithTag(kCtrlTagNoteDisplay)->SetTargetAndDrawRECTs(noteBounds);
      pGraphics->GetControlWithTag(kCtrlTagSprinkleCount)->SetTargetAndDrawRECTs(sprinkleCountBounds);
      pGraphics->GetControlWithTag(kCtrlTagTriggerLight)->SetTargetAndDrawRECTs(triggerLightBounds);
      pGraphics->GetControlWithTag(kCtrlTagShutUp)->SetTargetAndDrawRECTs(shutUpBounds);

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

      for (int i = 0; i < (int) flatControls.size(); i++)
        setTabbed(kCtrlTagFirstParamControl + i, flatControls[i].rect, flatControls[i].tab);
      for (int g = 0; g < kNumParamGroups; g++)
        setTabbed(kCtrlTagFirstParamControl + (int) flatControls.size() + g, groupBounds[g], kParamGroups[g].tab);
      return;
    }

    pGraphics->SetLayoutOnResize(true);
    pGraphics->AttachCornerResizer(EUIResizerMode::Size, true);
    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->AttachPanelBackground(COLOR_LIGHT_GRAY);

    // Attached first (right after the background) so the translucent bars render behind every
    // other control -- draw order follows attach order. Gets the full plugin bounds: the bars
    // anchor to the bottom edge and scale against the whole plugin height (see NoteBarsControl).
    pGraphics->AttachControl(new NoteBarsControl(bounds), kCtrlTagNoteBars);

    pGraphics->AttachControl(new ITextControl(titleBounds, "Sparkles", IText(14)), kCtrlTagTitle);
    WDL_String buildInfoStr;
    GetBuildInfoStr(buildInfoStr, __DATE__, __TIME__);
    pGraphics->AttachControl(new ITextControl(versionBounds, buildInfoStr.Get(), DEFAULT_TEXT.WithAlign(EAlign::Far)), kCtrlTagVersionNumber);

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
    pGraphics->AttachControl(new IVButtonControl(tabQuickGuideBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::QuickGuide; mLayoutFunc(pCaller->GetUI());
    }, "Guide", kCompactStyle), kCtrlTagTabQuickGuide);
    pGraphics->AttachControl(new IVButtonControl(tabGeneralBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::General; mLayoutFunc(pCaller->GetUI());
    }, "General", kCompactStyle), kCtrlTagTabGeneral);
    pGraphics->AttachControl(new IVButtonControl(tabDetectionBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::Detection; mLayoutFunc(pCaller->GetUI());
    }, "Detection", kCompactStyle), kCtrlTagTabDetection);
    pGraphics->AttachControl(new IVButtonControl(tabPitchTimingBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::PitchTiming; mLayoutFunc(pCaller->GetUI());
    }, "Pitch/Timing", kCompactStyle), kCtrlTagTabPitchTiming);
    pGraphics->AttachControl(new IVButtonControl(tabNoteMatrixBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::NoteMatrix; mLayoutFunc(pCaller->GetUI());
    }, "Note Matrix", kCompactStyle), kCtrlTagTabNoteMatrix);
    pGraphics->AttachControl(new IVButtonControl(tabSynthBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mActiveTab = (int) EUITab::Synth; mLayoutFunc(pCaller->GetUI());
    }, "Synth", kCompactStyle), kCtrlTagTabSynth);
    // Click-through, not a dropdown: each click advances to the next factory preset (wrapping) and
    // applies it immediately, updating the button's own label to the newly-active preset's name so
    // there's always visible feedback for which one is loaded.
    pGraphics->AttachControl(new IVButtonControl(presetsBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mPresetIndex = (mPresetIndex + 1) % kNumFactoryPresets;
      ApplyPreset(mPresetIndex);
      pCaller->As<IVectorBase>()->SetLabelStr(kPresets[mPresetIndex].name);
    }, kPresets[0].name, kCompactStyle), kCtrlTagPresets);

    pGraphics->AttachControl(new ITextControl(quickGuideBounds, "Quick Guide -- image pending", IText(16)), kCtrlTagQuickGuideImage);

    pGraphics->AttachControl(new EnvelopeMeterControl(meterBounds, kParamThreshold), kCtrlTagEnvelopeMeter);
    pGraphics->AttachControl(new ValueDisplayControl<2>(noteBounds, "--", IText(18), [](const std::array<float, 2>& vals, WDL_String& str) {
      const int note = static_cast<int>(std::lround(vals[0]));
      if (note < 0 || vals[1] < kMinDisplayConfidence) {
        str.Set("--");
        return;
      }
      WDL_String noteName;
      FormatNoteName(note, noteName);
      str.SetFormatted(16, "%s %d%%", noteName.Get(), static_cast<int>(std::lround(vals[1] * 100.f)));
    }), kCtrlTagNoteDisplay);
    pGraphics->AttachControl(new ValueDisplayControl<1>(sprinkleCountBounds, "0 sprinkles", IText(12), [](const std::array<float, 1>& vals, WDL_String& str) {
      str.SetFormatted(32, "%d sprinkles", static_cast<int>(std::lround(vals[0])));
    }), kCtrlTagSprinkleCount);
    pGraphics->AttachControl(new TriggerLightControl(triggerLightBounds), kCtrlTagTriggerLight);
    pGraphics->AttachControl(new IVButtonControl(shutUpBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mShutUpRequested.store(true, std::memory_order_release);
    }, "Shut Up", kCompactStyle), kCtrlTagShutUp);
    pGraphics->AttachControl(new IVMenuButtonControl(keyRootBounds, kParamKeyRoot, "Root", kCompactStyle), kCtrlTagKeyRoot);
    pGraphics->AttachControl(new IVMenuButtonControl(keyScaleDropdownBounds, kParamKeyScale, "Scale", kCompactStyle), kCtrlTagKeyScale);
    pGraphics->AttachControl(new IVMenuButtonControl(keyModeBounds, kParamKeyMode, "Mode", kCompactStyle), kCtrlTagKeyMode);
    pGraphics->AttachControl(new NoteMatrixControl(noteMatrixBounds, &mNoteMatrix), kCtrlTagNoteMatrix);

    for (int i = 0; i < (int) flatControls.size(); i++) {
      const FlatCtrl& ctrl = flatControls[i];
      const int tag = kCtrlTagFirstParamControl + i;
      switch (ctrl.kind) {
        case EParamCtrlKind::Knob:
          if (ctrl.isModifier)
            pGraphics->AttachControl(new ModifierValueControl(ctrl.rect, ctrl.paramIdx, ctrl.label), tag);
          else
            pGraphics->AttachControl(new IVKnobControl(ctrl.rect, ctrl.paramIdx, ctrl.label, kCompactStyle), tag);
          break;
        case EParamCtrlKind::TimeKnob:
          pGraphics->AttachControl(new TimeMagnitudeControl(ctrl.rect, ctrl.paramIdx, ctrl.unitParamIdx, ctrl.label), tag);
          break;
        case EParamCtrlKind::Dropdown:
        default:
          pGraphics->AttachControl(new IVMenuButtonControl(ctrl.rect, ctrl.paramIdx, ctrl.label, kCompactStyle), tag);
          break;
      }
    }
    for (int g = 0; g < kNumParamGroups; g++)
      pGraphics->AttachControl(new IVGroupControl(groupBounds[g], kParamGroups[g].name), kCtrlTagFirstParamControl + (int) flatControls.size() + g);

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
  for (int i = 0; i < kNumScopedParams; i++)
    SetParameterValue(kScopedParamIds[i], GetParam(kScopedParamIds[i])->ToNormalized(preset.values[i]));

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
      FireSprinkle(note, triggerSample, timelineSample, sparkleParams, outputMode, bpm, sampleRate);
  }
  else { // isNoteOff -- gate on the velocity the note was struck with, not the note-off's own byte.
    const int heldVelocity = mHeldNoteVelocity[note];
    mHeldNoteVelocity[note] = -1;
    const bool fireDown = triggerType == sparkle_core::TriggerType::Down || triggerType == sparkle_core::TriggerType::Both;
    if (fireDown && heldVelocity >= detection.minVelocity)
      FireSprinkle(note, triggerSample, timelineSample, sparkleParams, outputMode, bpm, sampleRate);
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
                            sparkle_core::OutputMode outputMode, double bpm, double sampleRate)
{
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
          FireSprinkle(mPitchTracker.LastConfidentNote(), blockStart + s, timelineSample, snapshot.sparkle, snapshot.outputMode, bpm, sampleRate);
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
            FireSprinkle(mPitchTracker.LastConfidentNote(), blockStart + s, timelineSample, snapshot.sparkle, snapshot.outputMode, bpm, sampleRate);
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
