#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "params/ParamRanges.h"
#include "core/EventScheduler.h"
#include "core/NoteMatrix.h"
#include "core/SparkleGenerator.h"
#include <array>
#include <vector>

const int kNumPresets = 1;

// EParams is generated from params/ParamList.h -- that file is the single source of truth for
// every param's id, name, range/options and default; see its header comment. Sparkles.cpp's
// constructor re-includes the same file to generate the matching InitXxx() calls.
#define SPARKLE_PARAM_DOUBLE(id, ...) id,
#define SPARKLE_PARAM_INT(id, ...) id,
#define SPARKLE_PARAM_ENUM(id, ...) id,
enum EParams
{
#include "params/ParamList.h"
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

  // Candidate-note bounds live in params/ParamRanges.h (kMinTriggerableNote/kMaxTriggerableNote)
  // since params/ParamList.h's kParamMinNote/kParamMaxNote entries need the same constants.
  static constexpr int kMaxNoteCandidates =
    sparkle_params::kMaxTriggerableNote - sparkle_params::kMinTriggerableNote + 1;

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

  // Hard ceiling on sprinkles (trigger bursts) in flight at once -- a new trigger that arrives
  // once this many are still sounding is dropped entirely, rather than truncating any one
  // sprinkle's own rays/sparkles. A constant, not a user-facing param -- it exists purely to bound
  // MIDI/voice load, not as a creative control.
  static constexpr int kMaxSimultaneousSprinkles = 10;

  // One-pole envelope follower level (§2), updated every sample: mEnvelope = mEnvelope * (1 -
  // reactiveness) + |in| * reactiveness. The threshold crossing that arms mPendingNoteOn below is
  // checked against this, not the raw instantaneous input sample.
  double mEnvelope = 0.0;

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

  // §5 note-eligibility matrix. Not persisted/quick-filled yet (see params/ParamSnapshot.h's
  // header comment) -- default-constructed, which leaves every cell/row/column enabled.
  sparkle_core::NoteMatrix mNoteMatrix;

  sparkle_core::EventScheduler<> mEventScheduler;

  // Scratch buffer for SparkleGenerator::Generate() output, reused block-to-block so its capacity
  // (reserved to SparkleGenerator::kMaxEventsPerTrigger on first use) doesn't need re-allocating
  // on every trigger.
  std::vector<sparkle_core::SparkleEvent> mScratchEvents;

  // Absolute sample position of the start of the current ProcessBlock call, counted from the last
  // OnReset() -- independent of host transport, since EventScheduler just needs a monotonically
  // increasing, gap-free position to schedule against.
  int64_t mBlockStartSample = 0;

  // One entry per in-flight sprinkle, holding the absolute sample position of its last note-off
  // (i.e. when it finishes sounding). Reaped (entries past their end sample dropped) each time a
  // new trigger arrives, so mNumActiveSprinkles reflects sprinkles still actually sounding at that
  // moment -- used to enforce kMaxSimultaneousSprinkles without needing EventScheduler to tag
  // individual note events by originating sprinkle.
  std::array<int64_t, kMaxSimultaneousSprinkles> mActiveSprinkleEndSamples{};
  int mNumActiveSprinkles = 0;
#endif
};
