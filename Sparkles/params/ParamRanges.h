#pragma once

// Shared numeric bounds referenced both by Sparkles.h's pitch-detector candidate table and by
// params/ParamList.h's kParamMinNote/kParamMaxNote entries. Kept in their own ordinary (single-
// inclusion) header rather than inline in ParamList.h, because ParamList.h is an X-macro file
// that's deliberately #include-d multiple times per translation unit and must contain nothing but
// macro invocations -- see its own header comment.
namespace sparkle_params
{
  // MIDI note 60 = C4 (middle C). Trigger range is restricted to musical notes so the pitch
  // tracker only has to check a couple dozen candidates instead of every possible frequency.
  constexpr int kMinTriggerableNote = 24;    // C1
  constexpr int kMaxTriggerableNote = 108;   // C8
  constexpr int kDefaultMinTriggerNote = 36; // C2
  constexpr int kDefaultMaxTriggerNote = 84; // C6

  // One per-note confidence bar in the UI's bottom strip (ui/NoteBarsControl.h) for every note
  // the detector could ever be configured to hear -- always the full triggerable span, not the
  // currently selected detect range, so the strip's geometry never shifts when Min/Max Note move.
  constexpr int kNumTriggerableNotes = kMaxTriggerableNote - kMinTriggerableNote + 1;
}
