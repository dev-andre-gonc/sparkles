#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include <array>

const int kNumPresets = 1;

enum EParams
{
  kParamGain = 0,
  kParamThreshold,
  kParamMinNote,
  kParamMaxNote,
  kNumParams
};

enum ECtrlTags
{
  kCtrlTagVersionNumber = 0,
  kCtrlTagSlider,
  kCtrlTagThresholdSlider,
  kCtrlTagMinNoteSlider,
  kCtrlTagMaxNoteSlider,
  kCtrlTagTitle
};

using namespace iplug;
using namespace igraphics;

class Sparkles final : public Plugin
{
public:
  Sparkles(const InstanceInfo& info);

#if IPLUG_EDITOR
  bool OnHostRequestingSupportedViewConfiguration(int width, int height) override { return true; }
#endif
  
#if IPLUG_DSP // http://bit.ly/2S64BDd
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnParamChange(int paramIdx) override;

private:
  static constexpr int kPitchBufferSize = 2048; // must be a power of two, see kPitchBufferMask
  static constexpr int kPitchBufferMask = kPitchBufferSize - 1;
  static constexpr double kMinPitchConfidence = 0.5; // normalized autocorrelation threshold
  static constexpr int kSamplesPerPitchStep = 64;     // throttles analysis cost independent of host block size

  // MIDI note 60 = C4 (middle C). Trigger range is restricted to musical notes so the pitch
  // tracker only has to check a couple dozen candidates instead of every possible frequency.
  static constexpr int kMinTriggerableNote = 24; // C1
  static constexpr int kMaxTriggerableNote = 108; // C8
  static constexpr int kDefaultMinTriggerNote = 48; // C3
  static constexpr int kDefaultMaxTriggerNote = 84; // C6
  static constexpr int kMaxNoteCandidates = kMaxTriggerableNote - kMinTriggerableNote + 1;

  // Recomputes the candidate note/lag table (and resets their scores) from the current
  // Min/Max Note params and sample rate. Called on reset and whenever those params change.
  void RebuildNoteCandidates();

  // Evaluates one candidate note's autocorrelation lag against the pitch buffer's current
  // contents and stores its score. Called a bounded number of times per ProcessBlock (see
  // kSamplesPerPitchStep) so the analysis cost is spread evenly over time - each note's score
  // is simply left in place between updates, ready to be read at trigger time.
  void StepPitchDetector();

  bool mGateNoteActive = false;
  int mSamplesUntilNoteOff = 0;
  int mActiveNoteNumber = -1; // note number sent for the currently sounding gate note

  std::array<float, kPitchBufferSize> mPitchBuffer{};
  int mPitchBufferPos = 0;

  std::array<int, kMaxNoteCandidates> mNoteCandidateMidi{};
  std::array<int, kMaxNoteCandidates> mNoteCandidateLag{};
  std::array<double, kMaxNoteCandidates> mNoteScores{};
  int mNumNoteCandidates = 0;
  int mNoteIndex = 0;
  int mPitchStepAccumulator = 0;
#endif
};
