// Host proof for components/vitohome/ring_buffer.h.
//
// RingBuffer replaces the three std::deque queues the hub pushed/popped in its
// run loop (read_queue_, write_queue_, raw_queue_) with a task-synchronized
// ring whose capacity is fixed by a single reserve() at setup(). This pins the
// semantics the call sites rely on:
//   - reserve() is ONE-SHOT: it sizes the lane once (capacity 0 included) and
//     every later reserve() is rejected; before it, capacity is 0 and every
//     push is rejected while the buffer reports empty AND full,
//   - FIFO via push_back + try_front + try_pop_front (the poll/write/raw lanes
//     all consume front-to-back),
//   - push_front prepends ahead of the queue (the write-ack read-back path),
//     including into an empty ring and across the wrap boundary,
//   - full() rejects without corruption (the raw lane's over-capacity drop;
//     every rejected push leaves the buffer byte-identical),
//   - the clock-sync chain rides the entity lanes without ever being droppable:
//     push_front() puts the sync read ahead of pending polls, and a lane sized
//     to entities_.size() always has the clock's own slot (vito_clock.h),
//   - consume_front_if() returns EMPTY / RETAINED / REMOVED and removes the
//     front item exactly when the consumer accepted it -- the hub's dispatch
//     hand-off (engine busy = RETAINED and retried; accepted = REMOVED),
//   - the head/tail wraparound arithmetic is correct across the modulo
//     boundary,
//   - a POD struct element (like RawOp) round-trips by trivial copy.
//
// Built under -fsanitize=address,undefined: any off-by-one in the wraparound,
// a push_front underflow, or a read of the unallocated backing would be an
// out-of-bounds access and trap here.
//
// -DVITOHOME_NATIVE_TEST selects the header's no-op host mutex stand-in, so
// this proof needs no ESPHome include tree; device builds always use the real
// esphome::Mutex. The proof is single-threaded by design -- it proves the
// queue semantics, not the locking; the lock itself is ESPHome's audited
// primitive.
//
// Build (see build_and_run.sh):
//   g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined
//       -DVITOHOME_NATIVE_TEST -I<component root> proof_ring_buffer.cpp

#include <cstdint>
#include <cstdio>

#include "ring_buffer.h"

using esphome::vitohome::RingBuffer;

