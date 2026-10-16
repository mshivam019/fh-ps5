// Native startup follows the platform contract used by the working PS5 ports.
#include "native/platform.h"
#include "../module/game_module.h"
#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/chrono/clock.h>
#include <time.h>
#include <rex/thread/fiber.h>
#include <rex/logging.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/windowed_app_context_sdl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>

extern "C" void ps5_input_initialize();
extern "C" void ps5_input_shutdown();
extern "C" void catchReturnFromMain(int);
extern "C" [[noreturn]] void __wrap_exit(int status) {
    catchReturnFromMain(status);
    for (;;) {}
}
extern "C" [[noreturn]] void __wrap__Exit(int status) { __wrap_exit(status); }
extern "C" [[noreturn]] void __wrap__exit(int status) { __wrap_exit(status); }
extern "C" [[noreturn]] void __wrap_abort() {
    say("Forza fatal error; inspect the runtime log");
    __wrap_exit(1);
}

namespace forza {
void PatchMotionBlurStencilThreshold();  // upstream src/platform/motion_blur_fix.cpp
}

class ForzaPS5App final : public rex::ReXApp {
public:
    explicit ForzaPS5App(rex::ui::WindowedAppContext& context)
        : rex::ReXApp(context, "forza", forza_module::MainImage()) {}
    void OnConfigurePaths(rex::PathConfig& paths) override {
        paths.game_data_root = "/app0/game";
        paths.user_data_root = "/app0/user";
        paths.cache_root = "/app0/user/cache";
        paths.metadata_root = "/app0/metadata";
        // Shipped with the app (replaced on update); saves and caches stay in user/.
        paths.config_path = "/app0/forza.toml";
        paths.update_data_root.clear();
    }
    // As upstream ForzaApp::OnPreLaunchModule: fix the speed blur's stencil threshold in
    // the loaded image, and back the game's utility drive (cache:, ghost and replay
    // streams) with a folder, which the runtime leaves unmounted.
    void OnPreLaunchModule() override {
        forza::PatchMotionBlurStencilThreshold();
        auto* vfs = runtime()->file_system();
        const std::filesystem::path host = cache_root() / "cache";
        std::error_code ec;
        std::filesystem::create_directories(host, ec);
        auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\CACHE", host, false);
        if (!device->Initialize() || !vfs->RegisterDevice(std::move(device)) ||
            !vfs->RegisterSymbolicLink("cache:", "\\CACHE")) {
            REXLOG_ERROR("Failed to mount cache: at {}", host.string());
        }
    }
};

struct FiberCheck {
    rex::thread::Fiber* parent;
    int step;
};

static void check_fiber_entry(void* arg) {
    auto* check = static_cast<FiberCheck*>(arg);
    for (int i = 1; i <= 3; i++) {
        check->step = i;
        rex::thread::Fiber::SwitchTo(check->parent);
    }
    std::abort();
}

static bool check_fibers() {
    auto* parent = rex::thread::Fiber::ConvertCurrentThread();
    if (!parent) return false;
    FiberCheck check{parent, 0};
    auto* child = rex::thread::Fiber::Create(256 * 1024, check_fiber_entry, &check);
    if (!child) {
        parent->Destroy();
        return false;
    }
    bool passed = true;
    for (int i = 1; i <= 3; i++) {
        rex::thread::Fiber::SwitchTo(child);
        if (check.step != i) passed = false;
    }
    child->Destroy();
    parent->Destroy();
    say("native fiber round-trip check: %s", passed ? "passed" : "failed");
    return passed;
}

void StartDiagnosticTrace(const char* marker_path, const char* output_path);
int main() {
    // RADV keeps its shader cache in a database of 50 parts by default, with
    // two open files per part. Once Forza's cache filled enough parts, fopen
    // failed with EMFILE at 265 open descriptors (RLIMIT_NOFILE says 13952):
    // the shader storage and the game's cache: drive could not open, and the
    // game showed a disc read error. Four parts keep RADV to a few files. Set
    // before the GPU starts.
    setenv("MESA_DISK_CACHE_DATABASE_NUM_PARTS", "4", 0);
    platform_init("Forza Horizon");
    mkdir("/app0/logs", 0777);
    StartDiagnosticTrace("/app0/trace.enable", "/app0/logs/frame-trace.bin");
    if (!check_fibers()) return 1;
    // The game code is a private module prepared by the desktop setup; load it
    // before anything else, as static initialization would have run.
    {
        std::string error;
        if (!forza_module::Load(forza_module::kPath, error)) {
            say("Forza game module not loaded: %s", error.c_str());
            return 1;
        }
    }
    timespec clock_resolution{};
    clock_getres(CLOCK_MONOTONIC, &clock_resolution);
    const auto tick_start = rex::chrono::Clock::host_tick_count_platform();
    const timespec clock_check_delay{0, 100000000};
    nanosleep(&clock_check_delay, nullptr);
    const auto tick_elapsed = rex::chrono::Clock::host_tick_count_platform() - tick_start;
    const auto tick_frequency = rex::chrono::Clock::host_tick_frequency_platform();
    say("clock check: resolution=%ld ns frequency=%llu elapsed=%.6f seconds",
        clock_resolution.tv_nsec, static_cast<unsigned long long>(tick_frequency),
        double(tick_elapsed) / double(tick_frequency));
    mkdir("/app0/user", 0777);
    mkdir("/app0/user/cache", 0777);
    // Prepare the full persistent shader path before the GPU worker starts.
    // Avoid recursive directory creation during shader storage initialization.
    mkdir("/app0/user/cache/shaders", 0777);
    mkdir("/app0/user/cache/shaders/shareable", 0777);
    SDL_Environment* environment = SDL_GetEnvironment();
    SDL_SetEnvironmentVariable(environment, "SDL_VIDEODRIVER", "dummy", true);
    SDL_SetEnvironmentVariable(environment, "SDL_AUDIODRIVER", "ps5", true);
    char executable[] = "/app0/eboot.bin";
    char gpu[] = "--gpu_plugin=xenos";
    char video[] = "--video_driver=dummy";
    // Native-title sandboxes reject chdir; optional files use absolute paths.
    char mappings[] = "--hid_mappings_file=/app0/gamecontrollerdb.txt";
    char* arguments[] = {executable, gpu, video, mappings, nullptr};
    rex::cvar::Init(4, arguments);
    rex::cvar::ApplyEnvironment();
    rex::InitLoggingEarly();
    try {
        rex::ui::SDLWindowedAppContext context;
        if (!context.Initialize()) return 1;
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            say("Controller initialization failed: %s", SDL_GetError());
            return 1;
        }
        ps5_input_initialize();
        int result = 1;
        {
            ForzaPS5App app(context);
            if (static_cast<rex::ui::WindowedApp&>(app).OnInitialize()) result = context.RunMainMessageLoop();
            app.InvokeOnDestroy();
        }
        ps5_input_shutdown();
        return result;
    } catch (const std::exception& error) {
        say("Forza startup failed: %s", error.what());
        return 1;
    }
}
