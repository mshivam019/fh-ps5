// Exercise the production residency path with a CPU-backed upload sink.
#include <rex/graphics/shared_memory.h>
#include <rex/cvar.h>
#include <rex/memory/sync_counters.h>
#include <rex/thread/mutex.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

class Residency : public rex::graphics::SharedMemory {
 public:
  explicit Residency(rex::memory::Memory& memory) : SharedMemory(memory) { InitializeCommon(); }
  unsigned uploads = 0;
  bool fail = false;
  bool invalidate_during_upload = false;
  void Resident(uint32_t start, uint32_t length) { MakeRangeValid(start, length, false); }
 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& ranges) override {
    ++uploads;
    if (fail) return false;
    for (auto [page, count] : ranges) MakeRangeValid(page << page_size_log2(), count << page_size_log2(), false);
    if (invalidate_during_upload) {
      invalidate_during_upload = false;
      MemoryInvalidationCallback(ranges.front().first << page_size_log2(), 1, true);
    }
    return true;
  }
};

// The Vulkan uploader's order: every chunk of a pool page is marked valid in
// one acquisition, then copied.
class Batch : public rex::graphics::SharedMemory {
 public:
  explicit Batch(rex::memory::Memory& memory) : SharedMemory(memory) { InitializeCommon(); }
  using SharedMemory::page_size_log2;
  unsigned uploads = 0;
  size_t last_chunks = 0;
  void Degenerate() {
    std::pair<uint32_t, uint32_t> ranges[] = {{0, 0}, {kBufferSize, 16}, {kBufferSize - 16, 64}};
    MakeRangesValid(ranges, 3, false);
    MakeRangesValid(nullptr, 5, false);
  }
 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& ranges) override {
    ++uploads;
    std::vector<std::pair<uint32_t, uint32_t>> chunks;
    for (auto [page, count] : ranges) {
      // Split like a pool page boundary would, to cover several chunks per range.
      chunks.emplace_back(page << page_size_log2(), 1u << page_size_log2());
      if (count > 1) chunks.emplace_back((page + 1) << page_size_log2(), (count - 1) << page_size_log2());
    }
    last_chunks = chunks.size();
    MakeRangesValid(chunks.data(), chunks.size(), false);
    return true;
  }
};

static void CheckBatch(rex::memory::Memory& memory) {
  Batch gpu(memory);
  constexpr uint32_t base = 0x300000, page = 0x4000;
  gpu.InvalidateAllPages();
  std::pair<uint32_t, uint32_t> ranges[] = {{base, 3 * page}, {base + 5 * page, page}, {base + 8 * page, 2 * page}};
  assert(gpu.RequestRanges(ranges, 3));
  // Two chunks per multi-page range; the one-page middle range is one chunk
  // with 16 KiB pages and two with 4 KiB host pages.
  const size_t expected_chunks = gpu.page_size_log2() >= 14 ? 5 : 6;
  assert(gpu.uploads == 1 && gpu.last_chunks == expected_chunks);
  // Exactly the requested pages are valid; the gaps stay invalid.
  assert(gpu.IsRangeValid(base, 3 * page));
  assert(!gpu.IsRangeValid(base + 3 * page, 2 * page));
  assert(gpu.IsRangeValid(base + 5 * page, page));
  assert(!gpu.IsRangeValid(base + 6 * page, 2 * page));
  assert(gpu.IsRangeValid(base + 8 * page, 2 * page));
  assert(!gpu.IsRangeValid(base + 10 * page, page));
  assert(gpu.RequestRanges(ranges, 3) && gpu.uploads == 1);
  // A write into one range after the batch re-uploads only the written page.
  gpu.MemoryInvalidationCallback(base + 8 * page, 4, true);
  assert(gpu.RequestRanges(ranges, 3));
  assert(gpu.uploads == 2 && gpu.last_chunks == 1);
  assert(gpu.IsRangeValid(base + 8 * page, 2 * page));
  // Degenerate inputs are skipped or clamped like MakeRangeValid does.
  std::pair<uint32_t, uint32_t> odd[] = {{base + 12 * page, 0}};
  assert(gpu.RequestRanges(odd, 1) && gpu.uploads == 2);
  gpu.Degenerate();
  assert(!gpu.IsRangeValid(0, 16));
  assert(gpu.IsRangeValid(rex::graphics::SharedMemory::kBufferSize - 16, 16));
  // Contended acquisitions on the upload path are counted with their wait time.
  auto& counters = rex::memory::sync_counters();
  const uint64_t waits = counters.lock_waits.load(), wait_cycles = counters.lock_wait_cycles.load();
  gpu.InvalidateAllPages();
  std::thread holder([] {
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(gpu.RequestRange(base, page) && gpu.uploads == 3);
  holder.join();
  assert(counters.lock_waits.load() >= waits + 1);
  assert(counters.lock_wait_cycles.load() > wait_cycles);
  assert(counters.upload_ranges.load() >= 1 && counters.upload_bytes.load() >= page);
}

int main() {
  rex::memory::Memory memory;
  assert(memory.Initialize());
  CheckBatch(memory);
  Residency gpu(memory);
  constexpr uint32_t base = 0x100000, length = 0x100000;
  // Pre-resident ranges (including data uploaded through a different range)
  // must be reusable, but any invalidation must still trigger an upload.
  gpu.Resident(base, length);
  assert(gpu.RequestRange(base, length));
  assert(gpu.uploads == 0);
  for (int i = 0; i < 100; ++i) assert(gpu.RequestRange(base, length));
  assert(gpu.uploads == 0);
  gpu.MemoryInvalidationCallback(base + 0x4000, 0x4000, true);
  assert(gpu.RequestRange(base, length));
  assert(gpu.uploads == 1);
  gpu.InvalidateAllPages();
  gpu.fail = true;
  assert(!gpu.RequestRange(base, length));
  gpu.fail = false;
  assert(gpu.RequestRange(base, length));
  assert(gpu.uploads == 3);
  assert(!gpu.RequestRange(rex::graphics::SharedMemory::kBufferSize - 4, 8));
  assert(!gpu.RequestRange(std::numeric_limits<uint32_t>::max(), 8));
  assert(gpu.RequestRange(base, 0));
  // Batching overlapping / adjacent vertex streams must preserve the union
  // and leave unrelated pages invalid.
  gpu.InvalidateAllPages();
  std::pair<uint32_t, uint32_t> ranges[] = {{base + 0x4000, 0x8000}, {base, 0x8000}, {base + 0xc000, 0x4000}};
  assert(gpu.RequestRanges(ranges, 3));
  assert(gpu.IsRangeValid(base, 0x10000));
  assert(!gpu.IsRangeValid(base + 0x10000, 0x4000));
  gpu.InvalidateAllPages();
  gpu.invalidate_during_upload = true;
  unsigned before = gpu.uploads;
  assert(gpu.RequestRange(base, length));
  assert(!gpu.IsRangeValid(base, 1));
  assert(gpu.RequestRange(base, length));
  assert(gpu.uploads == before + 2);
  // Reproduce the missing resident-cache insertion after an unrelated write.
  gpu.Resident(base, length);
  gpu.MemoryInvalidationCallback(base + length, 0x4000, true);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 2000000; ++i) assert(gpu.RequestRange(base, length));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("Residency/invalidation/batched-validity/contention/failure checks passed; 2M resident requests %.3f ms\n", ms);
}