static int g_fail = 0;
static void check(bool ok, const char *what) {
  std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

// A POD stand-in for RawOp: exercises trivial-copy of a struct element, not
// just a pointer/scalar.
struct Op {
  uint16_t address;
  uint8_t bytes[4];
  bool is_write;
};

static void test_unreserved() {
  std::printf("== before reserve(): capacity 0 rejects everything ==\n");
  RingBuffer<int> q;
  int out = -1;
  check(!q.initialized(), "unreserved buffer reports uninitialized");
  check(q.empty() && q.full() && q.size() == 0 && q.capacity() == 0, "unreserved buffer is empty, full, capacity 0");
  check(!q.push_back(1) && !q.push_front(2), "every push is rejected before reserve()");
  check(!q.try_front(out) && !q.try_pop_front(out) && out == -1, "try_front/try_pop_front reject and leave out alone");
  check(q.consume_front_if([](int) { return true; }) == RingBuffer<int>::ConsumeResult::EMPTY,
        "consume_front_if reports EMPTY before reserve()");
}

static void test_reserve_one_shot() {
  std::printf("== reserve() is one-shot ==\n");
  {
    RingBuffer<int> q;
    check(q.reserve(0) && q.initialized() && q.capacity() == 0, "reserve(0) initializes an unused lane");
    check(!q.push_back(1), "reserve(0) still rejects pushes");
    check(!q.reserve(4), "a second reserve() after reserve(0) is rejected");
    check(q.capacity() == 0, "the rejected reserve() changed nothing");
  }
  {
    RingBuffer<int> q;
    check(q.reserve(4) && q.initialized() && q.capacity() == 4, "reserve(4) succeeds once");
    check(!q.reserve(8), "a second reserve() is rejected");
    check(q.capacity() == 4, "capacity is unchanged by the rejected reserve()");
    check(q.push_back(1), "the buffer stays usable after the rejected reserve()");
  }
}

static void test_fifo_and_full() {
  std::printf("== FIFO fill / try_front / try_pop_front / full ==\n");
  RingBuffer<int> q;
  check(q.reserve(4), "reserve(4) succeeds");
  check(q.empty() && !q.full() && q.size() == 0, "reserved buffer starts empty");
  check(q.capacity() == 4, "capacity() reports the reserved size");

  check(q.push_back(10) && q.push_back(20) && q.push_back(30), "three push_back succeed");
  check(q.size() == 3 && !q.full(), "size tracks pushes");
  check(q.push_back(40), "fourth push_back fills it");
  check(q.full() && q.size() == 4, "full() true at capacity");

  int out = -1;
  check(!q.push_back(50), "push_back on a full buffer returns false");
  check(q.size() == 4 && q.try_front(out) && out == 10, "rejected push leaves the buffer unchanged");

  check(q.try_front(out) && out == 10 && q.size() == 4, "try_front peeks without removing");
  check(q.try_pop_front(out) && out == 10, "try_pop_front returns the oldest element");
  check(q.try_front(out) && out == 20 && q.size() == 3, "pop advanced to the next-oldest");
  check(q.try_pop_front(out) && out == 20, "FIFO order holds");
  check(q.try_pop_front(out) && out == 30, "FIFO order holds");
  check(q.try_pop_front(out) && out == 40 && q.empty(), "buffer empty after draining every element");
  out = -1;
  check(!q.try_pop_front(out) && out == -1, "try_pop_front on empty rejects and leaves out alone");
}

static void test_push_front() {
  std::printf("== push_front (read-back ahead of the queue) ==\n");
  RingBuffer<int> q;
  check(q.reserve(4), "reserve(4) succeeds");

  // Into an EMPTY ring first: head wraps from 0 to capacity-1.
  check(q.push_front(7), "push_front into an empty ring succeeds");
  int out = -1;
  check(q.try_pop_front(out) && out == 7 && q.empty(), "the prepended element is the front");

  check(q.push_back(1) && q.push_back(2), "two push_back succeed");
  check(q.push_front(9), "push_front prepends ahead of 1, 2");
  check(q.try_front(out) && out == 9 && q.size() == 3, "push_front prepends at the head");
  check(q.try_pop_front(out) && out == 9, "prepended element pops first");
  check(q.try_front(out) && out == 1, "after the prepended element, FIFO order resumes");

  // Buffer holds [1, 2] (size 2) in a capacity-4 ring; two more pushes fill it.
  check(q.push_back(3) && q.push_back(4) && q.full(), "mix of front/back fills to capacity");
  check(!q.push_front(8), "push_front on a full buffer returns false");
  check(q.try_front(out) && out == 1 && q.size() == 4, "rejected push_front leaves the buffer unchanged");
}

static void test_consume_front_if() {
  std::printf("== consume_front_if (the dispatch hand-off) ==\n");
  using RB = RingBuffer<int>;
  RingBuffer<int> q;
  check(q.reserve(3), "reserve(3) succeeds");

  int seen = -1;
  check(q.consume_front_if([&](int v) {
    seen = v;
    return true;
  }) == RB::ConsumeResult::EMPTY &&
            seen == -1,
        "EMPTY on an empty ring; consumer never invoked");

  check(q.push_back(11) && q.push_back(22), "two elements queued");

  // Engine busy: consumer declines, the item must stay at the front.
  int calls = 0;
  check(q.consume_front_if([&](int v) {
    ++calls;
    seen = v;
    return false;
  }) == RB::ConsumeResult::RETAINED,
        "RETAINED when the consumer declines");
  int out = -1;
  check(calls == 1 && seen == 11, "consumer saw the front exactly once");
  check(q.size() == 2 && q.try_front(out) && out == 11, "declined item stays at the front");

  // Engine accepted: the item is removed, order behind it intact.
  check(q.consume_front_if([&](int v) {
    seen = v;
    return true;
  }) == RB::ConsumeResult::REMOVED,
        "REMOVED when the consumer accepts");
  check(seen == 11 && q.size() == 1, "the accepted item was the observed front");
  check(q.try_front(out) && out == 22, "the next element moved up");

  check(q.consume_front_if([](int) { return true; }) == RB::ConsumeResult::REMOVED, "drains to empty");
  check(q.consume_front_if([](int) { return true; }) == RB::ConsumeResult::EMPTY, "EMPTY once drained");
}

static void test_clock_chain_on_entity_lanes() {
  std::printf("== clock-sync chain on the read/write entity lanes ==\n");
  // Mirrors VitoClock's chain (components/vitohome/vito_clock.h) after it moved
  // off the raw scan-console lane and onto the ordinary entity lanes.
  //
  // The claim being pinned: GIVEN a lane reserved to the true entity count, the
  // chain cannot be dropped -- the read_queued_/write_queued_ flags admit each
  // entity at most once, so a slot for the clock always exists, unlike the
  // shared raw lane where a sweep in progress could fill the queue and starve a
  // mid-chain clock write.
  //
  // WHAT THIS DOES *NOT* PIN, and cannot. ENTITY_COUNT below is a literal, so
  // this exercises the lane mechanics for a correct count and says nothing
  // about how setup() derives that count. That gap was real: setup() sampled
  // entities_.size() one line ABOVE the VITOHOME_TIME_SYNC block that registers
  // the clock, reserved size() - 1, and rejected one due entity per boot on
  // VScotHO1_72 (2026-07-16) while this proof stayed green. The derivation
  // lives inside VitoHomeComponent::setup() and needs all of ESPHome, so it is
  // not reachable from a host proof; it is held instead by the ordering in
  // setup() and by VitoHomeComponent::lanes_sized_, which makes any
  // registration arriving after the sample loud rather than silent.
  //
  // Entities are modelled as int ids (the real lanes are
  // RingBuffer<VitoEntityBase *>); id 0 is the clock, 1..N are polled entities.
  using RB = RingBuffer<int>;
  constexpr int CLOCK = 0;
  constexpr int ENTITY_COUNT = 5;  // 4 polled entities + the clock

  RingBuffer<int> reads;
  RingBuffer<int> writes;
  check(reads.reserve(ENTITY_COUNT), "read lane reserved to entities_.size()");
  check(writes.reserve(ENTITY_COUNT), "write lane reserved to entities_.size()");

  // A poll cycle has the bus busy: every non-clock entity is queued.
  for (int e = 1; e < ENTITY_COUNT; e++)
    check(reads.push_back(e), "poll cycle queues an entity");
  check(reads.size() == 4, "four polls pending");

  // --- step 1: the sync read jumps the poll queue -------------------------
  // VitoClock::tick() -> request_priority_read(), which push_front()s. This is
  // what preserves the dispatch priority the raw lane used to provide: without
  // it the clock read would wait out a full poll cycle (~150 s on a real
  // catalog), which sync_on_boot would feel.
  check(reads.push_front(CLOCK), "sync read is pushed to the HEAD of the read lane");
  int front = -1;
  check(reads.try_front(front) && front == CLOCK, "clock is ahead of every pending poll");
  // 4 polls + the clock == 5 == capacity. The lane is exactly full: the ceiling
  // can be HIT but never exceeded, because the read_queued_ dedup flag stops
  // any entity being queued twice. Note what that does and does not show --
  // it shows the flags bound the lane at the entity count, not that setup()
  // reserves the right count (see the header note above).
  check(reads.full(), "lane exactly full: every entity queued once, clock included");
  check(reads.size() == ENTITY_COUNT, "no entity was displaced to make room");

  int dispatched = -1;
  check(reads.consume_front_if([&](int e) {
    dispatched = e;
    return true;
  }) == RB::ConsumeResult::REMOVED,
        "engine accepts the clock read");
  check(dispatched == CLOCK, "the dispatched item was the clock");
  check(reads.size() == 4, "the pending polls are untouched behind it");

  // --- step 2: drift exceeded -> write ------------------------------------
  // handle_response() -> set_write_payload_() -> request_write(), which
  // push_back()s onto the write lane. Writes already preempt reads, so no
  // priority handling is needed here.
  check(writes.push_back(CLOCK), "drift correction queues the clock write");
  check(writes.consume_front_if([&](int e) {
    dispatched = e;
    return true;
  }) == RB::ConsumeResult::REMOVED,
        "engine accepts the clock write");

  // --- step 3: verify via the ordinary write-ACK read-back ----------------
  // wants_read_back() is true, so the hub's ACK path calls
  // request_priority_read() for us -- the verify step is not bespoke code.
  check(reads.push_front(CLOCK), "write-ACK read-back pushes the clock to the head again");
  check(reads.try_front(front) && front == CLOCK, "verify read-back is ahead of the pending polls");
  check(reads.consume_front_if([](int) { return true; }) == RB::ConsumeResult::REMOVED, "engine accepts the verify");
  check(reads.size() == 4, "chain completed; the poll backlog is still intact");

  // --- the starvation mode that the raw lane had, and this one cannot ------
  // Fill the read lane completely (every entity queued exactly once -- the
  // read_queued_ flag makes this the true ceiling), then confirm the clock is
  // ALREADY in it rather than being locked out. On the shared raw lane a burst
  // of unrelated SCAN work could occupy every slot and drop the clock.
  check(reads.push_front(CLOCK), "clock queued");
  check(reads.full(), "lane at capacity == entities_.size(), clock included");
  check(reads.try_front(front) && front == CLOCK, "a saturated poll backlog cannot displace the clock");
}

static void test_wraparound() {
  std::printf("== head/tail wraparound across the modulo boundary ==\n");
  // Capacity 3, many fill/drain cycles at a rotating offset so head_ and the
  // computed tail cross the capacity boundary in every position. An OOB slot
  // access from a bad modulo would trap under ASan; the value checks pin order.
  RingBuffer<int> q;
  check(q.reserve(3), "reserve(3) succeeds");
  int next = 0;
  int out = -1;
  for (int cycle = 0; cycle < 50; cycle++) {
    const int n = (cycle % 3) + 1;
    const int expect_first = next;
    for (int i = 0; i < n; i++)
      check(q.push_back(next++), "wrap: push");
    check(q.size() == static_cast<std::size_t>(n), "wrap: size after burst");
    for (int i = 0; i < n; i++) {
      check(q.try_pop_front(out) && out == expect_first + i, "wrap: FIFO value");
    }
    check(q.empty(), "wrap: drained");
  }

  // Interleaved push_front during wraparound: fill, pop one, prepend, ensure
  // the prepended element lands at the (wrapped) head slot, not out of bounds.
  check(q.push_back(100) && q.push_back(101) && q.push_back(102), "wrap: refill");
  check(q.try_pop_front(out) && out == 100, "wrap: head advances");
  check(q.push_front(200), "wrap: push_front reuses the freed slot");
  check(q.try_pop_front(out) && out == 200 && q.size() == 2, "wrap: prepended element pops first");
  check(q.try_pop_front(out) && out == 101, "wrap: order intact after wrapped push_front");
}

static void test_pod_element() {
  std::printf("== POD struct element (RawOp-shaped) ==\n");
  RingBuffer<Op> q;
  check(q.reserve(2), "reserve(2) succeeds");
  check(q.push_back(Op{0x1234, {0xDE, 0xAD, 0xBE, 0xEF}, true}), "first POD push succeeds");
  check(q.push_back(Op{0x5678, {0x01, 0x02, 0x03, 0x04}, false}), "second POD push succeeds");
  check(q.full(), "POD ring fills");
  Op op{};
  check(q.try_pop_front(op) && op.address == 0x1234 && op.bytes[0] == 0xDE && op.bytes[3] == 0xEF && op.is_write,
        "first POD element round-trips by trivial copy");
  check(q.try_pop_front(op) && op.address == 0x5678 && op.bytes[1] == 0x02 && !op.is_write,
        "second POD element round-trips");
}

static void test_hub_pattern() {
  std::printf("== hub dispatch pattern (schedule -> dispatch -> read-back) ==\n");
  // Mirror how the hub drives read_queue_: entities are appended by the poll
  // scheduler, handed to the engine via consume_front_if (a busy engine
  // retains the front), and a write-ack read-back is prepended ahead of the
  // pending polls.
  using RB = RingBuffer<int>;
  RingBuffer<int> reads;
  check(reads.reserve(8), "reserve(8) succeeds");
  for (int e = 1; e <= 5; e++)
    check(reads.push_back(e), "poll cycle queues an entity");

  // First dispatch attempt: engine busy -> the front must survive.
  check(reads.consume_front_if([](int) { return false; }) == RB::ConsumeResult::RETAINED,
        "busy engine retains the front entity");

  int dispatched = -1;
  check(reads.consume_front_if([&](int e) {
    dispatched = e;
    return true;
  }) == RB::ConsumeResult::REMOVED &&
            dispatched == 1,
        "dispatch consumes the oldest poll");

  int out = -1;
  check(reads.try_front(out) && out == 2, "next poll moved up");
  check(reads.push_front(99), "a write ACKs -> read-back jumps the queue");
  check(reads.try_pop_front(out) && out == 99, "read-back is served before the remaining polls");
  check(reads.try_front(out) && out == 2 && reads.size() == 4, "remaining polls keep their order");
}

int main() {
  test_unreserved();
  test_reserve_one_shot();
  test_fifo_and_full();
  test_push_front();
  test_consume_front_if();
  test_clock_chain_on_entity_lanes();
  test_wraparound();
  test_pod_element();
  test_hub_pattern();
  std::printf("ring_buffer proof: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
