// Targeted FH1 base-game waits, following Pinyon Shift #431 / NP-3.2.
// Original guest bodies always run: preserve alertable callbacks, timeout and
// kick logic, registers and game-visible results. No global Sleep(0) changes.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/gpu_write_signal.h>
#include <rex/thread.h>
#include "platform/guest_memory.h"

REXCVAR_DEFINE_UINT32(forza_render_idle_sleep_us, 200, "Forza",
                      "Delay only the render-idle SleepEx(0, TRUE) call; zero disables")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(forza_block_on_gpu_fence, true, "Forza",
                    "Bounded waits for CPU-visible GPU progress instead of guest polling");

namespace {
using Clock = std::chrono::steady_clock;
uint32_t Read32(uint8_t* base, uint32_t address) {
  // Same guest address translation as generated code, including physical views.
  const auto* p = reinterpret_cast<const volatile uint32_t*>(forza::GuestPtr(base, address));
  return __builtin_bswap32(*p);
}
struct FencePoll {
  uint32_t object = 0, address = 0, value = 0;
  Clock::time_point since{};
  uint64_t waits = 0, signalled = 0;
};
thread_local FencePoll fence_poll;
std::atomic<uint64_t> idle_waits{0};
}

extern "C" REX_FUNC(__imp__sub_82A6D7D8);
extern "C" REX_FUNC(sub_82A6D7D8) {
  const auto us = REXCVAR_GET(forza_render_idle_sleep_us);
  if (us && ctx.r3.u32 == 0 && ctx.r4.u32 == 1 && uint32_t(ctx.lr) == 0x8259FBEC) {
    const auto count = idle_waits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || (count % 32768) == 0)
      REXLOG_INFO("Forza render-idle wait: {} us, calls={}", us, count);
    rex::thread::Sleep(std::chrono::microseconds(us));
  }
  __imp__sub_82A6D7D8(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_829F04A8);
extern "C" REX_FUNC(sub_829F04A8) {
  if (REXCVAR_GET(forza_block_on_gpu_fence)) {
    // Snapshot the sequence BEFORE reading progress: a completion racing the
    // read/check/wait either changes the word or prevents the wait from sleeping.
    const uint32_t sequence = rex::system::GpuWriteSequence();
    const uint32_t device = Read32(base, ctx.r3.u32);
    const uint8_t flags = *reinterpret_cast<const volatile uint8_t*>(forza::GuestPtr(base, device + 11069));
    if (!(flags & 2)) {
      const uint32_t address = Read32(base, device + 11024);
      const uint32_t value = Read32(base, address);
      const auto now = Clock::now();
      if (fence_poll.object != ctx.r3.u32 || fence_poll.address != address || fence_poll.value != value) {
        fence_poll.object = ctx.r3.u32;
        fence_poll.address = address;
        fence_poll.value = value;
        fence_poll.since = now;
      } else if (now - fence_poll.since >= std::chrono::microseconds(20)) {
        rex::system::WaitForGpuWrite(sequence, std::chrono::microseconds(1000));
        ++fence_poll.waits;
        fence_poll.signalled += rex::system::GpuWriteSequence() != sequence;
        if (fence_poll.waits == 1 || (fence_poll.waits % 32768) == 0)
          REXLOG_INFO("Forza GPU fence wait: waits={} signalled={} timeout-us=1000", fence_poll.waits, fence_poll.signalled);
      }
    } else {
      fence_poll.object = 0;
    }
  } else {
    fence_poll.object = 0;
  }
  __imp__sub_829F04A8(ctx, base);
}
