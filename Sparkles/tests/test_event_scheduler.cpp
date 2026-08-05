#include "../core/EventScheduler.h"
#include "test_framework.h"

using namespace sparkle_core;

namespace
{
  // Small helper: flushes one block into a fixed local buffer and returns it as a vector for
  // easy indexing/CHECKs (the vector is test-only scaffolding, not part of the RT-safe API).
  template <typename Scheduler>
  std::vector<SchedEvent> Flush(Scheduler& sched, int64_t blockStart, int nBlockSamples, size_t outCapacity = 64)
  {
    std::vector<SchedEvent> out(outCapacity);
    const size_t n = sched.FlushBlock(blockStart, nBlockSamples, out.data(), out.size());
    out.resize(n);
    return out;
  }
}

TEST(EventScheduler_NoteOnThenNoteOffInLaterBlock)
{
  EventScheduler<> sched;
  CHECK(sched.Schedule(/*note=*/60, /*velocity=*/100, /*durationSamples=*/500, /*atSample=*/100));

  // Block 0: [0, 512) contains the note-on (100) but not the note-off (600).
  auto block0 = Flush(sched, 0, 512);
  CHECK(block0.size() == 1);
  CHECK(block0[0].type == SchedEventType::NoteOn);
  CHECK(block0[0].note == 60);
  CHECK(block0[0].velocity == 100);
  CHECK(block0[0].offsetInBlock == 100);

  // Block 1: [512, 1024) contains the note-off (600 -> offset 88).
  auto block1 = Flush(sched, 512, 512);
  CHECK(block1.size() == 1);
  CHECK(block1[0].type == SchedEventType::NoteOff);
  CHECK(block1[0].note == 60);
  CHECK(block1[0].offsetInBlock == 88);
}

TEST(EventScheduler_NoteOnAndNoteOffInSameShortBlock)
{
  // A short-duration note whose on and off both land inside the same block must come back as
  // two separate, correctly-ordered events in one FlushBlock call, not deferred to the next.
  EventScheduler<> sched;
  CHECK(sched.Schedule(60, 100, /*durationSamples=*/20, /*atSample=*/50));

  auto events = Flush(sched, 0, 128);
  CHECK(events.size() == 2);
  CHECK(events[0].type == SchedEventType::NoteOn);
  CHECK(events[0].offsetInBlock == 50);
  CHECK(events[1].type == SchedEventType::NoteOff);
  CHECK(events[1].offsetInBlock == 70);
}

TEST(EventScheduler_EventExactlyAtBlockBoundary_BelongsToNextBlock)
{
  // Block ranges are half-open [blockStart, blockStart + n) -- a note-on scheduled exactly on the
  // boundary must NOT appear in the earlier block. Duration is long enough that the paired
  // note-off isn't also due in block1, keeping this test focused on the note-on boundary alone.
  EventScheduler<> sched;
  CHECK(sched.Schedule(60, 100, /*durationSamples=*/10000, /*atSample=*/512));

  auto block0 = Flush(sched, 0, 512);
  CHECK(block0.empty());

  auto block1 = Flush(sched, 512, 512);
  CHECK(block1.size() == 1);
  CHECK(block1[0].type == SchedEventType::NoteOn);
  CHECK(block1[0].offsetInBlock == 0);
}

TEST(EventScheduler_EventOneSampleBeforeBoundary_BelongsToEarlierBlock)
{
  EventScheduler<> sched;
  CHECK(sched.Schedule(60, 100, /*durationSamples=*/10000, /*atSample=*/511));

  auto block0 = Flush(sched, 0, 512);
  CHECK(block0.size() == 1);
  CHECK(block0[0].type == SchedEventType::NoteOn);
  CHECK(block0[0].offsetInBlock == 511);

  auto block1 = Flush(sched, 512, 512);
  CHECK(block1.empty());
}

