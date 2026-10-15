cmake_minimum_required(VERSION 3.25)
project(forza_ps5_runtime LANGUAGES C CXX)

if(NOT PS5 OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    message(FATAL_ERROR "Use ps5/cmake/native.cmake for this cross-build")
endif()

get_filename_component(FORZA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(REXSDK_DIR "${FORZA_ROOT}/.deps/rexglue-sdk")
set(REXGLUE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(REXGLUE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_SANITIZERS OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_FIDELITYFX OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS ON CACHE BOOL "" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)

set(REXGLUE_PS5_SDL_SOURCE
    "/home/skm/Downloads/ps5/Dusklight-PS5/.deps/src/SDL" CACHE PATH "" FORCE)
set(REXGLUE_PS5_SDL_LIBRARY
    "/home/skm/Downloads/ps5/Dusklight-PS5/build/sdl3-ps5/libSDL3.a" CACHE FILEPATH "" FORCE)
set(REXGLUE_PS5_FFMPEG_PREFIX
    "${FORZA_ROOT}/build/ps5-ffmpeg/install" CACHE PATH "" FORCE)

# Compile against the exact Vulkan headers distributed with the working PS5 RADV archive.
set(VULKAN_HEADERS_INSTALL_DIR "" CACHE STRING "" FORCE)
add_subdirectory("${REXSDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/rexglue-sdk")

add_library(forza_ps5_runtime INTERFACE)
target_link_libraries(forza_ps5_runtime INTERFACE rex::runtime rex::gpu-xenos SDL3::SDL3)
target_include_directories(forza_ps5_runtime INTERFACE
    "${REXSDK_DIR}/include"
    "/home/skm/Downloads/ps5/PS5_Vulkan/.deps/native/radv-release/include")

option(FORZA_BUILD_APP "Compile the native PS5 app archive" OFF)
if(FORZA_BUILD_APP)
    add_subdirectory("${FORZA_ROOT}/ps5/app" "${CMAKE_CURRENT_BINARY_DIR}/app")
endif()
