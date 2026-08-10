#pragma once

#include "Sparkles.h"
#include "core/DetectionParams.h"
#include "core/NoteMatrix.h"
#include "core/SparkleGenerator.h"
#include "core/SynthEngine.h"

// Reads every IParam defined in params/ParamList.h into a plain-data snapshot the (framework-free)
// core/ generation code can consume, without core/ ever having to see an iPlug2 IParam.
//
// Unlike EParams/InitXxx (macro-generated straight from ParamList.h in Sparkles.h/.cpp), this
// per-field mapping is hand-written, because it isn't a mechanical 1:1: the four time-family
// params each pair a magnitude IParam with a separate *Unit enum IParam, collapsed here into one
// sparkle_core::TimeParam; and enum IParams need an explicit static_cast to their specific
// sparkle_core enum type. A generic per-id macro can't know either of those on its own, so
// BuildParamSnapshot() just lists every kParamXxx id by hand instead -- adding a param to
// ParamList.h means adding one matching line here too.
//
// The §5 note matrix and its 24 column/row toggles are deliberately NOT read here, and are not
// IParams at all: per docs/SPEC.md §8 they must survive preset changes untouched, and iPlug2
// doesn't offer a clean way to exclude specific IParams from preset load/save. So the matrix lives
// only in sparkle_core::NoteMatrix, to be persisted directly via the plugin's own state
// (de)serialization, entirely outside the preset mechanism (not yet implemented). key_root/
// key_scale (§5.1) ARE ordinary IParams below -- they're cheap, few, and worth having
// host-automatable even though, per §8, presets should eventually be made to leave them alone too
// (not yet implemented). Sparkles::OnParamChange regenerates the matrix from them directly (see
// sparkle_core::ApplyKeyScale), so they don't need to be read here.
namespace sparkle_params
{
  struct ParamSnapshot
  {
    sparkle_core::DetectionParams detection;
    sparkle_core::SparkleParams sparkle;

    // Which output path(s) FireSprinkle routes generated events to (§7's Output Mode) -- governs
    // both Sparkles::FireSprinkle (MIDI-schedule vs. synth-voice-schedule vs. both) and
    // Sparkles::ProcessBlock (whether the synth engine renders this block at all). Not nested under
    // `sparkle` above since sparkle_core::SparkleGenerator itself never reads it.
    sparkle_core::OutputMode outputMode = sparkle_core::OutputMode::Midi;

    // §7.7's global "Wave Shape" morph knob, consumed directly by sparkle_core::SynthEngine::Render
    // -- not per (ray_n, sparkle_n) like everything nested under `sparkle`, so it lives here instead.
    double waveShape = 0.0;

    // §5.1 quick-fill selectors. Not consumed by sparkle_core::SparkleGenerator itself (that
    // takes a NoteMatrix, not these), so they don't belong nested under `sparkle` above.
    int keyRoot = 0; // sparkle_core::PitchClass, or kNumPitchClasses for the "Trigger Note" option
    sparkle_core::Scale keyScale = sparkle_core::Scale::Ionian;
  };

  inline sparkle_core::TimeParam ReadTimeParam(const iplug::Plugin& plugin, int valueParamId, int unitParamId)
  {
    sparkle_core::TimeParam result;
    result.value = plugin.GetParam(valueParamId)->Value();
    result.unit = static_cast<sparkle_core::TimeUnit>(plugin.GetParam(unitParamId)->Int());
    return result;
  }

