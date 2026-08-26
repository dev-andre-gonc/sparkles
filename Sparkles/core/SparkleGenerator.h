#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "NoteMatrix.h"

// Sprinkle -> ray -> sparkle event generation, see docs/SPEC.md §4, §7. Deliberately free of
// iPlug2/IGraphics dependencies (like core/NoteMatrix.h) so it can be included by both the plugin
// and the standalone test binary.
namespace sparkle_core
{
  // Unit a given time-family parameter's raw `value` is expressed in (§4: pre_delay, duration,
  // ray_delay, delay each independently choose beats vs. absolute time).
  enum class TimeUnit
  {
    Beats,
    Milliseconds,
    Seconds
  };

  struct TimeParam
  {
    double value = 0.0;
    TimeUnit unit = TimeUnit::Beats;
  };

  enum class PanMode
  {
    Mono,
    Random,
    Sine,
    Triangle,
    Square,
    Saw
  };

  enum class RayRotation
  {
    L,
    R
  };

  enum class RayRotationMode
  {
    Keep,
    Invert
  };

  // A single emitted MIDI note (§1). Times/durations are already resolved to samples relative to
  // the trigger (i.e. excluding the trigger's own transport position).
  //
  // velocity/durationSamples/pan/attack/decay/sustain/release are all resolved by
  // ResolveEventProperties() below from a (rayN, sparkleN) pair plus whatever SparkleParams is
  // current -- Generate() calls it once, at generation time, to fill these fields (so nothing about
  // Generate()'s own behavior/tests changes); a caller wanting these values to reflect a *later*,
  // possibly-changed SparkleParams (i.e. a knob twisted after the sprinkle was triggered but before
  // this particular note actually sounds) calls ResolveEventProperties() again itself, using rayN/
  // sparkleN below, rather than trusting the value baked in here. See Sparkles.cpp's mPendingNotes
  // for that second call site.
  struct SparkleEvent
  {
    int64_t timeOffsetSamples = 0;
    int note = 0;
    int rayN = 0;
    int sparkleN = 0;

    int velocity = 0;
    int64_t durationSamples = 0;
    double pan = 0.0; // -1 = 100% L, +1 = 100% R (§7.6)

    // Synth envelope (§7.7), resolved per (ray_n, sparkle_n) same as loudness/duration above.
    // Only consumed by sparkle_core::SynthEngine when Output Mode includes Audio -- MIDI output
    // has no use for these (a MIDI synth downstream supplies its own envelope).
    int64_t attackSamples = 0;
    int64_t decaySamples = 0;
    double sustainLevel = 1.0; // 0-1
    int64_t releaseSamples = 0;
  };

  // Everything ResolveEventProperties() resolves for one (ray_n, sparkle_n) -- see SparkleEvent's
  // header comment above for why this is split out from the structural fields (timeOffsetSamples/
  // note/rayN/sparkleN).
  struct SparkleEventProperties
  {
    int velocity = 0;
    int64_t durationSamples = 0;
    double pan = 0.0;
    int64_t attackSamples = 0;
    int64_t decaySamples = 0;
    double sustainLevel = 1.0;
    int64_t releaseSamples = 0;
  };

  // Mirrors docs/SPEC.md §7 param names. `_rm` = per-ray exponential multiplier, `_sm` = per-
  // sparkle-within-ray exponential multiplier, applied as documented on each formula.
  struct SparkleParams
  {
    // Structure (§7.1)
    int nRays = 1;
    int nSparklesPerRay = 1;
    double nSparklesPerRayRm = 1.0;
    int rangeMin = 0;
    int rangeMax = 127;
    WrapMode wrapMode = WrapMode::Stop;

    // Trigger-to-sprinkle offset (§7.2) -- no _rm/_sm, applied once ahead of the ray/sparkle chains.
    TimeParam preDelay;
    // Absolute semitone transpose of the sprinkle's starting point (see Generate()'s
    // `anchorNote`) -- unlike rayInterval/interval below, NOT a step into the trigger note's
    // eligible-notes list, and does NOT change which matrix column governs eligibility.
    int preInterval = 0;

    // Base per-sparkle properties, evaluated directly (§7.3)
    double loudness = 127.0;
    double loudnessRm = 1.0;
    double loudnessSm = 1.0;

    TimeParam duration{ 1.0, TimeUnit::Beats };
    double durationRm = 1.0;
    double durationSm = 1.0;

