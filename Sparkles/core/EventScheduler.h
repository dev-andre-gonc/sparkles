#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Fixed-capacity, allocation-free scheduling of note-on/note-off pairs by absolute sample
// position. Deliberately free of iPlug2/IGraphics dependencies (like core/SparkleGenerator.h) so
// it can be included by both the plugin and the standalone test binary, and deliberately
// independent of SparkleGenerator itself -- it schedules any note/velocity/duration triple, not
// specifically a SparkleEvent, so callers translate at the boundary.
namespace sparkle_core
{
  enum class SchedEventType
  {
    NoteOn,
    NoteOff
  };

  // One MIDI-ready event due within a single FlushBlock call, with its sample offset already
  // resolved relative to that block's start.
  struct SchedEvent
  {
    SchedEventType type = SchedEventType::NoteOn;
    int note = 0;
    int velocity = 0;      // unused (0) for NoteOff
    int offsetInBlock = 0; // in [0, nBlockSamples) of the FlushBlock call that produced this
  };

  // Fixed-capacity pool of pending note-ons plus a fixed-capacity note-off queue, both kept
  // sorted ascending by absolute sample position. RT-safe: every member function is
  // allocation-free (std::array storage only, sized at compile time) and runs in time bounded by
  // the pool capacities -- there is no dynamic growth anywhere.
  //
  // Usage: Schedule() each note once, at its absolute transport sample position. Every audio
  // block, call FlushBlock() with that block's [blockStart, blockStart + nBlockSamples) range; it
  // hands back that block's note-ons *and* any note-offs paired with note-ons scheduled in
  // earlier blocks (or even in this same call, for short durations), already interleaved in
  // ascending time order.
  template <size_t NoteOnCapacity = 1024, size_t NoteOffCapacity = 1024>
  class EventScheduler
  {
  public:
    static constexpr size_t kMaxPendingNoteOns = NoteOnCapacity;
    static constexpr size_t kMaxPendingNoteOffs = NoteOffCapacity;

    // Drops every pending note-on/note-off without emitting them (e.g. on transport stop).
    void Reset()
    {
      mNoteOnCount = 0;
      mNoteOffCount = 0;
    }

    size_t PendingNoteOnCount() const { return mNoteOnCount; }
    size_t PendingNoteOffCount() const { return mNoteOffCount; }

    // Schedules a note-on for `note`/`velocity` at absolute sample position `atSample`. Its
    // paired note-off (at atSample + durationSamples) is *not* added to the note-off queue yet --
    // it only enters that queue once the note-on itself is popped by FlushBlock, so a note
    // scheduled far in the future doesn't tie up note-off pool capacity until it actually fires.
    // Returns false, dropping the note entirely, if the note-on pool is already at capacity --
    // callers must treat this as a real possibility (e.g. an unusually dense sprinkle), not an
    // assert-worthy bug.
    bool Schedule(int note, int velocity, int64_t durationSamples, int64_t atSample)
    {
      if (mNoteOnCount >= NoteOnCapacity)
        return false;

      PendingNoteOn pending;
      pending.atSample = atSample;
      pending.offSample = atSample + std::max<int64_t>(0, durationSamples);
      pending.note = note;
      pending.velocity = velocity;

      InsertSorted(mNoteOns, mNoteOnCount, pending);
      return true;
    }

