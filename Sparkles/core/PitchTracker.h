#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Continuous monophonic pitch tracker (§2). Unlike the original prototype -- which ran one
// autocorrelation scan per trigger, after a fixed full-buffer wait -- this scores the input on a
// steady hop cadence regardless of trigger state, and layers three levels of state on top of the
// raw per-hop winner:
//
//   - best-of-hop: raw argmax over the candidate lags, with an octave-error guard (see
//     kOctaveTiebreak below).
//   - stable note (DisplayNote/DisplayConfidence): best-of-hop debounced by hysteresis -- a
//     challenger note must win kHysteresisHops consecutive hops to replace it. This is what the
//     UI shows continuously, trigger or no trigger.
//   - last-confident note (LastConfidentNote/Time, HasConfidentNote): the stable note at the last
//     hop where its own score cleared the caller-set confidence threshold, remembered for a hold
//     window. Down-triggers resolve from this (the note is fading/gone by the time the envelope
//     crosses downward, so "what is playing right now" is the wrong question -- "what was
//     confidently playing just before" is the right one), and up-triggers poll it to fire as soon
//     as the new note becomes confident instead of always paying a worst-case wait.
//
// Deliberately free of iPlug2/IGraphics dependencies (like core/NoteMatrix.h and
// core/SparkleGenerator.h) so it can be included by both the plugin and the standalone test binary.
namespace sparkle_core
{
  class PitchTracker
  {
  public:
    static constexpr int kBufferSize = 2048; // must be a power of two, see kBufferMask; ~46ms at 44.1k
    static constexpr int kBufferMask = kBufferSize - 1;
    static constexpr int kHopSamples = 512; // analysis cadence: ~86 updates/sec at 44.1k
    static constexpr int kMaxCandidates = 128;

    // Floor for *tracking-state* updates (adopting a stable note, counting challenger hops) --
    // deliberately distinct from and lower than the user-facing trigger-accept threshold
    // (SetConfidenceThreshold): tracking wants to follow weak-but-real pitch (release tails,
    // soft playing) that a stricter trigger threshold would rightly refuse to fire on.
    static constexpr double kMinTrackConfidence = 0.3;

    // Hop peak amplitude below this treats the whole hop as silence (~ -60 dBFS): no scoring,
    // confidence collapses, and the stable note survives only as long as the hold window.
    static constexpr double kSilenceFloor = 1e-3;

    // Autocorrelation of a periodic signal peaks at every integer multiple of the true period,
    // so a plain argmax often lands an octave (or twelfth) below the actual note. A smaller-lag
    // candidate scoring within this fraction of the best steals the win -- but ONLY if its lag is
    // harmonically related to the best lag (see kHarmonicTolerance). An earlier version accepted
    // ANY near-tie at a smaller lag, which pinned detection at the top of the detect range:
    // low-frequency/broadband energy inflates the score at every short lag at once, and the
    // smallest lag in the table is by construction the highest note (C6 with default params).
    static constexpr double kOctaveTiebreak = 0.95;

    // How far bestLag/candidateLag may sit from an exact integer, relative -- adjacent notes are
    // ~6% apart in lag, so 3% (about half a semitone) accepts only the true harmonic division.
    static constexpr double kHarmonicTolerance = 0.03;

    // Lag ratio between adjacent semitones (2^(1/12)), used for the virtual edge neighbors in
    // the local-peak test below.
    static constexpr double kSemitoneRatio = 1.0594630943592953;

    // Consecutive hops a challenger note must win before it replaces the stable note -- kills
    // flicker from vibrato, beating strings, and analysis-window boundary effects.
    static constexpr int kHysteresisHops = 3;

