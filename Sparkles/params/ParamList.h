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
// Each inclusion site defines whichever of these four macros it cares about before the
// #include -- any left undefined expands to nothing here, and this file #undef's all four at the
// bottom, so a site never has to clean up after itself:
//
//   SPARKLE_PARAM_DOUBLE      (id, name, defaultVal, minVal, maxVal, step, label)
//   SPARKLE_PARAM_DOUBLE_CURVE(id, name, defaultVal, minVal, maxVal, step, label, curve)
//   SPARKLE_PARAM_INT         (id, name, defaultVal, minVal, maxVal, label)
//   SPARKLE_PARAM_ENUM        (id, name, defaultIdx, ...optionNames)
//
// SPARKLE_PARAM_DOUBLE_CURVE is identical to SPARKLE_PARAM_DOUBLE except for the trailing `curve`
// arg, passed straight through to an IParam::ShapePowCurve(curve) -- used on knobs where a plain
// linear response wastes most of the knob's travel on one end of the range (the wide ray/sparkle
// multiplier ranges, and the wide millisecond ranges on the tempo-synced and ADSR knobs below).
// Plain SPARKLE_PARAM_DOUBLE rows keep the default ShapeLinear.
//
// `id` is always the EParams enum name. Every consumer -- enum generation, the InitXxx() calls,
// and params/ParamSnapshot.h's hand-written GetParam(id) reads -- refers to a param by this same
// id, so adding/renaming/reordering a param only ever means editing this file (ParamSnapshot.h's
// per-field mapping still needs a matching line added by hand -- see that file's header comment
// for why that part isn't also macro-generated).

#ifndef SPARKLE_PARAM_DOUBLE
#define SPARKLE_PARAM_DOUBLE(id, name, defaultVal, minVal, maxVal, step, label)
#endif
#ifndef SPARKLE_PARAM_DOUBLE_CURVE
#define SPARKLE_PARAM_DOUBLE_CURVE(id, name, defaultVal, minVal, maxVal, step, label, curve)
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

// What output path(s) a fired sprinkle's events go to. Switching to Audio/Both opts into
// core/SynthEngine.h rendering sparkles directly to the plugin's own audio output instead of (or
// alongside) sending them out as MIDI. Same option order as Detection Mode below for a consistent
// UI, even though the two are independent axes (input trigger source vs. output path).
SPARKLE_PARAM_ENUM  (kParamOutputMode, "Output Mode", 1, "Audio", "MIDI", "Both")

// --- §2 Detection stage ---------------------------------------------------------------------------
// What input(s) can arm a trigger.
SPARKLE_PARAM_ENUM  (kParamDetectionMode, "Detection Mode", 1, "Audio", "MIDI", "Both")
SPARKLE_PARAM_ENUM  (kParamTriggerOn,     "Trigger On", 0, "Up", "Down", "Both")
SPARKLE_PARAM_DOUBLE(kParamThreshold,     "Audio Threshold", 50., 0., 100., 0.01, "%")
// MIDI equivalent of Threshold above: a note-on/off must meet this velocity to arm a trigger when
// Detection Mode is MIDI or Both (§2). Note-off velocity is usually 0 on real controllers, so this
// is checked against the velocity the note was originally played at, not the note-off's own data
// byte -- see Sparkles::ProcessMidiMsg's held-note-velocity tracking.
SPARKLE_PARAM_INT   (kParamVelocityDetect, "Min Trigger Velocity", 63, 1, 127, "")
// Envelope follower coefficient (§2): env = env*(1-reactiveness) + |in|*reactiveness, applied
// every sample. Expressed directly in this 0-1 coefficient space (not a %) since that's exactly
// what DetectionParams::reactiveness and the formula above consume -- no unit conversion needed
// at the read site.
SPARKLE_PARAM_DOUBLE(kParamReactiveness, "Envelope Reactiveness", 0.01, 0., 1., 0.0001, "")
// Minimum normalized pitch-tracker confidence (core/PitchTracker.h's autocorrelation score) a
// trigger needs before it fires -- crossings whose note can't be identified at least this
// confidently are dropped rather than guessed at. Same 0-1 space the tracker scores in, so no
// unit conversion at the read site (same rationale as reactiveness above).
SPARKLE_PARAM_DOUBLE(kParamConfidence,   "Note Detection Confidence", 0.6, 0., 1., 0.01, "")
// §2's detect_note_min/detect_note_max -- named kParamMinNote/kParamMaxNote to match the
// prototype's existing UI control tags. Displayed as a note name (e.g. "C3"), not a raw MIDI
// number -- see the SetDisplayFunc calls in Sparkles.cpp's constructor.
SPARKLE_PARAM_INT(kParamMinNote, "Min Detection Note", sparkle_params::kDefaultMinTriggerNote, sparkle_params::kMinTriggerableNote, sparkle_params::kMaxTriggerableNote, "")
SPARKLE_PARAM_INT(kParamMaxNote, "Max Detection Note", sparkle_params::kDefaultMaxTriggerNote, sparkle_params::kMinTriggerableNote, sparkle_params::kMaxTriggerableNote, "")