    // Timing chain, cumulative (§7.4)
    TimeParam rayDelay;
    double rayDelayRm = 1.0;

    TimeParam delay;
    double delayRm = 1.0;
    double delaySm = 1.0;

    // Pitch chain, cumulative (§7.5)
    int rayInterval = 0;
    double rayIntervalRm = 1.0;

    int interval = 1;
    double intervalRm = 1.0;
    double intervalSm = 1.0;

    // Panning (§7.6)
    PanMode panning = PanMode::Mono;
    // User-picked value combined with the trigger's project-timeline sample position to seed
    // PanMode::Random (see PanRandomUnit below) -- a plain saved number rather than a "reseed"
    // action, so a pattern the user liked can always be recovered by dialing the same seed back
    // in. Unused by every other panning mode.
    int seed = 0;
    double width = 0.0;
    double widthRm = 1.0;
    double widthSm = 1.0;

    // Combines additively with phaseRm/phaseSm (phase + phaseRm*rayN + phaseSm*sparkleN, see
    // Generate()), unlike every other _rm/_sm pair in this struct -- their defaults are step
    // sizes, not multiplier identities, so 0 (not 1) means "no per-ray/per-sparkle change".
    double phase = 0.0;
    double phaseRm = 1.0;
    double phaseSm = 1.0;

    RayRotation rayRotation = RayRotation::L;
    RayRotationMode rayRotationRm = RayRotationMode::Keep;

    // Synth envelope (§7.7) -- only audible when Output Mode includes Audio (see
    // core/SynthEngine.h), but resolved here unconditionally, same as every other per-sparkle
    // property, so SparkleEvent always carries a complete envelope regardless of which output
    // path(s) actually consume it. Attack/decay/release are plain milliseconds (no beats/ms
    // toggle, unlike the §7.4 time-family params) -- keeping this one unit avoids doubling the
    // param count for something that's a short percussive envelope, not a tempo-locked timing
    // chain; converted to seconds once in Generate() right before the sample conversion.
    double attack = 5.0; // ms
    double attackRm = 1.0;
    double attackSm = 1.0;

    double decay = 5.0; // ms
    double decayRm = 1.0;
    double decaySm = 1.0;

    double sustain = 0.6; // level, 0-1
    double sustainRm = 1.0;
    double sustainSm = 1.0;

    double release = 15.0; // ms
    double releaseRm = 1.0;
    double releaseSm = 1.0;
  };

  class SparkleGenerator
  {
  public:
    // Hard ceiling on events a single trigger can produce, regardless of what the _rm/_sm
    // multipliers compute to -- e.g. n_sparkles_per_ray_rm=3.0 with a dozen rays wants hundreds
    // of thousands of sparkles; this is the backstop that keeps Generate()'s one allocation
    // bounded no matter what a user (or a preset) dials in.
    static constexpr size_t kMaxEventsPerTrigger = 1024;

    // Number of sparkles ray `rayN` would emit before any wrap_mode=stop cutoff (§7.1's
    // n_sparkles_per_ray formula), floored at 0 and capped at kMaxEventsPerTrigger -- capping
    // here (rather than only in the MaxEventCount sum) also keeps the double->int cast below safe
    // from overflow/UB when nSparklesPerRayRm^rayN blows up.
    static int NumSparklesForRay(const SparkleParams& params, int rayN)
    {
      const double raw = params.nSparklesPerRay * std::pow(params.nSparklesPerRayRm, rayN);
      const double clamped = std::clamp(raw, 0.0, static_cast<double>(kMaxEventsPerTrigger));
      return static_cast<int>(std::lround(clamped));
    }

    // Upper bound on events a sprinkle could produce (sum of NumSparklesForRay over all rays),
    // truncated at kMaxEventsPerTrigger -- size the buffer passed to Generate() to at least this
    // so it never reallocates.
    static size_t MaxEventCount(const SparkleParams& params)
    {
      size_t total = 0;
      for (int rayN = 0; rayN < params.nRays; ++rayN)
      {
        total += static_cast<size_t>(NumSparklesForRay(params, rayN));
        if (total >= kMaxEventsPerTrigger)
          return kMaxEventsPerTrigger;
      }
      return total;
    }