    // Rebuilds the candidate note/lag table for the given detect range and sets the hold window.
    // Leaves the buffer and tracking state alone (call Reset() for a full clear) -- a mid-flight
    // range change shouldn't blank the display.
    void Configure(double sampleRate, int noteMin, int noteMax, double holdSeconds)
    {
      mHoldSamples = static_cast<int64_t>(std::llround(holdSeconds * sampleRate));

      const int lo = std::min(noteMin, noteMax);
      const int hi = std::max(noteMin, noteMax);

      mNumCandidates = 0;
      for (int note = lo; note <= hi && mNumCandidates < kMaxCandidates; note++)
      {
        const int lag = static_cast<int>(std::lround(sampleRate / NoteToFreq(note)));
        if (lag < 1 || lag >= kBufferSize)
          continue; // outside what the buffer can resolve

        mCandidateMidi[mNumCandidates] = note;
        mCandidateLag[mNumCandidates] = lag;
        mNumCandidates++;
      }
    }

    void Reset()
    {
      mBuffer.fill(0.f);
      mLastScores.fill(0.f);
      mWritePos = 0;
      mNow = 0;
      mSamplesSinceHop = 0;
      mHopPeak = 0.0;
      mStableNote = -1;
      mStableScore = 0.0;
      mChallengerNote = -1;
      mChallengerHops = 0;
      mLastConfidentNote = -1;
      mLastConfidentTime = 0;
    }

    // Trigger-accept threshold (the user's Confidence param) -- gates only what counts as a
    // "confident" hop for LastConfidentNote/HasConfidentNote, never the tracking itself.
    void SetConfidenceThreshold(double threshold) { mConfidenceThreshold = threshold; }

    void Push(float sample)
    {
      mBuffer[mWritePos] = sample;
      mWritePos = (mWritePos + 1) & kBufferMask;
      mNow++;
      mHopPeak = std::max(mHopPeak, static_cast<double>(std::abs(sample)));

      if (++mSamplesSinceHop >= kHopSamples)
      {
        mSamplesSinceHop = 0;
        AnalyzeHop();
      }
    }

    // Samples pushed since Reset() -- the clock every time query below is expressed in.
    int64_t Now() const { return mNow; }

    // Live estimate for the UI: the current stable note and its own score from the most recent
    // analysis hop. -1 / 0.0 when nothing is being tracked (silence, unpitched input).
    int DisplayNote() const { return mStableNote; }
    double DisplayConfidence() const { return mStableNote >= 0 ? mStableScore : 0.0; }

    // Last stable note whose score cleared the confidence threshold, and when. HasConfidentNote()
    // is that memory's freshness gate: true while the confident hop is within the hold window.
    int LastConfidentNote() const { return mLastConfidentNote; }
    int64_t LastConfidentTime() const { return mLastConfidentTime; }

    bool HasConfidentNote() const
    {
      return mLastConfidentNote >= 0 && (mNow - mLastConfidentTime) <= mHoldSamples;
    }

    // Per-note confidence from the most recent analysis hop, clamped to [0,1], for the UI's
    // note-bars display: fills out[0..noteMax-noteMin] with one value per MIDI note. Notes with
    // no candidate (outside the configured detect range, or lag out of buffer reach) read 0, as
    // does everything after a silent hop.
    void GetNoteConfidences(int noteMin, int noteMax, float* out) const
    {
      const int n = noteMax - noteMin + 1;
      std::fill(out, out + n, 0.f);

      for (int i = 0; i < mNumCandidates; i++)
      {
        const int note = mCandidateMidi[i];
        if (note >= noteMin && note <= noteMax)
          out[note - noteMin] = mLastScores[i];
      }
    }

  private:
    // Standard 12-TET frequency of a MIDI note number (A4 = note 69 = 440Hz).
    static double NoteToFreq(int note) { return 440. * std::pow(2., (note - 69) / 12.); }

