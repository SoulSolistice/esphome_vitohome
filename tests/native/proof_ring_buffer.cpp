// Host proof for components/vitohome/ring_buffer.h.
//
// RingBuffer replaces the three std::deque queues the hub pushed/popped in its
// run loop (read_queue_, write_queue_, raw_queue_) with a ring whose capacity is
// fixed by a single reserve() at setup(). This pins the semantics the call sites
// rely on:
//   - reserve() sizes the lane once; before it (capacity 0) every push is
//     rejected and the buffer reports empty,
//   - FIFO via push_back + front + pop_front (the poll/write/raw lanes all
//     consume front-to-back),
//   - push_front prepends ahead of the queue (the write-ack read-back path),
//   - full() rejects without corruption (the raw lane's RAW_QUEUE_MAX drop),
//   - the head/tail wraparound arithmetic is correct across the modulo boundary,
//   - a POD struct element (like RawOp) round-trips by trivial copy.
//
// Built under -fsanitize=address,undefined: any off-by-one in the wraparound, a
// push_front underflow, or a read of the unallocated backing would be an
// out-of-bounds access and trap here.
//
// Build: g++ -std=c++17 -Wall -Wextra -I<component root> proof_ring_buffer.cpp

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

// A POD stand-in for RawOp: exercises trivial-copy of a struct element, not just
// a pointer/scalar.
struct Op {
  uint16_t address;
  uint8_t bytes[4];
  bool is_write;
};

static void test_unreserved() {
  std::printf("== before reserve() / reserve(0): capacity 0 rejects ==\n");
  RingBuffer<int> q;
  check(q.empty() && q.full() && q.size() == 0 && q.capacity() == 0, "unreserved buffer is empty, full, capacity 0");
  check(!q.push_back(1) && !q.push_front(2), "every push is rejected before reserve()");
  check(q.size() == 0, "rejected pushes leave it empty");
  check(q.reserve(0) && q.capacity() == 0, "reserve(0) succeeds as a no-op (an unused lane)");
  check(!q.push_back(1), "reserve(0) still rejects pushes");
}

static void test_fifo_and_full() {
  std::printf("== FIFO fill / front / pop / full ==\n");
  RingBuffer<int> q;
  check(q.reserve(4), "reserve(4) succeeds");
  check(q.empty() && !q.full() && q.size() == 0, "reserved buffer starts empty");
  check(q.capacity() == 4, "capacity() reports the reserved size");

  check(q.push_back(10) && q.push_back(20) && q.push_back(30), "three push_back succeed");
  check(q.size() == 3 && !q.full(), "size tracks pushes");
  check(q.push_back(40), "fourth push_back fills it");
  check(q.full() && q.size() == 4, "full() true at capacity");

  check(!q.push_back(50), "push_back on a full buffer returns false");
  check(q.size() == 4 && q.front() == 10, "rejected push leaves the buffer unchanged");

  check(q.front() == 10, "front is the oldest element");
  q.pop_front();
  check(q.front() == 20 && q.size() == 3, "pop_front advances to the next-oldest");
  q.pop_front();
  q.pop_front();
  check(q.front() == 40 && q.size() == 1, "FIFO order preserved to the last element");
  q.pop_front();
  check(q.empty(), "buffer empty after draining every element");
}

static void test_push_front() {
  std::printf("== push_front (read-back ahead of the queue) ==\n");
  RingBuffer<int> q;
  q.reserve(4);
  q.push_back(1);
  q.push_back(2);
  q.push_front(9);  // 9 jumps ahead of 1, 2
  check(q.front() == 9 && q.size() == 3, "push_front prepends at the head");
  q.pop_front();
  check(q.front() == 1, "after the prepended element, FIFO order resumes");
  // Buffer holds [1, 2] (size 2) in a capacity-4 ring; two more pushes fill it.
  q.push_back(3);
  check(q.push_back(4) && q.full(), "mix of front/back fills to capacity");
  check(!q.push_front(8), "push_front on a full buffer returns false");
  check(q.front() == 1 && q.size() == 4, "rejected push_front leaves the buffer unchanged");
}

static void test_wraparound() {
  std::printf("== head/tail wraparound across the modulo boundary ==\n");
  // Capacity 3, run many fill/drain cycles at a rotating offset so head_ and the
  // computed tail cross the capacity boundary in every position. An OOB slot
  // access from a bad modulo would trap under ASan; the value checks pin order.
  RingBuffer<int> q;
  q.reserve(3);
  int next = 0;
  for (int cycle = 0; cycle < 50; cycle++) {
    // Push a rotating 1..3 elements, then drain them, verifying FIFO each time.
    const int n = (cycle % 3) + 1;
    int expect_first = next;
    for (int i = 0; i < n; i++)
      check(q.push_back(next++), "wrap: push");
    check(q.size() == static_cast<std::size_t>(n), "wrap: size after burst");
    for (int i = 0; i < n; i++) {
      check(q.front() == expect_first + i, "wrap: FIFO value");
      q.pop_front();
    }
    check(q.empty(), "wrap: drained");
  }

  // Interleaved push_front during wraparound: fill, pop one, prepend, ensure the
  // prepended element lands at the (wrapped) head slot, not out of bounds.
  q.push_back(100);
  q.push_back(101);
  q.push_back(102);
  q.pop_front();      // head advances; a slot frees at the tail side
  q.push_front(200);  // must wrap head_ down to the freed slot
  check(q.front() == 200 && q.size() == 3, "wrap: push_front reuses the wrapped slot");
  q.pop_front();
  check(q.front() == 101, "wrap: order intact after wrapped push_front");
}

static void test_pod_element() {
  std::printf("== POD struct element (RawOp-shaped) ==\n");
  RingBuffer<Op> q;
  q.reserve(2);
  q.push_back(Op{0x1234, {0xDE, 0xAD, 0xBE, 0xEF}, true});
  q.push_back(Op{0x5678, {0x01, 0x02, 0x03, 0x04}, false});
  check(q.full(), "POD ring fills");
  const Op &a = q.front();
  check(a.address == 0x1234 && a.bytes[0] == 0xDE && a.bytes[3] == 0xEF && a.is_write,
        "first POD element round-trips by trivial copy");
  q.pop_front();
  const Op &b = q.front();
  check(b.address == 0x5678 && b.bytes[1] == 0x02 && !b.is_write, "second POD element round-trips");
}

static void test_hub_pattern() {
  std::printf("== hub dispatch pattern (schedule -> dispatch -> read-back) ==\n");
  // Mirror how the hub drives read_queue_: entities are appended by the poll
  // scheduler, dispatched front-to-back, and a write-ack read-back is prepended
  // ahead of the pending polls.
  RingBuffer<int> reads;
  reads.reserve(8);
  for (int e = 1; e <= 5; e++)
    reads.push_back(e);  // poll cycle queues entities 1..5
  int dispatched = reads.front();
  reads.pop_front();  // dispatch entity 1
  check(dispatched == 1 && reads.front() == 2, "dispatch consumes the oldest poll");
  reads.push_front(99);  // a write to entity 99 acks -> read-back jumps the queue
  check(reads.front() == 99, "read-back is served before the remaining polls");
  reads.pop_front();
  check(reads.front() == 2 && reads.size() == 4, "remaining polls keep their order");
}

int main() {
  test_unreserved();
  test_fifo_and_full();
  test_push_front();
  test_wraparound();
  test_pod_element();
  test_hub_pattern();
  std::printf("ring_buffer proof: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
