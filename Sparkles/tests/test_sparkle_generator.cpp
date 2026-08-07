#include "../core/SparkleGenerator.h"
#include "test_framework.h"

using namespace sparkle_core;

namespace
{
  constexpr double kSampleRate = 44100.0;
  constexpr double kBpm = 120.0;
  constexpr double kBpm174 = 174.0; // second tempo used to prove beats-mode actually depends on bpm

  int64_t BeatsToSamplesAtBpm(double beats, double bpm)
  {
    return static_cast<int64_t>(std::llround(beats * (60.0 / bpm) * kSampleRate));
  }

  int64_t BeatsToSamples(double beats)
  {
    return BeatsToSamplesAtBpm(beats, kBpm);
  }

  int64_t MsToSamples(double ms)
  {
    return static_cast<int64_t>(std::llround(ms * 0.001 * kSampleRate));
  }

  bool ApproxEqual(double a, double b, double eps = 1e-9)
  {
    return std::fabs(a - b) < eps;
  }

  // Generates a single ray/single sparkle sprinkle and returns its pan, isolating the §7.6 pan
  // formula from the rest of the chain. Range/wrap_mode are chosen wide/permissive so the walk
  // never dies -- only panning is under test here.
  double SinglePan(PanMode mode, double phase, double width = 1.0, RayRotation rotation = RayRotation::L)
  {
    NoteMatrix matrix;
    SparkleParams params;
    params.nRays = 1;
    params.nSparklesPerRay = 1;
    params.rangeMin = 60;
    params.rangeMax = 71;
    params.wrapMode = WrapMode::Stop;
    params.panning = mode;
    params.phase = phase;
    params.width = width;
    params.rayRotation = rotation;

    std::vector<SparkleEvent> events;
    SparkleGenerator::Generate(matrix, params, 60, kBpm, kSampleRate, events);
    return events.empty() ? std::nan("") : events[0].pan;
  }

  // Deterministic stand-in for PanMode::Random's injected RandomFn (a plain function pointer, so
  // it can't be a capturing lambda) -- cycles through a fixed sequence each call.
  double TestRandomSequence()
  {
    static const double kValues[] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    static size_t index = 0;
    const double value = kValues[index % 5];
    ++index;
    return value;
  }
}

TEST(SparkleGenerator_SingleRaySingleSparkle)
{
  NoteMatrix matrix; // default: every cell/row/column ON
  SparkleParams params;
  params.nRays = 1;
  params.nSparklesPerRay = 1;
  params.rangeMin = 60; // C4
  params.rangeMax = 71; // B4
  params.wrapMode = WrapMode::Stop;
  params.duration = { 1.0, TimeUnit::Beats };
  params.loudness = 127.0;

  const int triggerNote = 60;

  CHECK(SparkleGenerator::MaxEventCount(params) == 1);

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, triggerNote, kBpm, kSampleRate, events);

  CHECK(events.size() == 1);
  // pre_interval=0, ray_interval=0 -> RawSteps is exactly 0 -> lands on the trigger note (§7.5).
  CHECK(events[0].note == 60);
  CHECK(events[0].timeOffsetSamples == 0); // no pre_delay/ray_delay configured
  CHECK(events[0].velocity == 127);
  CHECK(events[0].durationSamples == BeatsToSamples(1.0));
  CHECK(events[0].pan == 0.0); // panning defaults to Mono
}

