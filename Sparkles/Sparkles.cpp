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
    int preferredCols = 0; // 0 = auto (min(numClusters, 3)); set explicitly where pairing matters
  };

  constexpr ParamClusterDesc kGeneralControls[] = {
    { kParamOutputMode,     "Output",       EParamCtrlKind::Dropdown },
    { kParamGain,           "Passthrough",  EParamCtrlKind::Knob },
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

  constexpr ParamClusterDesc kSprinkleStructureControls[] = {
    { kParamNRays,           "Rays",         EParamCtrlKind::Knob },
    { kParamNSparklesPerRay, "Sparkles/Ray", EParamCtrlKind::Knob, -1, kParamNSparklesPerRayRm },
    { kParamRangeMin,        "Range Min",    EParamCtrlKind::Knob },
    { kParamRangeMax,        "Range Max",    EParamCtrlKind::Knob },
    { kParamWrapMode,        "Wrap",         EParamCtrlKind::Dropdown },
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
  };

  constexpr ParamClusterDesc kSynthControls[] = {
    { kParamWaveShape, "Wave Shape", EParamCtrlKind::Knob },
    { kParamAttack,    "Attack",     EParamCtrlKind::Knob, -1, kParamAttackRm,  EParamCtrlKind::Knob, kParamAttackSm },
    { kParamDecay,     "Decay",      EParamCtrlKind::Knob, -1, kParamDecayRm,   EParamCtrlKind::Knob, kParamDecaySm },
    { kParamSustain,   "Sustain",    EParamCtrlKind::Knob, -1, kParamSustainRm, EParamCtrlKind::Knob, kParamSustainSm },
    { kParamRelease,   "Release",    EParamCtrlKind::Knob, -1, kParamReleaseRm, EParamCtrlKind::Knob, kParamReleaseSm },
  };

  constexpr ParamGroupDesc kParamGroups[] = {
    { "General",             kGeneralControls,          (int) std::size(kGeneralControls) },
    { "Technical",           kTechnicalControls,         (int) std::size(kTechnicalControls) },
    { "Sprinkle Structure",  kSprinkleStructureControls, (int) std::size(kSprinkleStructureControls) },
    { "Trigger Offset",      kOffsetControls,            (int) std::size(kOffsetControls) },
    { "Sparkle Properties",  kSparklePropertyControls,   (int) std::size(kSparklePropertyControls) },
    { "Timing",              kTimingControls,            (int) std::size(kTimingControls), 2 },
    { "Pitch",               kPitchControls,             (int) std::size(kPitchControls) },
    { "Stereo (Audio Output)", kStereoControls,          (int) std::size(kStereoControls) },
    { "Synth",               kSynthControls,              (int) std::size(kSynthControls) },
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
  // Rm/Sm modifier controls render smaller than their base, side by side with it in the same
  // cluster -- see ClusterWidth/ClusterHeight below.
  constexpr float kModW = 30.f;
  constexpr float kModH = 30.f;
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
  // real widget behind. kCompactModStyle additionally drops the value readout for the small Rm/Sm
  // knobs -- at kModW/kModH there isn't room for label + value + a clickable knob face all at once,
  // and the base control right beside it already shows a value.
  const IVStyle kCompactStyle = DEFAULT_STYLE
    .WithLabelText(DEFAULT_LABEL_TEXT.WithSize(10.f))
    .WithValueText(DEFAULT_VALUE_TEXT.WithSize(10.f));
  const IVStyle kCompactModStyle = kCompactStyle.WithShowValue(false);

  float ClusterWidth(const ParamClusterDesc& c)
  {
    float w = kCellW;
    if (c.rmParamIdx >= 0) w += kClusterGap + kModW;
    if (c.smParamIdx >= 0) w += kClusterGap + kModW;
    return w;
  }

  float ClusterHeight(const ParamClusterDesc& c)
  {
    return c.kind == EParamCtrlKind::Dropdown ? kDropdownCellH : kKnobCellH;
  }
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

  // Note-name display for the detection-range knobs, instead of a raw MIDI number (§2). Set here
  // rather than threaded through the X-macro above -- SetDisplayFunc is a one-off per param, not
  // worth a fifth macro shape for two call sites.
  GetParam(kParamMinNote)->SetDisplayFunc([](double value, WDL_String& str) {
    FormatNoteName(static_cast<int>(std::lround(value)), str);
  });
  GetParam(kParamMaxNote)->SetDisplayFunc([](double value, WDL_String& str) {
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

    const IRECT titleBounds = innerBounds.GetFromTop(18.f).GetFromLeft(110.f);
    const IRECT versionBounds = innerBounds.GetFromTop(13.f).GetFromRight(200.f);
    const IRECT contentBounds = innerBounds.GetReducedFromTop(20.f);

    // Right-hand column, fixed width: visual-indicator panel on top (a tall, narrow envelope meter
    // beside a stacked note/sprinkle-count/trigger-light readout), then the §5.1 Key/Scale quick-
    // fill pair, then the §5 note-eligibility matrix (12x12 grid + column/row toggles, see
    // ui/NoteMatrixControl.h and mNoteMatrix in Sparkles.h) -- Key/Scale are hand-placed here
    // (rather than in the flowing param-group grid below) since they act on the matrix directly.
    // Everything left of this column is the param-group flow layout below.
    const IRECT sideColumn = contentBounds.GetFromRight(210.f);
    const IRECT paramsArea = contentBounds.GetReducedFromRight(216.f);
    const IRECT visualArea = sideColumn.GetFromTop(210.f);
    const IRECT keyHeaderBounds = sideColumn.GetReducedFromTop(210.f).GetFromTop(26.f);
    const IRECT noteMatrixBounds = sideColumn.GetReducedFromTop(236.f);
    const IRECT keyRootBounds = keyHeaderBounds.GetFromLeft(keyHeaderBounds.W() * 0.5f).GetPadded(-2.f);
    const IRECT keyScaleDropdownBounds = keyHeaderBounds.GetFromRight(keyHeaderBounds.W() * 0.5f).GetPadded(-2.f);
    const IRECT meterBounds = visualArea.GetFromLeft(60.f).GetPadded(-4.f);
    const IRECT infoArea = visualArea.GetReducedFromLeft(64.f);
    const IRECT noteBounds = infoArea.GetGridCell(0, 4, 1).GetPadded(-3.f);
    const IRECT sprinkleCountBounds = infoArea.GetGridCell(1, 4, 1).GetPadded(-3.f);
    const IRECT triggerLightBounds = infoArea.GetGridCell(2, 4, 1).GetCentredInside(22.f);
    const IRECT shutUpBounds = infoArea.GetGridCell(3, 4, 1).GetPadded(-3.f);

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
    // Two passes: pass 1 walks the groups in declared order and buckets them into rows using their
    // nominal (unstretched) width -- estimated from each group's widest cluster times its column
    // count, since real per-cluster widths aren't known until a group's final rect is chosen below.
    // Pass 2 then widens every row to actually fill paramsArea's width -- leftover space is split
    // evenly across that row's groups (capped by kMaxExtraPerGroupW) instead of being left as dead
    // space on the right. A group given more than its nominal width can only fit as many or more
    // clusters per row than the nominal estimate assumed, so the real per-group content never grows
    // taller than the nominal estimate computed here.
    constexpr float kGroupGap = 6.f;

    struct FlatCtrl
    {
      IRECT rect;
      int paramIdx;
      const char* label;
      EParamCtrlKind kind;
      int unitParamIdx; // TimeKnob only, else -1
      bool isModifier = false; // Rm/Sm control -- picks kCompactModStyle over kCompactStyle
    };

    std::vector<float> nominalW(kNumParamGroups), nominalH(kNumParamGroups);
    std::vector<std::vector<int>> rows;
    rows.reserve(kNumParamGroups);

    {
      std::vector<int> currentRow;
      float rowW = 0.f;
      for (int g = 0; g < kNumParamGroups; g++) {
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

    std::vector<IRECT> groupBounds(kNumParamGroups);
    std::vector<FlatCtrl> flatControls;
    flatControls.reserve(96);

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
        const IRECT groupRect(x, rowY, x + w, rowY + h);
        groupBounds[g] = groupRect;
        x += w + kGroupGap;
        rowH = std::max(rowH, h);

        // Flow clusters left-to-right within the group's real content width, wrapping to a new
        // row when the next cluster wouldn't fit -- mirrors the group-row-wrap above, one level
        // down. Each cluster contributes a base control, then (if present) a smaller Rm and Sm
        // control immediately to its right.
        const IRECT groupContent = groupRect.GetPadded(-8.f, -22.f, -8.f, -8.f); // room for the group's own label
        const ParamGroupDesc& group = kParamGroups[g];

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
          flatControls.push_back(FlatCtrl{ clusterRect.GetFromLeft(kCellW).GetPadded(-2.f), cluster.paramIdx, cluster.label, cluster.kind, cluster.unitParamIdx });

          // "Rm"/"Sm" rather than repeating the base param's (often much longer) label -- the base
          // control right beside it already names the property, and the full label has no chance
          // of fitting legibly in a ~30px modifier knob.
          float modX = cx + kCellW + kClusterGap;
          if (cluster.rmParamIdx >= 0) {
            const IRECT modRect(modX, clusterRect.MH() - kModH * 0.5f, modX + kModW, clusterRect.MH() + kModH * 0.5f);
            flatControls.push_back(FlatCtrl{ modRect.GetPadded(-1.f), cluster.rmParamIdx, "Rm", cluster.rmKind, -1, true });
            modX += kModW + kClusterGap;
          }
          if (cluster.smParamIdx >= 0) {
            const IRECT modRect(modX, clusterRect.MH() - kModH * 0.5f, modX + kModW, clusterRect.MH() + kModH * 0.5f);
            flatControls.push_back(FlatCtrl{ modRect.GetPadded(-1.f), cluster.smParamIdx, "Sm", EParamCtrlKind::Knob, -1, true });
          }

          cx += clusterW + kClusterGap;
          clusterRowH = std::max(clusterRowH, clusterH);
        }
      }
      rowY += rowH + kGroupGap;
    }

    if (pGraphics->NControls()) {
      pGraphics->GetBackgroundControl()->SetTargetAndDrawRECTs(bounds);
      pGraphics->GetControlWithTag(kCtrlTagNoteBars)->SetTargetAndDrawRECTs(bounds);
      pGraphics->GetControlWithTag(kCtrlTagTitle)->SetTargetAndDrawRECTs(titleBounds);
      pGraphics->GetControlWithTag(kCtrlTagVersionNumber)->SetTargetAndDrawRECTs(versionBounds);
      pGraphics->GetControlWithTag(kCtrlTagEnvelopeMeter)->SetTargetAndDrawRECTs(meterBounds);
      pGraphics->GetControlWithTag(kCtrlTagNoteDisplay)->SetTargetAndDrawRECTs(noteBounds);
      pGraphics->GetControlWithTag(kCtrlTagSprinkleCount)->SetTargetAndDrawRECTs(sprinkleCountBounds);
      pGraphics->GetControlWithTag(kCtrlTagTriggerLight)->SetTargetAndDrawRECTs(triggerLightBounds);
      pGraphics->GetControlWithTag(kCtrlTagShutUp)->SetTargetAndDrawRECTs(shutUpBounds);
      pGraphics->GetControlWithTag(kCtrlTagKeyRoot)->SetTargetAndDrawRECTs(keyRootBounds);
      pGraphics->GetControlWithTag(kCtrlTagKeyScale)->SetTargetAndDrawRECTs(keyScaleDropdownBounds);
      pGraphics->GetControlWithTag(kCtrlTagNoteMatrix)->SetTargetAndDrawRECTs(noteMatrixBounds);

      for (int i = 0; i < (int) flatControls.size(); i++)
        pGraphics->GetControlWithTag(kCtrlTagFirstParamControl + i)->SetTargetAndDrawRECTs(flatControls[i].rect);
      for (int g = 0; g < kNumParamGroups; g++)
        pGraphics->GetControlWithTag(kCtrlTagFirstParamControl + (int) flatControls.size() + g)->SetTargetAndDrawRECTs(groupBounds[g]);
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
    pGraphics->AttachControl(new NoteMatrixControl(noteMatrixBounds, &mNoteMatrix), kCtrlTagNoteMatrix);

    for (int i = 0; i < (int) flatControls.size(); i++) {
      const FlatCtrl& ctrl = flatControls[i];
      const int tag = kCtrlTagFirstParamControl + i;
      switch (ctrl.kind) {
        case EParamCtrlKind::Knob:
          pGraphics->AttachControl(new IVKnobControl(ctrl.rect, ctrl.paramIdx, ctrl.label, ctrl.isModifier ? kCompactModStyle : kCompactStyle), tag);
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
  };
#endif
}

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

void Sparkles::HandleMidiTrigger(const IMidiMsg& msg, int64_t triggerSample,
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
      FireSprinkle(note, triggerSample, sparkleParams, outputMode, bpm, sampleRate);
  }
  else { // isNoteOff -- gate on the velocity the note was struck with, not the note-off's own byte.
    const int heldVelocity = mHeldNoteVelocity[note];
    mHeldNoteVelocity[note] = -1;
    const bool fireDown = triggerType == sparkle_core::TriggerType::Down || triggerType == sparkle_core::TriggerType::Both;
    if (fireDown && heldVelocity >= detection.minVelocity)
      FireSprinkle(note, triggerSample, sparkleParams, outputMode, bpm, sampleRate);
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
  else if (paramIdx == kParamKeyRoot || paramIdx == kParamKeyScale) {
    // §5.1 quick-fill: regenerates the whole note matrix from scratch, discarding any hand-edits
    // made since the last key/scale change (see mNoteMatrix's comment in Sparkles.h).
    const int keyRoot = GetParam(kParamKeyRoot)->Int();
    const auto keyScale = static_cast<sparkle_core::Scale>(GetParam(kParamKeyScale)->Int());
    if (keyRoot == sparkle_core::kNumPitchClasses) // "Trigger Note", see params/ParamList.h
      sparkle_core::ApplyKeyScalePerColumn(mNoteMatrix, keyScale);
    else
      sparkle_core::ApplyKeyScale(mNoteMatrix, keyRoot, keyScale);
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

void Sparkles::FireSprinkle(int triggerNote, int64_t triggerSample, const sparkle_core::SparkleParams& params,
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

  sparkle_core::SparkleGenerator::Generate(mNoteMatrix, params, triggerNote, bpm, sampleRate, mScratchEvents);

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

  mPitchTracker.SetConfidenceThreshold(snapshot.detection.confidence);
  const int64_t triggerTimeoutSamples = static_cast<int64_t>(std::llround(kTriggerTimeoutSeconds * sampleRate));

  // Whether the audio envelope path below is allowed to arm/fire triggers this block -- Detection
  // Mode gates it the same way it gates HandleMidiTrigger's MIDI path (see that function).
  const bool audioTriggerEnabled = snapshot.detection.detectionMode != sparkle_core::DetectionMode::Midi;

  for (int s = 0; s < nFrames; s++) {
    // Drain every MIDI message queued (by ProcessMidiMsg) for this exact sample offset before
    // doing anything else this sample -- keeps MIDI-triggered sprinkles sample-accurate rather
    // than all landing at block start.
    while (!mMidiQueue.Empty()) {
      const IMidiMsg& msg = mMidiQueue.Peek();
      if (msg.mOffset > s)
        break;
      HandleMidiTrigger(msg, blockStart + s, snapshot.detection, snapshot.sparkle, snapshot.outputMode, bpm, sampleRate);
      mMidiQueue.Remove();
    }

    // Fed unconditionally (0 when no input connected) so the tracker's clock never drifts from
    // the sample count -- mTriggerArmTime/mTriggerDeadline below are on that clock.
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

    if (!audioTriggerEnabled) {
      // Detection Mode is MIDI-only -- don't arm/fire from the envelope. Drop (rather than leave
      // stranded) a trigger that was already armed before a mode switch landed mid-block.
      mTriggerPending = false;
    }
    else if (mTriggerPending) {
      // Armed up-crossing: fire the moment the tracker has a confident hop at/after arming.
      // The one-hop slack means a note that was already confidently sounding when the envelope
      // crossed (e.g. a swell on a sustained note) fires immediately instead of waiting for the
      // next analysis hop.
      if (mPitchTracker.HasConfidentNote() &&
          mPitchTracker.LastConfidentTime() + sparkle_core::PitchTracker::kHopSamples >= mTriggerArmTime) {
        mTriggerPending = false;
        FireSprinkle(mPitchTracker.LastConfidentNote(), blockStart + s, snapshot.sparkle, snapshot.outputMode, bpm, sampleRate);
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
          FireSprinkle(mPitchTracker.LastConfidentNote(), blockStart + s, snapshot.sparkle, snapshot.outputMode, bpm, sampleRate);
      }
      else if (fireUp) {
        // The note is just starting -- the analysis buffer is mid-transient, so defer to the
        // adaptive wait above rather than trusting (or fabricating) a pitch right now.
        mTriggerPending = true;
        mTriggerArmTime = mPitchTracker.Now();
        mTriggerDeadline = mTriggerArmTime + triggerTimeoutSamples;
      }
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
