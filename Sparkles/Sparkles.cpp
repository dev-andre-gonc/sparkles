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
  GetParam(kParamGain)->InitDouble("Gain", 0., 0., 100.0, 0.01, "%");
  GetParam(kParamThreshold)->InitDouble("Threshold", 50., 0., 100.0, 0.01, "%");
  GetParam(kParamMinNote)->InitInt("Min Note", kDefaultMinTriggerNote, kMinTriggerableNote, kMaxTriggerableNote);
  GetParam(kParamMaxNote)->InitInt("Max Note", kDefaultMaxTriggerNote, kMinTriggerableNote, kMaxTriggerableNote);

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

  for (int s = 0; s < nFrames; s++) {
    if (nInChans > 0) {
      const double pitchSample = nInChans > 1 ? (inputs[0][s] + inputs[1][s]) * 0.5 : inputs[0][s];
      mPitchBuffer[mPitchBufferPos] = static_cast<float>(pitchSample);
      mPitchBufferPos = (mPitchBufferPos + 1) & kPitchBufferMask;
    }

    if (mGateNoteActive) {
      if (--mSamplesUntilNoteOff <= 0) {
        IMidiMsg msg;
        msg.MakeNoteOffMsg(mActiveNoteNumber, s);
        SendMidiMsg(msg);
        mGateNoteActive = false;
      }
    }
    else if (mPendingNoteOn) {
      // Waiting for the pitch buffer to fill with the new note's audio before deciding
      // which pitch it is; see mSamplesUntilNoteDecision in Sparkles.h.
      if (--mSamplesUntilNoteDecision <= 0) {
        const int bestIndex = FindBestNoteCandidate();

        const int minNote = std::min(GetParam(kParamMinNote)->Int(), GetParam(kParamMaxNote)->Int());
        const int maxNote = std::max(GetParam(kParamMinNote)->Int(), GetParam(kParamMaxNote)->Int());
        const int fallbackNote = std::clamp(kDefaultMidiNote, minNote, maxNote);
        mActiveNoteNumber = bestIndex >= 0 ? mNoteCandidateMidi[bestIndex] : fallbackNote;

        IMidiMsg msg;
        msg.MakeNoteOnMsg(mActiveNoteNumber, 127, s);
        SendMidiMsg(msg);
        mPendingNoteOn = false;
        mGateNoteActive = true;
        mSamplesUntilNoteOff = static_cast<int>(GetSampleRate());
      }
    }
    else {
      double inputLevel = 0.;
      for (int c = 0; c < nInChans; c++) {
        inputLevel = std::max(inputLevel, std::abs(inputs[c][s]));
      }

      if (inputLevel > threshold) {
        mPendingNoteOn = true;
        mSamplesUntilNoteDecision = kPitchBufferSize;
      }
    }

    for (int c = 0; c < nChans; c++) {
      outputs[c][s] = inputs[c][s] * gain;
    }
  }
}
#endif
