// The game's facade DLLs are part of the private game module; the runtime asks
// for them by name when the game loads them.
#include <rex/image_info.h>
#include <rex/system/function_dispatcher.h>

#include "../module/game_module.h"

extern "C" bool rex_ps5_resolve_builtin_module(
    const char* name,
    rex::runtime::FunctionDispatcher::RegisterFn* register_fn,
    const rex::PPCImageInfo* (**image_info_fn)()) {
    if (!name || !register_fn || !image_info_fn) return false;
    *register_fn = nullptr;
    *image_info_fn = nullptr;
    return forza_module::FindFacade(name, register_fn, image_info_fn);
}