// --- §5.1 Key + scale quick-fill --------------------------------------------------------------------
// Sparkles::OnParamChange regenerates the note matrix from these two via sparkle_core::ApplyKeyScale.
// Excluding them from presets per §8 is still future work.
// The first 12 options' order matches sparkle_core::PitchClass exactly, see core/NoteMatrix.h. The
// trailing "Trigger Note" option is a 13th, non-PitchClass value (index kNumPitchClasses == 12):
// selecting it switches OnParamChange to sparkle_core::ApplyKeyScalePerColumn instead of
// ApplyKeyScale, so each column uses its own trigger pitch class as the scale root rather than one
// fixed root shared by all columns.
SPARKLE_PARAM_ENUM(kParamKeyRoot, "Key Root", 3, "A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "Trigger Note")
// Option order matches sparkle_core::Scale exactly, see core/NoteMatrix.h.
SPARKLE_PARAM_ENUM(kParamKeyScale, "Key Scale", 0,
  "Ionian", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Aeolian", "Locrian",
  "Harmonic Minor", "Melodic Minor", "Major Pentatonic", "Minor Pentatonic", "Blues", "Chromatic")

// --- §7.1 Structure --------------------------------------------------------------------------------
SPARKLE_PARAM_INT        (kParamNRays,             "Num Rays", 3, 1, 16, "")
SPARKLE_PARAM_INT        (kParamNSparklesPerRay,   "Sparkles Per Ray", 4, 1, 32, "")
SPARKLE_PARAM_DOUBLE_CURVE(kParamNSparklesPerRayRm, "Sparkles Per Ray Rm", 1.2, 0.1, 4., 0.01, "x", 2.)
SPARKLE_PARAM_INT        (kParamRangeMin,          "Min Output Note", 48, 0, 127, "")
SPARKLE_PARAM_INT        (kParamRangeMax,          "Max Output Note", 96, 0, 127, "")
// Option order matches sparkle_core::WrapMode exactly, see core/NoteMatrix.h.
SPARKLE_PARAM_ENUM       (kParamWrapMode, "Wrap Mode", 2, "Mirror", "Around", "Stop")

// --- §7.2 Trigger-to-sprinkle offset ----------------------------------------------------------------
// Pre Delay/Duration/Ray Delay/Delay below share one range, [0, 6000], with a steep pow curve (4):
// the same physical param stores a plain beats magnitude when its companion *Unit param is Beats,
// or a plain millisecond magnitude when Milliseconds -- exactly like today, just widened so 6000ms
// fits. Min is 0 (not the smallest note value) so "None"/0 beats stays reachable as an actual value,
// not just clamped up to the nearest note the first time a knob touches it. In the UI (Sparkles.cpp),
// these four get a bespoke ui/TimeMagnitudeControl.h instead of a plain IVKnobControl: in Beats mode
// it snaps to named note values (128th..16 beats, +dotted/triplet, plus a leading "None"=0) while
// still writing/reading this same continuous-beats param, so ray/sparkle multiplier math downstream
// is never rounded; in ms mode it behaves like a normal curved knob. Default 0 ("None") for Pre
// Delay -- the note-value list's leading entry.
SPARKLE_PARAM_DOUBLE_CURVE(kParamPreDelay, "Pre Delay", 0., 0., 6000., 0.001, "", 4.)
// Option order matches sparkle_core::TimeUnit exactly, see core/SparkleGenerator.h. "Seconds" is
// deliberately not offered here anymore -- Beats/ms cover every practical use, and dropping it
// keeps the note-value knob's two modes (see above) exhaustive.
SPARKLE_PARAM_ENUM  (kParamPreDelayUnit, "Pre Delay Unit", 0, "Beats", "ms")
// Absolute chromatic transpose, in real semitones (§7.2) -- unlike Ray Interval/Interval below,
// this does NOT step through the trigger note's eligible-notes list (whose steps aren't evenly
// spaced in semitones), so it's the only interval param that reliably gives an exact interval
// like "always start an octave above the trigger note" (set to 12). Shifts only the sprinkle's
// starting point, not which matrix column governs eligibility -- that's still the trigger note's
// own column. See sparkle_core::SparkleGenerator::Generate's `anchorNote` and
// sparkle_core::NoteMatrix::Walk's two-note overload.
SPARKLE_PARAM_INT   (kParamPreInterval, "Pre Interval", 0, -48, 48, "")

// --- §7.3 Base per-sparkle properties, evaluated directly --------------------------------------------
SPARKLE_PARAM_DOUBLE      (kParamVelocity,   "Velocity", 63., 1., 127., 1., "")
SPARKLE_PARAM_DOUBLE_CURVE(kParamLoudnessRm, "Velocity Rm", 0.7, 0.1, 4., 0.01, "x", 2.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamLoudnessSm, "Velocity Sm", 0.8, 0.1, 4., 0.01, "x", 2.)

SPARKLE_PARAM_DOUBLE_CURVE(kParamDuration,     "Duration", 0.25, 0., 6000., 0.001, "", 4.)
SPARKLE_PARAM_ENUM        (kParamDurationUnit, "Duration Unit", 0, "Beats", "ms")
SPARKLE_PARAM_DOUBLE_CURVE(kParamDurationRm,   "Duration Rm", 1., 0.1, 4., 0.01, "x", 2.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamDurationSm,   "Duration Sm", 0.9, 0.1, 4., 0.01, "x", 2.)

// --- §7.4 Timing chain, cumulative -------------------------------------------------------------------
SPARKLE_PARAM_DOUBLE_CURVE(kParamRayDelay,     "Ray Delay", 1., 0., 6000., 0.001, "", 4.)
SPARKLE_PARAM_ENUM        (kParamRayDelayUnit, "Ray Delay Unit", 0, "Beats", "ms")
SPARKLE_PARAM_DOUBLE_CURVE(kParamRayDelayRm,   "Ray Delay Rm", 1., 0.1, 4., 0.01, "x", 2.)

SPARKLE_PARAM_DOUBLE_CURVE(kParamDelay,     "Delay", 0.5, 0., 6000., 0.001, "", 4.)
SPARKLE_PARAM_ENUM        (kParamDelayUnit, "Delay Unit", 0, "Beats", "ms")
SPARKLE_PARAM_DOUBLE_CURVE(kParamDelayRm,   "Delay Rm", 0.9, 0.1, 4., 0.01, "x", 2.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamDelaySm,   "Delay Sm", 1.1, 0.1, 4., 0.01, "x", 2.)

// --- §7.5 Pitch chain, cumulative --------------------------------------------------------------------
// Unlike Pre Interval above, these step through the trigger note's eligible-notes list (§7.5) --
// still gated by the note matrix, not a raw semitone transpose.
SPARKLE_PARAM_INT         (kParamRayInterval,   "Ray Interval", 2, -48, 48, "")
SPARKLE_PARAM_DOUBLE_CURVE(kParamRayIntervalRm, "Ray Interval Rm", 0.9, -4., 4., 0.01, "x", 2.)

SPARKLE_PARAM_INT         (kParamInterval,   "Interval", 1, -48, 48, "")
SPARKLE_PARAM_DOUBLE_CURVE(kParamIntervalRm, "Interval Rm", 1., -4., 4., 0.01, "x", 2.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamIntervalSm, "Interval Sm", 1., -4., 4., 0.01, "x", 2.)

// --- §7.6 Panning ----------------------------------------------------------------------------------
SPARKLE_PARAM_ENUM  (kParamPanning, "Panning", 2, "Mono", "Random", "Sine", "Triangle", "Square", "Saw")
// Width > 1 already saturates/crops at the pan extremes -- see sparkle_core::SparkleGenerator::Pan's
// std::clamp(..., -1.0, 1.0). Range widened to 2 so that headroom is reachable from the knob.
SPARKLE_PARAM_DOUBLE      (kParamWidth,   "Width", 1., 0., 2., 0.001, "")
SPARKLE_PARAM_DOUBLE_CURVE(kParamWidthRm, "Width Rm", 1., 0.1, 4., 0.01, "x", 2.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamWidthSm, "Width Sm", 1., 0.1, 4., 0.01, "x", 2.)

// Phase Rm/Sm combine ADDITIVELY with the base Phase (phase + phaseRm*rayN + phaseSm*sparkleN, see
// sparkle_core::SparkleGenerator::Generate), not multiplicatively like every other _rm/_sm pair --
// so these stay plain linear step sizes rather than a pow-curved multiplier. Any value pushed
// outside [0,1) wraps to its decimal part naturally via Wave()'s own frac computation.
SPARKLE_PARAM_DOUBLE(kParamPhase,   "Phase", 0., 0., 1., 0.001, "")
SPARKLE_PARAM_DOUBLE(kParamPhaseRm, "Phase Rm", 0.5, 0.1, 2., 0.01, "x")
SPARKLE_PARAM_DOUBLE(kParamPhaseSm, "Phase Sm", 0.2, 0.1, 2., 0.01, "x")

SPARKLE_PARAM_ENUM(kParamRayRotation,   "Ray Rotation", 0, "L", "R")
SPARKLE_PARAM_ENUM(kParamRayRotationRm, "Ray Rotation Rm", 1, "Keep", "Invert")

// --- §7.7 Synth (Audio Output Mode only) ------------------------------------------------------------
// Continuous morph across the four classic waveforms: 0 = pure Sine, 1 = pure Triangle, 2 = pure
// Square, 3 = pure Saw, linearly crossfading the two neighbouring shapes at fractional values (e.g.
// 1.5 = half Triangle/half Square). One global knob -- unlike every other §7 property, this isn't
// resolved per (ray_n, sparkle_n), so it has no _rm/_sm modifiers. Displayed as a named blend (e.g.
// "Sine -> Tri 40%"), not a raw number -- see the SetDisplayFunc call in Sparkles.cpp's constructor.
SPARKLE_PARAM_DOUBLE(kParamWaveShape, "Sparkle Synth Wave Shape", 0.5, 0., 3., 0.001, "")

// ADSR envelope applied per sparkle voice in core/SynthEngine.h, in milliseconds (curved range,
// like the tempo-synced knobs above) -- deliberately not a beats/ms TimeParam like §7.4's delay
// family, since that would double this section's param count for a short percussive envelope where
// tempo-relative timing isn't the point. Sustain is a level (0-1), not a time.
SPARKLE_PARAM_DOUBLE_CURVE(kParamAttack,   "Sparkle Synth Attack", 5., 1., 6000., 1., "ms", 4.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamAttackRm, "Attack Rm", 1., 0.1, 4., 0.01, "x", 2.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamAttackSm, "Attack Sm", 1., 0.1, 4., 0.01, "x", 2.)

SPARKLE_PARAM_DOUBLE_CURVE(kParamDecay,   "Sparkle Synth Decay", 5., 1., 6000., 1., "ms", 4.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamDecayRm, "Decay Rm", 1., 0.1, 4., 0.01, "x", 2.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamDecaySm, "Decay Sm", 1., 0.1, 4., 0.01, "x", 2.)

SPARKLE_PARAM_DOUBLE      (kParamSustain,   "Sparkle Synth Sustain", 0.6, 0., 1., 0.001, "")
SPARKLE_PARAM_DOUBLE_CURVE(kParamSustainRm, "Sustain Rm", 1., 0.1, 4., 0.01, "x", 2.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamSustainSm, "Sustain Sm", 1., 0.1, 4., 0.01, "x", 2.)

SPARKLE_PARAM_DOUBLE_CURVE(kParamRelease,   "Sparkle Synth Release", 15., 1., 6000., 1., "ms", 4.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamReleaseRm, "Release Rm", 1., 0.1, 4., 0.01, "x", 2.)
SPARKLE_PARAM_DOUBLE_CURVE(kParamReleaseSm, "Release Sm", 1., 0.1, 4., 0.01, "x", 2.)

// clang-format on

#undef SPARKLE_PARAM_DOUBLE
#undef SPARKLE_PARAM_DOUBLE_CURVE
#undef SPARKLE_PARAM_INT
#undef SPARKLE_PARAM_ENUM