    // Generates every sparkle event for one sprinkle triggered by `triggerNote`, in ray-major /
    // sparkle-minor order. `outEvents` is cleared and reserve()'d once to MaxEventCount(params)
    // (never more than kMaxEventsPerTrigger) -- the only allocation this function performs --
    // then filled via push_back, which never reallocates since capacity already covers the upper
    // bound. If the requested rays/sparkles would exceed kMaxEventsPerTrigger, generation stops
    // early (mid-ray if necessary) rather than growing the buffer.
    //
    // `timelineSample` is the trigger's absolute position in the DAW project timeline (e.g. the
    // host's transport sample position at the moment of the trigger, NOT a plugin-internal
    // sample counter -- see Sparkles.cpp's ProcessBlock for how it's derived from GetSamplePos()).
    // It feeds PanMode::Random's hash alongside params.seed (§7.6) so the same trigger, at the
    // same point in the same project, reproduces the same "random" pan across playthroughs --
    // defaults to 0 since only PanMode::Random ever reads it.
    static void Generate(const NoteMatrix& matrix, const SparkleParams& params, int triggerNote,
                          double bpm, double sampleRate, std::vector<SparkleEvent>& outEvents,
                          int64_t timelineSample = 0)
    {
      outEvents.clear();
      outEvents.reserve(MaxEventCount(params));

      const double preDelaySamples = ToSamples(params.preDelay, bpm, sampleRate);

      // §7.2: Pre Interval is an absolute chromatic transpose (in real semitones, unlike Ray
      // Interval/Interval below which step through the eligible-notes list) of the sprinkle's
      // starting point -- NOT of which matrix column governs eligibility. `anchorNote` is passed
      // as the two-note Walk() overload's `anchorNote` arg below, so it only shifts where `steps`
      // counts from; `triggerNote` itself still supplies the column (see NoteMatrix::Walk).
      const int anchorNote = triggerNote + params.preInterval;

      double rayDelayAccumSamples = 0.0; // Sigma (ray_delay * ray_delay_rm^i) for i = 0..rayN
      double rayIntervalAccum = 0.0;     // Sigma (ray_interval * ray_interval_rm^i) for i = 0..rayN

      for (int rayN = 0; rayN < params.nRays; ++rayN)
      {
        if (outEvents.size() >= kMaxEventsPerTrigger)
          break; // hard cap reached -- truncate the remaining rays entirely

        rayDelayAccumSamples += ToSamples(
          params.rayDelay.value * std::pow(params.rayDelayRm, rayN), params.rayDelay.unit, bpm, sampleRate);
        rayIntervalAccum += params.rayInterval * std::pow(params.rayIntervalRm, rayN);

        const double rayStartSamples = preDelaySamples + rayDelayAccumSamples;
        const double rayIntervalOffset = rayIntervalAccum;

        const int numSparkles = NumSparklesForRay(params, rayN);

        double withinRaySamplesAccum = 0.0; // used for sparkleN == 0, then advanced below
        double withinRayIntervalAccum = 0.0;

        for (int sparkleN = 0; sparkleN < numSparkles; ++sparkleN)
        {
          if (outEvents.size() >= kMaxEventsPerTrigger)
            break; // hard cap reached mid-ray -- truncate gracefully rather than grow the buffer

          const double rawSteps = rayIntervalOffset + withinRayIntervalAccum;
          const int steps = static_cast<int>(std::lround(rawSteps));

          const auto note = matrix.Walk(triggerNote, anchorNote, steps, params.rangeMin, params.rangeMax, params.wrapMode);
          if (!note.has_value())
            break; // wrap_mode=stop past a boundary, or the trigger's column has no eligible notes at all

          SparkleEvent event;
          event.timeOffsetSamples = static_cast<int64_t>(std::llround(rayStartSamples + withinRaySamplesAccum));
          event.note = *note;
          event.rayN = rayN;
          event.sparkleN = sparkleN;

          // Resolved here (with `params` as it stands right now) purely so this event is already
          // playable as-is if nothing changes before it sounds -- see SparkleEvent's header comment
          // for why a caller that DOES see a changed SparkleParams before this note fires should
          // call ResolveEventProperties() again itself, using event.rayN/event.sparkleN, rather than
          // trust these fields.
          const SparkleEventProperties props =
            ResolveEventProperties(params, rayN, sparkleN, triggerNote, timelineSample, bpm, sampleRate);
          event.velocity = props.velocity;
          event.durationSamples = props.durationSamples;
          event.pan = props.pan;
          event.attackSamples = props.attackSamples;
          event.decaySamples = props.decaySamples;
          event.sustainLevel = props.sustainLevel;
          event.releaseSamples = props.releaseSamples;

          outEvents.push_back(event);

          const int k = sparkleN + 1;
          const double delayTerm = params.delay.value * std::pow(params.delayRm, rayN) * std::pow(params.delaySm, k - 1);
          withinRaySamplesAccum += ToSamples(delayTerm, params.delay.unit, bpm, sampleRate);
          withinRayIntervalAccum += params.interval * std::pow(params.intervalRm, rayN) * std::pow(params.intervalSm, k - 1);
        }
      }
    }

