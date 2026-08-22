#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "params/ParamRanges.h"
#include "core/DetectionParams.h"
#include "core/EventScheduler.h"
#include "core/NoteMatrix.h"
#include "core/PitchTracker.h"
#include "core/SparkleGenerator.h"
#include "core/SynthEngine.h"
#include "ISender.h"
#include <array>
#include <atomic>
#include <vector>

#if IPLUG_EDITOR
#include "ui/BackgroundImageControl.h"
#include "ui/EnvelopeMeterControl.h"
#include "ui/ModifierValueControl.h"
#include "ui/NoteBarsControl.h"
#include "ui/NoteMatrixControl.h"
#include "ui/Palette.h"
#include "ui/TimeMagnitudeControl.h"
#include "ui/TriggerLightControl.h"
#include "ui/ValueDisplayControl.h"
#endif

const int kNumPresets = 1;

// EParams is generated from params/ParamList.h -- that file is the single source of truth for
// every param's id, name, range/options and default; see its header comment. Sparkles.cpp's
// constructor re-includes the same file to generate the matching InitXxx() calls.
#define SPARKLE_PARAM_DOUBLE(id, ...) id,
#define SPARKLE_PARAM_DOUBLE_CURVE(id, ...) id,
#define SPARKLE_PARAM_INT(id, ...) id,
#define SPARKLE_PARAM_ENUM(id, ...) id,
enum EParams
{
#include "params/ParamList.h"
  kNumParams
};

enum ECtrlTags
{
  kCtrlTagEnvelopeMeter = 0, // envelope level bar + threshold line, see ui/EnvelopeMeterControl.h
  kCtrlTagNoteDisplay,     // last detected note name, see ui/ValueDisplayControl.h
  kCtrlTagTriggerLight,    // blinks on each threshold crossing, see ui/TriggerLightControl.h
  kCtrlTagSprinkleCount,   // number of sprinkles currently sounding, see ui/ValueDisplayControl.h
  kCtrlTagNoteMatrix,      // §5 note-eligibility grid + column/row toggles, see ui/NoteMatrixControl.h
  kCtrlTagNoteBars,        // per-note confidence bars along the bottom edge, see ui/NoteBarsControl.h
  kCtrlTagShutUp,          // kills all in-flight sprinkles/rays/sparkles, see mShutUpRequested
  kCtrlTagKeyRoot,         // §5.1 key/scale quick-fill, hand-placed beside kCtrlTagNoteMatrix
  kCtrlTagKeyScale,
  kCtrlTagKeyMode,
  kCtrlTagQuickGuideImage, // placeholder for the Quick Guide tab's static image, see mLayoutFunc

  // One tag per tab-selector button plus the Presets button -- always visible regardless of
  // mActiveTab, unlike everything below. Colored IVButtonControl pills, one per
  // sparkle_palette::kTabColors entry -- see mLayoutFunc.
  kCtrlTagTabQuickGuide,
  kCtrlTagTabGeneral,
  kCtrlTagTabDetection,
  kCtrlTagTabPitchTiming,
  kCtrlTagTabNoteMatrix,
  kCtrlTagTabSynth,
  kCtrlTagPresets,

  // Every param in kParamGroups (see Sparkles.cpp) gets one tag here, in group/table order,
  // followed by one tag per group's labelled IVGroupControl frame -- both runs are assigned at
  // runtime as kCtrlTagFirstParamControl + index rather than hand-named, since the param table has
  // ~35 entries. See mLayoutFunc for how tags are computed and matched back up.
  kCtrlTagFirstParamControl
};

using namespace iplug;
using namespace igraphics;

class Sparkles final : public Plugin
{
public:
  Sparkles(const InstanceInfo& info);

#if IPLUG_EDITOR
  // PLUG_HOST_RESIZE is 0 (config.h) with MIN/MAX pinned to WIDTH/HEIGHT, so ConstrainEditorResize
  // (the base Plugin class's default impl -- just clamps to that fixed range) already rejects any
  // size but the one true one; no override needed. See config.h's comment and CLAUDE.md.
  bool OnHostRequestingSupportedViewConfiguration(int width, int height) override { return ConstrainEditorResize(width, height); }

private:
  // Which of EUITab (see Sparkles.cpp's anonymous namespace) is currently shown -- plain int here
  // so this header doesn't need EUITab's definition, cast at the few call sites that touch it.
  // Purely a UI presentation choice, not persisted/automatable.
  int mActiveTab = 1; // EUITab::General

  // Click-through index into the fixed factory preset list (docs/SPEC.md §8) -- the Presets button
  // has no dropdown/menu, each click just advances to (mPresetIndex + 1) % kNumFactoryPresets and
  // applies it, updating its own label to the newly-active preset's name.
  int mPresetIndex = 0;

