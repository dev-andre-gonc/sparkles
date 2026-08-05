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

namespace
{
  constexpr int kDefaultMidiNote = 60; // Middle C (C4), used when pitch detection is not confident

  // Standard 12-TET frequency of a MIDI note number (A4 = note 69 = 440Hz).
  double NoteToFreq(int note)
  {
    return 440. * std::pow(2., (note - 69) / 12.);
  }
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
    const IRECT slidersArea = innerBounds.GetFromLeft(600);
    const IRECT sliderBounds = slidersArea.GetGridCell(0, 1, 4).GetMidVPadded(100);
    const IRECT thresholdBounds = slidersArea.GetGridCell(1, 1, 4).GetMidVPadded(100);
    const IRECT minNoteBounds = slidersArea.GetGridCell(2, 1, 4).GetMidVPadded(100);
    const IRECT maxNoteBounds = slidersArea.GetGridCell(3, 1, 4).GetMidVPadded(100);
    const IRECT versionBounds = innerBounds.GetFromTRHC(300, 20);
    const IRECT titleBounds = innerBounds.GetCentredInside(200, 50);

    if (pGraphics->NControls()) {
      pGraphics->GetBackgroundControl()->SetTargetAndDrawRECTs(bounds);
      pGraphics->GetControlWithTag(kCtrlTagSlider)->SetTargetAndDrawRECTs(sliderBounds);
      pGraphics->GetControlWithTag(kCtrlTagThresholdSlider)->SetTargetAndDrawRECTs(thresholdBounds);
      pGraphics->GetControlWithTag(kCtrlTagMinNoteSlider)->SetTargetAndDrawRECTs(minNoteBounds);
      pGraphics->GetControlWithTag(kCtrlTagMaxNoteSlider)->SetTargetAndDrawRECTs(maxNoteBounds);
      pGraphics->GetControlWithTag(kCtrlTagTitle)->SetTargetAndDrawRECTs(titleBounds);
      pGraphics->GetControlWithTag(kCtrlTagVersionNumber)->SetTargetAndDrawRECTs(versionBounds);
      return;
    }

    pGraphics->SetLayoutOnResize(true);
    pGraphics->AttachCornerResizer(EUIResizerMode::Size, true);
    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->AttachPanelBackground(COLOR_LIGHT_GRAY);
    pGraphics->AttachControl(new IVSliderControl(sliderBounds, kParamGain), kCtrlTagSlider);
    pGraphics->AttachControl(new IVSliderControl(thresholdBounds, kParamThreshold), kCtrlTagThresholdSlider);
    pGraphics->AttachControl(new IVSliderControl(minNoteBounds, kParamMinNote), kCtrlTagMinNoteSlider);
    pGraphics->AttachControl(new IVSliderControl(maxNoteBounds, kParamMaxNote), kCtrlTagMaxNoteSlider);
    pGraphics->AttachControl(new ITextControl(titleBounds, "Sparkles", IText(30)), kCtrlTagTitle);
    WDL_String buildInfoStr;
    GetBuildInfoStr(buildInfoStr, __DATE__, __TIME__);
    pGraphics->AttachControl(new ITextControl(versionBounds, buildInfoStr.Get(), DEFAULT_TEXT.WithAlign(EAlign::Far)), kCtrlTagVersionNumber);
  };
#endif
}

#if IPLUG_DSP
void Sparkles::OnReset()
{
  RebuildNoteCandidates();
  mPendingNoteOn = false;
  mEventScheduler.Reset();
  mNumActiveSprinkles = 0;
  mBlockStartSample = 0;
  mEnvelope = 0.0;
}

void Sparkles::OnParamChange(int paramIdx)
{
  if (paramIdx == kParamMinNote || paramIdx == kParamMaxNote) {
    RebuildNoteCandidates();
  }
}

void Sparkles::RebuildNoteCandidates()
{
  const int minNote = std::min(GetParam(kParamMinNote)->Int(), GetParam(kParamMaxNote)->Int());
  const int maxNote = std::max(GetParam(kParamMinNote)->Int(), GetParam(kParamMaxNote)->Int());
  const double sampleRate = GetSampleRate();

  mNumNoteCandidates = 0;
  for (int note = minNote; note <= maxNote && mNumNoteCandidates < kMaxNoteCandidates; note++) {
    const int lag = static_cast<int>(std::lround(sampleRate / NoteToFreq(note)));
    if (lag < 1 || lag >= kPitchBufferSize) continue; // outside what our buffer can resolve

    mNoteCandidateMidi[mNumNoteCandidates] = note;
    mNoteCandidateLag[mNumNoteCandidates] = lag;
    mNumNoteCandidates++;
  }
}

