#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>

// Audio-mode synth (§7.7): a fixed-capacity polyphonic voice pool that renders sparkle events
// directly to the plugin's own stereo output, instead of (or alongside) sending them out as MIDI.
// Deliberately free of iPlug2/IGraphics dependencies (like core/SparkleGenerator.h and
// core/EventScheduler.h) so it can be included by both the plugin and the standalone test binary.
//
// Deliberately independent of core/EventScheduler.h even though both schedule-by-absolute-sample:
// EventScheduler's SchedEvent only carries note/velocity, which is all MIDI needs, but a synth
// voice also needs pan and a resolved ADSR envelope (SparkleEvent already carries both, see
// core/SparkleGenerator.h §7.6/§7.7) -- reusing EventScheduler would mean bolting those fields onto
// a type MIDI doesn't want. So this keeps its own small pending-spawn queue instead.
namespace sparkle_core
{
  // Which output path(s) a sprinkle's events are sent to. Governs routing in Sparkles::FireSprinkle
  // and Sparkles::ProcessBlock -- this engine itself doesn't read it, it just renders whatever
  // voices it's told to schedule.
  enum class OutputMode
  {
    Audio,
    Midi,
    Both
  };

  inline double MidiNoteToFrequency(int note)
  {
    return 440.0 * std::pow(2.0, (note - 69) / 12.0);
  }

  // Continuous morph across the four classic waveforms (§7.7's "Wave Shape" knob): `shape` in
  // [0,3] selects a position on Sine(0) -> Triangle(1) -> Square(2) -> Saw(3), linearly
  // crossfading the two neighbouring shapes at fractional values (e.g. 1.5 = half Triangle/half
  // Square). `phase` is periodic with period 1, same convention as SparkleGenerator's pan LFO
  // shapes, but this is a separate small function rather than a shared one -- that one switches on
  // a discrete PanMode enum for a per-sparkle *pan position* lookup, this blends continuously for a
  // *per-sample audio* oscillator, different enough signatures/call sites that sharing would cost
  // more in indirection than it saves in duplication.
  inline double BasicWaveSample(int shapeIndex, double frac)
  {
    switch (shapeIndex)
    {
      case 1: // Triangle
        return (frac <= 0.5) ? (-1.0 + 4.0 * frac) : (1.0 - 4.0 * (frac - 0.5));
      case 2: // Square
        return (frac < 0.5) ? -1.0 : 1.0;
      case 3: // Saw
        return -1.0 + 2.0 * frac;
      case 0: // Sine
      default:
      {
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        return std::sin(kTwoPi * frac);
      }
    }
  }

  inline double WaveformSample(double shape, double phase)
  {
    const double clampedShape = std::clamp(shape, 0.0, 3.0);
    const double frac = phase - std::floor(phase);

    int segment = static_cast<int>(std::floor(clampedShape));
    segment = std::clamp(segment, 0, 2); // last segment is Square(2) -> Saw(3)
    const double blend = clampedShape - segment;

    const double a = BasicWaveSample(segment, frac);
    const double b = BasicWaveSample(segment + 1, frac);
    return a + (b - a) * blend;
  }

  // Fixed-capacity polyphonic synth: pending sparkle-triggered voice spawns, scheduled by absolute
  // sample position (mirrors core/EventScheduler.h's Schedule/FlushBlock split), plus a fixed pool
  // of currently-sounding voices with oldest/quietest-first stealing once the pool is full. RT-safe
  // -- std::array storage only, no dynamic allocation anywhere.
  //
  // A voice's envelope is entirely determined by its gate length (the sparkle's own `duration`,
  // §7.3) and its attack/decay/sustain/release (§7.7): there's no separate "note off" message like
  // MIDI has -- a sparkle's lifetime is fixed at generation time, so the gate simply counts down
  // and release begins automatically once it elapses, continuing seamlessly from whatever level the
  // attack/decay phase had reached (so a gate shorter than attack+decay releases early without a
  // click, rather than waiting for attack/decay to finish).
  template <size_t MaxVoices = 64, size_t MaxPendingSpawns = 1024>
  class SynthEngine
  {
  public:
    void Reset()
    {
      mPendingCount = 0;
      for (auto& voice : mVoices)
        voice.active = false;
    }

