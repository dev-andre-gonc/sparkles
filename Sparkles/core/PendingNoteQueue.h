#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Holds notes whose structure (pitch + timing, from SparkleGenerator::Generate()) is already
// resolved but whose properties (velocity/pan/duration/ADSR, from
// SparkleGenerator::ResolveEventProperties()) deliberately aren't yet -- see SparkleEvent's header
// comment in core/SparkleGenerator.h. A note sits here until its atSample actually arrives, at
// which point the caller resolves its properties against whatever SparkleParams is current *then*
// (rather than whatever it was at trigger time) and hands it off to EventScheduler::Schedule()/
// SynthEngine::ScheduleVoice() -- both of which, like this queue, only ever take fully-resolved
// values, so neither needed to change to support this.
//
// Deliberately free of iPlug2/IGraphics dependencies, and of SparkleParams itself, so it can be
// included by both the plugin and the standalone test binary -- same rationale as
// core/EventScheduler.h, whose fixed-capacity/insertion-sort-from-the-back style this mirrors.
namespace sparkle_core
{
  struct PendingNote
  {
    int64_t atSample = 0;
    int note = 0;
    int rayN = 0;
    int sparkleN = 0;
    int triggerNote = 0;
    int64_t timelineSample = 0;
    bool sendMidi = false;
    bool sendAudio = false;
  };

  // Fixed-capacity pool of pending notes, kept sorted ascending by atSample. RT-safe: every member
  // function is allocation-free (std::array storage only) and runs in time bounded by Capacity --
  // there is no dynamic growth anywhere.
  template <size_t Capacity = 1024>
  class PendingNoteQueue
  {
  public:
    void Reset() { mCount = 0; }

    size_t Count() const { return mCount; }

    // Schedules `note`'s properties to be resolved once `atSample` arrives. Returns false, dropping
    // the note entirely, if the pool is already at capacity -- same "must be handled, not
    // assert-worthy" contract as EventScheduler::Schedule.
    bool Push(int64_t atSample, int note, int rayN, int sparkleN, int triggerNote, int64_t timelineSample,
              bool sendMidi, bool sendAudio)
    {
      if (mCount >= Capacity)
        return false;

      PendingNote pending;
      pending.atSample = atSample;
      pending.note = note;
      pending.rayN = rayN;
      pending.sparkleN = sparkleN;
      pending.triggerNote = triggerNote;
      pending.timelineSample = timelineSample;
      pending.sendMidi = sendMidi;
      pending.sendAudio = sendAudio;

      // Insertion-sort by atSample, scanning from the back -- new pushes are almost always at or
      // near the newest (largest) atSample already pending, same rationale as
      // EventScheduler::InsertSorted.
      size_t i = mCount;
      while (i > 0 && mPending[i - 1].atSample > pending.atSample)
      {
        mPending[i] = mPending[i - 1];
        --i;
      }
      mPending[i] = pending;
      ++mCount;
      return true;
    }

    // Pops every note due before `blockEnd` (an absolute sample position) into `out` (capacity
    // `outCapacity`), in ascending atSample order. Mirrors EventScheduler::FlushBlock's due-by-time
    // draining: if more notes are due than `outCapacity` allows, the remainder is left pending for
    // the next call, never dropped.
    size_t PopDue(int64_t blockEnd, PendingNote* out, size_t outCapacity)
    {
      size_t count = 0;
      while (count < outCapacity && mCount > 0 && mPending[0].atSample < blockEnd)
      {
        out[count++] = mPending[0];
        for (size_t i = 1; i < mCount; ++i)
          mPending[i - 1] = mPending[i];
        --mCount;
      }
      return count;
    }

  private:
    std::array<PendingNote, Capacity> mPending{};
    size_t mCount = 0;
  };
}