double Sparkles::ScoreNoteCandidate(int lag) const
{
  const int n = kPitchBufferSize - lag;
  double cross = 0., energy0 = 0., energy1 = 0.;

  for (int i = 0; i < n; i++) {
    const float a = mPitchBuffer[(mPitchBufferPos + i) & kPitchBufferMask];
    const float b = mPitchBuffer[(mPitchBufferPos + i + lag) & kPitchBufferMask];
    cross += a * b;
    energy0 += a * a;
    energy1 += b * b;
  }

  const double denom = std::sqrt(energy0 * energy1);
  return denom > 0. ? cross / denom : 0.;
}

int Sparkles::FindBestNoteCandidate() const
{
  int bestIndex = -1;
  double bestScore = kMinPitchConfidence;

  for (int i = 0; i < mNumNoteCandidates; i++) {
    const double score = ScoreNoteCandidate(mNoteCandidateLag[i]);
    if (score > bestScore) {
      bestScore = score;
      bestIndex = i;
    }
  }

  return bestIndex;
}

void Sparkles::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
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

  for (int s = 0; s < nFrames; s++) {
    if (nInChans > 0) {
      const double pitchSample = nInChans > 1 ? (inputs[0][s] + inputs[1][s]) * 0.5 : inputs[0][s];
      mPitchBuffer[mPitchBufferPos] = static_cast<float>(pitchSample);
      mPitchBufferPos = (mPitchBufferPos + 1) & kPitchBufferMask;
    }

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

    if (mPendingNoteOn) {
      // Waiting for the pitch buffer to fill with the new note's audio before deciding
      // which pitch it is; see mSamplesUntilNoteDecision in Sparkles.h.
      if (--mSamplesUntilNoteDecision <= 0) {
        const int bestIndex = FindBestNoteCandidate();

        const int minNote = std::min(GetParam(kParamMinNote)->Int(), GetParam(kParamMaxNote)->Int());
        const int maxNote = std::max(GetParam(kParamMinNote)->Int(), GetParam(kParamMaxNote)->Int());
        const int fallbackNote = std::clamp(kDefaultMidiNote, minNote, maxNote);
        const int triggerNote = bestIndex >= 0 ? mNoteCandidateMidi[bestIndex] : fallbackNote;
        mPendingNoteOn = false;

        const int64_t triggerSample = blockStart + s;

        // Reap sprinkles that have finished sounding by now, so mNumActiveSprinkles reflects only
        // ones still actually in flight.
        int writeIdx = 0;
        for (int i = 0; i < mNumActiveSprinkles; i++) {
          if (mActiveSprinkleEndSamples[i] > triggerSample)
            mActiveSprinkleEndSamples[writeIdx++] = mActiveSprinkleEndSamples[i];
        }
        mNumActiveSprinkles = writeIdx;

        // At the cap -- drop this trigger's sprinkle entirely rather than truncating any one
        // sprinkle's own rays/sparkles.
        if (mNumActiveSprinkles < kMaxSimultaneousSprinkles) {
          sparkle_core::SparkleGenerator::Generate(
            mNoteMatrix, snapshot.sparkle, triggerNote, bpm, sampleRate, mScratchEvents);

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
      }
    }
    else {
      // A true crossing -- the envelope must have been on the other side of threshold on the
      // previous sample -- not just "currently above it". Without this, a sustained note whose
      // envelope sits above threshold would re-arm and refire the instant the previous decision
      // resolved (mPendingNoteOn -> false), rather than waiting for it to actually dip and cross
      // again.
      const bool crossedUp = prevEnvelope <= threshold && mEnvelope > threshold;
      const bool crossedDown = prevEnvelope >= threshold && mEnvelope < threshold;
      const bool crossed = snapshot.detection.triggerType == sparkle_core::TriggerType::Up     ? crossedUp
                            : snapshot.detection.triggerType == sparkle_core::TriggerType::Down ? crossedDown
                                                                                                  : crossedUp || crossedDown;

      if (crossed) {
        mPendingNoteOn = true;
        mSamplesUntilNoteDecision = kPitchBufferSize;
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
}
#endif
