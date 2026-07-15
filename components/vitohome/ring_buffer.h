#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#ifdef VITOHOME_NATIVE_TEST
// Host test harness build (tests/native, which defines this macro): ESPHome
// headers are not on the include path, and esphome::Mutex has no out-of-line
// definitions on a bare host anyway (helpers.h declares them for "new
// platforms" without shipping a host implementation). The proofs are
// single-threaded, so no-op stand-ins preserve the locking API without
// pulling the framework in. Every device build takes the #else branch and
// uses the real ESPHome Mutex (FreeRTOS-backed on ESP32; an inline no-op on
// the single-threaded ESP8266/RP2040 platforms).
namespace esphome::vitohome {
class Mutex {
 public:
  Mutex() = default;
  Mutex(const Mutex &) = delete;
  Mutex &operator=(const Mutex &) = delete;
  void lock() {}
  bool try_lock() { return true; }
  void unlock() {}
};

class LockGuard {
 public:
  explicit LockGuard(Mutex &mutex) : mutex_(mutex) { mutex_.lock(); }
  ~LockGuard() { mutex_.unlock(); }

 private:
  Mutex &mutex_;
};
}  // namespace esphome::vitohome
#else
#include "esphome/core/helpers.h"
#endif

namespace esphome::vitohome {

// Double-ended, task-synchronized ring buffer whose capacity is fixed once,
// during setup().
//
// Replaces the three std::deque queues the hub previously pushed and popped in
// its run loop (read_queue_, write_queue_, raw_queue_). A deque may allocate
// additional storage as it grows; mutating one after setup() can therefore
// allocate from and fragment the heap on a memory-constrained target.
//
// This ring makes exactly one allocation for its element backing store, in
// reserve() during setup(), and never reallocates it. Every later push, peek,
// and pop is O(1), with no element-storage allocation.
//
// Synchronization is per RingBuffer instance. Separate instances own separate
// storage and do not share queue state. Concurrent task access to the same
// instance is serialized through ESPHome's Mutex abstraction.
//
// IMPORTANT: This is task/thread synchronized, not ISR-safe. A normal mutex
// must not be acquired from an interrupt handler. An ISR should instead signal
// or schedule work that accesses the queue from task/loop context.
//
// Capacity is set at reserve() rather than at compile time so each lane can be
// sized to its actual requirement without wasting BSS:
//
//   * Entity lanes are sized to the registered entity count.
//   * The raw lane is sized to its configured scan-sweep cap.
//
// Under normal hub operation an entity is enqueued at most once, guarded by its
// read_queued_ or write_queued_ flag. All callers must nevertheless check the
// result of push_back()/push_front(): a rejected push must roll back its
// companion bookkeeping flag rather than leave an entity permanently marked
// as queued.
//
// reserve() is one-shot. After a successful call, including reserve(0), later
// calls are rejected. A failed allocation leaves the buffer uninitialized so
// setup may retry or mark the component failed.
//
// Before reserve(), and after reserve(0), capacity is zero. The buffer reports
// both empty and full and rejects all pushes. This is the correct state for an
// unused lane.
//
// push_back() and push_front() return false and leave the buffer unchanged when
// full. Overflow is therefore a rejected element rather than undefined
// behavior.
//
// Reference-returning front() is deliberately not exposed. Returning a
// reference would let it escape after the queue mutex had been released.
// try_front() returns a synchronized copy. try_pop_front() atomically copies
// and removes the front item.
//
// consume_front_if() supports the hub's dispatch pattern. It holds the queue
// lock while the caller attempts to hand the front item to the protocol engine
// and removes the item only when that hand-off succeeds. This avoids the race
// inherent in a separate try_front() followed later by try_pop_front().
//
// T is stored in a preconstructed array and copied by trivial assignment. This
// keeps queue operations bounded and prevents constructors, destructors, or
// user-defined assignment operators from running in the hot path.
template<typename T> class RingBuffer {
  static_assert(std::is_trivially_copyable<T>::value, "RingBuffer requires trivially copyable elements");
  static_assert(std::is_trivially_default_constructible<T>::value,
                "RingBuffer requires trivially default-constructible elements");
  static_assert(std::is_trivially_copy_assignable<T>::value, "RingBuffer requires trivially copy-assignable elements");

 public:
  enum class ConsumeResult : uint8_t {
    EMPTY,
    RETAINED,
    REMOVED,
  };

  RingBuffer() = default;

  RingBuffer(const RingBuffer &) = delete;
  RingBuffer &operator=(const RingBuffer &) = delete;
  RingBuffer(RingBuffer &&) = delete;
  RingBuffer &operator=(RingBuffer &&) = delete;

  // One-time sizing; call during setup() before publishing the buffer to other
  // tasks.
  //
  // capacity == 0 is valid and permanently initializes an unused lane.
  //
  // Returns false if:
  //   * the buffer was already initialized;
  //   * the requested element count cannot be represented safely; or
  //   * the backing allocation failed.
  //
  // A failed first allocation leaves the buffer uninitialized and unchanged.
  [[nodiscard]] bool reserve(std::size_t capacity) {
    LockGuard lock(this->mutex_);

    if (this->initialized_)
      return false;

    if (capacity == 0) {
      this->slots_.reset();
      this->capacity_ = 0;
      this->head_ = 0;
      this->count_ = 0;
      this->initialized_ = true;
      return true;
    }

    if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(T))
      return false;

    // No trailing parentheses: slots do not need zero-initialization because a
    // slot is never read before a successful push has assigned it.
    std::unique_ptr<T[]> slots(new (std::nothrow) T[capacity]);
    if (slots == nullptr)
      return false;

    this->slots_ = std::move(slots);
    this->capacity_ = capacity;
    this->head_ = 0;
    this->count_ = 0;
    this->initialized_ = true;
    return true;
  }

