#include "../core/PitchTracker.h"
#include "test_framework.h"

#include <cmath>
#include <cstdint>

using namespace sparkle_core;

namespace
{
  constexpr double kSampleRate = 48000.0;
  constexpr double kHoldSeconds = 0.25;
  constexpr int kNoteMin = 33; // A1
  constexpr int kNoteMax = 96; // C7

  double NoteFreq(int note)
  {
    return 440. * std::pow(2., (note - 69) / 12.);
  }

  // Phase-continuous sine source, so consecutive Render() calls (e.g. a note change) don't
  // inject a discontinuity click that isn't part of what's being tested.
  struct SineSource
  {
    double phase = 0.0;

    void Render(PitchTracker& tracker, double freq, int nSamples, double amp = 0.5)
    {
      constexpr double kTwoPi = 6.283185307179586476925286766559;
      for (int i = 0; i < nSamples; i++)
      {
        tracker.Push(static_cast<float>(amp * std::sin(phase)));
        phase += kTwoPi * freq / kSampleRate;
      }
    }
  };

  void PushSilence(PitchTracker& tracker, int nSamples)
  {
    for (int i = 0; i < nSamples; i++)
      tracker.Push(0.f);
  }

  PitchTracker MakeTracker(double confidenceThreshold = 0.6)
  {
    PitchTracker tracker;
    tracker.Configure(kSampleRate, kNoteMin, kNoteMax, kHoldSeconds);
    tracker.Reset();
    tracker.SetConfidenceThreshold(confidenceThreshold);
    return tracker;
  }
}

TEST(PitchTracker_DetectsSineNote_A4)
{
  PitchTracker tracker = MakeTracker();
  SineSource sine;

  // Two full buffers of A4 -- an octave-guard test as much as a detection test: a pure 440Hz
  // sine autocorrelates just as strongly at A3's lag (double period) and below, so only the
  // smallest-lag-among-ties rule lands 69 rather than 57/45.
  sine.Render(tracker, NoteFreq(69), 2 * PitchTracker::kBufferSize);

  CHECK(tracker.DisplayNote() == 69);
  CHECK(tracker.DisplayConfidence() > 0.9);
  CHECK(tracker.HasConfidentNote());
  CHECK(tracker.LastConfidentNote() == 69);
}

TEST(PitchTracker_OctaveGuard_DoesNotOvershoot_A3)
{
  PitchTracker tracker = MakeTracker();
  SineSource sine;

  // 220Hz: A4's lag is half A3's period, where a sine anti-correlates -- the guard must pick
  // A3 (smallest lag among the near-ties A3/A2/A1), not jump an octave up.
  sine.Render(tracker, NoteFreq(57), 2 * PitchTracker::kBufferSize);

  CHECK(tracker.DisplayNote() == 57);
  CHECK(tracker.DisplayConfidence() > 0.9);
}

TEST(PitchTracker_Silence_NoNoteNoConfidence)
{
  PitchTracker tracker = MakeTracker();

  PushSilence(tracker, 2 * PitchTracker::kBufferSize);

  CHECK(tracker.DisplayNote() == -1);
  CHECK(tracker.DisplayConfidence() == 0.0);
  CHECK(!tracker.HasConfidentNote());
}

TEST(PitchTracker_Noise_NotConfident)
{
  PitchTracker tracker = MakeTracker();

  // Deterministic LCG noise, loud enough to clear the silence floor. White noise's normalized
  // autocorrelation at the candidate lags is ~1/sqrt(n) -- far below kMinTrackConfidence, so
  // the tracker should refuse to adopt a note at all rather than report a low-confidence one.
  uint32_t state = 0x12345678u;
  for (int i = 0; i < 4 * PitchTracker::kBufferSize; i++)
  {
    state = state * 1664525u + 1013904223u;
    const float sample = (static_cast<float>(state >> 8) / 8388608.f - 1.f) * 0.5f;
    tracker.Push(sample);
  }

  CHECK(tracker.DisplayNote() == -1);
  CHECK(!tracker.HasConfidentNote());
}

TEST(PitchTracker_HoldWindow_SurvivesReleaseThenExpires)
{
  PitchTracker tracker = MakeTracker();
  SineSource sine;

  sine.Render(tracker, NoteFreq(69), 2 * PitchTracker::kBufferSize);
  CHECK(tracker.HasConfidentNote());

  // 0.2s of silence: inside the 0.25s hold window (with a hop of slack for when the last
  // confident analysis actually ran) -- a down-trigger here should still resolve to A4.
  PushSilence(tracker, static_cast<int>(0.2 * kSampleRate));
  CHECK(tracker.HasConfidentNote());
  CHECK(tracker.LastConfidentNote() == 69);

  // Well past the hold window now -- the note must no longer be usable.
  PushSilence(tracker, static_cast<int>(0.2 * kSampleRate));
  CHECK(!tracker.HasConfidentNote());
  CHECK(tracker.DisplayNote() == -1);
}

