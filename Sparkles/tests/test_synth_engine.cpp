#include "../core/SynthEngine.h"
#include "test_framework.h"

#include <vector>

using namespace sparkle_core;

namespace
{
  constexpr double kSampleRate = 44100.0;
  constexpr double kSqrtHalf = 0.70710678118654752; // cos(pi/4) == sin(pi/4), center-pan gain

  bool ApproxEqual(double a, double b, double eps = 1e-6)
  {
    return std::fabs(a - b) < eps;
  }

  // Fixed-size stereo scratch buffer, zeroed every call -- Render() only ever adds to outputs
  // (matching how it mixes onto the plugin's own dry passthrough), so tests must start from silence
  // themselves rather than relying on Render to overwrite stale contents.
  struct StereoBuffer
  {
    std::vector<double> l, r;
    std::array<double*, 2> ptrs;

    explicit StereoBuffer(int nFrames) : l(nFrames, 0.0), r(nFrames, 0.0), ptrs{ l.data(), r.data() } {}
    double** Data() { return ptrs.data(); }
  };
}

TEST(WaveformSample_PureShapesMatchClassicFormulas)
{
  CHECK(ApproxEqual(WaveformSample(0.0, 0.0), 0.0));   // sine
  CHECK(ApproxEqual(WaveformSample(0.0, 0.25), 1.0));
  CHECK(ApproxEqual(WaveformSample(0.0, 0.5), 0.0));
  CHECK(ApproxEqual(WaveformSample(0.0, 0.75), -1.0));

  CHECK(ApproxEqual(WaveformSample(1.0, 0.0), -1.0));  // triangle
  CHECK(ApproxEqual(WaveformSample(1.0, 0.25), 0.0));
  CHECK(ApproxEqual(WaveformSample(1.0, 0.5), 1.0));
  CHECK(ApproxEqual(WaveformSample(1.0, 0.75), 0.0));

  CHECK(ApproxEqual(WaveformSample(2.0, 0.25), -1.0)); // square
  CHECK(ApproxEqual(WaveformSample(2.0, 0.75), 1.0));

  CHECK(ApproxEqual(WaveformSample(3.0, 0.0), -1.0));  // saw
  CHECK(ApproxEqual(WaveformSample(3.0, 0.5), 0.0));
  CHECK(ApproxEqual(WaveformSample(3.0, 0.999), 0.998, 1e-2));
}

TEST(WaveformSample_FractionalShapeBlendsNeighbours)
{
  // Halfway between Sine(0) and Triangle(1) at a given phase is the plain average of the two.
  const double phase = 0.1;
  const double expected = (WaveformSample(0.0, phase) + WaveformSample(1.0, phase)) * 0.5;
  CHECK(ApproxEqual(WaveformSample(0.5, phase), expected));

  // Halfway between Square(2) and Saw(3).
  const double expected23 = (WaveformSample(2.0, phase) + WaveformSample(3.0, phase)) * 0.5;
  CHECK(ApproxEqual(WaveformSample(2.5, phase), expected23));

  // Out-of-range shape values are clamped rather than extrapolated.
  CHECK(ApproxEqual(WaveformSample(-1.0, phase), WaveformSample(0.0, phase)));
  CHECK(ApproxEqual(WaveformSample(4.0, phase), WaveformSample(3.0, phase)));
}

TEST(WaveformSample_PeriodicWithPeriodOne)
{
  CHECK(ApproxEqual(WaveformSample(1.7, 0.3), WaveformSample(1.7, 1.3)));
  CHECK(ApproxEqual(WaveformSample(1.7, 0.3), WaveformSample(1.7, -0.7)));
}

TEST(MidiNoteToFrequency_KnownValues)
{
  CHECK(ApproxEqual(MidiNoteToFrequency(69), 440.0));  // A4
  CHECK(ApproxEqual(MidiNoteToFrequency(57), 220.0));  // A3, one octave down
  CHECK(ApproxEqual(MidiNoteToFrequency(21), 27.5));   // A0
}

TEST(SoftClip_IdentityBelowKnee)
{
  CHECK(SoftClip(0.0) == 0.0);
  CHECK(SoftClip(0.5) == 0.5);
  CHECK(SoftClip(-0.7) == -0.7);
}

TEST(SoftClip_SaturatesTowardUnityAboveKnee)
{
  const double clipped = SoftClip(1.5);
  CHECK(clipped > 0.9 && clipped < 1.0);
  CHECK(ApproxEqual(SoftClip(-1.5), -clipped));

  // Strictly increasing as input grows further past the knee (using values modest enough that
  // tanh hasn't yet saturated to exactly 1.0 in double precision).
  CHECK(SoftClip(2.0) > SoftClip(1.5));

  // Never *exceeds* +-1 even for extreme input -- tanh saturates to exactly 1.0 well before this,
  // which is fine: the guarantee is "never overshoots", not "never reaches".
  CHECK(SoftClip(50.0) <= 1.0);
  CHECK(SoftClip(-50.0) >= -1.0);
}

