// Single source of truth for every "knob" iPlug2 IParam derived from docs/SPEC.md (§2 detection
// stage, §5.1 key/scale quick-fill selectors, §7 structure/timing/pitch/loudness/duration/
// panning). Deliberately excludes the §5 note-eligibility matrix itself (144 cells + 24 column/
// row toggles) -- see params/ParamSnapshot.h's header comment for why those live only in
// sparkle_core::NoteMatrix, outside the IParam list entirely.
//
// This is an X-macro list: it contains ONLY macro invocations, nothing else -- deliberately no
// #pragma once, since it's meant to be #include-d once per consumer, potentially several times
// within the same translation unit (e.g. Sparkles.h includes it once to generate `enum EParams`;
// Sparkles.cpp includes it again, later, to generate the constructor's InitXxx() calls).
//
// Each inclusion site defines whichever of these three macros it cares about before the
// #include -- any left undefined expands to nothing here, and this file #undef's all three at the
// bottom, so a site never has to clean up after itself:
//
//   SPARKLE_PARAM_DOUBLE(id, name, defaultVal, minVal, maxVal, step, label)
//   SPARKLE_PARAM_INT   (id, name, defaultVal, minVal, maxVal, label)
//   SPARKLE_PARAM_ENUM  (id, name, defaultIdx, ...optionNames)
//
// `id` is always the EParams enum name. Every consumer -- enum generation, the InitXxx() calls,
// and params/ParamSnapshot.h's hand-written GetParam(id) reads -- refers to a param by this same
// id, so adding/renaming/reordering a param only ever means editing this file (ParamSnapshot.h's
// per-field mapping still needs a matching line added by hand -- see that file's header comment
// for why that part isn't also macro-generated).

#ifndef SPARKLE_PARAM_DOUBLE
#define SPARKLE_PARAM_DOUBLE(id, name, defaultVal, minVal, maxVal, step, label)
#endif
#ifndef SPARKLE_PARAM_INT
#define SPARKLE_PARAM_INT(id, name, defaultVal, minVal, maxVal, label)
#endif
#ifndef SPARKLE_PARAM_ENUM
#define SPARKLE_PARAM_ENUM(id, name, defaultIdx, ...)
#endif

// clang-format off

// --- Pre-spec scaffold ---------------------------------------------------------------------------
// Dry passthrough gain from the original prototype. docs/SPEC.md §1 says the plugin's only output
// is MIDI (no audio out) -- kept until ProcessBlock's passthrough is actually removed. Named
// "Passthrough" rather than "Gain" for the user-facing display string since it never boosts the
// signal (0-100%) -- it only controls how much of the dry input bleeds through to the output.
SPARKLE_PARAM_DOUBLE(kParamGain, "Passthrough", 0., 0., 100., 0.01, "%")

// --- §2 Detection stage ---------------------------------------------------------------------------
SPARKLE_PARAM_ENUM  (kParamTriggerType,  "Trigger Type", 0, "Up", "Down", "Both")
SPARKLE_PARAM_DOUBLE(kParamThreshold,    "Threshold", 50., 0., 100., 0.01, "%")
// Envelope follower coefficient (§2): env = env*(1-reactiveness) + |in|*reactiveness, applied
// every sample. Expressed directly in this 0-1 coefficient space (not a %) since that's exactly
// what DetectionParams::reactiveness and the formula above consume -- no unit conversion needed
// at the read site. Default is deliberately tiny -- see ProcessBlock's envelope-tracking comment.
SPARKLE_PARAM_DOUBLE(kParamReactiveness, "Reactiveness", 0.001, 0., 1., 0.0001, "")
// Minimum normalized pitch-tracker confidence (core/PitchTracker.h's autocorrelation score) a
// trigger needs before it fires -- crossings whose note can't be identified at least this
// confidently are dropped rather than guessed at. Same 0-1 space the tracker scores in, so no
// unit conversion at the read site (same rationale as reactiveness above).
SPARKLE_PARAM_DOUBLE(kParamConfidence,   "Confidence", 0.6, 0., 1., 0.01, "")
// §2's detect_note_min/detect_note_max -- named kParamMinNote/kParamMaxNote to match the
// prototype's existing UI control tags.
SPARKLE_PARAM_INT(kParamMinNote, "Min Note", sparkle_params::kDefaultMinTriggerNote, sparkle_params::kMinTriggerableNote, sparkle_params::kMaxTriggerableNote, "")
SPARKLE_PARAM_INT(kParamMaxNote, "Max Note", sparkle_params::kDefaultMaxTriggerNote, sparkle_params::kMinTriggerableNote, sparkle_params::kMaxTriggerableNote, "")