    // Schedules a voice for `note`/`velocity` (1-127, MIDI convention like SparkleEvent::velocity)
    // at absolute sample position `atSample`, with `pan` (-1=100%L, +1=100%R, §7.6) and an
    // already-resolved ADSR envelope in samples/level (§7.7). Returns false, dropping the voice
    // entirely, if the pending-spawn pool is already at capacity -- same "must be handled, not
    // assert-worthy" contract as EventScheduler::Schedule.
    bool ScheduleVoice(int note, int velocity, double pan, int64_t attackSamples, int64_t decaySamples,
                        double sustainLevel, int64_t releaseSamples, int64_t gateSamples, int64_t atSample)
    {
      if (mPendingCount >= MaxPendingSpawns)
        return false;

      PendingSpawn spawn;
      spawn.atSample = atSample;
      spawn.note = note;
      spawn.velocityGain = std::clamp(velocity / 127.0, 0.0, 1.0);
      spawn.pan = std::clamp(pan, -1.0, 1.0);
      spawn.attackSamples = std::max<int64_t>(0, attackSamples);
      spawn.decaySamples = std::max<int64_t>(0, decaySamples);
      spawn.sustainLevel = std::clamp(sustainLevel, 0.0, 1.0);
      spawn.releaseSamples = std::max<int64_t>(0, releaseSamples);
      spawn.gateSamples = std::max<int64_t>(0, gateSamples);

      // Insertion-sort by atSample, scanning from the back -- new spawns are almost always at or
      // near the newest (largest) atSample already pending, same rationale as
      // EventScheduler::InsertSorted.
      size_t i = mPendingCount;
      while (i > 0 && mPending[i - 1].atSample > spawn.atSample)
      {
        mPending[i] = mPending[i - 1];
        --i;
      }
      mPending[i] = spawn;
      ++mPendingCount;
      return true;
    }

    // Immediately silences every active voice (no release tail) and drops every pending spawn that
    // hasn't sounded yet -- for the "Shut Up" panic button, matching the hard-stop spirit of
    // Sparkles::ShutUp's MIDI side (explicit note-offs sent immediately, not faded).
    void StopAll()
    {
      mPendingCount = 0;
      for (auto& voice : mVoices)
        voice.active = false;
    }

    // Renders this block's due voice spawns plus every already-active voice into `outputs`,
    // *adding* to whatever is already there (e.g. the dry passthrough) rather than overwriting it.
    // `nOutChans` may be 0 (nothing to render into), 1 (mono sum, no panning), or >=2 (channels 0/1
    // treated as L/R; any channel beyond 1 is left untouched). Templated on the output sample type
    // (rather than depending on iPlug2's `sample` typedef) so this stays framework-free.
    template <typename T>
    void Render(int64_t blockStart, int nFrames, double sampleRate, double waveShape, T** outputs, int nOutChans)
    {
      if (nOutChans <= 0 || nFrames <= 0)
      {
        // Still due to consume this block's pending spawns so they don't pile up waiting for a
        // future call that increases nOutChans -- FlushBlock-style, drain by time regardless.
        DrainDueSpawns(blockStart, nFrames, sampleRate);
        return;
      }

      DrainDueSpawns(blockStart, nFrames, sampleRate);

      const double phaseIncrementScale = 1.0 / sampleRate;

      for (auto& voice : mVoices)
      {
        if (!voice.active)
          continue;

        for (int s = 0; s < nFrames; ++s)
        {
          if (voice.samplesUntilStart > 0)
          {
            --voice.samplesUntilStart;
            continue;
          }

          const double level = EnvelopeLevel(voice);
          voice.lastLevel = level;

          const double raw = WaveformSample(waveShape, voice.phase);
          voice.phase += voice.freq * phaseIncrementScale;
          voice.phase -= std::floor(voice.phase);

          const double sampleValue = raw * level * voice.velocityGain;

          if (nOutChans >= 2)
          {
            outputs[0][s] += static_cast<T>(sampleValue * voice.gainL);
            outputs[1][s] += static_cast<T>(sampleValue * voice.gainR);
          }
          else
          {
            outputs[0][s] += static_cast<T>(sampleValue);
          }

          ++voice.elapsedSamples;
          if (voice.elapsedSamples >= voice.gateSamples &&
              (voice.releaseSamples <= 0 || voice.elapsedSamples - voice.gateSamples >= voice.releaseSamples))
          {
            voice.active = false;
            break;
          }
        }
      }
    }

