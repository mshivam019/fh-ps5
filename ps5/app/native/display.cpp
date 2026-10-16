// Observe the VideoOut handle owned by Vulkan without opening a second output.
// A queued Vulkan present is not a completed display flip.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <rex/diagnostic_trace.h>

extern "C" {
int __real_sceVideoOutOpen(int32_t, int32_t, int32_t, const void*);
int __real_sceVideoOutClose(int32_t);
int __real_sceVideoOutSubmitFlip(int32_t, int32_t, uint32_t, int64_t);
int sceVideoOutWaitVblank(int32_t);
int sceVideoOutGetFlipStatus(int32_t, uint64_t*);
}

namespace {
std::atomic<int32_t> display_handle{-1};
std::atomic<uint64_t> submitted_flip{0};
}

extern "C" int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index,
                                      const void* parameter) {
  const int handle = __real_sceVideoOutOpen(user, bus, index, parameter);
  if (handle >= 0) {
    submitted_flip.store(0, std::memory_order_release);
    display_handle.store(handle, std::memory_order_release);
  }
  return handle;
}

extern "C" int __wrap_sceVideoOutClose(int32_t handle) {
  const int result = __real_sceVideoOutClose(handle);
  if (result == 0) {
    int32_t expected = handle;
    display_handle.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
  }
  return result;
}

extern "C" int __wrap_sceVideoOutSubmitFlip(int32_t handle, int32_t buffer,
                                            uint32_t mode, int64_t argument) {
  const int result = __real_sceVideoOutSubmitFlip(handle, buffer, mode, argument);
  if (result == 0 && handle == display_handle.load(std::memory_order_acquire)) {
    submitted_flip.store(uint64_t(argument), std::memory_order_release);
    #if REX_PLATFORM_PS5
    rex::diagnostic::Event(14, uint64_t(argument), buffer);
    #endif
  }
  return result;
}

extern "C" uint64_t rex_ps5_display_flip_serial() {
  return submitted_flip.load(std::memory_order_acquire);
}

extern "C" bool rex_ps5_display_wait_vblank() {
  const int32_t handle = display_handle.load(std::memory_order_acquire);
  return handle >= 0 && sceVideoOutWaitVblank(handle) == 0;
}

extern "C" bool rex_ps5_display_wait_for_flip(uint64_t previous) {
  const int32_t handle = display_handle.load(std::memory_order_acquire);
  if (handle < 0) return false;
  // Bound recovery/shutdown latency if submission fails or the output disappears.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  do {
    uint64_t status[16] = {};
    if (sceVideoOutGetFlipStatus(handle, status) != 0) return false;
    // Word 3 is VideoOut's last displayed flip argument, not the queued argument.
    // The local Vulkan WSI assigns monotonically increasing positive arguments.
    const uint64_t submitted = submitted_flip.load(std::memory_order_acquire);
    // Do not chase the newest submission while the UI continues queueing flips.
    // Any completed serial after the captured one proves display progress.
    if (submitted > previous && int64_t(status[3]) > int64_t(previous)) {
      static bool reported = false;  // Called only by the presenter thread.
      if (!reported) {
        std::fprintf(stderr, "[display pacing] confirmed VideoOut flip %llu on handle %d\n",
                     (unsigned long long)status[3], handle);
        reported = true;
      }
      #if REX_PLATFORM_PS5
      rex::diagnostic::Event(15, status[3], previous);
      #endif
      return true;
    }
    if (sceVideoOutWaitVblank(handle) != 0) return false;
  } while (std::chrono::steady_clock::now() < deadline);
  std::fprintf(stderr, "[display pacing] timed out awaiting flip after %llu\n",
               (unsigned long long)previous);
  return false;
}