  // Applies factory preset `idx`'s param values (General/Pitch&Timing/Synth tabs plus Key Mode,
  // per docs/SPEC.md §8 -- Key Root/Scale stay untouched) via SetParameterValue, then resyncs every
  // visible control bound to one of those params so the knobs update immediately instead of only on
  // the next redraw. SetParameterValue's own OnParamChange call regenerates the note matrix from
  // the newly-applied Key Mode exactly as a manual Mode-dropdown change would, so it's never stale.
  void ApplyPreset(int idx);

public:
#endif

#if IPLUG_DSP // http://bit.ly/2S64BDd
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void OnReset() override;
  void OnParamChange(int paramIdx) override;
  // Overridden (rather than just the 1-arg version above) so the Beats/ms unit-conversion branch
  // can tell a genuine UI-driven toggle (source == kUI) apart from iPlug2's own re-announcement of
  // every param's current value -- see OnParamReset in IPlugEditorDelegate.h, which iterates every
  // param and calls this on plugin construction/preset recall/etc. even though nothing actually
  // changed. Converting on those spurious calls too would silently rescale an already-correct
  // magnitude every time the plugin loads.
  void OnParamChange(int paramIdx, EParamSource source, int sampleOffset = -1) override;
  void OnIdle() override;

private:
  // Handles one queued incoming note-on/note-off at its sample-accurate position (see mMidiQueue
  // below). Resolves velocity gating and Up/Down/Both trigger-type routing the same way the audio
  // envelope crossings below do, then fires straight through FireSprinkle -- unlike the audio path,
  // the note is already known exactly, so there's no pitch tracker/confidence step to wait on.
  // `timelineSample` is the trigger's absolute position in the DAW project timeline (host
  // transport, not `triggerSample`'s plugin-internal scheduling clock) -- see FireSprinkle.
  void HandleMidiTrigger(const IMidiMsg& msg, int64_t triggerSample, int64_t timelineSample,
                          const sparkle_core::DetectionParams& detection,
                          const sparkle_core::SparkleParams& sparkleParams, sparkle_core::OutputMode outputMode,
                          double bpm, double sampleRate);

  // Incoming MIDI queued by ProcessMidiMsg (audio thread, called before ProcessBlock) and drained
  // sample-by-sample inside ProcessBlock's main loop, so a note-on/off lands on the same sample
  // offset it arrived at rather than being handled all at once at block start.
  IMidiQueue mMidiQueue;

  // Velocity a currently-held MIDI note was struck with, indexed by note number; -1 = not held.
  // Needed because a note-off's own velocity byte is usually 0 on real controllers, so Min Velocity
  // gating for a Down/end-of-note trigger has to look back at the note-on that started it instead
  // (see HandleMidiTrigger).
  std::array<int, 128> mHeldNoteVelocity;
  // How long an up-crossing waits for mPitchTracker to produce a confident note before the
  // trigger is dropped silently (§2) -- clean notes fire as soon as confidence clears the
  // Confidence param, so this is a ceiling, not a fixed wait like the old full-buffer one.
  static constexpr double kTriggerTimeoutSeconds = 0.05;

  // How long the tracker's last confident note stays usable after confidence collapses --
  // down-crossings resolve from this held note, since by the time the envelope falls the note
  // itself is fading or gone. Past this window a down-trigger is dropped instead.
  static constexpr double kNoteHoldSeconds = 0.25;

  // Re-runs mPitchTracker.Configure() from the current Min/Max Note params and sample rate.
  // Called on reset and whenever those params change. (Candidate-note bounds live in
  // params/ParamRanges.h since params/ParamList.h's kParamMinNote/kParamMaxNote need them too.)
  void ConfigurePitchTracker();

  // Called from OnParamChange whenever one of the four Beats/ms *Unit params (Pre Delay/Duration/
  // Ray Delay/Delay, §7.2/§7.4) flips, so the sibling magnitude param keeps representing the same
  // real duration across the switch instead of having its raw number reinterpreted under the new
  // unit. `unitParamIdx`'s value has already been updated by the time OnParamChange fires. No-ops
  // while mApplyingPreset is set -- see that flag's comment.
  void ConvertTimeMagnitudeUnit(int magnitudeParamIdx, int unitParamIdx);

