#pragma once

// Detection-stage (§2) config: what fires a trigger and which input pitches it's willing to
// recognize. Deliberately free of iPlug2/IGraphics dependencies (like core/NoteMatrix.h and
// core/SparkleGenerator.h) so it can be included by both the plugin and the standalone test binary.
namespace sparkle_core
{
  // Which envelope-crossing direction(s) fire a trigger (§2). Shared between the audio and MIDI
  // detection paths: for MIDI, "Up"/"Down" mean note-on/note-off instead of envelope crossings.
  enum class TriggerType
  {
    Up,
    Down,
    Both
  };

  // Which input(s) can arm a trigger. Audio uses the envelope-follower/pitch-tracker path below;
  // MIDI reads incoming note-on/note-off directly (see Sparkles::ProcessMidiMsg) -- the two are
  // independent trigger sources that both feed the same FireSprinkle path, and both can be active
  // at once under Both.
  enum class DetectionMode
  {
    Audio,
    Midi,
    Both
  };

  struct DetectionParams
  {
    DetectionMode detectionMode = DetectionMode::Audio;
    TriggerType triggerType = TriggerType::Up;
    double threshold = 0.5;    // normalized 0-1; envelope level that must be crossed (§2)
    double reactiveness = 0.5; // normalized 0-1; envelope follower responsiveness (§2)
    double confidence = 0.6;   // normalized 0-1; min pitch-tracker score to accept a trigger (§2)
    int detectNoteMin = 48;    // MIDI note, inclusive (§2, §3)
    int detectNoteMax = 84;    // MIDI note, inclusive (§2, §3)
    int minVelocity = 1;       // [1, 127]; MIDI note velocity a note-on/off must meet to trigger
    // Minimum time, in ms, that must elapse after a trigger fires before another one is allowed to
    // (§2) -- resolved triggers (audio or MIDI) that land inside this window are dropped silently,
    // same as an unconfident pitch or an over-the-cap sprinkle. Guards against a single physical
    // event (e.g. a noisy attack transient, a fast repeated MIDI note) firing multiple sprinkles in
    // quick succession.
    double triggerCooloffMs = 100.0;
  };
}
