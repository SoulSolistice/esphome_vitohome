#pragma once
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

namespace esphome::vitohome {

// Double-ended ring buffer whose capacity is fixed once, at setup().
//
// Replaces the three std::deque queues the hub pushed/popped in its run loop
// (read_queue_, write_queue_, raw_queue_). std::deque takes a >=512-byte control
// block and allocates in 512-byte chunks as it grows, so a deque mutated in
// loop() is heap allocation after setup() -- the guideline's canonical
// reliability bug on a memory-constrained target. This ring makes exactly ONE
// allocation, in reserve() during setup(), and never allocates again: every
// push/pop is O(1) with no allocation, so run-loop queue traffic can neither
// churn nor fragment the heap. (setup()-time allocation is fine -- the hub
// already make_unique's the protocol engine there; the rule is no allocation
// *after* setup.)
//
// Capacity is set at reserve() rather than at compile time so each lane is sized
// to its real need with no wasted BSS and no arbitrary ceiling: the entity lanes
// to the registered entity count (a device's full generated catalog can be many
// hundreds of datapoints, and that count is only known once registration
// finishes at setup), the raw lane to its scan-sweep cap. An entity is enqueued
// at most once (guarded by its read_queued_/write_queued_ flag), so sizing an
// entity lane to the entity count means it can never fill.
//
// The surface is the std::deque subset the hub used -- empty / full / size /
// front / push_back / push_front / pop_front. push_back/push_front return false
// and leave the buffer unchanged when full, so an overflow is a rejected element
// rather than undefined behaviour (the raw lane logs the drop; the entity lanes
// cannot fill). Before reserve() -- or when reserved to 0 -- capacity is 0: every
// push is rejected and the buffer reports empty, which is correct for a hub with
// nothing to enqueue on that lane. front()/pop_front() keep the non-empty
// precondition the deque calls they replace already assumed. T is stored by
// trivial copy (the hub uses a raw pointer and a POD RawOp).
template<typename T> class RingBuffer {
  static_assert(std::is_trivially_copyable<T>::value, "RingBuffer stores elements by trivial copy");

 public:
  // One-time sizing; call once in setup() before any push. capacity 0 is a
  // valid no-op (the lane is unused). Returns false only if the single backing
  // allocation could not be made (out of heap) -- the caller marks the component
  // failed. Uses nothrow new so an OOM is a false return, not an abort, under
  // ESPHome's exception-free build.
  bool reserve(std::size_t capacity) {
    if (capacity == 0) {
      this->capacity_ = 0;
      return true;
    }
    std::unique_ptr<T[]> slots(new (std::nothrow) T[capacity]());
    if (slots == nullptr)
      return false;
    this->slots_ = std::move(slots);
    this->capacity_ = capacity;
    this->head_ = 0;
    this->count_ = 0;
    return true;
  }

  bool empty() const { return this->count_ == 0; }
  bool full() const { return this->count_ == this->capacity_; }
  std::size_t size() const { return this->count_; }
  std::size_t capacity() const { return this->capacity_; }

  // Precondition: !empty(). Matches front() on the deque this replaces (every
  // call site already guards with empty() first).
  T &front() { return this->slots_[this->head_]; }
  const T &front() const { return this->slots_[this->head_]; }

  // Append at the tail. Returns false (buffer unchanged) when full.
  bool push_back(const T &value) {
    if (this->full())
      return false;
    this->slots_[this->slot_(this->count_)] = value;
    ++this->count_;
    return true;
  }

  // Prepend at the head. The write-ack read-back jumps ahead of the poll queue
  // this way. Returns false (buffer unchanged) when full.
  bool push_front(const T &value) {
    if (this->full())
      return false;
    this->head_ = this->head_ == 0 ? this->capacity_ - 1 : this->head_ - 1;
    this->slots_[this->head_] = value;
    ++this->count_;
    return true;
  }

  // Precondition: !empty().
  void pop_front() {
    this->head_ = this->slot_(1);
    --this->count_;
  }

 private:
  // Absolute slot index for the element `offset` positions ahead of head_.
  // head_ < capacity_ and offset <= capacity_, so head_ + offset < 2*capacity_
  // and a single conditional subtraction is the modulo (no division on the MCU).
  std::size_t slot_(std::size_t offset) const {
    const std::size_t i = this->head_ + offset;
    return i >= this->capacity_ ? i - this->capacity_ : i;
  }

  std::unique_ptr<T[]> slots_;
  std::size_t capacity_{0};
  std::size_t head_{0};
  std::size_t count_{0};
};

}  // namespace esphome::vitohome