  bool initialized() const {
    LockGuard lock(this->mutex_);
    return this->initialized_;
  }

  bool empty() const {
    LockGuard lock(this->mutex_);
    return this->count_ == 0;
  }

  // A zero-capacity buffer is intentionally both empty and full.
  bool full() const {
    LockGuard lock(this->mutex_);
    return this->count_ == this->capacity_;
  }

  std::size_t size() const {
    LockGuard lock(this->mutex_);
    return this->count_;
  }

  std::size_t capacity() const {
    LockGuard lock(this->mutex_);
    return this->capacity_;
  }

  [[nodiscard]] bool push_back(const T &value) {
    LockGuard lock(this->mutex_);

    if (this->count_ == this->capacity_)
      return false;

    this->slots_[this->slot_unlocked_(this->count_)] = value;
    ++this->count_;
    return true;
  }

  [[nodiscard]] bool push_front(const T &value) {
    LockGuard lock(this->mutex_);

    if (this->count_ == this->capacity_)
      return false;

    this->head_ = this->head_ == 0 ? this->capacity_ - 1 : this->head_ - 1;
    this->slots_[this->head_] = value;
    ++this->count_;
    return true;
  }

  // Copy, but do not remove, the current front item.
  //
  // Returns false and leaves `value` unchanged when empty. The returned value
  // is a snapshot and may become stale immediately after this method returns.
  [[nodiscard]] bool try_front(T &value) const {
    LockGuard lock(this->mutex_);

    if (this->count_ == 0)
      return false;

    value = this->slots_[this->head_];
    return true;
  }

  // Atomically copy and remove the current front item.
  //
  // Returns false and leaves `value` unchanged when empty.
  [[nodiscard]] bool try_pop_front(T &value) {
    LockGuard lock(this->mutex_);

    if (this->count_ == 0)
      return false;

    value = this->slots_[this->head_];
    this->head_ = this->slot_unlocked_(1);
    --this->count_;
    return true;
  }

  // Invoke `consumer(front)` while holding the queue mutex.
  //
  // The front item is removed only if consumer() returns true. The consumer
  // must remain short and must not call another method on this RingBuffer,
  // because ESPHome's Mutex is non-recursive.
  //
  // This is used for protocol dispatch: the engine copies the request
  // synchronously, so holding the queue lock over read()/write() is bounded and
  // prevents another consumer or push_front() from changing the observed item.
  template<typename Consumer> ConsumeResult consume_front_if(Consumer &&consumer) {
    LockGuard lock(this->mutex_);

    if (this->count_ == 0)
      return ConsumeResult::EMPTY;

    const T value = this->slots_[this->head_];
    if (!std::forward<Consumer>(consumer)(value))
      return ConsumeResult::RETAINED;

    this->head_ = this->slot_unlocked_(1);
    --this->count_;
    return ConsumeResult::REMOVED;
  }

 private:
  // Absolute slot index for the element `offset` positions ahead of head_.
  //
  // Preconditions maintained by locked callers:
  //   * capacity_ > 0
  //   * head_ < capacity_
  //   * offset <= capacity_
  //
  // This form avoids both division and overflow in head_ + offset. At most one
  // wrap is needed because offset <= capacity_.
  std::size_t slot_unlocked_(std::size_t offset) const {
    const std::size_t until_wrap = this->capacity_ - this->head_;

    if (offset >= until_wrap)
      return offset - until_wrap;

    return this->head_ + offset;
  }

  mutable Mutex mutex_;

  std::unique_ptr<T[]> slots_;
  std::size_t capacity_{0};
  std::size_t head_{0};
  std::size_t count_{0};
  bool initialized_{false};
};

}  // namespace esphome::vitohome
