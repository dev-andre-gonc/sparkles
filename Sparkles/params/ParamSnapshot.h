#pragma once

#include "Sparkles.h"
#include "core/DetectionParams.h"
#include "core/NoteMatrix.h"
#include "core/SparkleGenerator.h"

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
// (also not yet implemented; nothing currently regenerates the matrix from them either).
namespace sparkle_params
{
  struct ParamSnapshot
  {
    sparkle_core::DetectionParams detection;
    sparkle_core::SparkleParams sparkle;

    // §5.1 quick-fill selectors. Not consumed by sparkle_core::SparkleGenerator itself (that
    // takes a NoteMatrix, not these), so they don't belong nested under `sparkle` above.
    int keyRoot = 0; // sparkle_core::PitchClass
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
    d.triggerType = static_cast<sparkle_core::TriggerType>(plugin.GetParam(kParamTriggerType)->Int());
    d.threshold = plugin.GetParam(kParamThreshold)->Value() / 100.;
    d.reactiveness = plugin.GetParam(kParamReactiveness)->Value();
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

    s.loudness = plugin.GetParam(kParamLoudness)->Value();
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

    // Panning IParams are commented out in params/ParamList.h (no way to make MIDI-only panning
    // land well until a v2 synth module exists) -- so their SparkleParams fields are left at
    // whatever sparkle_core::SparkleParams's own defaults are (PanMode::Mono, width 0), not read
    // from IParams here. See that file's §7.6 comment.
    // s.panning = static_cast<sparkle_core::PanMode>(plugin.GetParam(kParamPanning)->Int());
    // s.width = plugin.GetParam(kParamWidth)->Value();
    // s.widthRm = plugin.GetParam(kParamWidthRm)->Value();
    // s.widthSm = plugin.GetParam(kParamWidthSm)->Value();
    //
    // s.phase = plugin.GetParam(kParamPhase)->Value();
    // s.phaseRm = plugin.GetParam(kParamPhaseRm)->Value();
    // s.phaseSm = plugin.GetParam(kParamPhaseSm)->Value();
    //
    // s.rayRotation = static_cast<sparkle_core::RayRotation>(plugin.GetParam(kParamRayRotation)->Int());
    // s.rayRotationRm = static_cast<sparkle_core::RayRotationMode>(plugin.GetParam(kParamRayRotationRm)->Int());

    snapshot.keyRoot = plugin.GetParam(kParamKeyRoot)->Int();
    snapshot.keyScale = static_cast<sparkle_core::Scale>(plugin.GetParam(kParamKeyScale)->Int());

    return snapshot;
  }
}