    // Resolves velocity/duration/pan/ADSR for one (rayN, sparkleN) pair directly from `params`,
    // with no dependency on any other ray/sparkle -- every formula here is a pure function of
    // (rayN, sparkleN) (plus triggerNote/timelineSample, for pan's random mode), unlike the delay/
    // interval chains Generate() accumulates ray-by-ray above, which depend on the whole sequence
    // leading up to that ray. That purity is what lets a caller re-resolve a specific, already-
    // generated note's properties later against a *different*, possibly-live SparkleParams and get
    // a meaningful answer -- see SparkleEvent's header comment and Sparkles.cpp's mPendingNotes.
    static SparkleEventProperties ResolveEventProperties(const SparkleParams& params, int rayN, int sparkleN,
                                                           int triggerNote, int64_t timelineSample,
                                                           double bpm, double sampleRate)
    {
      SparkleEventProperties props;

      const double loudness = params.loudness * std::pow(params.loudnessRm, rayN) * std::pow(params.loudnessSm, sparkleN);
      props.velocity = std::clamp(static_cast<int>(std::lround(loudness)), 1, 127);

      const double durationMagnitude =
        params.duration.value * std::pow(params.durationRm, rayN) * std::pow(params.durationSm, sparkleN);
      const double durationSamples = ToSamples(durationMagnitude, params.duration.unit, bpm, sampleRate);
      props.durationSamples = std::max<int64_t>(0, static_cast<int64_t>(std::llround(durationSamples)));

      // sign(ray_n) = (ray_rotation==L ? 1 : -1) * (ray_rotation_rm==Invert ? -1 : 1)^ray_n -- the
      // closed form of the raySign recurrence Generate() used to walk ray-by-ray (each Invert ray
      // flips the running sign starting from ray 0's own sign), reproduced here since this function
      // only ever sees one rayN at a time, not the sequence leading up to it.
      const double initialSign = (params.rayRotation == RayRotation::L) ? 1.0 : -1.0;
      const bool flipped = params.rayRotationRm == RayRotationMode::Invert && (rayN % 2) != 0;
      const double signForThisRay = flipped ? -initialSign : initialSign;

      const double width = params.width * std::pow(params.widthRm, rayN) * std::pow(params.widthSm, sparkleN);
      // Phase's _rm/_sm combine ADDITIVELY, unlike every other property here -- a per-ray/
      // per-sparkle rotation offset reads far more usefully than an exponential multiplier
      // would on a cyclic 0-1 quantity. Any value pushed outside [0,1) wraps to its decimal
      // part naturally via Wave()'s own `frac = p - floor(p)`, so no explicit wrap is needed
      // here (e.g. 5.3 -> 0.3, including for negative sums).
      const double phase = params.phase + params.phaseRm * rayN + params.phaseSm * sparkleN;
      props.pan = Pan(params.panning, signForThisRay, width, phase, params.seed, timelineSample, triggerNote, rayN, sparkleN);

      // attack/decay/release are resolved in milliseconds (§7.7); converted to seconds here,
      // once, right before the sample conversion below.
      const double attackSeconds = (params.attack * std::pow(params.attackRm, rayN) * std::pow(params.attackSm, sparkleN)) * 0.001;
      const double decaySeconds = (params.decay * std::pow(params.decayRm, rayN) * std::pow(params.decaySm, sparkleN)) * 0.001;
      const double releaseSeconds = (params.release * std::pow(params.releaseRm, rayN) * std::pow(params.releaseSm, sparkleN)) * 0.001;
      props.attackSamples = std::max<int64_t>(0, static_cast<int64_t>(std::llround(std::max(0.0, attackSeconds) * sampleRate)));
      props.decaySamples = std::max<int64_t>(0, static_cast<int64_t>(std::llround(std::max(0.0, decaySeconds) * sampleRate)));
      props.releaseSamples = std::max<int64_t>(0, static_cast<int64_t>(std::llround(std::max(0.0, releaseSeconds) * sampleRate)));
      props.sustainLevel = std::clamp(
        params.sustain * std::pow(params.sustainRm, rayN) * std::pow(params.sustainSm, sparkleN), 0.0, 1.0);

      return props;
    }

