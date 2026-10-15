// Bounded reusable capture. Producers never allocate, wait, or write files.
#include "diagnostic_trace_queue.h"
#include <rex/diagnostic_trace.h>
#include <cerrno>
#include <sys/resource.h>
#include <time.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <pthread.h>

namespace {
using diagnostic_trace::Event;
diagnostic_trace::Queue queue;
std::atomic<bool> enabled{false};
std::atomic<double> ns_per_tick{0};
uint64_t now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
extern "C" bool rex_ps5_trace_enabled() { return enabled.load(std::memory_order_relaxed); }
extern "C" double rex_ps5_trace_ns_per_tick() {
  return ns_per_tick.load(std::memory_order_relaxed);
}
extern "C" uint64_t rex_ps5_trace_thread_cpu_time(uint32_t* method) {
  static thread_local int selected = -1;
  auto select = [&](int value, int error) {
    if (selected != value) {
      selected = value;
      rex_ps5_trace(41, uint64_t(value), uint64_t(error), 0, nullptr);
      fprintf(stderr, "[frame trace] thread CPU clock method=%d error=%d (1=clock,2=rusage,0=unavailable)\n", value, error);
    }
    *method = uint32_t(value);
  };
  if (selected == 0) { *method = 0; return 0; }
  if (selected != 2) {
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0) {
      select(1, 0);
      return uint64_t(time.tv_sec) * 1000000000 + uint64_t(time.tv_nsec);
    }
  }
  rusage usage{};
  if (getrusage(RUSAGE_THREAD, &usage) == 0) {
    select(2, 0);
    return (uint64_t(usage.ru_utime.tv_sec) + uint64_t(usage.ru_stime.tv_sec)) * 1000000000 +
           (uint64_t(usage.ru_utime.tv_usec) + uint64_t(usage.ru_stime.tv_usec)) * 1000;
  }
  select(0, errno);
  return 0;
}
extern "C" void rex_ps5_trace(uint32_t stage, uint64_t id, uint64_t argument,
                               uint64_t duration_ns, const char* path) {
  if (!rex_ps5_trace_enabled()) return;
  Event event;
  event.timestamp = now(); event.duration = duration_ns; event.id = id;
  event.argument = argument; event.thread = (uint64_t)pthread_self(); event.stage = stage;
  memset(event.path, 0, sizeof(event.path));
  if (path) {
    const size_t length = strnlen(path, sizeof(event.path) - 1);
    memcpy(event.path, path, length);
  }
  queue.Push(event);
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
  const uint64_t start_ticks = rex::diagnostic::ActivityTicks();
  const uint64_t start_ns = now();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const uint64_t elapsed_ns = now() - start_ns;
  const uint64_t elapsed_ticks = rex::diagnostic::ActivityTicks() - start_ticks;
  if (!elapsed_ticks || !elapsed_ns) { fclose(output); return; }
  const double scale = double(elapsed_ns) / double(elapsed_ticks);
  ns_per_tick.store(scale, std::memory_order_relaxed);
  Event config{};
  config.timestamp = now(); config.stage = 18;
  config.id = rex::diagnostic::kFrameCategories;
  config.argument = uint64_t(1000000000.0 / scale);
  queue.Push(config);
  fprintf(stderr, "[frame trace] calibrated ticks_hz=%llu categories=%u\n",
          (unsigned long long)config.argument, rex::diagnostic::kFrameCategories);
  enabled.store(true, std::memory_order_release);
  std::thread([output] {
    constexpr uint64_t kMaxEvents = (64 * 1024 * 1024 - 8) / sizeof(Event);
    Event batch[128];
    uint64_t written = 0;
    bool failed = false;
    auto next_health = std::chrono::steady_clock::now();
    const auto end = next_health + std::chrono::minutes(10);
    fprintf(stderr, "[frame trace] enabled: reusable_queue=%zu events, max_file=64MiB, duration=10min\n",
            diagnostic_trace::Queue::kCapacity);
    while (std::chrono::steady_clock::now() < end && written + 2 < kMaxEvents && !failed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      size_t count;
      // Limit work per drain cycle so a producer flood cannot starve this thread.
      for (unsigned batches = 0; batches < 32; ++batches) {
        const size_t room = size_t(kMaxEvents - written - 2);
        count = queue.Drain(batch, room < 128 ? room : 128);
        if (!count) break;
        if (fwrite(batch, sizeof(Event), count, output) != count) { failed = true; break; }
        written += count;
      }
      const auto time = std::chrono::steady_clock::now();
      if (time >= next_health && !failed) {
        Event health{};
        health.timestamp = now(); health.stage = 17;
        health.id = queue.dropped(); health.argument = written;
        if (fwrite(&health, sizeof(health), 1, output) != 1) failed = true;
        else ++written;
        next_health = time + std::chrono::seconds(2);
      }
      if (fflush(output) != 0) failed = true;
    }
    enabled.store(false, std::memory_order_release);
    // Always attempt a final health record; room is reserved above.
    Event health{};
    health.timestamp = now(); health.stage = 17;
    health.id = queue.dropped(); health.argument = written;
    if (fwrite(&health, sizeof(health), 1, output) != 1 || fflush(output) != 0) failed = true;
    fclose(output);
    fprintf(stderr, "[frame trace] stopped: written=%llu dropped=%llu write_failed=%d\n",
            (unsigned long long)written, (unsigned long long)queue.dropped(), int(failed));
  }).detach();
}
