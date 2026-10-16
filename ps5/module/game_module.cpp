#include "game_module.h"

#include <cstring>
#include <stdexcept>

#include "loader.h"
#include "module_abi.h"  // generated: FORZA_MODULE_ABI

// Generated from ps5/module/host-imports.txt and runtime-lookups.txt: the runtime
// symbols the module may use, by name.
void* forza_host_symbol(const char* name);

namespace forza_module {
namespace {
using ImageInfoFn = const rex::PPCImageInfo* (*)();

struct Facade {
  const char* name;    // shared library name the runtime asks for
  const char* prefix;  // module entry point prefix (build-game-module.py MODULES)
  rex::runtime::FunctionDispatcher::RegisterFn register_fn;
  ImageInfoFn image_info_fn;
};

game_module::Image g_image;
const rex::PPCImageInfo* g_main_image = nullptr;
Facade g_facades[] = {
    {"forza_XMediaFacade_default", "ForzaXMF", nullptr, nullptr},
    {"forza_SpeechFacade_default", "ForzaSF", nullptr, nullptr},
};

void* Required(const std::string& name, size_t size = 1) {
  void* result = g_image.symbol(name.c_str(), size);
  if (!result) throw std::runtime_error("Missing game module symbol: " + name);
  return result;
}
}  // namespace

bool Load(const char* path, std::string& error) {
  if (g_main_image) return true;
  if (!g_image.load(path, forza_host_symbol, error)) return false;
  try {
    if (std::memcmp(Required("forza_module_abi", sizeof(FORZA_MODULE_ABI)), FORZA_MODULE_ABI,
                    sizeof(FORZA_MODULE_ABI))) {
      throw std::runtime_error(
          "Game module was built for a different app version; rerun setup from the matching "
          "release");
    }
    auto* image = static_cast<const rex::PPCImageInfo*>(
        Required("PPCImageConfig", sizeof(rex::PPCImageInfo)));
    for (auto& facade : g_facades) {
      facade.register_fn = reinterpret_cast<rex::runtime::FunctionDispatcher::RegisterFn>(
          Required(std::string(facade.prefix) + "_Register"));
      facade.image_info_fn =
          reinterpret_cast<ImageInfoFn>(Required(std::string(facade.prefix) + "_GetImageInfo"));
    }
    g_main_image = image;
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

const rex::PPCImageInfo& MainImage() { return *g_main_image; }

bool FindFacade(const char* name, rex::runtime::FunctionDispatcher::RegisterFn* register_fn,
                ImageInfoFn* image_info_fn) {
  for (const auto& facade : g_facades) {
    if (std::strcmp(name, facade.name) == 0 && facade.register_fn) {
      *register_fn = facade.register_fn;
      *image_info_fn = facade.image_info_fn;
      return true;
    }
  }
  return false;
}
}  // namespace forza_module