TEST(EventScheduler_MultipleNotesStraddlingBoundary_NoneLostOrDuplicated)
{
  EventScheduler<> sched;
  // Interleave several notes around the 512-sample boundary, each with its own duration so their
  // note-offs also straddle later boundaries.
  CHECK(sched.Schedule(60, 100, 100, 0));   // on@0 off@100 -- entirely in block 0
  CHECK(sched.Schedule(61, 100, 100, 400)); // on@400 off@500 -- both still in block 0
  CHECK(sched.Schedule(62, 100, 50, 511));  // on@511 (last sample of block 0), off@561 (in block 1)
  CHECK(sched.Schedule(63, 100, 50, 512));  // on@512 (first sample of block 1), off@562 (in block 1)

  auto block0 = Flush(sched, 0, 512);
  CHECK(block0.size() == 5); // on60, off60, on61, off61, on62 -- note63 not yet due (512 >= blockEnd)
  CHECK(block0[0].type == SchedEventType::NoteOn && block0[0].note == 60 && block0[0].offsetInBlock == 0);
  CHECK(block0[1].type == SchedEventType::NoteOff && block0[1].note == 60 && block0[1].offsetInBlock == 100);
  CHECK(block0[2].type == SchedEventType::NoteOn && block0[2].note == 61 && block0[2].offsetInBlock == 400);
  CHECK(block0[3].type == SchedEventType::NoteOff && block0[3].note == 61 && block0[3].offsetInBlock == 500);
  CHECK(block0[4].type == SchedEventType::NoteOn && block0[4].note == 62 && block0[4].offsetInBlock == 511);

  auto block1 = Flush(sched, 512, 512);
  CHECK(block1.size() == 3); // on63, off62, off63
  CHECK(block1[0].type == SchedEventType::NoteOn && block1[0].note == 63 && block1[0].offsetInBlock == 0);
  CHECK(block1[1].type == SchedEventType::NoteOff && block1[1].note == 62 && block1[1].offsetInBlock == 49);
  CHECK(block1[2].type == SchedEventType::NoteOff && block1[2].note == 63 && block1[2].offsetInBlock == 50);

  // Every note produced exactly one on and one off across the two blocks -- nothing lost, nothing
  // duplicated.
  CHECK(sched.PendingNoteOnCount() == 0);
  CHECK(sched.PendingNoteOffCount() == 0);
}

TEST(EventScheduler_NoteOffDueBeforeBlockStart_ClampsToOffsetZero)
{
  // Defensive case: an event whose absolute sample position precedes blockStart (should not
  // normally happen with consecutive FlushBlock calls) must clamp to offset 0 rather than
  // producing a negative or out-of-range offset.
  EventScheduler<> sched;
  CHECK(sched.Schedule(60, 100, 10, /*atSample=*/5)); // on@5 off@15

  // Skip straight to a block starting after both on and off would have fired.
  auto events = Flush(sched, 100, 64);
  CHECK(events.size() == 2);
  CHECK(events[0].type == SchedEventType::NoteOn);
  CHECK(events[0].offsetInBlock == 0);
  CHECK(events[1].type == SchedEventType::NoteOff);
  CHECK(events[1].offsetInBlock == 0);
}

TEST(EventScheduler_SameSampleTieBreak_NoteOffBeforeNoteOn)
{
  // Two notes: the first's note-off lands on the exact same absolute sample as the second's
  // note-on. The off must be emitted first so a retriggered note is never seen as overlapping.
  EventScheduler<> sched;
  CHECK(sched.Schedule(60, 100, /*durationSamples=*/100, /*atSample=*/0));   // on@0 off@100
  CHECK(sched.Schedule(61, 100, /*durationSamples=*/100, /*atSample=*/100)); // on@100 off@200

  auto events = Flush(sched, 0, 256);
  CHECK(events.size() == 4);
  CHECK(events[0].type == SchedEventType::NoteOn && events[0].note == 60);
  CHECK(events[1].type == SchedEventType::NoteOff && events[1].note == 60 && events[1].offsetInBlock == 100);
  CHECK(events[2].type == SchedEventType::NoteOn && events[2].note == 61 && events[2].offsetInBlock == 100);
  CHECK(events[3].type == SchedEventType::NoteOff && events[3].note == 61 && events[3].offsetInBlock == 200);
}

