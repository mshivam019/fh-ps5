// The game code lives in a private module that setup builds from the player's
// own disc; the app (eboot.bin) contains only the runtime.
#pragma once

#include <string>

#include <rex/image_info.h>
#include <rex/system/function_dispatcher.h>

namespace forza_module {
inline constexpr char kPath[] = "/app0/game/forza.module";

// Loads and binds the module once. On failure, error describes the problem.
bool Load(const char* path, std::string& error);

// Valid after Load succeeded.
const rex::PPCImageInfo& MainImage();

// The facade DLL the runtime asks for by its shared library name
// (forza_XMediaFacade_default, forza_SpeechFacade_default); false for others.
bool FindFacade(const char* name, rex::runtime::FunctionDispatcher::RegisterFn* register_fn,
                const rex::PPCImageInfo* (**image_info_fn)());
}  // namespace forza_module
