// clang++ -std=c++23 -O2 -pthread -fsanitize=address,undefined ps5/tests/trace_queue.cpp -o /tmp/trace-queue-test
#include "../app/diagnostic_trace_queue.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using diagnostic_trace::Event;
using diagnostic_trace::Queue;
static Queue queue;
int main() {
  Event batch[128];
  assert(queue.Drain(batch, 128) == 0);
  uint64_t next = 0;
  // Reuse the complete queue repeatedly, including beyond the old 65,536 limit.
  for (unsigned cycle = 0; cycle < 20; ++cycle) {
    const uint64_t first = next;
    for (size_t i = 0; i < Queue::kCapacity; ++i) {
      Event event{}; event.id = next++; event.argument = event.id ^ 0xfeed;
      assert(queue.Push(event));
    }
    Event overflow{};
    assert(!queue.Push(overflow));
    assert(queue.Drain(batch, 0) == 0);
    for (size_t i = 0; i < Queue::kCapacity;) {
      const size_t count = queue.Drain(batch, 128);
      assert(count > 0);
      for (size_t j = 0; j < count; ++j, ++i) {
        assert(batch[j].id == first + i);
        assert(batch[j].argument == ((first + i) ^ 0xfeed));
      }
    }
    assert(queue.Drain(batch, 128) == 0);
  }
  assert(queue.dropped() == 20);
  constexpr unsigned kProducers = 4, kPerProducer = 20000;
  std::array<std::vector<uint64_t>, kProducers> accepted;
  std::atomic<unsigned> finished{0};
  std::vector<uint64_t> received;
  std::thread consumer([&] {
    Event events[128];
    for (;;) {
      const bool all_finished = finished.load(std::memory_order_acquire) == kProducers;
      const size_t count = queue.Drain(events, 128);
      for (size_t i = 0; i < count; ++i) {
        assert(events[i].argument == (events[i].id ^ 0xcafe));
        received.push_back(events[i].id);
      }
      if (all_finished && !count) break;
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> producers;
  for (unsigned p = 0; p < kProducers; ++p) producers.emplace_back([&, p] {
    for (unsigned i = 0; i < kPerProducer; ++i) {
      Event event{}; event.id = p * kPerProducer + i;
      event.argument = event.id ^ 0xcafe;
      if (queue.Push(event)) accepted[p].push_back(event.id);
    }
    finished.fetch_add(1, std::memory_order_release);
  });
  for (auto& producer : producers) producer.join();
  consumer.join();
  std::vector<uint64_t> expected;
  for (auto& ids : accepted) expected.insert(expected.end(), ids.begin(), ids.end());
  std::sort(expected.begin(), expected.end());
  std::sort(received.begin(), received.end());
  assert(received == expected);
  assert(queue.dropped() - 20 == kProducers * kPerProducer - expected.size());
  puts("Trace queue: saturation, FIFO wraparound beyond 65536 events, concurrent integrity and exact drop accounting passed");
}