  private:
    struct PendingSpawn
    {
      int64_t atSample = 0;
      int note = 0;
      double velocityGain = 1.0;
      double pan = 0.0;
      int64_t attackSamples = 0;
      int64_t decaySamples = 0;
      double sustainLevel = 1.0;
      int64_t releaseSamples = 0;
      int64_t gateSamples = 0;
    };

    struct Voice
    {
      bool active = false;
      double freq = 440.0;
      double phase = 0.0; // 0-1
      double velocityGain = 1.0;
      double gainL = 0.70710678118654752; // constant-power pan, recomputed per spawn from `pan`
      double gainR = 0.70710678118654752;

      int64_t attackSamples = 0;
      int64_t decaySamples = 0;
      double sustainLevel = 1.0;
      int64_t releaseSamples = 0;
      int64_t gateSamples = 0;

      int64_t samplesUntilStart = 0; // spawned mid-future-block; stays silent until this hits 0
      int64_t elapsedSamples = 0;    // counted only once samplesUntilStart has reached 0
      double lastLevel = 0.0;        // envelope level as of the most recent sample rendered --
                                      // used to pick a "quietest" voice to steal, see StealVoiceIndex
      int64_t startedAtSample = 0;   // absolute sample the voice actually started at, oldest-steal tiebreak
    };

    // Envelope level (0-1) at the voice's current mElapsedSamples, per the class-level comment
    // above: attack/decay/sustain run for as long as the gate is open, and release begins the
    // instant the gate elapses, continuing from whatever level attack/decay had reached (no jump).
    static double EnvelopeLevel(const Voice& voice)
    {
      const int64_t t = voice.elapsedSamples;
      const int64_t preReleaseT = std::min(t, voice.gateSamples);
      const double preReleaseLevel = PreReleaseLevel(voice, preReleaseT);

      if (t < voice.gateSamples)
        return preReleaseLevel;

      const int64_t releaseElapsed = t - voice.gateSamples;
      if (voice.releaseSamples <= 0)
        return 0.0;

      const double frac = std::clamp(static_cast<double>(releaseElapsed) / static_cast<double>(voice.releaseSamples), 0.0, 1.0);
      return preReleaseLevel * (1.0 - frac);
    }

    // Attack/decay/sustain level at `t` samples into the voice, ignoring the gate/release entirely
    // (the caller clamps `t` to the gate length before calling, and handles release itself).
    static double PreReleaseLevel(const Voice& voice, int64_t t)
    {
      if (voice.attackSamples > 0 && t < voice.attackSamples)
        return static_cast<double>(t) / static_cast<double>(voice.attackSamples);

      const int64_t decayEnd = voice.attackSamples + voice.decaySamples;
      if (voice.decaySamples > 0 && t < decayEnd)
      {
        const double frac = static_cast<double>(t - voice.attackSamples) / static_cast<double>(voice.decaySamples);
        return 1.0 - (1.0 - voice.sustainLevel) * frac;
      }

      return voice.sustainLevel;
    }