  inline ParamSnapshot BuildParamSnapshot(const iplug::Plugin& plugin)
  {
    ParamSnapshot snapshot;

    sparkle_core::DetectionParams& d = snapshot.detection;
    d.detectionMode = static_cast<sparkle_core::DetectionMode>(plugin.GetParam(kParamDetectionMode)->Int());
    d.triggerType = static_cast<sparkle_core::TriggerType>(plugin.GetParam(kParamTriggerOn)->Int());
    d.threshold = plugin.GetParam(kParamThreshold)->Value() / 100.;
    d.minVelocity = plugin.GetParam(kParamVelocityDetect)->Int();
    d.reactiveness = plugin.GetParam(kParamReactiveness)->Value();
    d.confidence = plugin.GetParam(kParamConfidence)->Value();
    d.detectNoteMin = plugin.GetParam(kParamMinNote)->Int();
    d.detectNoteMax = plugin.GetParam(kParamMaxNote)->Int();

    sparkle_core::SparkleParams& s = snapshot.sparkle;

    s.nRays = plugin.GetParam(kParamNRays)->Int();
    s.nSparklesPerRay = plugin.GetParam(kParamNSparklesPerRay)->Int();
    s.nSparklesPerRayRm = plugin.GetParam(kParamNSparklesPerRayRm)->Value();
    s.rangeMin = plugin.GetParam(kParamRangeMin)->Int();
    s.rangeMax = plugin.GetParam(kParamRangeMax)->Int();
    s.wrapMode = static_cast<sparkle_core::WrapMode>(plugin.GetParam(kParamWrapMode)->Int());

    s.preDelay = ReadTimeParam(plugin, kParamPreDelay, kParamPreDelayUnit);
    s.preInterval = plugin.GetParam(kParamPreInterval)->Int();

    s.loudness = plugin.GetParam(kParamVelocity)->Value();
    s.loudnessRm = plugin.GetParam(kParamLoudnessRm)->Value();
    s.loudnessSm = plugin.GetParam(kParamLoudnessSm)->Value();

    s.duration = ReadTimeParam(plugin, kParamDuration, kParamDurationUnit);
    s.durationRm = plugin.GetParam(kParamDurationRm)->Value();
    s.durationSm = plugin.GetParam(kParamDurationSm)->Value();

    s.rayDelay = ReadTimeParam(plugin, kParamRayDelay, kParamRayDelayUnit);
    s.rayDelayRm = plugin.GetParam(kParamRayDelayRm)->Value();

    s.delay = ReadTimeParam(plugin, kParamDelay, kParamDelayUnit);
    s.delayRm = plugin.GetParam(kParamDelayRm)->Value();
    s.delaySm = plugin.GetParam(kParamDelaySm)->Value();

    s.rayInterval = plugin.GetParam(kParamRayInterval)->Int();
    s.rayIntervalRm = plugin.GetParam(kParamRayIntervalRm)->Value();

    s.interval = plugin.GetParam(kParamInterval)->Int();
    s.intervalRm = plugin.GetParam(kParamIntervalRm)->Value();
    s.intervalSm = plugin.GetParam(kParamIntervalSm)->Value();

    s.panning = static_cast<sparkle_core::PanMode>(plugin.GetParam(kParamPanning)->Int());
    s.width = plugin.GetParam(kParamWidth)->Value();
    s.widthRm = plugin.GetParam(kParamWidthRm)->Value();
    s.widthSm = plugin.GetParam(kParamWidthSm)->Value();

    s.phase = plugin.GetParam(kParamPhase)->Value();
    s.phaseRm = plugin.GetParam(kParamPhaseRm)->Value();
    s.phaseSm = plugin.GetParam(kParamPhaseSm)->Value();

    s.rayRotation = static_cast<sparkle_core::RayRotation>(plugin.GetParam(kParamRayRotation)->Int());
    s.rayRotationRm = static_cast<sparkle_core::RayRotationMode>(plugin.GetParam(kParamRayRotationRm)->Int());

    s.attack = plugin.GetParam(kParamAttack)->Value();
    s.attackRm = plugin.GetParam(kParamAttackRm)->Value();
    s.attackSm = plugin.GetParam(kParamAttackSm)->Value();

    s.decay = plugin.GetParam(kParamDecay)->Value();
    s.decayRm = plugin.GetParam(kParamDecayRm)->Value();
    s.decaySm = plugin.GetParam(kParamDecaySm)->Value();

    s.sustain = plugin.GetParam(kParamSustain)->Value();
    s.sustainRm = plugin.GetParam(kParamSustainRm)->Value();
    s.sustainSm = plugin.GetParam(kParamSustainSm)->Value();

    s.release = plugin.GetParam(kParamRelease)->Value();
    s.releaseRm = plugin.GetParam(kParamReleaseRm)->Value();
    s.releaseSm = plugin.GetParam(kParamReleaseSm)->Value();

    snapshot.outputMode = static_cast<sparkle_core::OutputMode>(plugin.GetParam(kParamOutputMode)->Int());
    snapshot.waveShape = plugin.GetParam(kParamWaveShape)->Value();

    snapshot.keyRoot = plugin.GetParam(kParamKeyRoot)->Int();
    snapshot.keyScale = static_cast<sparkle_core::Scale>(plugin.GetParam(kParamKeyScale)->Int());

    return snapshot;
  }
}