    // Normalized autocorrelation of the analysis window against itself at `lag`. Higher is a
    // stronger periodicity match at that lag. Reads mScratch, the linearized, mean-removed copy
    // AnalyzeHop prepares once per hop: without mean removal, any DC offset inflates the score at
    // every lag simultaneously (constant terms add to cross and both energies alike), which is
    // one half of the everything-near-ties failure described at kOctaveTiebreak.
    double Score(int lag) const
    {
      const int n = kBufferSize - lag;
      double cross = 0., energy0 = 0., energy1 = 0.;

      for (int i = 0; i < n; i++)
      {
        const float a = mScratch[i];
        const float b = mScratch[i + lag];
        cross += a * b;
        energy0 += a * a;
        energy1 += b * b;
      }

      const double denom = std::sqrt(energy0 * energy1);
      return denom > 0. ? cross / denom : 0.;
    }

    // Silent/unpitched hop: confidence collapses immediately, but the stable note is kept while
    // the hold window would still let a down-trigger use it -- past that it's forgotten, so the
    // next pitched hop adopts fresh instead of having to out-vote a long-dead note.
    void ForgetIfStale()
    {
      if (mStableNote >= 0 && (mLastConfidentNote < 0 || (mNow - mLastConfidentTime) > mHoldSamples))
        mStableNote = -1;

      mStableScore = 0.0;
      mChallengerNote = -1;
      mChallengerHops = 0;
    }