// --- §5.1 Key + scale quick-fill --------------------------------------------------------------------
// Sparkles::OnParamChange regenerates the note matrix from these two via sparkle_core::ApplyKeyScale.
// Excluding them from presets per §8 is still future work.
// The first 12 options' order matches sparkle_core::PitchClass exactly, see core/NoteMatrix.h. The
// trailing "Trigger Note" option is a 13th, non-PitchClass value (index kNumPitchClasses == 12):
// selecting it switches OnParamChange to sparkle_core::ApplyKeyScalePerColumn instead of
// ApplyKeyScale, so each column uses its own trigger pitch class as the scale root rather than one
// fixed root shared by all columns.
SPARKLE_PARAM_ENUM(kParamKeyRoot, "Key Root", 0, "A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "Trigger Note")
// Option order matches sparkle_core::Scale exactly, see core/NoteMatrix.h.
SPARKLE_PARAM_ENUM(kParamKeyScale, "Key Scale", 0,
  "Ionian", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Aeolian", "Locrian",
  "Harmonic Minor", "Melodic Minor", "Major Pentatonic", "Minor Pentatonic", "Blues", "Chromatic")

// --- §7.1 Structure --------------------------------------------------------------------------------
SPARKLE_PARAM_INT   (kParamNRays,             "Num Rays", 3, 1, 16, "")
SPARKLE_PARAM_INT   (kParamNSparklesPerRay,   "Sparkles Per Ray", 4, 1, 32, "")
SPARKLE_PARAM_DOUBLE(kParamNSparklesPerRayRm, "Sparkles Per Ray Rm", 1., 0.1, 3., 0.01, "x")
SPARKLE_PARAM_INT   (kParamRangeMin,          "Range Min", 48, 0, 127, "")
SPARKLE_PARAM_INT   (kParamRangeMax,          "Range Max", 96, 0, 127, "")
// Option order matches sparkle_core::WrapMode exactly, see core/NoteMatrix.h.
SPARKLE_PARAM_ENUM  (kParamWrapMode, "Wrap Mode", 2, "Mirror", "Around", "Stop")

// --- §7.2 Trigger-to-sprinkle offset ----------------------------------------------------------------
SPARKLE_PARAM_DOUBLE(kParamPreDelay, "Pre Delay", 0., 0., 60., 0.001, "")
// Option order matches sparkle_core::TimeUnit exactly, see core/SparkleGenerator.h.
SPARKLE_PARAM_ENUM  (kParamPreDelayUnit, "Pre Delay Unit", 0, "Beats", "ms", "s")
SPARKLE_PARAM_INT   (kParamPreInterval, "Pre Interval", 0, -48, 48, "")

// --- §7.3 Base per-sparkle properties, evaluated directly --------------------------------------------
// Display name is "Velocity" (MIDI's term for the same concept) rather than SPEC.md's "loudness" --
// the ids (kParamLoudness*) are left alone since renaming them would ripple into ParamSnapshot.h,
// SparkleGenerator, and the tests for a purely cosmetic, user-facing change.
SPARKLE_PARAM_DOUBLE(kParamLoudness,   "Velocity", 127., 1., 127., 1., "")
SPARKLE_PARAM_DOUBLE(kParamLoudnessRm, "Velocity Rm", 1., 0.1, 3., 0.01, "x")
SPARKLE_PARAM_DOUBLE(kParamLoudnessSm, "Velocity Sm", 1., 0.1, 3., 0.01, "x")

SPARKLE_PARAM_DOUBLE(kParamDuration,     "Duration", 0.0625, 0.001, 60., 0.001, "")
SPARKLE_PARAM_ENUM  (kParamDurationUnit, "Duration Unit", 0, "Beats", "ms", "s")
SPARKLE_PARAM_DOUBLE(kParamDurationRm,   "Duration Rm", 1., 0.1, 3., 0.01, "x")
SPARKLE_PARAM_DOUBLE(kParamDurationSm,   "Duration Sm", 1., 0.1, 3., 0.01, "x")

// --- §7.4 Timing chain, cumulative -------------------------------------------------------------------
SPARKLE_PARAM_DOUBLE(kParamRayDelay,     "Ray Delay", 1., 0., 60., 0.001, "")
SPARKLE_PARAM_ENUM  (kParamRayDelayUnit, "Ray Delay Unit", 0, "Beats", "ms", "s")
SPARKLE_PARAM_DOUBLE(kParamRayDelayRm,   "Ray Delay Rm", 1.5, 0.1, 3., 0.01, "x")

SPARKLE_PARAM_DOUBLE(kParamDelay,     "Delay", 0.25, 0., 60., 0.001, "")
SPARKLE_PARAM_ENUM  (kParamDelayUnit, "Delay Unit", 0, "Beats", "ms", "s")
SPARKLE_PARAM_DOUBLE(kParamDelayRm,   "Delay Rm", 1.5, 0.1, 3., 0.01, "x")
SPARKLE_PARAM_DOUBLE(kParamDelaySm,   "Delay Sm", 0.75, 0.1, 3., 0.01, "x")

// --- §7.5 Pitch chain, cumulative --------------------------------------------------------------------
SPARKLE_PARAM_INT   (kParamRayInterval,   "Ray Interval", 2, -48, 48, "")
SPARKLE_PARAM_DOUBLE(kParamRayIntervalRm, "Ray Interval Rm", 1., 0.1, 3., 0.01, "x")

SPARKLE_PARAM_INT   (kParamInterval,   "Interval", 1, -48, 48, "")
SPARKLE_PARAM_DOUBLE(kParamIntervalRm, "Interval Rm", 1., 0.1, 3., 0.01, "x")
// Range widened to allow negative values (default below) -- intervalSm is always raised to an
// integer power (sparkle_n), so a negative base is well-defined (alternates sign per sparkle
// within a ray) rather than producing NaN.
SPARKLE_PARAM_DOUBLE(kParamIntervalSm, "Interval Sm", -1., -3., 3., 0.01, "x")

// --- §7.6 Panning ----------------------------------------------------------------------------------
// Commented out rather than deleted: MIDI has no real per-note pan (CC10 is per-channel, not
// per-voice), so there's no way to make this land well until a v2 with its own synth module can
// apply pan directly to audio instead of faking it over MIDI. sparkle_core::SparkleGenerator's pan
// computation (PanMode, width/phase/ray_rotation math, SparkleEvent::pan) is untouched and still
// fully tested by tests/test_sparkle_generator.cpp -- only the exposed IParams are disabled, so
// there's nothing live for a user to dial in that wouldn't do anything.
// SPARKLE_PARAM_ENUM  (kParamPanning, "Panning", 2, "Mono", "Random", "Sine", "Triangle", "Square", "Saw")
// SPARKLE_PARAM_DOUBLE(kParamWidth,   "Width", 1., 0., 2., 0.001, "")
// SPARKLE_PARAM_DOUBLE(kParamWidthRm, "Width Rm", 1., 0.1, 3., 0.01, "x")
// SPARKLE_PARAM_DOUBLE(kParamWidthSm, "Width Sm", 1., 0.1, 3., 0.01, "x")
//
// SPARKLE_PARAM_DOUBLE(kParamPhase,   "Phase", 0.25, 0., 1., 0.001, "")
// SPARKLE_PARAM_DOUBLE(kParamPhaseRm, "Phase Rm", 1., 0.1, 3., 0.01, "x")
// SPARKLE_PARAM_DOUBLE(kParamPhaseSm, "Phase Sm", 1.5, 0.1, 3., 0.01, "x")
//
// SPARKLE_PARAM_ENUM(kParamRayRotation,   "Ray Rotation", 0, "L", "R")
// SPARKLE_PARAM_ENUM(kParamRayRotationRm, "Ray Rotation Rm", 1, "Keep", "Invert")

// clang-format on

#undef SPARKLE_PARAM_DOUBLE
#undef SPARKLE_PARAM_INT
#undef SPARKLE_PARAM_ENUM