TEST(SparkleGenerator_UnityMultipliers_PerfectlyRegular)
{
  // With every _rm/_sm at 1.0, the timing/pitch/velocity chains degrade to plain arithmetic
  // sequences -- no geometric shaping at all.
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 1;
  params.nSparklesPerRay = 4;
  params.rangeMin = 60;
  params.rangeMax = 80;
  params.wrapMode = WrapMode::Stop;

  // Nonzero ray_interval keeps RawSteps away from the sparkle_n=0 zero-crossing edge case so the
  // step sequence is a clean arithmetic progression (1, 2, 3, 4) rather than colliding at 1 twice.
  params.rayInterval = 1;
  params.interval = 1;

  params.delay = { 0.25, TimeUnit::Beats };
  params.loudness = 100.0;
  params.duration = { 0.5, TimeUnit::Beats };

  const int triggerNote = 60;

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, triggerNote, kBpm, kSampleRate, events);

  CHECK(events.size() == 4);

  // The generator accumulates the delay chain as an exact running sum and rounds only once, at
  // the point of use -- so the expected offsets are round(0.25*i beats), not round(0.25 beats)*i
  // (those diverge once the per-step rounding would itself have introduced error).
  const double dRaw = 0.25 * (60.0 / kBpm) * kSampleRate;
  const int64_t dur = BeatsToSamples(0.5);
  for (int i = 0; i < 4; ++i)
  {
    CHECK(events[i].note == 60 + (i + 1));                          // steps 1,2,3,4 -> consecutive eligible notes
    CHECK(events[i].timeOffsetSamples == std::llround(dRaw * i));    // evenly spaced
    CHECK(events[i].velocity == 100);                                // loudness_sm=1 -> constant
    CHECK(events[i].durationSamples == dur);                         // duration_sm=1 -> constant
  }
}

TEST(SparkleGenerator_GeometricDecay)
{
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 1;
  params.nSparklesPerRay = 5;
  params.rangeMin = 60;
  params.rangeMax = 90;
  params.wrapMode = WrapMode::Stop;
  params.rayInterval = 1;
  params.interval = 1;
  params.loudness = 127.0;
  params.loudnessSm = 0.5; // halves every sparkle within the ray

  const int triggerNote = 60;

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, triggerNote, kBpm, kSampleRate, events);

  CHECK(events.size() == 5);

  int expectedVelocity = 0;
  for (int sparkleN = 0; sparkleN < 5; ++sparkleN)
  {
    const double raw = 127.0 * std::pow(0.5, sparkleN);
    expectedVelocity = std::clamp(static_cast<int>(std::lround(raw)), 1, 127);
    CHECK(events[sparkleN].velocity == expectedVelocity);
  }

  // Strictly decreasing (until it bottoms out at the velocity=1 floor).
  for (size_t i = 1; i < events.size(); ++i)
    CHECK(events[i].velocity <= events[i - 1].velocity);
}

TEST(SparkleGenerator_WrapModeStop_RayDiesAtBoundary)
{
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 1;
  params.nSparklesPerRay = 10; // requests more sparkles than the range can hold
  params.rangeMin = 60;
  params.rangeMax = 64; // 5 chromatic notes -> eligible list [60,61,62,63,64]
  params.wrapMode = WrapMode::Stop;
  params.rayInterval = 1;
  params.interval = 1;

  const int triggerNote = 60;

  CHECK(SparkleGenerator::MaxEventCount(params) == 10);

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, triggerNote, kBpm, kSampleRate, events);

  // Steps walk 1,2,3,4,5,... ; step 5 runs past the 5-note list (index 5 >= n=5) and the ray
  // stops emitting entirely, well short of the 10 sparkles requested (§6, §8 #2).
  CHECK(events.size() == 4);
  CHECK(events[0].note == 61);
  CHECK(events[3].note == 64);
}

TEST(SparkleGenerator_WrapModeStop_OtherRaysUnaffected)
{
  // §8 #2: wrap_mode=stop only kills the ray that ran out of range -- other rays in the same
  // sprinkle keep going.
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 2;
  params.nSparklesPerRay = 10;
  params.rangeMin = 60;
  params.rangeMax = 64;
  params.wrapMode = WrapMode::Stop;
  // ray_interval left at 0 so RayIntervalOffset is 0 for every ray_n (a nonzero ray_interval
  // accumulates cumulatively across rays per §7.5 and would make ray 1's step sequence diverge
  // from ray 0's -- besides the point of this test, which is just that one dead ray doesn't
  // silence the other).
  params.interval = 1;

  const int triggerNote = 60;

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, triggerNote, kBpm, kSampleRate, events);

  // Both rays walk the identical within-ray step sequence (0, 1, 2, 3, 4, then dies at step 5)
  // from the same trigger note, so both produce 5 sparkles before running out of range -> 10
  // total, not zero.
  CHECK(events.size() == 10);
}

