#include "../core/PendingNoteQueue.h"
#include "test_framework.h"

using namespace sparkle_core;

namespace
{
  // Small helper: pops one block into a fixed local buffer and returns it as a vector for easy
  // indexing/CHECKs (the vector is test-only scaffolding, not part of the RT-safe API) -- same
  // pattern as test_event_scheduler.cpp's Flush() helper.
  template <typename Queue>
  std::vector<PendingNote> PopDue(Queue& queue, int64_t blockEnd, size_t outCapacity = 64)
  {
    std::vector<PendingNote> out(outCapacity);
    const size_t n = queue.PopDue(blockEnd, out.data(), out.size());
    out.resize(n);
    return out;
  }
}

TEST(PendingNoteQueue_PopDue_OnlyReturnsNotesBeforeBlockEnd)
{
  PendingNoteQueue<> queue;
  CHECK(queue.Push(/*atSample=*/100, /*note=*/60, /*rayN=*/0, /*sparkleN=*/0, /*triggerNote=*/60,
                    /*timelineSample=*/0, /*sendMidi=*/true, /*sendAudio=*/false));
  CHECK(queue.Count() == 1);

  // Not yet due -- blockEnd (100) is exclusive, matching EventScheduler::FlushBlock's half-open
  // [blockStart, blockEnd) convention.
  auto notDue = PopDue(queue, 100);
  CHECK(notDue.empty());
  CHECK(queue.Count() == 1);

  auto due = PopDue(queue, 101);
  CHECK(due.size() == 1);
  CHECK(due[0].atSample == 100);
  CHECK(due[0].note == 60);
  CHECK(queue.Count() == 0);
}

TEST(PendingNoteQueue_PopDue_AscendingOrderAcrossMultiplePushes)
{
  // Pushed out of order -- PopDue must still return them sorted by atSample, same guarantee
  // EventScheduler::FlushBlock gives for note-ons.
  PendingNoteQueue<> queue;
  CHECK(queue.Push(300, 62, 1, 0, 60, 0, true, false));
  CHECK(queue.Push(100, 60, 0, 0, 60, 0, true, false));
  CHECK(queue.Push(200, 61, 0, 1, 60, 0, true, false));

  auto due = PopDue(queue, 1000);
  CHECK(due.size() == 3);
  CHECK(due[0].atSample == 100 && due[0].note == 60);
  CHECK(due[1].atSample == 200 && due[1].note == 61);
  CHECK(due[2].atSample == 300 && due[2].note == 62);
}

TEST(PendingNoteQueue_PopDue_LeavesRemainderPendingWhenOutCapacityIsSmall)
{
  // Mirrors EventScheduler::FlushBlock's "never dropped, just deferred" contract when the caller's
  // own output buffer is smaller than what's due.
  PendingNoteQueue<> queue;
  for (int i = 0; i < 5; i++)
    CHECK(queue.Push(i, 60 + i, 0, i, 60, 0, true, false));

  auto first = PopDue(queue, 1000, /*outCapacity=*/2);
  CHECK(first.size() == 2);
  CHECK(first[0].note == 60);
  CHECK(first[1].note == 61);
  CHECK(queue.Count() == 3);

  auto rest = PopDue(queue, 1000, /*outCapacity=*/64);
  CHECK(rest.size() == 3);
  CHECK(rest[0].note == 62);
  CHECK(rest[2].note == 64);
  CHECK(queue.Count() == 0);
}

TEST(PendingNoteQueue_Push_DropsOncePoolIsFull)
{
  PendingNoteQueue<2> tinyQueue;
  CHECK(tinyQueue.Push(0, 60, 0, 0, 60, 0, true, false));
  CHECK(tinyQueue.Push(10, 61, 0, 1, 60, 0, true, false));
  CHECK(!tinyQueue.Push(20, 62, 0, 2, 60, 0, true, false)); // pool already full
  CHECK(tinyQueue.Count() == 2);
}

TEST(PendingNoteQueue_Reset_DropsEverythingPending)
{
  PendingNoteQueue<> queue;
  CHECK(queue.Push(0, 60, 0, 0, 60, 0, true, false));
  CHECK(queue.Push(10, 61, 0, 1, 60, 0, true, false));
  CHECK(queue.Count() == 2);

  queue.Reset();
  CHECK(queue.Count() == 0);
  auto due = PopDue(queue, 1000);
  CHECK(due.empty());
}

TEST(PendingNoteQueue_CarriesRoutingAndContextFieldsThrough)
{
  // sendMidi/sendAudio (baked from Output Mode at trigger time) and triggerNote/timelineSample
  // (needed by ResolveEventProperties' pan random-mode hash) must survive the round trip
  // unchanged, same as rayN/sparkleN.
  PendingNoteQueue<> queue;
  CHECK(queue.Push(/*atSample=*/50, /*note=*/67, /*rayN=*/2, /*sparkleN=*/3, /*triggerNote=*/64,
                    /*timelineSample=*/123456, /*sendMidi=*/false, /*sendAudio=*/true));

  auto due = PopDue(queue, 51);
  CHECK(due.size() == 1);
  CHECK(due[0].note == 67);
  CHECK(due[0].rayN == 2);
  CHECK(due[0].sparkleN == 3);
  CHECK(due[0].triggerNote == 64);
  CHECK(due[0].timelineSample == 123456);
  CHECK(!due[0].sendMidi);
  CHECK(due[0].sendAudio);
}