TEST(EventScheduler_Reset_ClearsAllPendingState)
{
  EventScheduler<> sched;
  CHECK(sched.Schedule(60, 100, 500, 0));
  CHECK(sched.PendingNoteOnCount() == 1);

  sched.Reset();
  CHECK(sched.PendingNoteOnCount() == 0);
  CHECK(sched.PendingNoteOffCount() == 0);

  auto events = Flush(sched, 0, 512);
  CHECK(events.empty());
}

TEST(EventScheduler_NoteOnPoolExhaustion_ScheduleFailsGracefullyWithoutLosingExistingEvents)
{
  // Tiny capacity so exhaustion is trivial to reach: 2 pending note-ons, 8 pending note-offs.
  EventScheduler<2, 8> sched;

  CHECK(sched.Schedule(60, 100, 10, 0));
  CHECK(sched.Schedule(61, 100, 10, 10));
  CHECK(sched.PendingNoteOnCount() == 2);

  // Pool is now full -- further Schedule() calls must fail (return false) and must not disturb
  // the notes already accepted.
  CHECK(!sched.Schedule(62, 100, 10, 20));
  CHECK(!sched.Schedule(63, 100, 10, 30));
  CHECK(sched.PendingNoteOnCount() == 2);

  // The two accepted notes still flush correctly -- rejection of later Schedule() calls didn't
  // corrupt the pool.
  auto events = Flush(sched, 0, 64);
  CHECK(events.size() == 4); // on60, off60, on61, off61
  CHECK(events[0].note == 60 && events[0].type == SchedEventType::NoteOn);
  CHECK(events[2].note == 61 && events[2].type == SchedEventType::NoteOn);

  // Pool has room again after flushing -- capacity is reusable, not permanently exhausted.
  CHECK(sched.Schedule(64, 100, 10, 100));
  CHECK(sched.PendingNoteOnCount() == 1);
}

TEST(EventScheduler_NoteOffPoolExhaustion_DropsExcessNoteOffsWithoutCrashing)
{
  // Note-on pool sized for 8 concurrent notes, but the note-off pool can only hold 2 -- firing
  // all 8 note-ons in one block will overflow the note-off queue. The scheduler must not crash
  // or corrupt state; excess note-offs are simply dropped (documented behavior in EventScheduler.h).
  EventScheduler<8, 2> sched;

  for (int i = 0; i < 8; ++i)
    CHECK(sched.Schedule(60 + i, 100, /*durationSamples=*/1000, /*atSample=*/i));

  auto events = Flush(sched, 0, 64);

  // All 8 note-ons fire; only the first 2 successfully-enqueued note-offs remain pending
  // (insertion order into the note-off pool follows note-on flush order here since all 8 offs
  // share strictly increasing offSample values).
  int noteOnCount = 0;
  for (const auto& e : events)
    if (e.type == SchedEventType::NoteOn)
      ++noteOnCount;
  CHECK(noteOnCount == 8);
  CHECK(sched.PendingNoteOffCount() == 2);

  // The 2 that made it into the queue still flush cleanly on a later block.
  auto laterEvents = Flush(sched, 1000, 100);
  CHECK(laterEvents.size() == 2);
  for (const auto& e : laterEvents)
    CHECK(e.type == SchedEventType::NoteOff);
}