TEST(PitchTracker_NoteChange_ConvergesToNewNote)
{
  PitchTracker tracker = MakeTracker();
  SineSource sine;

  sine.Render(tracker, NoteFreq(69), 2 * PitchTracker::kBufferSize);
  CHECK(tracker.DisplayNote() == 69);

  // Immediately after the switch the buffer is still mostly A4 and hysteresis hasn't been
  // outvoted -- one hop of E5 must not flip the stable note.
  sine.Render(tracker, NoteFreq(76), PitchTracker::kHopSamples);
  CHECK(tracker.DisplayNote() == 69);

  // After the buffer refills with E5 and the challenger wins its consecutive hops, the stable
  // note (and the confident record a trigger would use) must have moved.
  sine.Render(tracker, NoteFreq(76), 3 * PitchTracker::kBufferSize);
  CHECK(tracker.DisplayNote() == 76);
  CHECK(tracker.LastConfidentNote() == 76);
  CHECK(tracker.DisplayConfidence() > 0.9);
}

TEST(PitchTracker_DCOffset_DoesNotPinToTopNote)
{
  PitchTracker tracker = MakeTracker();

  // Sine + heavy DC offset. Without mean removal, the DC term inflates the normalized
  // autocorrelation at EVERY lag toward 1, the whole table near-ties, and the old
  // any-near-tie octave guard then picked the smallest lag -- i.e. the top of the detect
  // range, which is exactly the "keeps detecting C6" field bug (C7 here, note 96).
  constexpr double kTwoPi = 6.283185307179586476925286766559;
  double phase = 0.0;
  for (int i = 0; i < 2 * PitchTracker::kBufferSize; i++)
  {
    tracker.Push(static_cast<float>(0.4 + 0.3 * std::sin(phase)));
    phase += kTwoPi * NoteFreq(69) / kSampleRate;
  }

  CHECK(tracker.DisplayNote() == 69);
  CHECK(tracker.DisplayConfidence() > 0.9);
}

TEST(PitchTracker_SubRangeRumble_NotDetectedAsHighNote)
{
  PitchTracker tracker = MakeTracker();
  SineSource sine;

  // 30Hz sits below the detect range (A1 = 55Hz), but its period (1600 samples at 48k) dwarfs
  // every candidate lag, so raw autocorrelation paints a smooth high-scoring ramp over the top
  // octaves -- cos(2*pi*lag/1600) is ~0.996 at the table's shortest lag. A plain argmax
  // "detects" a top-range note with near-perfect confidence ("wall" in the note-bars display);
  // the local-peak mask must recognize the ramp has no interior maximum and report nothing.
  sine.Render(tracker, 30.0, 4 * PitchTracker::kBufferSize);

  CHECK(tracker.DisplayNote() == -1);
  CHECK(!tracker.HasConfidentNote());
}

TEST(PitchTracker_LowNotePlusRumble_StillDetectsLowNote)
{
  PitchTracker tracker = MakeTracker();

  // A real note with sub-range rumble underneath: the ramp inflates the top octaves, but the
  // note's own lag is still a genuine local peak and must win over the wall.
  constexpr double kTwoPi = 6.283185307179586476925286766559;
  double phaseNote = 0.0, phaseRumble = 0.0;
  for (int i = 0; i < 4 * PitchTracker::kBufferSize; i++)
  {
    const double sample = 0.4 * std::sin(phaseNote) + 0.25 * std::sin(phaseRumble);
    tracker.Push(static_cast<float>(sample));
    phaseNote += kTwoPi * NoteFreq(48) / kSampleRate; // C3
    phaseRumble += kTwoPi * 30.0 / kSampleRate;
  }

  CHECK(tracker.DisplayNote() == 48);
}

TEST(PitchTracker_NoteConfidences_PeakAtDetectedNote)
{
  PitchTracker tracker = MakeTracker();
  SineSource sine;

  sine.Render(tracker, NoteFreq(69), 2 * PitchTracker::kBufferSize);

  float confidences[kNoteMax - kNoteMin + 1] = {};
  tracker.GetNoteConfidences(kNoteMin, kNoteMax, confidences);

  CHECK(confidences[69 - kNoteMin] > 0.9f);
  // A tritone away shares no harmonic relation with A4 -- its bar must stay well below.
  CHECK(confidences[63 - kNoteMin] < 0.5f);

  // Silence zeroes the whole strip on the next hop.
  PushSilence(tracker, 2 * PitchTracker::kHopSamples);
  tracker.GetNoteConfidences(kNoteMin, kNoteMax, confidences);
  CHECK(confidences[69 - kNoteMin] == 0.f);
}

TEST(PitchTracker_ConfidenceThreshold_GatesConfidentRecordOnly)
{
  // An impossibly strict threshold: tracking still works (display shows the note), but nothing
  // ever qualifies as confident, so a trigger would be dropped.
  PitchTracker tracker = MakeTracker(1.01);
  SineSource sine;

  sine.Render(tracker, NoteFreq(69), 2 * PitchTracker::kBufferSize);

  CHECK(tracker.DisplayNote() == 69);
  CHECK(tracker.DisplayConfidence() > 0.9);
  CHECK(!tracker.HasConfidentNote());
}