    // Pops every note-on/note-off due before `blockStart + nBlockSamples` into `outEvents`
    // (capacity `outCapacity`), in ascending absolute-time order -- ties are broken
    // note-off-before-note-on so a note retriggered on the same sample never appears to overlap
    // itself. A note-on due in this block has its paired note-off enqueued into the note-off pool
    // as it is popped, so a same-block note-off (zero/short duration) is correctly interleaved
    // and flushed here too, not deferred a block late.
    //
    // Events due before `blockStart` (which should not happen if FlushBlock is called for every
    // consecutive block with no gaps) get their offset clamped to 0 rather than going negative.
    //
    // Returns the number of events written. If more events are due than `outCapacity` allows, the
    // remainder is left pending in the pools for the next FlushBlock call -- never dropped.
    size_t FlushBlock(int64_t blockStart, int nBlockSamples, SchedEvent* outEvents, size_t outCapacity)
    {
      const int64_t blockEnd = blockStart + nBlockSamples;
      size_t count = 0;

      while (count < outCapacity)
      {
        const bool haveOn = mNoteOnCount > 0 && mNoteOns[0].atSample < blockEnd;
        const bool haveOff = mNoteOffCount > 0 && mNoteOffs[0].atSample < blockEnd;
        if (!haveOn && !haveOff)
          break;

        const bool takeOff = haveOff && (!haveOn || mNoteOffs[0].atSample <= mNoteOns[0].atSample);

        if (takeOff)
        {
          const PendingNoteOff due = mNoteOffs[0];
          RemoveFront(mNoteOffs, mNoteOffCount);
          outEvents[count++] = MakeEvent(SchedEventType::NoteOff, due.note, 0, blockStart, due.atSample, nBlockSamples);
        }
        else
        {
          const PendingNoteOn due = mNoteOns[0];
          RemoveFront(mNoteOns, mNoteOnCount);
          outEvents[count++] =
            MakeEvent(SchedEventType::NoteOn, due.note, due.velocity, blockStart, due.atSample, nBlockSamples);

          // Enqueue the paired note-off now that the note-on has actually fired. If the note-off
          // pool is full, the note-off is dropped -- callers must size NoteOffCapacity to at
          // least the expected number of simultaneously-sounding notes to avoid stuck notes.
          PendingNoteOff off;
          off.atSample = due.offSample;
          off.note = due.note;
          InsertSorted(mNoteOffs, mNoteOffCount, off);
        }
      }

      return count;
    }

    // Immediately treats every currently pending note-off as due right now (offset 0), regardless
    // of its scheduled sample position -- for a "kill everything currently sounding" action (e.g.
    // a panic/shut-up button), as opposed to FlushBlock's normal due-by-transport-time draining.
    // Every pending note-on is dropped without emitting anything, since it hasn't sounded yet and
    // so has no corresponding note-off to speak of. Like FlushBlock, only pops up to outCapacity
    // per call -- call in a loop until it returns fewer than outCapacity to fully drain both pools.
    size_t StopAll(SchedEvent* outEvents, size_t outCapacity)
    {
      mNoteOnCount = 0;

      size_t count = 0;
      while (count < outCapacity && mNoteOffCount > 0)
      {
        const PendingNoteOff due = mNoteOffs[0];
        RemoveFront(mNoteOffs, mNoteOffCount);

        SchedEvent event;
        event.type = SchedEventType::NoteOff;
        event.note = due.note;
        event.offsetInBlock = 0;
        outEvents[count++] = event;
      }
      return count;
    }

  private:
    struct PendingNoteOn
    {
      int64_t atSample = 0;
      int64_t offSample = 0;
      int note = 0;
      int velocity = 0;
    };

    struct PendingNoteOff
    {
      int64_t atSample = 0;
      int note = 0;
    };

    // Shared by both pools: insertion-sorts `item` into `pool[0..count)` by ascending atSample,
    // scanning from the back since new items are almost always at or near the newest (largest)
    // atSample already in the pool. No-op (silently drops `item`) if the pool is at capacity --
    // callers are responsible for checking/handling that (Schedule()'s return value; the note-off
    // pool has no equivalent signal, see FlushBlock's comment on that).
    template <typename T, size_t Capacity>
    static void InsertSorted(std::array<T, Capacity>& pool, size_t& count, const T& item)
    {
      if (count >= Capacity)
        return;

      size_t i = count;
      while (i > 0 && pool[i - 1].atSample > item.atSample)
      {
        pool[i] = pool[i - 1];
        --i;
      }
      pool[i] = item;
      ++count;
    }

    // Removes pool[0], shifting the remaining count-1 entries down by one.
    template <typename T, size_t Capacity>
    static void RemoveFront(std::array<T, Capacity>& pool, size_t& count)
    {
      for (size_t i = 1; i < count; ++i)
        pool[i - 1] = pool[i];
      --count;
    }

    static SchedEvent MakeEvent(SchedEventType type, int note, int velocity, int64_t blockStart, int64_t atSample,
                                 int nBlockSamples)
    {
      SchedEvent event;
      event.type = type;
      event.note = note;
      event.velocity = velocity;

      const int64_t maxOffset = (nBlockSamples > 0) ? (nBlockSamples - 1) : 0;
      const int64_t offset = std::clamp<int64_t>(atSample - blockStart, 0, maxOffset);
      event.offsetInBlock = static_cast<int>(offset);
      return event;
    }

    std::array<PendingNoteOn, NoteOnCapacity> mNoteOns{};
    size_t mNoteOnCount = 0;

    std::array<PendingNoteOff, NoteOffCapacity> mNoteOffs{};
    size_t mNoteOffCount = 0;
  };
}
