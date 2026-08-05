#pragma once

// Detection-stage (§2) config: what fires a trigger and which input pitches it's willing to
// recognize. Deliberately free of iPlug2/IGraphics dependencies (like core/NoteMatrix.h and
// core/SparkleGenerator.h) so it can be included by both the plugin and the standalone test binary.
namespace sparkle_core
{
  // Which envelope-crossing direction(s) fire a trigger (§2).
  enum class TriggerType
  {
    Up,
    Down,
    Both
  };

  struct DetectionParams
  {
    TriggerType triggerType = TriggerType::Up;
    double threshold = 0.5;    // normalized 0-1; envelope level that must be crossed (§2)
    double reactiveness = 0.5; // normalized 0-1; envelope follower responsiveness (§2)
    int detectNoteMin = 48;    // MIDI note, inclusive (§2, §3)
    int detectNoteMax = 84;    // MIDI note, inclusive (§2, §3)
  };
}
