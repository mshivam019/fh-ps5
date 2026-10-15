#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace diagnostic_trace {
struct Event {
  uint64_t timestamp, duration, id, argument, thread;
  uint32_t stage;
  char path[116];
};
static_assert(sizeof(Event) == 160);

// A failed producer lock or full queue drops an event instead of delaying the game.
// The single consumer copies a bounded batch, releases the lock, then performs I/O.
class Queue {
 public:
  static constexpr size_t kCapacity = 4096;
  bool Push(const Event& event) {
    if (lock_.test_and_set(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    bool accepted = size_ < kCapacity;
    if (accepted) {
      events_[(read_ + size_) % kCapacity] = event;
      ++size_;
    }
    lock_.clear(std::memory_order_release);
    if (!accepted) dropped_.fetch_add(1, std::memory_order_relaxed);
    return accepted;
  }
  size_t Drain(Event* output, size_t capacity) {
    if (lock_.test_and_set(std::memory_order_acquire)) return 0;
    const size_t count = size_ < capacity ? size_ : capacity;
    for (size_t i = 0; i < count; ++i) output[i] = events_[(read_ + i) % kCapacity];
    read_ = (read_ + count) % kCapacity;
    size_ -= count;
    lock_.clear(std::memory_order_release);
    return count;
  }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
 private:
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  std::atomic<uint64_t> dropped_{0};
  size_t read_ = 0, size_ = 0;
  Event events_[kCapacity];
};
}
