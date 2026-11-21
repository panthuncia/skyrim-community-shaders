# Compile-time settings for the plugin target. They change how the DLL is built, not what it does.
# Measurements and rules: docs/development/build-speed.md.

# The plugin has no C++20 modules. Without this, CMake scans every source for module imports
# before compiling it, which is one extra preprocessor pass per source and a collation step.
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_SCAN_FOR_MODULES OFF)

# The render graph's heavy submodule headers join the precompiled header (include/PCHRenderGraph.h).
# Kept out of include/PCH.h, which is upstream's.
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCHRenderGraph.h)

# Unity builds: several sources compile as one translation unit, so their shared headers are
# parsed once per batch instead of once per file. Most of a file's compile time is header parsing
# (Drawcall Limit Fix and the render graph pull in most of their headers in every file).
# Off by default; the Dev-Fast preset (and presets that inherit it) turn it on.
option(SC_UNITY_BUILD "Compile the plugin's sources in batches (unity build), for dev builds" OFF)
set(SC_UNITY_BATCH_SIZE 8 CACHE STRING "Sources per unity batch with SC_UNITY_BUILD")

# The trees batched, under src/. They are where header parsing dominates: nearly every one of
# their files includes most of its tree's headers and OpenRenderGraph's. The rest of src/ costs
# about a second of parsing per file, compiles one file per object, and stays close to upstream.
set(SC_UNITY_DIRS "Features/DrawcallLimitFix;RenderGraph")

if(SC_UNITY_BUILD)
    get_target_property(_sc_unity_sources ${PROJECT_NAME} SOURCES)
    set(_sc_unity_regex "")
    foreach(_d IN LISTS SC_UNITY_DIRS)
        list(APPEND _sc_unity_regex "^${CMAKE_SOURCE_DIR}/src/${_d}/.*\\.(cpp|cxx)$")
    endforeach()
    list(JOIN _sc_unity_regex "|" _sc_unity_regex)
    list(FILTER _sc_unity_sources INCLUDE REGEX "${_sc_unity_regex}")
    list(SORT _sc_unity_sources)

    # Batches follow the sorted path order, so neighbours in a directory share a batch and most
    # of their headers. Each directory two levels under src/ (Features/DrawcallLimitFix,
    # RenderGraph/Engine) is cut into batches of its own: adding a file only reshuffles the
    # batches of its own subtree, and small subdirectories still fill whole batches.
    set(_sc_unity_dir "")
    set(_sc_unity_count 0)
    set(_sc_unity_batch 0)
    foreach(_src IN LISTS _sc_unity_sources)
        # Files with their own flags (the CPU cull's AVX kernels) or without the PCH stay alone.
        get_source_file_property(_skip "${_src}" SKIP_UNITY_BUILD_INCLUSION)
        get_source_file_property(_nopch "${_src}" SKIP_PRECOMPILE_HEADERS)
        get_source_file_property(_opts "${_src}" COMPILE_OPTIONS)
        if(_skip OR _nopch OR _opts)
            continue()
        endif()
        file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_src}")
        get_filename_component(_dir "${_rel}" DIRECTORY)
        string(REPLACE "/" ";" _parts "${_dir}")
        list(LENGTH _parts _depth)
        if(_depth GREATER 3)
            list(SUBLIST _parts 0 3 _parts)
        endif()
        list(JOIN _parts "_" _key)
        string(MAKE_C_IDENTIFIER "${_key}" _key)
        if(NOT _key STREQUAL _sc_unity_dir)
            set(_sc_unity_dir "${_key}")
            set(_sc_unity_count 0)
            set(_sc_unity_batch 0)
        elseif(_sc_unity_count EQUAL SC_UNITY_BATCH_SIZE)
            set(_sc_unity_count 0)
            math(EXPR _sc_unity_batch "${_sc_unity_batch} + 1")
        endif()
        math(EXPR _sc_unity_count "${_sc_unity_count} + 1")
        set_source_files_properties("${_src}" PROPERTIES UNITY_GROUP "${_key}_${_sc_unity_batch}")
    endforeach()

    set_target_properties(${PROJECT_NAME} PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)

    # A batch is one translation unit, so each file's file-local names are visible to the files
    # after it. Two settings keep that harmless:
    # - /bigobj: a batch holds more sections than one object allows by default.
    # - C4459 off: a local that shares its name with another file's file-local global is not a
    #   problem in a batch (the local wins, as it does when the file compiles alone). Single-file
    #   builds keep the warning.
    # File-local names that clash outright (two definitions in one namespace) must be renamed.
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /wd4459)
    endif()

    # volk must come before any other Vulkan header in a translation unit (volk.h stops the build
    # otherwise). Alone, every file that uses volk includes it first; in a batch, an earlier file
    # may have included vulkan.h on its own. Including volk at the top of each batch keeps the
    # order. It defines VK_NO_PROTOTYPES; a batched file that calls Vulkan's exported functions
    # directly would fail to compile, and would have to be marked SKIP_UNITY_BUILD_INCLUSION.
    set_property(TARGET ${PROJECT_NAME} PROPERTY UNITY_BUILD_CODE_BEFORE_INCLUDE "#include <volk.h>")

    message(STATUS "Unity build: on, ${SC_UNITY_BATCH_SIZE} sources per batch")
endif()