namespace
{
  // MIDI note 21 (A0, 27.5 Hz, see MidiNoteToFrequency_KnownValues) -- at 44100 Hz its phase only
  // advances ~0.00062/sample, so over the short windows these tests render, a Square voice's raw
  // oscillator sample stays a constant -1.0 (phase never reaches the 0.5 crossing). That turns
  // Render()'s output directly into a readable trace of the envelope alone, isolating
  // attack/decay/sustain/release from the oscillator entirely.
  constexpr int kLowNote = 21;

  bool ScheduleSquareVoice(SynthEngine<>& engine, double pan, int velocity, int64_t attackSamples,
                            int64_t decaySamples, double sustainLevel, int64_t releaseSamples, int64_t gateSamples)
  {
    return engine.ScheduleVoice(kLowNote, velocity, pan, attackSamples, decaySamples, sustainLevel,
                                 releaseSamples, gateSamples, /*atSample=*/0);
  }
}

TEST(SynthEngine_FullAdsrShapeTracksExactly)
{
  // attack=10, decay=20 (sustain=0.5), gate=100, release=15 -- see the header comment above for
  // why Square + this note makes Render()'s output a direct envelope trace.
  SynthEngine<> engine;
  CHECK(ScheduleSquareVoice(engine, /*pan=*/0.0, /*velocity=*/127, 10, 20, 0.5, 15, 100));

  constexpr int nFrames = 130;
  StereoBuffer buf(nFrames);
  engine.Render(0, nFrames, kSampleRate, /*waveShape=*/2.0, buf.Data(), 2);

  // raw = -1 (constant, see above), center pan -> gainL = gainR = kSqrtHalf, velocity 127 -> gain 1.
  // output(t) = -kSqrtHalf * level(t).
  auto levelAt = [&](int t) { return -buf.l[t] / kSqrtHalf; };

  // Attack: linear ramp 0 -> 1 over samples [0, 10).
  CHECK(ApproxEqual(levelAt(0), 0.0));
  CHECK(ApproxEqual(levelAt(5), 0.5));

  // Decay: continues from 1.0 at t=10, linearly down to sustain (0.5) at t=30.
  CHECK(ApproxEqual(levelAt(10), 1.0));
  CHECK(ApproxEqual(levelAt(20), 0.75));
  CHECK(ApproxEqual(levelAt(29), 1.0 - 0.5 * (19.0 / 20.0)));

  // Sustain: holds flat at 0.5 for the rest of the gate, [30, 100).
  CHECK(ApproxEqual(levelAt(30), 0.5));
  CHECK(ApproxEqual(levelAt(99), 0.5));

  // Release: starts at the sustain level (continuous, no jump) at gate end (t=100), linearly to 0
  // over 15 samples.
  CHECK(ApproxEqual(levelAt(100), 0.5));
  CHECK(ApproxEqual(levelAt(107), 0.5 * (1.0 - 7.0 / 15.0)));

  // L and R are identical throughout -- center pan.
  for (int s = 0; s < nFrames; s++)
    CHECK(ApproxEqual(buf.l[s], buf.r[s]));
}

TEST(SynthEngine_VoiceGoesSilentAndFreesAfterRelease)
{
  SynthEngine<> engine;
  CHECK(ScheduleSquareVoice(engine, 0.0, 127, /*attack=*/0, /*decay=*/0, /*sustain=*/1.0, /*release=*/10, /*gate=*/20));

  constexpr int nFrames = 40; // gate(20) + release(10) = 30, comfortably inside one block
  StereoBuffer buf(nFrames);
  engine.Render(0, nFrames, kSampleRate, 2.0, buf.Data(), 2);
  // The voice deactivates mid-call once release completes (at sample 30) -- the render loop
  // breaks out for it right there, leaving every later sample in this same call untouched (still
  // the buffer's zero-initialized value).
  CHECK(buf.l[30] == 0.0);

  // A second block after the voice has finished must render pure silence -- nothing left active.
  StereoBuffer buf2(nFrames);
  engine.Render(nFrames, nFrames, kSampleRate, 2.0, buf2.Data(), 2);
  for (int s = 0; s < nFrames; s++) {
    CHECK(buf2.l[s] == 0.0);
    CHECK(buf2.r[s] == 0.0);
  }
}

TEST(SynthEngine_ShortGateInterruptsAttackWithoutClick)
{
  // Gate (5) ends mid-attack (attack=10) -- release must pick up continuously from whatever level
  // attack had reached at t=5 (0.5), not jump or wait for attack to finish.
  SynthEngine<> engine;
  CHECK(ScheduleSquareVoice(engine, 0.0, 127, /*attack=*/10, /*decay=*/0, /*sustain=*/1.0, /*release=*/10, /*gate=*/5));

  constexpr int nFrames = 20;
  StereoBuffer buf(nFrames);
  engine.Render(0, nFrames, kSampleRate, 2.0, buf.Data(), 2);

  auto levelAt = [&](int t) { return -buf.l[t] / kSqrtHalf; };
  CHECK(ApproxEqual(levelAt(4), 0.4));  // still in (interrupted) attack: 4/10
  CHECK(ApproxEqual(levelAt(5), 0.5));  // release begins here, continuous with attack's level
  CHECK(ApproxEqual(levelAt(10), 0.5 * (1.0 - 5.0 / 10.0)));
}