TEST(SparkleGenerator_HardCap_TruncatesWithoutAllocatingUnbounded)
{
  // n_sparkles_per_ray_rm=3.0 across 12 rays wants 1+3+9+...+3^11 = 265720 sparkles -- the hard
  // cap must truncate this to kMaxEventsPerTrigger rather than let MaxEventCount()/reserve() try
  // to size a buffer for the uncapped total.
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 12;
  params.nSparklesPerRay = 1;
  params.nSparklesPerRayRm = 3.0;
  params.rangeMin = 0;
  params.rangeMax = 127;
  // Around never dies at a boundary (unlike Stop), so nothing but the hard cap itself limits how
  // many sparkles actually get emitted -- isolates the cap from wrap_mode behavior.
  params.wrapMode = WrapMode::Around;
  params.rayInterval = 1;
  params.interval = 1;

  const int triggerNote = 60;

  const size_t maxCount = SparkleGenerator::MaxEventCount(params);
  CHECK(maxCount == SparkleGenerator::kMaxEventsPerTrigger);
  CHECK(maxCount == 1024);

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, triggerNote, kBpm, kSampleRate, events);

  CHECK(events.size() == SparkleGenerator::kMaxEventsPerTrigger);
  // reserve() was only ever asked for the capped amount -- capacity should never have grown past
  // it (no reallocation happened while filling, and no oversized up-front allocation either).
  CHECK(events.capacity() == SparkleGenerator::kMaxEventsPerTrigger);
}

TEST(SparkleGenerator_TimeBase_Beats_ScalesWithBpm)
{
  // §4: pre_delay/duration (like ray_delay/delay) support project/tempo-relative time -- a beats
  // value must convert to a different sample count at a different tempo.
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 1;
  params.nSparklesPerRay = 1;
  params.rangeMin = 60;
  params.rangeMax = 90;
  params.wrapMode = WrapMode::Stop;
  params.preDelay = { 2.0, TimeUnit::Beats };
  params.duration = { 0.5, TimeUnit::Beats };

  const int triggerNote = 60;

  for (double bpm : { kBpm, kBpm174 })
  {
    std::vector<SparkleEvent> events;
    SparkleGenerator::Generate(matrix, params, triggerNote, bpm, kSampleRate, events);

    CHECK(events.size() == 1);
    CHECK(events[0].timeOffsetSamples == BeatsToSamplesAtBpm(2.0, bpm));
    CHECK(events[0].durationSamples == BeatsToSamplesAtBpm(0.5, bpm));
  }

  // Guards against a bug where bpm is silently ignored -- 120 and 174 must actually disagree.
  CHECK(BeatsToSamplesAtBpm(2.0, kBpm) != BeatsToSamplesAtBpm(2.0, kBpm174));
}

TEST(SparkleGenerator_TimeBase_Milliseconds_IsBpmIndependent)
{
  // §4: the same four params can instead be set to absolute time -- a milliseconds value must
  // convert to the *same* sample count no matter the tempo.
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 1;
  params.nSparklesPerRay = 1;
  params.rangeMin = 60;
  params.rangeMax = 90;
  params.wrapMode = WrapMode::Stop;
  params.preDelay = { 750.0, TimeUnit::Milliseconds };
  params.duration = { 250.0, TimeUnit::Milliseconds };

  const int triggerNote = 60;
  const int64_t expectedOffset = MsToSamples(750.0);
  const int64_t expectedDuration = MsToSamples(250.0);

  for (double bpm : { kBpm, kBpm174 })
  {
    std::vector<SparkleEvent> events;
    SparkleGenerator::Generate(matrix, params, triggerNote, bpm, kSampleRate, events);

    CHECK(events.size() == 1);
    CHECK(events[0].timeOffsetSamples == expectedOffset);
    CHECK(events[0].durationSamples == expectedDuration);
  }
}

