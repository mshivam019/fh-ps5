set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-march=x86-64-v2)

# The app contains no game code. Game code is a private module that setup
# compiles from the player's disc (ps5/tools/build-game-module.py); the app
# loads it and binds the runtime symbols listed in ps5/module/host-imports.txt
# and ps5/module/runtime-lookups.txt.
set(FORZA_SDK "${CMAKE_CURRENT_LIST_DIR}/../../.deps/rexglue-sdk")
set(FORZA_UPSTREAM "${CMAKE_CURRENT_LIST_DIR}/../../upstream/forza/forza-recomp")
set(FORZA_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}/../module")
set(FORZA_MODULE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/module-generated")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
# The module ABI identity covers these inputs and the SDK headers listed in
# abi-headers.txt; regenerate the app's copy when any of them changes.
file(STRINGS "${FORZA_MODULE_DIR}/abi-headers.txt" FORZA_ABI_HEADERS REGEX "^[^#]")
list(TRANSFORM FORZA_ABI_HEADERS PREPEND "${FORZA_SDK}/")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${FORZA_MODULE_DIR}/host-imports.txt" "${FORZA_MODULE_DIR}/runtime-lookups.txt"
    "${FORZA_MODULE_DIR}/compile-flags.txt" "${FORZA_MODULE_DIR}/abi-headers.txt"
    "${FORZA_MODULE_DIR}/toolchain.json" ${FORZA_ABI_HEADERS})
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/module-support.py"
    --output "${FORZA_MODULE_GENERATED}"
    COMMAND_ERROR_IS_FATAL ANY)

add_library(forza_app STATIC main.cpp builtin-module.cpp input.cpp button_prompts.cpp
    native/platform.c native/display.cpp diagnostic_trace.cpp
    "${FORZA_MODULE_DIR}/loader.cpp" "${FORZA_MODULE_DIR}/game_module.cpp"
    "${FORZA_MODULE_GENERATED}/module_exports.cpp"
    "${FORZA_UPSTREAM}/src/platform/motion_blur_fix.cpp"
    "${FORZA_SDK}/src/ui/rex_app.cpp")
target_link_libraries(forza_app PRIVATE forza_ps5_runtime imgui)
target_include_directories(forza_app PRIVATE "${FORZA_MODULE_GENERATED}"
    "${FORZA_SDK}/thirdparty/xxHash")
target_compile_definitions(forza_app PRIVATE REXGLUE_BUILD_CONFIG="Release")
target_compile_options(forza_app PRIVATE -O2 -gline-tables-only)
