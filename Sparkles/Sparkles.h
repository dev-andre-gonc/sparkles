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

  // MIDI note 60 = C4 (middle C). Trigger range is restricted to musical notes so the pitch
  // tracker only has to check a couple dozen candidates instead of every possible frequency.
  static constexpr int kMinTriggerableNote = 24; // C1
  static constexpr int kMaxTriggerableNote = 108; // C8
  static constexpr int kDefaultMinTriggerNote = 48; // C3
  static constexpr int kDefaultMaxTriggerNote = 84; // C6
  static constexpr int kMaxNoteCandidates = kMaxTriggerableNote - kMinTriggerableNote + 1;

  // Recomputes the candidate note/lag table from the current Min/Max Note params and sample
  // rate. Called on reset and whenever those params change.
  void RebuildNoteCandidates();

  // Scores one candidate lag via normalized autocorrelation against the pitch buffer's
  // current contents. Higher is a stronger periodicity match at that lag.
  double ScoreNoteCandidate(int lag) const;

  // Scores every candidate against the pitch buffer's current contents and returns the index
  // of the best match, or -1 if none clears kMinPitchConfidence. Only called once per note
  // onset (see mPendingNoteOn below), so an exhaustive scan is cheap enough to not need
  // throttling.
  int FindBestNoteCandidate() const;

  bool mGateNoteActive = false;
  int mSamplesUntilNoteOff = 0;
  int mActiveNoteNumber = -1; // note number sent for the currently sounding gate note

  // Set when the amplitude threshold is crossed. The pitch decision is deferred rather than
  // made immediately: the pitch buffer still holds pre-onset (e.g. silent) audio at the exact
  // moment of crossing, so scoring it right away would misdetect. mSamplesUntilNoteDecision
  // counts down kPitchBufferSize samples so the whole analysis window has been overwritten by
  // the new note's audio before FindBestNoteCandidate() runs.
  bool mPendingNoteOn = false;
  int mSamplesUntilNoteDecision = 0;

  std::array<float, kPitchBufferSize> mPitchBuffer{};
  int mPitchBufferPos = 0;

  std::array<int, kMaxNoteCandidates> mNoteCandidateMidi{};
  std::array<int, kMaxNoteCandidates> mNoteCandidateLag{};
  int mNumNoteCandidates = 0;
#endif
};
