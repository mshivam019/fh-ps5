// Bounded append-only capture. Producers never allocate, wait, or write files.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <pthread.h>
#include <sys/stat.h>

namespace {
constexpr uint32_t kCapacity = 65536;
struct Event {
  uint64_t timestamp, duration, id, argument, thread;
  uint32_t stage;
  char path[116];
};
static_assert(sizeof(Event) == 160);
struct Slot { Event event; std::atomic<bool> ready{false}; };
Slot slots[kCapacity];
std::atomic<uint32_t> reserved{0};
std::atomic<bool> enabled{false};
std::atomic<uint64_t> dropped{0};
uint64_t now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
extern "C" bool rex_ps5_trace_enabled() { return enabled.load(std::memory_order_relaxed); }
extern "C" void rex_ps5_trace(uint32_t stage, uint64_t id, uint64_t argument,
                               uint64_t duration_ns, const char* path) {
  if (!rex_ps5_trace_enabled()) return;
  const uint32_t index = reserved.fetch_add(1, std::memory_order_relaxed);
  if (index >= kCapacity) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
  auto& slot = slots[index];
  auto& event = slot.event;
  event.timestamp = now(); event.duration = duration_ns; event.id = id;
  event.argument = argument; event.thread = (uint64_t)pthread_self(); event.stage = stage;
  if (path) {
    const size_t length = strnlen(path, sizeof(event.path) - 1);
    memcpy(event.path, path, length); event.path[length] = 0;
  }
  slot.ready.store(true, std::memory_order_release);
}
void StartDiagnosticTrace(const char* marker_path, const char* output_path) {
  FILE* marker = fopen(marker_path, "rb");
  if (!marker) return;
  fclose(marker);
  FILE* output = fopen(output_path, "wb");
  if (!output) { fprintf(stderr, "[frame trace] cannot open capture\n"); return; }
  const char magic[8] = {'F','H','T','R','A','C','E','1'};
  if (fwrite(magic, 1, sizeof(magic), output) != sizeof(magic)) { fclose(output); return; }
  fflush(output);
  enabled.store(true, std::memory_order_release);
  std::thread([output] {
    uint32_t consumed = 0;
    bool failed = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    fprintf(stderr, "[frame trace] enabled: 10 minutes, capacity=%u events, record_bytes=%zu\n", kCapacity, sizeof(Event));
    while (std::chrono::steady_clock::now() < end && consumed < kCapacity && !failed) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      const uint32_t limit = std::min(reserved.load(std::memory_order_acquire), kCapacity);
      while (consumed < limit && slots[consumed].ready.load(std::memory_order_acquire)) {
        if (fwrite(&slots[consumed].event, sizeof(Event), 1, output) != 1) { failed = true; break; }
        ++consumed;
      }
      if (fflush(output) != 0) failed = true;
    }
    enabled.store(false, std::memory_order_release);
    fclose(output);
    fprintf(stderr, "[frame trace] stopped: written=%u dropped=%llu write_failed=%d\n",
            consumed, (unsigned long long)dropped.load(), int(failed));
  }).detach();
}
