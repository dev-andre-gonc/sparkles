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
  // UI presentation table for every param in params/ParamList.h (minus the disabled panning block,
  // which has no live IParams -- see that file's comment), grouped and labelled for mLayoutFunc.
  // This grouping is a presentation-layer decision independent of the DSP param table itself, so
  // it's hand-maintained here rather than macro-generated from ParamList.h -- same rationale as
  // params/ParamSnapshot.h's per-field mapping (see that file's header comment).
  //
  // Every §7 "chain" section in ParamList.h (structure/properties/timing/pitch) has a base param
  // plus one or two "_rm"/"_sm" multipliers (see docs/SPEC.md §7.3-7.5: `_rm` scales the value per
  // ray, `_sm` scales it per sparkle within a ray). Rather than keep each multiplier tucked next to
  // its base param (which forced every label to spell out "X Rm"/"X Sm" to stay unambiguous), all
  // `_rm` params live in one "Ray Multipliers" group and all `_sm` params in one "Sparkle
  // Multipliers" group -- the group name says "multiplier" once, so each control just needs its
  // base param's short name.
  enum class EParamCtrlKind { Knob, Dropdown };

  struct ParamCtrlDesc
  {
    int paramIdx;
    const char* label;
    EParamCtrlKind kind;
  };

  struct ParamGroupDesc
  {
    const char* name;
    const ParamCtrlDesc* controls;
    int numControls;
    int preferredCols = 0; // 0 = auto (min(numControls, 3)); set explicitly where pairing matters
  };

  constexpr ParamCtrlDesc kOutputControls[] = {
    { kParamGain, "Passthrough", EParamCtrlKind::Knob },
  };

  constexpr ParamCtrlDesc kDetectionControls[] = {
    { kParamDetectionMode,"Detect",       EParamCtrlKind::Dropdown },
    { kParamTriggerType,  "Trigger",      EParamCtrlKind::Dropdown },
    { kParamThreshold,    "Threshold",    EParamCtrlKind::Knob },
    { kParamMinVelocity,  "Min Velocity", EParamCtrlKind::Knob },
    { kParamReactiveness, "Reactiveness", EParamCtrlKind::Knob },
    { kParamConfidence,   "Confidence",   EParamCtrlKind::Knob },
    { kParamMinNote,      "Min Note",     EParamCtrlKind::Knob },
    { kParamMaxNote,      "Max Note",     EParamCtrlKind::Knob },
  };

  constexpr ParamCtrlDesc kKeyScaleControls[] = {
    { kParamKeyRoot,  "Root",  EParamCtrlKind::Dropdown },
    { kParamKeyScale, "Scale", EParamCtrlKind::Dropdown },
  };

  constexpr ParamCtrlDesc kStructureControls[] = {
    { kParamNRays,           "Rays",         EParamCtrlKind::Knob },
    { kParamNSparklesPerRay, "Sparkles/Ray", EParamCtrlKind::Knob },
    { kParamRangeMin,        "Range Min",    EParamCtrlKind::Knob },
    { kParamRangeMax,        "Range Max",    EParamCtrlKind::Knob },
    { kParamWrapMode,        "Wrap",         EParamCtrlKind::Dropdown },
  };

  constexpr ParamCtrlDesc kOffsetControls[] = {
    { kParamPreDelay,     "Pre Delay",    EParamCtrlKind::Knob },
    { kParamPreDelayUnit, "Delay Unit",   EParamCtrlKind::Dropdown },
    { kParamPreInterval,  "Pre Interval", EParamCtrlKind::Knob },
  };

  constexpr ParamCtrlDesc kSparklePropertyControls[] = {
    { kParamLoudness,     "Velocity",     EParamCtrlKind::Knob },
    { kParamDuration,     "Duration",     EParamCtrlKind::Knob },
    { kParamDurationUnit, "Duration Unit",EParamCtrlKind::Dropdown },
  };

  // Two Rate-Delay-Unit pairs in one group -- kTimingGroupCols below forces a 2-column sub-grid so
  // each unit dropdown sits directly under its own delay knob instead of getting shuffled apart by
  // the default 3-column packing.
  constexpr ParamCtrlDesc kTimingControls[] = {
    { kParamRayDelay,     "Ray Delay",      EParamCtrlKind::Knob },
    { kParamRayDelayUnit, "Ray Delay Unit", EParamCtrlKind::Dropdown },
    { kParamDelay,        "Delay",          EParamCtrlKind::Knob },
    { kParamDelayUnit,    "Delay Unit",     EParamCtrlKind::Dropdown },
  };

  constexpr ParamCtrlDesc kPitchControls[] = {
    { kParamRayInterval, "Ray Interval", EParamCtrlKind::Knob },
    { kParamInterval,    "Interval",     EParamCtrlKind::Knob },
  };

  // §7.1/7.3/7.4/7.5's "_rm" params -- see the header comment above for why these are pooled here
  // instead of living beside each base param.
  constexpr ParamCtrlDesc kRayMultiplierControls[] = {
    { kParamNSparklesPerRayRm, "Sparkles/Ray", EParamCtrlKind::Knob },
    { kParamLoudnessRm,        "Velocity",     EParamCtrlKind::Knob },
    { kParamDurationRm,        "Duration",     EParamCtrlKind::Knob },
    { kParamRayDelayRm,        "Ray Delay",    EParamCtrlKind::Knob },
    { kParamDelayRm,           "Delay",        EParamCtrlKind::Knob },
    { kParamRayIntervalRm,     "Ray Interval", EParamCtrlKind::Knob },
    { kParamIntervalRm,        "Interval",     EParamCtrlKind::Knob },
  };

  // §7.3/7.4/7.5's "_sm" params -- see the header comment above.
  constexpr ParamCtrlDesc kSparkleMultiplierControls[] = {
    { kParamLoudnessSm, "Velocity", EParamCtrlKind::Knob },
    { kParamDurationSm, "Duration", EParamCtrlKind::Knob },
    { kParamDelaySm,    "Delay",    EParamCtrlKind::Knob },
    { kParamIntervalSm, "Interval", EParamCtrlKind::Knob },
  };

  constexpr ParamGroupDesc kParamGroups[] = {
    { "Output",             kOutputControls,           (int) std::size(kOutputControls) },
    { "Detection",          kDetectionControls,        (int) std::size(kDetectionControls) },
    { "Key + Scale",        kKeyScaleControls,         (int) std::size(kKeyScaleControls) },
    { "Structure",          kStructureControls,        (int) std::size(kStructureControls) },
    { "Trigger Offset",     kOffsetControls,           (int) std::size(kOffsetControls) },
    { "Sparkle Properties", kSparklePropertyControls,  (int) std::size(kSparklePropertyControls) },
    { "Timing Chain",       kTimingControls,           (int) std::size(kTimingControls), 2 },
    { "Pitch Chain",        kPitchControls,            (int) std::size(kPitchControls) },
    { "Ray Multipliers",    kRayMultiplierControls,    (int) std::size(kRayMultiplierControls) },
    { "Sparkle Multipliers",kSparkleMultiplierControls,(int) std::size(kSparkleMultiplierControls) },
  };

  constexpr int kNumParamGroups = (int) std::size(kParamGroups);

  // Per-control cell size used by mLayoutFunc's group-flow layout below. Knob cells are taller
  // than dropdown cells since a knob needs room for its widget plus label plus value text above
  // and below it -- cropping those was the whole reason the fixed-outer-grid layout got replaced
  // with this flow-packed one. Kept small on purpose -- each row is then stretched to fill any
  // leftover width (see kMaxExtraPerGroupW below), so these are a floor, not the final size.
  constexpr float kCellW = 110.f;
  constexpr float kKnobCellH = 82.f;
  constexpr float kDropdownCellH = 34.f;
  // Cap on how much extra width a single group can be stretched by when justifying a row to fill
  // paramsArea -- without this, a row with only one or two small groups (e.g. Ray Multipliers
  // alone on its row) would balloon its knobs to an ungainly size on a wide window.
  constexpr float kMaxExtraPerGroupW = 90.f;
