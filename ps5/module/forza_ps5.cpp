// PS5 replacements for the parts of the upstream desktop app that the game
// module's hooks and kernel overrides call but the PS5 build leaves out.
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "debug/cpu_profile.h"
#include "platform/hud_overlay.h"
#include "platform/sdk_import.h"

// Defined by the desktop app's main.cpp upstream; the hooks read it. The PS5 runs the
// console's 30 FPS.
REXCVAR_DEFINE_INT32(frame_rate, 30, "Display", "Frame rate: 30 (the console's)")
    .validator([](std::string_view v) { return v == "30"; })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Resolved by the app from ps5/module/runtime-lookups.txt.
extern "C" void* forza_runtime_symbol(const char* name);

namespace forza {
// The runtime's own `__imp__Name`: the module defines its overrides itself, so the
// generated code calls those, and the app hands out the runtime's by name.
GuestFunc SdkImport(const char* name) {
  return reinterpret_cast<GuestFunc>(forza_runtime_symbol(name));
}

// The head-to-head challenge text box is an ImGui overlay on the desktop; the PS5 app
// has no overlay drawer, so its texts go to the log.
void HudShow(const std::string& title, const std::string& body, double seconds, bool log) {
  if (log) REXLOG_INFO("HUD: {} | {} ({:.0f} s)", title, body, seconds);
}
void HudClear() {}

// The sampling profiler reads Linux signal contexts; the PS5 build has none.
namespace cpuprof {
void Start() { REXLOG_WARN("CPU profiler is not available on PS5"); }
void Stop() {}
}  // namespace cpuprof
}  // namespace forza