TEST(SparkleGenerator_TimeBase_PerParamUnitIsIndependent)
{
  // §4: each of the four time params picks beats vs. absolute time independently of the others.
  // ray_delay is set to ms (bpm-independent) while delay stays in beats (bpm-dependent) -- run at
  // two tempos to confirm neither chain leaks its bpm-(in)dependence into the other.
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 2;
  params.nSparklesPerRay = 2;
  params.rangeMin = 60;
  params.rangeMax = 90;
  params.wrapMode = WrapMode::Around; // never dies -- isolates timing from wrap_mode
  params.rayInterval = 1;
  params.interval = 1;

  params.rayDelay = { 300.0, TimeUnit::Milliseconds };
  params.delay = { 0.25, TimeUnit::Beats };

  const int triggerNote = 60;

  for (double bpm : { kBpm, kBpm174 })
  {
    std::vector<SparkleEvent> events;
    SparkleGenerator::Generate(matrix, params, triggerNote, bpm, kSampleRate, events);

    CHECK(events.size() == 4);

    // RayStart(ray_n) = pre_delay + Sigma(ray_delay * ray_delay_rm^i, i=0..ray_n); ray_delay_rm=1
    // here, so it's just (ray_n+1) copies of the 300ms ray_delay, bpm-independent throughout.
    const int64_t ray0Start = MsToSamples(300.0);
    const int64_t ray1Start = MsToSamples(300.0) * 2;
    // WithinRay(_, sparkle_n=1) = delay (delay_rm=delay_sm=1), bpm-dependent, same for both rays.
    const int64_t within1 = BeatsToSamplesAtBpm(0.25, bpm);

    CHECK(events[0].timeOffsetSamples == ray0Start);
    CHECK(events[1].timeOffsetSamples == ray0Start + within1);
    CHECK(events[2].timeOffsetSamples == ray1Start);
    CHECK(events[3].timeOffsetSamples == ray1Start + within1);
  }
}

TEST(SparkleGenerator_Panning_Mono_IsAlwaysCenter)
{
  // "mono ignores all panning params below" (§7.6) -- even a large width/nonzero phase must not
  // move it off center.
  CHECK(SinglePan(PanMode::Mono, /*phase=*/0.9, /*width=*/5.0) == 0.0);
}

TEST(SparkleGenerator_Panning_SineWave)
{
  CHECK(ApproxEqual(SinglePan(PanMode::Sine, 0.0), 0.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Sine, 0.25), 1.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Sine, 0.5), 0.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Sine, 0.75), -1.0));
}

TEST(SparkleGenerator_Panning_TriangleWave)
{
  // Ramps -1 -> 1 over [0, 0.5], then 1 -> -1 over [0.5, 1] (§7.6).
  CHECK(ApproxEqual(SinglePan(PanMode::Triangle, 0.0), -1.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Triangle, 0.25), 0.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Triangle, 0.5), 1.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Triangle, 0.75), 0.0));
}

TEST(SparkleGenerator_Panning_SquareWave)
{
  // -1 over [0, 0.5), +1 over [0.5, 1) (§7.6).
  CHECK(ApproxEqual(SinglePan(PanMode::Square, 0.0), -1.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Square, 0.49), -1.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Square, 0.5), 1.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Square, 0.99), 1.0));
}

TEST(SparkleGenerator_Panning_SawWave)
{
  // Ramps -1 -> 1 over [0, 1), then jumps back to -1 (§7.6).
  CHECK(ApproxEqual(SinglePan(PanMode::Saw, 0.0), -1.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Saw, 0.25), -0.5));
  CHECK(ApproxEqual(SinglePan(PanMode::Saw, 0.5), 0.0));
  CHECK(ApproxEqual(SinglePan(PanMode::Saw, 0.75), 0.5));
}

TEST(SparkleGenerator_Panning_PhaseWrapsNaturally)
{
  // "any phase value, including ones pushed outside [0,1) by phase_rm/phase_sm, wraps naturally"
  // (§7.6) -- phase > 1 and negative phase must fold back into the same period-1 cycle.
  CHECK(ApproxEqual(SinglePan(PanMode::Saw, 1.25), SinglePan(PanMode::Saw, 0.25)));
  CHECK(ApproxEqual(SinglePan(PanMode::Saw, -0.25), SinglePan(PanMode::Saw, 0.75)));
  CHECK(ApproxEqual(SinglePan(PanMode::Sine, 2.25), SinglePan(PanMode::Sine, 0.25)));
}

TEST(SparkleGenerator_Panning_ClampsWhenWidthExceedsOne)
{
  // width can exceed 1 via width_rm/width_sm growth, which would otherwise push pan outside the
  // valid +-1 range -- the clamp is what keeps it legal (§7.6).
  CHECK(SinglePan(PanMode::Sine, 0.25, /*width=*/3.0) == 1.0);
  CHECK(SinglePan(PanMode::Sine, 0.75, /*width=*/3.0) == -1.0);
}