  // Set around ApplyPreset's SetParameterValue loop (editor code, single-object build -- see
  // mShutUpRequested's comment below for why a DSP-declared member can be touched directly from
  // there). SetParameterValue unconditionally calls OnParamChange(idx, kUI) for every param it
  // touches, including the four *Unit params ApplyPreset sets alongside their magnitudes -- so
  // without this guard, ConvertTimeMagnitudeUnit would fire right after each magnitude is set to
  // its preset value and rescale it again as if it were a genuine Beats/ms toggle, corrupting Pre
  // Delay/Duration/Ray Delay/Delay on every single preset load. The `source != kUI` check above
  // doesn't catch this: ApplyPreset drives the exact same SetParameterValue path a real UI control
  // would, so its calls arrive as kUI too. Plain bool, not atomic -- ApplyPreset runs synchronously
  // on the UI thread and never yields mid-loop, so there's no concurrent reader to race.
  bool mApplyingPreset = false;

  // Launches one sprinkle for a resolved trigger: blinks the trigger light, enforces
  // kMaxSimultaneousSprinkles, generates the events and schedules them. The light fires here
  // (i.e. only for triggers that resolved to a confident note) rather than at the raw envelope
  // crossing -- a crossing the pitch tracker vetoes produces no sprinkle and no blink. It still
  // fires even when Generate() comes back empty (e.g. triggerNote's column has no eligible notes
  // under the current Key/Scale + note matrix, §5) -- the light reports "a trigger resolved",
  // not "a sprinkle sounded", so an out-of-scale note still gets visible feedback that it was
  // heard and rejected by the matrix rather than looking like detection missed it entirely.
  // `triggerSample` anchors this sprinkle's own event scheduling (mEventScheduler/mFlashScheduler/
  // mSynthEngine, all clocked off ProcessBlock's free-running mBlockStartSample). `timelineSample`
  // is a separate concept -- the trigger's absolute position in the DAW's project timeline (from
  // the host's transport, when available) -- fed only to SparkleGenerator::Generate() so
  // PanMode::Random can reproduce the same pan across playthroughs of the same project (§7.6).
  // `triggerCooloffMs` (sparkle_core::DetectionParams::triggerCooloffMs, §2) is enforced here --
  // the single funnel both the audio and MIDI trigger paths call through -- rather than at each
  // arming site, so a trigger arriving inside the cooloff window is dropped the same way
  // regardless of source. See mLastTriggerSample.
  void FireSprinkle(int triggerNote, int64_t triggerSample, int64_t timelineSample, const sparkle_core::SparkleParams& params,
                    sparkle_core::OutputMode outputMode, double bpm, double sampleRate, double triggerCooloffMs);

  // Set from the UI thread by the "Shut Up" button's action function (see mLayoutFunc), consumed
  // (and cleared) at the top of the next ProcessBlock call on the audio thread. A plain atomic
  // flag rather than routing through iPlug2's parameter-change/message plumbing -- this project
  // builds as a single non-distributed object where the UI and DSP share this instance directly
  // (see mNoteMatrix's cross-thread sharing below), and landing within the next block is plenty
  // fast for a manual "stop the sound" button.
  std::atomic<bool> mShutUpRequested{ false };

  // Kills every sprinkle/ray/sparkle currently in flight or still pending: sends MIDI All-Notes-
  // Off, drops every pending note-on/off in mEventScheduler, and cancels a trigger that was armed
  // but hadn't fired yet. Called from ProcessBlock when mShutUpRequested is consumed, and from
  // OnReset(). Unlike OnReset(), leaves pitch-tracking/envelope state alone -- this is "stop the
  // sound", not "reinitialize".
  void ShutUp();

  // Hard ceiling on sprinkles (trigger bursts) in flight at once -- a new trigger that arrives
  // once this many are still sounding is dropped entirely, rather than truncating any one
  // sprinkle's own rays/sparkles. A constant, not a user-facing param -- it exists purely to bound
  // MIDI/voice load, not as a creative control.
  static constexpr int kMaxSimultaneousSprinkles = 10;

  // One-pole envelope follower level (§2), updated every sample: mEnvelope = mEnvelope * (1 -
  // reactiveness) + |in| * reactiveness. The threshold crossing that arms mPendingNoteOn below is
  // checked against this, not the raw instantaneous input sample.
  double mEnvelope = 0.0;

  // Continuous pitch tracker (see core/PitchTracker.h's header comment): fed every input sample,
  // analyzes on its own hop cadence regardless of trigger state. Trigger resolution below only
  // queries it; the UI's note display mirrors it via mNoteSender.
  sparkle_core::PitchTracker mPitchTracker;

  // Set when an up-crossing arms a trigger whose pitch isn't confident yet. ProcessBlock then
  // polls the tracker each sample: the sprinkle fires the moment a confident hop lands at/after
  // mTriggerArmTime (one hop of slack, so a note already confidently sounding fires immediately),
  // or the trigger is dropped silently once mTriggerDeadline passes. Down-crossings never arm
  // this -- they resolve immediately from the tracker's held note (see kNoteHoldSeconds above).
  // Both times are on mPitchTracker.Now()'s clock.
  bool mTriggerPending = false;
  int64_t mTriggerArmTime = 0;
  int64_t mTriggerDeadline = 0;