TEST(SynthEngine_VelocityScalesOutputLinearly)
{
  SynthEngine<> fullVel;
  CHECK(ScheduleSquareVoice(fullVel, 0.0, 127, 0, 0, 1.0, 0, 50));
  StereoBuffer bufFull(1);
  fullVel.Render(0, 1, kSampleRate, 2.0, bufFull.Data(), 2);

  SynthEngine<> halfVel;
  CHECK(ScheduleSquareVoice(halfVel, 0.0, 64, 0, 0, 1.0, 0, 50));
  StereoBuffer bufHalf(1);
  halfVel.Render(0, 1, kSampleRate, 2.0, bufHalf.Data(), 2);

  CHECK(ApproxEqual(bufHalf.l[0] / bufFull.l[0], 64.0 / 127.0));
}

TEST(SynthEngine_PanLawHardLeftAndRight)
{
  SynthEngine<> hardLeft;
  CHECK(ScheduleSquareVoice(hardLeft, /*pan=*/-1.0, 127, 0, 0, 1.0, 0, 50));
  StereoBuffer bufLeft(1);
  hardLeft.Render(0, 1, kSampleRate, 2.0, bufLeft.Data(), 2);
  CHECK(ApproxEqual(std::fabs(bufLeft.l[0]), 1.0));
  CHECK(ApproxEqual(bufLeft.r[0], 0.0));

  SynthEngine<> hardRight;
  CHECK(ScheduleSquareVoice(hardRight, /*pan=*/1.0, 127, 0, 0, 1.0, 0, 50));
  StereoBuffer bufRight(1);
  hardRight.Render(0, 1, kSampleRate, 2.0, bufRight.Data(), 2);
  CHECK(ApproxEqual(bufRight.l[0], 0.0));
  CHECK(ApproxEqual(std::fabs(bufRight.r[0]), 1.0));
}

TEST(SynthEngine_MonoOutputSumsWithoutPanning)
{
  SynthEngine<> engine;
  CHECK(ScheduleSquareVoice(engine, /*pan=*/-1.0, 127, 0, 0, 1.0, 0, 50)); // pan ignored for mono
  std::vector<double> mono(1, 0.0);
  std::array<double*, 1> ptrs{ mono.data() };
  engine.Render(0, 1, kSampleRate, 2.0, ptrs.data(), 1);
  CHECK(ApproxEqual(std::fabs(mono[0]), 1.0));
}

TEST(SynthEngine_ScheduleVoiceDropsOncePendingPoolIsFull)
{
  SynthEngine<4, 2> tinyEngine; // 4 voices, only 2 pending-spawn slots
  CHECK(tinyEngine.ScheduleVoice(60, 100, 0.0, 0, 0, 1.0, 0, 100, 0));
  CHECK(tinyEngine.ScheduleVoice(61, 100, 0.0, 0, 0, 1.0, 0, 100, 10));
  CHECK(!tinyEngine.ScheduleVoice(62, 100, 0.0, 0, 0, 1.0, 0, 100, 20)); // pool already full
}

TEST(SynthEngine_StopAllSilencesImmediatelyAndDropsPending)
{
  SynthEngine<> engine;
  CHECK(ScheduleSquareVoice(engine, 0.0, 127, 0, 0, 1.0, /*release=*/1000, /*gate=*/1000));

  StereoBuffer warmup(5);
  engine.Render(0, 5, kSampleRate, 2.0, warmup.Data(), 2);
  CHECK(std::fabs(warmup.l[4]) > 0.0); // confirms the voice really was sounding

  engine.StopAll();

  StereoBuffer after(5);
  engine.Render(5, 5, kSampleRate, 2.0, after.Data(), 2);
  for (int s = 0; s < 5; s++) {
    CHECK(after.l[s] == 0.0);
    CHECK(after.r[s] == 0.0);
  }
}

TEST(SynthEngine_VoiceStealingKeepsRenderingWithinCapacity)
{
  // Only 2 voice slots, but 4 simultaneous spawns due in the same block -- must not crash or
  // silently corrupt state; exactly 2 of the 4 end up sounding, chosen by StealVoiceIndex's
  // quietest/oldest policy (see core/SynthEngine.h), which this test doesn't need to pin down
  // exactly -- just that the engine stays well-behaved under oversubscription.
  SynthEngine<2, 64> engine;
  for (int i = 0; i < 4; i++)
    CHECK(engine.ScheduleVoice(60 + i, 127, 0.0, 0, 0, 1.0, 0, 1000, 0));

  StereoBuffer buf(10);
  engine.Render(0, 10, kSampleRate, 2.0, buf.Data(), 2);
  CHECK(std::fabs(buf.l[0]) > 0.0); // something is sounding
}