TEST(SparkleGenerator_Panning_RayRotation_KeepVsInvert)
{
  // sign(ray_n) = (ray_rotation==L ? 1 : -1) * (ray_rotation_rm==Keep ? 1 : -1)^ray_n (§7.6).
  // Isolate sign from width/Wave by fixing width=1 and phase=0.25 (Wave(sine, 0.25) = 1), so pan
  // reads out as exactly the sign for that ray.
  auto pansFor = [](RayRotation rotation, RayRotationMode rotationMode) {
    NoteMatrix matrix;
    SparkleParams params;
    params.nRays = 4;
    params.nSparklesPerRay = 1;
    params.rangeMin = 60;
    params.rangeMax = 90;
    params.wrapMode = WrapMode::Around; // never dies -- isolates rotation from wrap_mode
    params.panning = PanMode::Sine;
    params.phase = 0.25;
    params.width = 1.0;
    params.rayRotation = rotation;
    params.rayRotationRm = rotationMode;

    std::vector<SparkleEvent> events;
    SparkleGenerator::Generate(matrix, params, 60, kBpm, kSampleRate, events);

    std::vector<double> pans;
    for (const auto& e : events)
      pans.push_back(e.pan);
    return pans;
  };

  const auto keepL = pansFor(RayRotation::L, RayRotationMode::Keep);
  CHECK(keepL.size() == 4);
  for (double p : keepL)
    CHECK(ApproxEqual(p, 1.0)); // every ray keeps ray 0's sign

  const auto invertL = pansFor(RayRotation::L, RayRotationMode::Invert);
  CHECK(invertL.size() == 4);
  CHECK(ApproxEqual(invertL[0], 1.0));
  CHECK(ApproxEqual(invertL[1], -1.0));
  CHECK(ApproxEqual(invertL[2], 1.0));
  CHECK(ApproxEqual(invertL[3], -1.0));

  const auto keepR = pansFor(RayRotation::R, RayRotationMode::Keep);
  CHECK(keepR.size() == 4);
  for (double p : keepR)
    CHECK(ApproxEqual(p, -1.0));

  const auto invertR = pansFor(RayRotation::R, RayRotationMode::Invert);
  CHECK(invertR.size() == 4);
  CHECK(ApproxEqual(invertR[0], -1.0));
  CHECK(ApproxEqual(invertR[1], 1.0));
  CHECK(ApproxEqual(invertR[2], -1.0));
  CHECK(ApproxEqual(invertR[3], 1.0));
}

TEST(SparkleGenerator_Panning_WidthDecayPerRay)
{
  // width(ray_n, sparkle_n) = width * width_rm^ray_n * width_sm^sparkle_n, evaluated directly at
  // each ray (§7.6/§7.3 pattern) -- not a cumulative sum like the delay/interval chains.
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 4;
  params.nSparklesPerRay = 1;
  params.rangeMin = 60;
  params.rangeMax = 90;
  params.wrapMode = WrapMode::Around;
  params.panning = PanMode::Sine;
  params.phase = 0.25; // Wave = 1, isolates width as the only variable in pan
  params.width = 1.0;
  params.widthRm = 0.5;

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, 60, kBpm, kSampleRate, events);

  CHECK(events.size() == 4);
  double expected = 1.0;
  for (const auto& event : events)
  {
    CHECK(ApproxEqual(event.pan, expected));
    expected *= 0.5;
  }
}

TEST(SparkleGenerator_Panning_WidthDecayPerSparkle)
{
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 1;
  params.nSparklesPerRay = 4;
  params.rangeMin = 60;
  params.rangeMax = 90;
  params.wrapMode = WrapMode::Around;
  params.rayInterval = 1;
  params.interval = 1;
  params.panning = PanMode::Sine;
  params.phase = 0.25; // Wave = 1
  params.width = 1.0;
  params.widthSm = 0.5;

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, 60, kBpm, kSampleRate, events);

  CHECK(events.size() == 4);
  double expected = 1.0;
  for (const auto& event : events)
  {
    CHECK(ApproxEqual(event.pan, expected));
    expected *= 0.5;
  }
}