  private:
    static double ToSamples(const TimeParam& param, double bpm, double sampleRate)
    {
      return ToSamples(param.value, param.unit, bpm, sampleRate);
    }

    static double ToSamples(double magnitude, TimeUnit unit, double bpm, double sampleRate)
    {
      switch (unit)
      {
        case TimeUnit::Milliseconds: return magnitude * 0.001 * sampleRate;
        case TimeUnit::Seconds: return magnitude * sampleRate;
        case TimeUnit::Beats:
        default: return magnitude * (60.0 / bpm) * sampleRate;
      }
    }

    // Periodic (period 1) waveform lookup for the pan LFO shapes (§7.6).
    static double Wave(PanMode mode, double p)
    {
      double frac = p - std::floor(p);

      switch (mode)
      {
        case PanMode::Triangle:
          return (frac <= 0.5) ? (-1.0 + 4.0 * frac) : (1.0 - 4.0 * (frac - 0.5));
        case PanMode::Square: return (frac < 0.5) ? -1.0 : 1.0;
        case PanMode::Saw: return -1.0 + 2.0 * frac;
        case PanMode::Sine:
        default:
        {
          constexpr double kTwoPi = 6.283185307179586476925286766559;
          return std::sin(kTwoPi * frac);
        }
      }
    }

    // SplitMix64's finalizer -- a fast, well-avalanched integer hash (not cryptographic, doesn't
    // need to be for pan noise). Used as the mixing step in HashCombine below.
    static uint64_t MixBits(uint64_t x)
    {
      x ^= x >> 30;
      x *= 0xbf58476d1ce4e5b9ULL;
      x ^= x >> 27;
      x *= 0x94d049bb133111ebULL;
      x ^= x >> 31;
      return x;
    }

    // boost::hash_combine-shaped fold of one more integer into a running hash, using MixBits as
    // the avalanche step so every input bit affects every output bit regardless of fold order.
    static uint64_t HashCombine(uint64_t seed, uint64_t value)
    {
      return seed ^ MixBits(value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }

    // Stateless replacement for a stream-based RNG (§7.6's PanMode::Random): a pure function of
    // the seed param, the trigger's project-timeline position, which note triggered, and which
    // ray/sparkle this is -- same inputs always produce the same [0,1) output, in any order, on
    // any machine, with no shared/mutable state. That's what makes "random" panning reproducible
    // across playthroughs of the same project (same trigger, same timeline position -> same pan)
    // while still varying per-note (chords) and per-sparkle within one trigger.
    static double PanRandomUnit(int seed, int64_t timelineSample, int triggerNote, int rayN, int sparkleN)
    {
      uint64_t h = static_cast<uint64_t>(static_cast<int64_t>(seed));
      h = HashCombine(h, static_cast<uint64_t>(timelineSample));
      h = HashCombine(h, static_cast<uint64_t>(static_cast<uint32_t>(triggerNote)));
      h = HashCombine(h, static_cast<uint64_t>(static_cast<uint32_t>(rayN)));
      h = HashCombine(h, static_cast<uint64_t>(static_cast<uint32_t>(sparkleN)));
      // Top 53 bits -> uniform double in [0, 1), matching a typical double's mantissa precision.
      return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); // 2^53
    }

    static double Pan(PanMode mode, double signForThisRay, double width, double phase,
                       int seed, int64_t timelineSample, int triggerNote, int rayN, int sparkleN)
    {
      switch (mode)
      {
        case PanMode::Mono: return 0.0;
        case PanMode::Random:
        {
          const double unit = PanRandomUnit(seed, timelineSample, triggerNote, rayN, sparkleN);
          return std::clamp(width * (unit * 2.0 - 1.0), -1.0, 1.0);
        }
        case PanMode::Sine:
        case PanMode::Triangle:
        case PanMode::Square:
        case PanMode::Saw:
        default: return std::clamp(signForThisRay * width * Wave(mode, phase), -1.0, 1.0);
      }
    }
  };
}