  // triggerSample of the last trigger FireSprinkle actually accepted, on the same free-running
  // clock as blockStart/triggerSample (see FireSprinkle) -- a new trigger arriving less than
  // triggerCooloffMs after this is dropped there (§2). Sentinel far below any real triggerSample
  // so the very first trigger after construction/OnReset is never blocked by cooloff.
  static constexpr int64_t kNoLastTrigger = -1'000'000'000LL;
  int64_t mLastTriggerSample = kNoLastTrigger;

  // §5 note-eligibility matrix, edited directly by ui/NoteMatrixControl.h and regenerated from
  // kParamKeyRoot/kParamKeyScale/kParamKeyMode in OnParamChange (§5.1). Not persisted yet across plugin
  // save/reload (see params/ParamSnapshot.h's header comment) -- default-constructed, which leaves
  // every cell/row/column enabled.
  sparkle_core::NoteMatrix mNoteMatrix;

  sparkle_core::EventScheduler<> mEventScheduler;

  // Purely visual: schedules a note-matrix cell flash for the sample each individual sparkle event
  // actually fires at, independent of mEventScheduler/mSynthEngine above and populated regardless
  // of Output Mode (see FireSprinkle) so a cell flashes when its note is actually heard rather than
  // all at once when the sprinkle is generated. `note` is repurposed to carry the flashing cell,
  // packed as `column * sparkle_core::kNumPitchClasses + row` (see ProcessBlock's flush loop for the
  // unpack) -- velocity/duration are unused (0) since there's no matching "note off" to schedule.
  sparkle_core::EventScheduler<> mFlashScheduler;

  // Audio Output Mode's voice pool (§7.7) -- fed alongside or instead of mEventScheduler above,
  // depending on Output Mode, see FireSprinkle. Rendered directly into ProcessBlock's output
  // buffer rather than sent as MIDI.
  sparkle_core::SynthEngine<> mSynthEngine;

  // Scratch buffer for SparkleGenerator::Generate() output, reused block-to-block so its capacity
  // (reserved to SparkleGenerator::kMaxEventsPerTrigger on first use) doesn't need re-allocating
  // on every trigger.
  std::vector<sparkle_core::SparkleEvent> mScratchEvents;

  // Absolute sample position of the start of the current ProcessBlock call, counted from the last
  // OnReset() -- independent of host transport, since EventScheduler just needs a monotonically
  // increasing, gap-free position to schedule against.
  int64_t mBlockStartSample = 0;

  // One entry per in-flight sprinkle, holding the absolute sample position of its last note-off
  // (i.e. when it finishes sounding). Reaped (entries past their end sample dropped) each time a
  // new trigger arrives, so mNumActiveSprinkles reflects sprinkles still actually sounding at that
  // moment -- used to enforce kMaxSimultaneousSprinkles without needing EventScheduler to tag
  // individual note events by originating sprinkle.
  std::array<int64_t, kMaxSimultaneousSprinkles> mActiveSprinkleEndSamples{};
  int mNumActiveSprinkles = 0;

  // Drops entries from mActiveSprinkleEndSamples that have finished sounding by nowSample. Called
  // both when a new trigger arrives (to know whether it's under kMaxSimultaneousSprinkles) and
  // once per block purely so mSprinkleCountSender reports a live count instead of one that only
  // ever shrinks at the next trigger.
  void ReapFinishedSprinkles(int64_t nowSample);

  // Visual-indicator senders (see the UI: Sparkles/Visual Indicators docs comment above OnIdle's
  // impl in Sparkles.cpp). Declared alongside the DSP state they mirror since PushData() is called
  // from ProcessBlock; TransmitData() drains them from OnIdle on the main thread.
  ISender<1> mEnvelopeSender;
  ISender<2> mNoteSender; // {stable note (-1 = none), its confidence 0-1}, pushed once per block
  ISender<sparkle_params::kNumTriggerableNotes> mNoteBarsSender; // per-note confidences, once per block
  ISender<1> mTriggerSender;
  ISender<1> mSprinkleCountSender;
  // {column, row} pitch-class pair. row >= 0, once per distinct cell that fired a note-on within
  // the current block (see mFlashScheduler and ProcessBlock's flush loop), flashes that cell in
  // ui/NoteMatrixControl.h at the moment the note is actually heard. row == -1, pushed immediately
  // from FireSprinkle, flashes only that column's header at the moment the sparkle is created --
  // see ui/NoteMatrixControl.h's OnMsgFromDelegate for both conventions.
  ISender<2> mNoteMatrixFlashSender;
#endif
};