TEST(SparkleGenerator_Panning_PhaseSm_DirectFormula)
{
  // phase(ray_n, sparkle_n) = phase * phase_rm^ray_n * phase_sm^sparkle_n, evaluated directly.
  // Triangle wave makes the resulting values easy to hand-check: Wave(triangle, p) = -1 + 4*p for
  // p <= 0.5.
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 1;
  params.nSparklesPerRay = 3;
  params.rangeMin = 60;
  params.rangeMax = 90;
  params.wrapMode = WrapMode::Around;
  params.rayInterval = 1;
  params.interval = 1;
  params.panning = PanMode::Triangle;
  params.phase = 0.1;
  params.phaseSm = 2.0;
  params.width = 1.0;

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, 60, kBpm, kSampleRate, events);

  CHECK(events.size() == 3);
  // phase: 0.1, 0.2, 0.4 -> Wave: -0.6, -0.2, 0.6
  CHECK(ApproxEqual(events[0].pan, -0.6));
  CHECK(ApproxEqual(events[1].pan, -0.2));
  CHECK(ApproxEqual(events[2].pan, 0.6));
}

TEST(SparkleGenerator_NoteMatrix_RestrictsGeneratedPitchClasses)
{
  // Only C and G are eligible sparkle pitch classes (rows), regardless of trigger column -- every
  // generated note across a wide range/many sparkles must land on one of those two pitch classes.
  // This is the actual §5 wiring test: SparkleGenerator must consult the matrix per-sparkle, not
  // just accept whatever NoteMatrix::Walk() returns for the default all-on matrix (every other test
  // in this file uses a default matrix, so this is the only one that would catch the matrix being
  // silently ignored).
  NoteMatrix matrix;
  for (int row = 0; row < kNumPitchClasses; ++row)
    matrix.SetRowEnabled(row, row == PitchClassOf(60) || row == PitchClassOf(67)); // C, G

  SparkleParams params;
  params.nRays = 3;
  params.nSparklesPerRay = 8;
  params.rangeMin = 48;
  params.rangeMax = 96;
  params.wrapMode = WrapMode::Around;
  params.rayInterval = 3;
  params.interval = 1;

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, 60, kBpm, kSampleRate, events);

  CHECK(events.size() == 24);
  for (const auto& event : events)
  {
    const int pc = PitchClassOf(event.note);
    CHECK(pc == PitchClassOf(60) || pc == PitchClassOf(67));
  }
}

TEST(SparkleGenerator_NoteMatrix_DisabledTriggerColumnKillsSprinkle)
{
  // The trigger note's own column has zero eligible rows -- no sprinkle should fire at all, not
  // just a truncated one (§5: "no sprinkle is generated for it").
  NoteMatrix matrix;
  matrix.SetColumnEnabled(PitchClassOf(60), false);

  SparkleParams params;
  params.nRays = 2;
  params.nSparklesPerRay = 4;
  params.rangeMin = 48;
  params.rangeMax = 96;
  params.wrapMode = WrapMode::Around;

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, 60, kBpm, kSampleRate, events);

  CHECK(events.empty());
}

TEST(SparkleGenerator_Panning_Random_UsesInjectedSourceAndOnlyWidth)
{
  // "random uses only the width params" (§7.6) -- ray_rotation is deliberately set to R here to
  // confirm it has no effect on random's output, unlike the sine/triangle/square/saw modes.
  NoteMatrix matrix;
  SparkleParams params;
  params.nRays = 1;
  params.nSparklesPerRay = 5;
  params.rangeMin = 60;
  params.rangeMax = 90;
  params.wrapMode = WrapMode::Around;
  params.rayInterval = 1;
  params.interval = 1;
  params.panning = PanMode::Random;
  params.width = 0.4;
  params.rayRotation = RayRotation::R;

  std::vector<SparkleEvent> events;
  SparkleGenerator::Generate(matrix, params, 60, kBpm, kSampleRate, events, TestRandomSequence);

  CHECK(events.size() == 5);
  const double expected[] = { -0.4, -0.2, 0.0, 0.2, 0.4 }; // width * (random*2 - 1)
  for (size_t i = 0; i < 5; ++i)
    CHECK(ApproxEqual(events[i].pan, expected[i]));
}
