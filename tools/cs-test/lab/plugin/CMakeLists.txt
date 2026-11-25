# cs-test's lab helper: an SKSE plugin with no CommonLib, so it never touches the shared Address Library table.
# README "Lab" section; build with build.cmd.
cmake_minimum_required(VERSION 3.24)
project(CSTestLab VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

find_package(detours CONFIG QUIET)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h" REQUIRED)
find_library(DETOURS_LIBRARY detours REQUIRED)

add_library(CSTestLab SHARED src/Plugin.cpp)
target_include_directories(CSTestLab PRIVATE "${DETOURS_INCLUDE_DIRS}")
target_link_libraries(CSTestLab PRIVATE "${DETOURS_LIBRARY}" shell32 user32 ole32)
target_compile_definitions(CSTestLab PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(CSTestLab PRIVATE /W4 /permissive- /utf-8)
# "!" sorts before every other plugin name, and SKSE loads plugins in directory order (NTFS: by name).
set_target_properties(CSTestLab PROPERTIES OUTPUT_NAME "!CSTestLab")