    void DrainDueSpawns(int64_t blockStart, int nFrames, double sampleRate)
    {
      const int64_t blockEnd = blockStart + nFrames;

      size_t i = 0;
      while (i < mPendingCount && mPending[i].atSample < blockEnd)
        ++i;

      for (size_t k = 0; k < i; ++k)
        SpawnVoice(mPending[k], blockStart, nFrames, sampleRate);

      // Shift the remaining (not-yet-due) spawns down to the front.
      const size_t remaining = mPendingCount - i;
      for (size_t k = 0; k < remaining; ++k)
        mPending[k] = mPending[i + k];
      mPendingCount = remaining;
    }

    void SpawnVoice(const PendingSpawn& spawn, int64_t blockStart, int nBlockSamples, double sampleRate)
    {
      const int slot = FindVoiceSlot();
      Voice& voice = mVoices[slot];

      voice.active = true;
      voice.freq = MidiNoteToFrequency(spawn.note);
      voice.phase = 0.0;
      voice.velocityGain = spawn.velocityGain;

      // Constant-power pan law (§7.6: -1 = 100% L, +1 = 100% R).
      constexpr double kQuarterPi = 0.78539816339744830961566084582;
      const double theta = (spawn.pan + 1.0) * kQuarterPi;
      voice.gainL = std::cos(theta);
      voice.gainR = std::sin(theta);

      voice.attackSamples = spawn.attackSamples;
      voice.decaySamples = spawn.decaySamples;
      voice.sustainLevel = spawn.sustainLevel;
      voice.releaseSamples = spawn.releaseSamples;
      voice.gateSamples = spawn.gateSamples;

      const int64_t maxOffset = (nBlockSamples > 0) ? (nBlockSamples - 1) : 0;
      voice.samplesUntilStart = std::clamp<int64_t>(spawn.atSample - blockStart, 0, maxOffset);
      voice.elapsedSamples = 0;
      voice.lastLevel = 0.0;
      voice.startedAtSample = spawn.atSample;

      (void) sampleRate; // frequency is sample-rate-independent; kept as a param for symmetry/future use
    }

    // Finds a free voice slot, or steals one if the pool is full: prefers the quietest currently-
    // sounding voice (lowest lastLevel), breaking ties by oldest startedAtSample -- a cheap
    // approximation of "steal whichever voice the listener will least notice disappearing".
    int FindVoiceSlot()
    {
      for (size_t i = 0; i < MaxVoices; ++i)
      {
        if (!mVoices[i].active)
          return static_cast<int>(i);
      }

      size_t stealIdx = 0;
      for (size_t i = 1; i < MaxVoices; ++i)
      {
        const Voice& candidate = mVoices[i];
        const Voice& best = mVoices[stealIdx];
        if (candidate.lastLevel < best.lastLevel ||
            (candidate.lastLevel == best.lastLevel && candidate.startedAtSample < best.startedAtSample))
        {
          stealIdx = i;
        }
      }
      return static_cast<int>(stealIdx);
    }

    std::array<PendingSpawn, MaxPendingSpawns> mPending{};
    size_t mPendingCount = 0;

    std::array<Voice, MaxVoices> mVoices{};
  };

  // Soft-clip safety net for the final mixed output (dry passthrough + synth voices): identity
  // below `kKnee`, then asymptotically approaches +-1 -- protects against overs when many
  // simultaneous sparkles stack up, without audibly colouring normal-level signal the way a
  // full-range tanh would. Not a user-facing param -- see docs/SPEC.md §7.7.
  inline double SoftClip(double x)
  {
    constexpr double kKnee = 0.9;
    const double ax = std::abs(x);
    if (ax <= kKnee)
      return x;

    const double sign = (x < 0.0) ? -1.0 : 1.0;
    const double over = (ax - kKnee) / (1.0 - kKnee);
    return sign * (kKnee + (1.0 - kKnee) * std::tanh(over));
  }
}