#endif
}

Sparkles::Sparkles(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  // Generated from params/ParamList.h -- see that file's header comment for the macro contract.
#define SPARKLE_PARAM_DOUBLE(id, name, defaultVal, minVal, maxVal, step, label) \
  GetParam(id)->InitDouble(name, defaultVal, minVal, maxVal, step, label);
#define SPARKLE_PARAM_INT(id, name, defaultVal, minVal, maxVal, label) \
  GetParam(id)->InitInt(name, defaultVal, minVal, maxVal, label);
#define SPARKLE_PARAM_ENUM(id, name, defaultIdx, ...) \
  GetParam(id)->InitEnum(name, defaultIdx, { __VA_ARGS__ });
#include "params/ParamList.h"

#if IPLUG_EDITOR // http://bit.ly/2S64BDd
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS);
  };
  
  mLayoutFunc = [&](IGraphics* pGraphics) {
    const IRECT bounds = pGraphics->GetBounds();
    const IRECT innerBounds = bounds.GetPadded(-10.f);

    const IRECT titleBounds = innerBounds.GetFromTop(40.f).GetFromLeft(220.f);
    const IRECT versionBounds = innerBounds.GetFromTop(20.f).GetFromRight(300.f);
    const IRECT contentBounds = innerBounds.GetReducedFromTop(45.f);

    // Right-hand column, fixed width: visual-indicator panel on top (a tall, narrow envelope meter
    // beside a stacked note/sprinkle-count/trigger-light readout), and below it the §5 note-
    // eligibility matrix (12x12 grid + column/row toggles, see ui/NoteMatrixControl.h and
    // mNoteMatrix in Sparkles.h). Everything left of this column is the param-group flow layout
    // below.
    const IRECT sideColumn = contentBounds.GetFromRight(320.f);
    const IRECT paramsArea = contentBounds.GetReducedFromRight(330.f);
    const IRECT visualArea = sideColumn.GetFromTop(340.f);
    const IRECT noteMatrixBounds = sideColumn.GetReducedFromTop(350.f);
    const IRECT meterBounds = visualArea.GetFromLeft(110.f).GetPadded(-6.f);
    const IRECT infoArea = visualArea.GetReducedFromLeft(116.f);
    const IRECT noteBounds = infoArea.GetGridCell(0, 4, 1).GetPadded(-5.f);
    const IRECT sprinkleCountBounds = infoArea.GetGridCell(1, 4, 1).GetPadded(-5.f);
    const IRECT triggerLightBounds = infoArea.GetGridCell(2, 4, 1).GetCentredInside(36.f);
    const IRECT shutUpBounds = infoArea.GetGridCell(3, 4, 1).GetPadded(-5.f);

    // One IRECT per param control (in kParamGroups' group/table order) and one per group's labelled
    // frame, computed once here so both the resize branch and the initial-attach branch below stay
    // fed by the same layout math -- see CLAUDE.md's "How mLayoutFunc is structured". Groups are
    // shelf-packed left to right, wrapping to a new row when they'd overflow paramsArea's width,
    // rather than forced into a fixed outer grid -- a uniform grid either wasted space on small
    // groups or squeezed big ones down to where their controls' labels got cropped. Each group's own
    // controls still tile a simple sub-grid sized to its control count and kind (see kCellW/
    // kKnobCellH/kDropdownCellH above).
    //
    // Two passes: pass 1 walks the groups in declared order and buckets them into rows using their
    // nominal (unstretched) width, exactly like the old single-pass version did. Pass 2 then widens
    // every row to actually fill paramsArea's width -- leftover space is split evenly across that
    // row's groups (capped by kMaxExtraPerGroupW) instead of being left as dead space on the right,
    // which is what made the layout look unresponsive to resizing/widening the window. Because
    // GetGridCell() re-divides whatever rect it's given, widening a group's rect automatically grows
    // its controls too -- no per-control size logic needed here.
    constexpr float kGroupGap = 10.f;

    std::vector<float> nominalW(kNumParamGroups), nominalH(kNumParamGroups);
    std::vector<int> groupCols(kNumParamGroups), groupRows(kNumParamGroups);
    std::vector<std::vector<int>> rows;
    rows.reserve(kNumParamGroups);

    {
      std::vector<int> currentRow;
      float rowW = 0.f;
      for (int g = 0; g < kNumParamGroups; g++) {
        const ParamGroupDesc& group = kParamGroups[g];
        const bool hasKnob = std::any_of(group.controls, group.controls + group.numControls,
          [](const ParamCtrlDesc& d) { return d.kind == EParamCtrlKind::Knob; });
        const float cellH = hasKnob ? kKnobCellH : kDropdownCellH;

        const int cols = group.preferredCols > 0 ? group.preferredCols : std::min(group.numControls, 3);
        const int gRows = (group.numControls + cols - 1) / cols;
        groupCols[g] = cols;
        groupRows[g] = gRows;
        nominalW[g] = cols * kCellW + 16.f;
        nominalH[g] = gRows * cellH + 26.f + 16.f; // + label offset + top/bottom padding

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
    std::vector<IRECT> controlBounds;
    controlBounds.reserve(64);

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

        const int cols = groupCols[g];
        const int gRows = groupRows[g];
        const IRECT groupContent = groupRect.GetPadded(-8.f, -22.f, -8.f, -8.f); // room for the group's own label
        const ParamGroupDesc& group = kParamGroups[g];
        for (int c = 0; c < group.numControls; c++) {
          controlBounds.push_back(groupContent.GetGridCell(c, gRows, cols).GetPadded(-4.f));
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
      pGraphics->GetControlWithTag(kCtrlTagNoteMatrix)->SetTargetAndDrawRECTs(noteMatrixBounds);

      for (int i = 0; i < (int) controlBounds.size(); i++)
        pGraphics->GetControlWithTag(kCtrlTagFirstParamControl + i)->SetTargetAndDrawRECTs(controlBounds[i]);
      for (int g = 0; g < kNumParamGroups; g++)
        pGraphics->GetControlWithTag(kCtrlTagFirstParamControl + (int) controlBounds.size() + g)->SetTargetAndDrawRECTs(groupBounds[g]);
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

    pGraphics->AttachControl(new ITextControl(titleBounds, "Sparkles", IText(30)), kCtrlTagTitle);
    WDL_String buildInfoStr;
    GetBuildInfoStr(buildInfoStr, __DATE__, __TIME__);
    pGraphics->AttachControl(new ITextControl(versionBounds, buildInfoStr.Get(), DEFAULT_TEXT.WithAlign(EAlign::Far)), kCtrlTagVersionNumber);

    pGraphics->AttachControl(new EnvelopeMeterControl(meterBounds, kParamThreshold), kCtrlTagEnvelopeMeter);
    pGraphics->AttachControl(new ValueDisplayControl<2>(noteBounds, "--", IText(28), [](const std::array<float, 2>& vals, WDL_String& str) {
      const int note = static_cast<int>(std::lround(vals[0]));
      if (note < 0 || vals[1] < kMinDisplayConfidence) {
        str.Set("--");
        return;
      }
      WDL_String noteName;
      FormatNoteName(note, noteName);
      str.SetFormatted(16, "%s %d%%", noteName.Get(), static_cast<int>(std::lround(vals[1] * 100.f)));
    }), kCtrlTagNoteDisplay);
    pGraphics->AttachControl(new ValueDisplayControl<1>(sprinkleCountBounds, "0 sprinkles", IText(18), [](const std::array<float, 1>& vals, WDL_String& str) {
      str.SetFormatted(32, "%d sprinkles", static_cast<int>(std::lround(vals[0])));
    }), kCtrlTagSprinkleCount);
    pGraphics->AttachControl(new TriggerLightControl(triggerLightBounds), kCtrlTagTriggerLight);
    pGraphics->AttachControl(new IVButtonControl(shutUpBounds, [&](IControl* pCaller) {
      SplashClickActionFunc(pCaller);
      mShutUpRequested.store(true, std::memory_order_release);
    }, "Shut Up"), kCtrlTagShutUp);
    pGraphics->AttachControl(new NoteMatrixControl(noteMatrixBounds, &mNoteMatrix), kCtrlTagNoteMatrix);

    int ctrlIndex = 0;
    for (int g = 0; g < kNumParamGroups; g++) {
      const ParamGroupDesc& group = kParamGroups[g];
      for (int c = 0; c < group.numControls; c++, ctrlIndex++) {
        const ParamCtrlDesc& desc = group.controls[c];
        const int tag = kCtrlTagFirstParamControl + ctrlIndex;
        const IRECT& rect = controlBounds[ctrlIndex];
        if (desc.kind == EParamCtrlKind::Knob)
          pGraphics->AttachControl(new IVKnobControl(rect, desc.paramIdx, desc.label), tag);
        else
          pGraphics->AttachControl(new IVMenuButtonControl(rect, desc.paramIdx, desc.label), tag);
      }
      pGraphics->AttachControl(new IVGroupControl(groupBounds[g], group.name), kCtrlTagFirstParamControl + (int) controlBounds.size() + g);
    }
  };
#endif
}

#if IPLUG_DSP
void Sparkles::OnReset()
{
  mPitchTracker.Reset();
  ConfigurePitchTracker();
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
                                  const sparkle_core::SparkleParams& sparkleParams, double bpm, double sampleRate)
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
      FireSprinkle(note, triggerSample, sparkleParams, bpm, sampleRate);
  }
  else { // isNoteOff -- gate on the velocity the note was struck with, not the note-off's own byte.
    const int heldVelocity = mHeldNoteVelocity[note];
    mHeldNoteVelocity[note] = -1;
    const bool fireDown = triggerType == sparkle_core::TriggerType::Down || triggerType == sparkle_core::TriggerType::Both;
    if (fireDown && heldVelocity >= detection.minVelocity)
      FireSprinkle(note, triggerSample, sparkleParams, bpm, sampleRate);
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

void Sparkles::ConfigurePitchTracker()
{
  mPitchTracker.Configure(
    GetSampleRate(), GetParam(kParamMinNote)->Int(), GetParam(kParamMaxNote)->Int(), kNoteHoldSeconds);
}

void Sparkles::FireSprinkle(int triggerNote, int64_t triggerSample, const sparkle_core::SparkleParams& params,
                            double bpm, double sampleRate)
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

  int64_t sprinkleEndSample = triggerSample;
  for (const auto& event : mScratchEvents) {
    mEventScheduler.Schedule(
      event.note, event.velocity, event.durationSamples, triggerSample + event.timeOffsetSamples);
    sprinkleEndSample =
      std::max(sprinkleEndSample, triggerSample + event.timeOffsetSamples + event.durationSamples);
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
      HandleMidiTrigger(msg, blockStart + s, snapshot.detection, snapshot.sparkle, bpm, sampleRate);
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
        FireSprinkle(mPitchTracker.LastConfidentNote(), blockStart + s, snapshot.sparkle, bpm, sampleRate);
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
          FireSprinkle(mPitchTracker.LastConfidentNote(), blockStart + s, snapshot.sparkle, bpm, sampleRate);
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
}
#endif