    void AnalyzeHop()
    {
      const double peak = mHopPeak;
      mHopPeak = 0.0;

      if (peak < kSilenceFloor)
      {
        mLastScores.fill(0.f);
        ForgetIfStale();
        return;
      }

      // Linearize the ring buffer (oldest sample first) and remove its mean, once per hop --
      // Score() then runs on contiguous, DC-free data. See Score()'s comment for why the mean
      // removal is load-bearing and not just hygiene.
      double sum = 0.0;
      for (int i = 0; i < kBufferSize; i++)
      {
        mScratch[i] = mBuffer[(mWritePos + i) & kBufferMask];
        sum += mScratch[i];
      }
      const float mean = static_cast<float>(sum / kBufferSize);
      for (int i = 0; i < kBufferSize; i++)
        mScratch[i] -= mean;

      std::array<double, kMaxCandidates> scores;
      for (int i = 0; i < mNumCandidates; i++)
      {
        scores[i] = Score(mCandidateLag[i]);
        mLastScores[i] = static_cast<float>(std::clamp(scores[i], 0.0, 1.0));
      }

      // Local-peak mask: a candidate may only compete for selection if it out-scores its
      // semitone neighbors in lag-space. Autocorrelation is structurally generous to short
      // lags -- anything with a period much longer than a lag barely changes across it, so a
      // low fundamental (or sub-range rumble) paints a smooth ramp of high scores over the top
      // octaves of the table ("wall" in the note-bars display). A real pitch is a local peak;
      // the ramp has no interior local maximum, so this mask makes it unelectable while leaving
      // fundamentals, harmonics and undertones alone. mLastScores above stays raw on purpose --
      // the bars show what the input actually correlates to, the mask only gates selection.
      // Strict > toward the lower-note side, >= toward the higher-note side, so of two adjacent
      // candidates whose lags quantized to the same integer (top notes at low sample rates)
      // exactly one can still qualify. The table edges get virtual one-semitone-out neighbors.
      std::array<bool, kMaxCandidates> isPeak;
      for (int i = 0; i < mNumCandidates; i++)
      {
        const int lag = mCandidateLag[i];

        double lowerNoteScore = -2.0; // neighbor at the next-larger lag (one note down)
        if (i > 0)
          lowerNoteScore = scores[i - 1];
        else
        {
          const int vlag = static_cast<int>(std::lround(lag * kSemitoneRatio));
          if (vlag < kBufferSize)
            lowerNoteScore = Score(vlag);
        }

        double higherNoteScore = -2.0; // neighbor at the next-smaller lag (one note up)
        if (i + 1 < mNumCandidates)
          higherNoteScore = scores[i + 1];
        else
        {
          int vlag = static_cast<int>(std::lround(lag / kSemitoneRatio));
          if (vlag >= lag)
            vlag = lag - 1; // tiny lags can round back onto themselves -- force a step
          if (vlag >= 1)
            higherNoteScore = Score(vlag);
        }

        isPeak[i] = scores[i] > lowerNoteScore && scores[i] >= higherNoteScore;
      }

      // Argmax over peak candidates only...
      int bestIdx = -1;
      for (int i = 0; i < mNumCandidates; i++)
      {
        if (isPeak[i] && (bestIdx < 0 || scores[i] > scores[bestIdx]))
          bestIdx = i;
      }

      if (bestIdx < 0 || scores[bestIdx] < kMinTrackConfidence)
      {
        ForgetIfStale();
        return;
      }

      // ...then the octave-error guard: a smaller lag may steal the win only when it's a
      // near-tie (kOctaveTiebreak) AND harmonically related to the argmax's lag -- an integer
      // division of it within kHarmonicTolerance. Unrelated short-lag ties (broadband or
      // low-frequency energy) stay losers instead of dragging detection to the range's top note.
      const double tieFloor = scores[bestIdx] * kOctaveTiebreak;
      const int bestLag = mCandidateLag[bestIdx];
      int chosenIdx = bestIdx;
      for (int i = 0; i < mNumCandidates; i++)
      {
        if (!isPeak[i] || mCandidateLag[i] >= mCandidateLag[chosenIdx] || scores[i] < tieFloor)
          continue;

        const double ratio = static_cast<double>(bestLag) / mCandidateLag[i];
        const double harmonic = std::round(ratio);
        if (harmonic < 2.0 || std::abs(ratio - harmonic) > harmonic * kHarmonicTolerance)
          continue;

        chosenIdx = i;
      }
      const int bestNote = mCandidateMidi[chosenIdx];

      // Hysteresis: from nothing, adopt immediately (the fast path a fresh note onset takes);
      // while tracking, a challenger must win kHysteresisHops consecutive hops.
      if (mStableNote < 0)
      {
        mStableNote = bestNote;
        mChallengerNote = -1;
        mChallengerHops = 0;
      }
      else if (bestNote == mStableNote)
      {
        mChallengerNote = -1;
        mChallengerHops = 0;
      }
      else
      {
        if (bestNote == mChallengerNote)
          mChallengerHops++;
        else
        {
          mChallengerNote = bestNote;
          mChallengerHops = 1;
        }

        if (mChallengerHops >= kHysteresisHops)
        {
          mStableNote = mChallengerNote;
          mChallengerNote = -1;
          mChallengerHops = 0;
        }
      }

      // Confidence reported (and confident-record gating below) is the stable note's OWN score
      // this hop -- not best-of-hop's, which belongs to a challenger during a transition.
      mStableScore = 0.0;
      for (int i = 0; i < mNumCandidates; i++)
      {
        if (mCandidateMidi[i] == mStableNote)
        {
          mStableScore = std::max(0.0, scores[i]);
          break;
        }
      }

      if (mStableScore >= mConfidenceThreshold)
      {
        mLastConfidentNote = mStableNote;
        mLastConfidentTime = mNow;
      }
    }

    std::array<float, kBufferSize> mBuffer{};
    std::array<float, kBufferSize> mScratch{}; // linearized, mean-removed copy, valid during AnalyzeHop
    std::array<float, kMaxCandidates> mLastScores{}; // per-candidate [0,1] scores from the last hop
    int mWritePos = 0;
    int64_t mNow = 0;
    int mSamplesSinceHop = 0;
    double mHopPeak = 0.0;

    std::array<int, kMaxCandidates> mCandidateMidi{};
    std::array<int, kMaxCandidates> mCandidateLag{};
    int mNumCandidates = 0;

    double mConfidenceThreshold = 0.6;
    int64_t mHoldSamples = 0;

    int mStableNote = -1;
    double mStableScore = 0.0;
    int mChallengerNote = -1;
    int mChallengerHops = 0;

    int mLastConfidentNote = -1;
    int64_t mLastConfidentTime = 0;
  };
}
