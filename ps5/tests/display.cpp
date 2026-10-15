// Host regression for the native display completion bridge. Build with:
// clang++ -std=c++23 -O2 ps5/tests/display.cpp ps5/app/native/display.cpp -o /tmp/forza-display-test
#include <cassert>
#include <cstdint>
#include <cstdio>

extern "C" {
int __wrap_sceVideoOutOpen(int32_t, int32_t, int32_t, const void*);
int __wrap_sceVideoOutClose(int32_t);
int __wrap_sceVideoOutSubmitFlip(int32_t, int32_t, uint32_t, int64_t);
uint64_t rex_ps5_display_flip_serial();
bool rex_ps5_display_wait_for_flip(uint64_t);
bool rex_ps5_display_wait_vblank();
}
namespace {
uint64_t shown = uint64_t(-1);  // No displayed flip must not look newer than a positive serial.
unsigned blanks = 0;
bool fail_wait = false, fail_status = false, fail_submit = false;
int64_t deferred_flip = 0;
unsigned submit_at = 0, show_at = 0;
}
extern "C" int __real_sceVideoOutOpen(int32_t, int32_t, int32_t, const void*) { return 7; }
extern "C" int __real_sceVideoOutClose(int32_t) { return 0; }
extern "C" int __real_sceVideoOutSubmitFlip(int32_t, int32_t, uint32_t, int64_t) {
  return fail_submit ? -1 : 0;
}
extern "C" int sceVideoOutGetFlipStatus(int32_t handle, uint64_t* status) {
  assert(handle == 7);
  if (fail_status) return -1;
  status[3] = shown;
  return 0;
}
extern "C" int sceVideoOutWaitVblank(int32_t handle) {
  assert(handle == 7);
  if (fail_wait) return -1;
  ++blanks;
  if (blanks == submit_at) assert(__wrap_sceVideoOutSubmitFlip(7, 0, 1, deferred_flip) == 0);
  if (blanks == show_at) shown = uint64_t(deferred_flip);
  return 0;
}
int main() {
  assert(!rex_ps5_display_wait_vblank());
  assert(!rex_ps5_display_wait_for_flip(0));
  assert(__wrap_sceVideoOutOpen(255, 0, 0, nullptr) == 7);
  // Vulkan returns before its flip thread submits; a submitted flip also takes
  // another refresh before VideoOut reports it on screen.
  deferred_flip = 1; submit_at = 2; show_at = 4;
  assert(rex_ps5_display_wait_for_flip(0));
  assert(blanks == 4 && rex_ps5_display_flip_serial() == 1);
  // The previous displayed frame must not satisfy the next presentation.
  deferred_flip = 2; submit_at = 6; show_at = 8;
  assert(rex_ps5_display_wait_for_flip(1));
  assert(blanks == 8);
  assert(rex_ps5_display_wait_vblank() && blanks == 9);
  // Unrelated handles and failed submissions must not advance the serial.
  assert(__wrap_sceVideoOutSubmitFlip(99, 0, 1, 100) == 0);
  fail_submit = true;
  assert(__wrap_sceVideoOutSubmitFlip(7, 0, 1, 3) != 0);
  assert(rex_ps5_display_flip_serial() == 2);
  fail_wait = true;
  assert(!rex_ps5_display_wait_for_flip(2));
  fail_status = true;
  assert(!rex_ps5_display_wait_for_flip(2));
  assert(__wrap_sceVideoOutClose(7) == 0);
  assert(!rex_ps5_display_wait_vblank());
  // Continuous UI presentation may already have queued a later frame. Wait for
  // progress past the requested serial, not for the queue to become empty.
  fail_submit = fail_status = false;
  assert(__wrap_sceVideoOutOpen(255, 0, 0, nullptr) == 7);
  assert(__wrap_sceVideoOutSubmitFlip(7, 0, 1, 1) == 0);
  assert(__wrap_sceVideoOutSubmitFlip(7, 0, 1, 2) == 0);
  shown = 1;
  assert(rex_ps5_display_wait_for_flip(0));
  assert(!rex_ps5_display_wait_for_flip(1));
  assert(__wrap_sceVideoOutClose(7) == 0);
  puts("Display completion: delayed submission, delayed scanout, stale frame, errors and close passed");
}