TEST(EventScheduler_FlushBlockOutputCapacityOverflow_DefersRemainderToNextFlush)
{
  // Four notes due in the same block, but the caller's output buffer only holds 2 -- the
  // remaining events must stay pending (not dropped) and come out on a subsequent FlushBlock
  // call for the *same* block range. Durations are made long enough that no note-off is due
  // within the block either, so the truncation is only exercised against the note-ons.
  EventScheduler<> sched;
  for (int i = 0; i < 4; ++i)
    CHECK(sched.Schedule(60 + i, 100, /*durationSamples=*/10000, /*atSample=*/i * 10));

  auto first = Flush(sched, 0, 512, /*outCapacity=*/2);
  CHECK(first.size() == 2);
  CHECK(first[0].note == 60);
  CHECK(first[1].note == 61);
  CHECK(sched.PendingNoteOnCount() == 2); // notes 62/63 still pending, not lost

  auto second = Flush(sched, 0, 512, /*outCapacity=*/2);
  CHECK(second.size() == 2);
  CHECK(second[0].note == 62);
  CHECK(second[1].note == 63);
  CHECK(sched.PendingNoteOnCount() == 0);
}

TEST(EventScheduler_TenSecondsOfBlocks_EveryNoteOnGetsExactlyOneNoteOff)
{
  // End-to-end simulation: drive real 512-sample ProcessBlock-sized chunks across a 10 second
  // window, scheduling a new trigger every 200ms as its sample position enters range (the way a
  // plugin would react to incoming envelope-crossing triggers), and confirm every note-on that
  // comes out of FlushBlock is eventually balanced by exactly one note-off -- none lost, none
  // duplicated, none left dangling.
  constexpr double kTestSampleRate = 44100.0;
  constexpr int kBlockSize = 512;
  constexpr int64_t kTotalSamples = static_cast<int64_t>(10.0 * kTestSampleRate);
  constexpr int64_t kTriggerIntervalSamples = static_cast<int64_t>(0.2 * kTestSampleRate); // 8820
  // Well under the trigger interval so consecutive notes never overlap -- keeps the "outstanding
  // note count never negative" check meaningful without needing per-note identity tracking.
  constexpr int64_t kNoteDurationSamples = static_cast<int64_t>(0.05 * kTestSampleRate);

  EventScheduler<> sched;
  std::array<SchedEvent, 64> buf;

  int64_t nextTriggerSample = 0;
  int triggersScheduled = 0;
  int noteOnsSeen = 0;
  int noteOffsSeen = 0;
  int outstanding = 0; // note-ons seen so far without a matching note-off yet

  auto drainBlock = [&](int64_t blockStart) {
    const size_t n = sched.FlushBlock(blockStart, kBlockSize, buf.data(), buf.size());
    for (size_t i = 0; i < n; ++i)
    {
      if (buf[i].type == SchedEventType::NoteOn)
      {
        ++noteOnsSeen;
        ++outstanding;
      }
      else
      {
        ++noteOffsSeen;
        --outstanding;
        CHECK(outstanding >= 0); // a note-off must never appear without a prior unmatched note-on
      }
    }
    return n;
  };

  int64_t blockStart = 0;
  while (blockStart < kTotalSamples)
  {
    while (nextTriggerSample < blockStart + kBlockSize && nextTriggerSample < kTotalSamples)
    {
      CHECK(sched.Schedule(60 + (triggersScheduled % 12), 100, kNoteDurationSamples, nextTriggerSample));
      ++triggersScheduled;
      nextTriggerSample += kTriggerIntervalSamples;
    }

    drainBlock(blockStart);
    blockStart += kBlockSize;
  }

  // The last trigger's note-off can land after the 10 second window -- keep flushing consecutive
  // blocks until both pools are empty rather than stopping exactly at kTotalSamples.
  while (sched.PendingNoteOnCount() > 0 || sched.PendingNoteOffCount() > 0)
  {
    const size_t n = drainBlock(blockStart);
    CHECK(n > 0); // pools non-empty but nothing due would mean a scheduling bug, not just a quiet block
    blockStart += kBlockSize;
  }

  CHECK(triggersScheduled == 50); // 10s at one trigger every 200ms
  CHECK(noteOnsSeen == triggersScheduled);
  CHECK(noteOffsSeen == triggersScheduled);
  CHECK(outstanding == 0);
}
