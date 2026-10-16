// Bounded reusable capture. Producers never allocate, wait, or write files.
#include "diagnostic_trace_queue.h"
#include <rex/cvar.h>
#include <rex/diagnostic_trace.h>
#include <rex/logging.h>
#include <rex/memory/sync_counters.h>
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

REXCVAR_DEFINE_BOOL(ps5_long_frame_log, false, "PS5",
                    "Log every GPU command frame over 45 ms with its stage breakdown, CPU time "
                    "and memory synchronization counters (costs a cycle read per stage)");

namespace {
using diagnostic_trace::Event;
diagnostic_trace::Queue queue;
std::atomic<bool> enabled{false};
// Optional (ps5_long_frame_log): the command worker's exclusive stage
// accounting runs without a capture, and command frames over 45 ms are written
// to the runtime log with their category breakdown, thread CPU time and the
// memory synchronization counters' change during the frame. Costs one ordered
// cycle read per stage boundary on the command thread.
std::atomic<bool> stage_log{false};
std::atomic<double> ns_per_tick{0};
uint64_t now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct FrameAccumulator {
  uint64_t id = UINT64_MAX;
  uint64_t totals_ns[rex::diagnostic::kFrameCategories] = {};
  uint64_t cpu_ns = 0;
  bool have_cpu = false;
  uint64_t counts[8] = {};  // snapshot of memory sync counters at the last frame
};
thread_local FrameAccumulator frame_accumulator;
const char* const kCategoryNames[rex::diagnostic::kFrameCategories] = {
    "unclassified", "dispatch", "empty_ring", "register_wait", "fence_wait", "swap",
    "submission", "primitives", "textures", "targets", "pipelines", "bindings",
    "vertex_buffers", "constants", "resolve_readback", "shader_translation", "samplers"};
void SnapshotCounts(uint64_t* out) {
  auto& c = rex::memory::sync_counters();
  const uint64_t values[8] = {c.lock_waits.load(std::memory_order_relaxed),
                              c.lock_wait_cycles.load(std::memory_order_relaxed),
                              c.lock_sleeps.load(std::memory_order_relaxed),
                              c.upload_ranges.load(std::memory_order_relaxed),
                              c.upload_bytes.load(std::memory_order_relaxed),
                              c.protect_syscalls.load(std::memory_order_relaxed),
                              c.protect_cycles.load(std::memory_order_relaxed),
                              c.watch_faults.load(std::memory_order_relaxed)};
  memcpy(out, values, sizeof(values));
}
void AccountStage(uint32_t stage, uint64_t id, uint64_t argument, uint64_t duration_ns) {
  FrameAccumulator& frame = frame_accumulator;
  if (frame.id != id) {
    frame.id = id;
    memset(frame.totals_ns, 0, sizeof(frame.totals_ns));
    frame.cpu_ns = 0;
    frame.have_cpu = false;
  }
  if (stage == 40) {
    frame.cpu_ns = duration_ns;
    frame.have_cpu = true;
    return;
  }
  const unsigned category = stage - 20;
  frame.totals_ns[category] = duration_ns;
  if (category != rex::diagnostic::kFrameCategories - 1) return;
  // Last category of the frame: evaluate.
  uint64_t total_ns = 0;
  for (uint64_t value : frame.totals_ns) total_ns += value;
  uint64_t counts[8];
  SnapshotCounts(counts);
  uint64_t delta[8];
  // The five-second memory log takes (resets) these counters; after a reset
  // the frame's delta is the new value itself.
  for (unsigned i = 0; i < 8; ++i) {
    delta[i] = counts[i] >= frame.counts[i] ? counts[i] - frame.counts[i] : counts[i];
  }
  memcpy(frame.counts, counts, sizeof(counts));
  if (total_ns < 45000000) return;
  // Cycle counters to ms: the sync counters use the same TSC as the stages.
  const double ns_per_cycle = ns_per_tick.load(std::memory_order_relaxed);
  auto cycles_ms = [&](uint64_t cycles) { return double(cycles) * ns_per_cycle / 1e6; };
  std::string categories;
  for (unsigned i = 0; i < rex::diagnostic::kFrameCategories; ++i) {
    if (frame.totals_ns[i] < 200000) continue;  // omit categories under 0.2 ms
    char piece[64];
    snprintf(piece, sizeof(piece), " %s=%.1f", kCategoryNames[i], double(frame.totals_ns[i]) / 1e6);
    categories += piece;
  }
  REXLOG_INFO("PS5 long frame: id={} total_ms={:.1f} cpu_ms={} lock_waits={} lock_wait_ms={:.1f} lock_sleeps={} uploads={} upload_kb={} mprotects={} protect_ms={:.1f} faults={}{}",
              id, double(total_ns) / 1e6,
              frame.have_cpu ? std::to_string(frame.cpu_ns / 100000 / 10.0) : std::string("?"),
              delta[0], cycles_ms(delta[1]), delta[2], delta[3], delta[4] / 1024, delta[5],
              cycles_ms(delta[6]), delta[7], categories);
}
}
extern "C" bool rex_ps5_trace_enabled() {
  return enabled.load(std::memory_order_relaxed) || stage_log.load(std::memory_order_relaxed);
}
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
  if ((stage >= 20 && stage < 20 + rex::diagnostic::kFrameCategories) || stage == 40) {
    AccountStage(stage, id, argument, duration_ns);
  }
  if (!enabled.load(std::memory_order_relaxed)) return;
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
  stage_log.store(REXCVAR_GET(ps5_long_frame_log), std::memory_order_relaxed);
  // Calibrate the cycle counter first: the long-frame log needs it whether or
  // not a capture is enabled.
  {
    const uint64_t start_ticks = rex::diagnostic::ActivityTicks();
    const uint64_t start_ns = now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t elapsed_ns = now() - start_ns;
    const uint64_t elapsed_ticks = rex::diagnostic::ActivityTicks() - start_ticks;
    if (elapsed_ticks && elapsed_ns) {
      ns_per_tick.store(double(elapsed_ns) / double(elapsed_ticks), std::memory_order_relaxed);
    }
    fprintf(stderr, "[frame trace] long-frame stage log %s; ns_per_tick=%.4f\n",
            stage_log.load(std::memory_order_relaxed) ? "enabled" : "disabled",
            ns_per_tick.load(std::memory_order_relaxed));
  }
  FILE* marker = fopen(marker_path, "rb");
  if (!marker) return;
  fclose(marker);
  FILE* output = fopen(output_path, "wb");
  if (!output) { fprintf(stderr, "[frame trace] cannot open capture\n"); return; }
  const char magic[8] = {'F','H','T','R','A','C','E','1'};
  if (fwrite(magic, 1, sizeof(magic), output) != sizeof(magic)) { fclose(output); return; }
  fflush(output);
  const double scale = ns_per_tick.load(std::memory_order_relaxed);
  if (scale <= 0) { fclose(output); return; }
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
