# OpenRenderGraph on DXVK's Vulkan device (src/RenderGraph, Light Limit Fix light culling).
#
# BasicRHI adopts the VkDevice DXVK creates for the game, and OpenRenderGraph runs its
# passes on it in persistent execution. The libraries are built from source with the
# Vulkan backend, and the D3D12 one with CS_ORG_D3D12 (the native mode's
# CS_ORG_BACKEND=d3d12), from the extern/ submodules (BasicRHI, BasicTelemetry,
# OpenRenderGraph, volk). To build against local changes to those libraries, point
# CS_ORG_ROOT at another directory with the same side-by-side layout.
# They are required: every build has the render graph. Plain D3D11 (CS_ORG=0) is a runtime mode.

# BasicRHI's D3D12 backend too, for the native mode's CS_ORG_BACKEND=d3d12 (src/RenderGraph/Engine/D3D12Device.cpp).
# Off builds the Vulkan backend alone; CS_ORG_BACKEND=d3d12 then fails at startup with that reason.
option(CS_ORG_D3D12 "Build BasicRHI's D3D12 backend for CS_ORG_BACKEND=d3d12" ON)
set(CS_ORG_ROOT "${CMAKE_SOURCE_DIR}/extern" CACHE PATH
    "Directory holding BasicRHI, BasicTelemetry, OpenRenderGraph and volk side by side")

set(CS_DXC_RUNTIME_DLLS)

set(_cs_org_missing)
foreach(_lib IN ITEMS OpenRenderGraph BasicRHI BasicTelemetry volk ORGModuleServices)
    if(NOT EXISTS "${CS_ORG_ROOT}/${_lib}/CMakeLists.txt")
        list(APPEND _cs_org_missing "${_lib}")
    endif()
endforeach()

if(_cs_org_missing)
    message(FATAL_ERROR "Render graph: ${_cs_org_missing} not found under CS_ORG_ROOT=${CS_ORG_ROOT}")
endif()

# The Vulkan backend always: CS reaches the GPU through DXVK by default. The D3D12 backend with CS_ORG_D3D12, for the
# native mode's CS_ORG_BACKEND=d3d12. PIX and Tracy GPU zones belong to the host. Streamline: CS owns slInit and the
# evaluates; native mode's device comes up through BasicRHI's Streamline support (DeviceCreateInfo::streamlineHost), on the
# fork's headers. BasicRHI's direct Streamline calls resolve to CS's forwarders (Features/Upscaling/StreamlineImports.cpp).
set(BASICRHI_ENABLE_D3D12 ${CS_ORG_D3D12} CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_STREAMLINE ON CACHE BOOL "" FORCE)
set(BASICRHI_STREAMLINE_HEADERS_DIR "${CMAKE_SOURCE_DIR}/extern/Streamline/include" CACHE PATH "" FORCE)
set(BASICRHI_ENABLE_PIX OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_RESHAPE OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_TRACY_GPU_PROFILING OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_TRACY_CPU_PROFILING ${TRACY_SUPPORT} CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_IMGUI OFF CACHE BOOL "" FORCE)
set(BASICRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# CS links the compiled spdlog library (SPDLOG_COMPILED_LIB); header-only copies would collide.
set(BASICRHI_SPDLOG_TARGET spdlog::spdlog CACHE STRING "" FORCE)
set(OPENRENDERGRAPH_SPDLOG_TARGET spdlog::spdlog CACHE STRING "" FORCE)
set(ORG_ASYNC_STATE_GRAPH_SPDLOG_TARGET spdlog::spdlog CACHE STRING "" FORCE)
set(BASICTELEMETRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BASICTELEMETRY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BASICTELEMETRY_BUILD_ARTIFACTS OFF CACHE BOOL "" FORCE)
set(OPENRENDERGRAPH_ENABLE_SUBMODULE_FALLBACK ON CACHE BOOL "" FORCE)
set(OPENRENDERGRAPH_FORWARD_BASICRHI_DEP_OPTIONS OFF CACHE BOOL "" FORCE)
# The persistent schedule inspector (the menu's "Render graph schedule" window): Dear ImGui only, CS's imgui::imgui.
set(OPENRENDERGRAPH_ENABLE_SCHEDULE_UI ON CACHE BOOL "" FORCE)
# The memory introspection widget (the menu's "Render graph memory" window): Dear ImGui and ImPlot.
set(OPENRENDERGRAPH_ENABLE_MEMORY_UI ON CACHE BOOL "" FORCE)
set(OPENRENDERGRAPH_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Nsight Perf for OpenRenderGraph's Telemetry/NvPerfCapture (NvPerf's Vulkan backend): ORG_ENABLE_NVPERF and the
# NVPerf::Host target come from CMakeLists.txt's NVPERF_SUPPORT block, which also delay-loads and installs the DLL.
add_subdirectory("${CS_ORG_ROOT}/OpenRenderGraph" "${CMAKE_BINARY_DIR}/extern/OpenRenderGraph" EXCLUDE_FROM_ALL)

# Runtime SPIR-V compilation (every render-graph shader): ORGModuleServices' content-addressed DXC service. Required.
set(ORG_MODULE_SERVICES_ENABLE_DXC ON CACHE BOOL "" FORCE)
set(ORG_MODULE_SERVICES_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(ORG_MODULE_SERVICES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CS_ORG_ROOT}/ORGModuleServices" "${CMAKE_BINARY_DIR}/extern/ORGModuleServices" EXCLUDE_FROM_ALL)
target_link_libraries(${PROJECT_NAME} PRIVATE ORGModuleServices::ORGModuleServices)
# OMS's passes' shaders (GenerateMips.hlsl, ResolveMultisample.hlsl and their include), which CS compiles at runtime
# with ComputeProgram from Data/Shaders/ORGModuleServices (org::services::kGenerateMipsShader). Copied into
# package/Shaders at configure time, so every path that ships package/Shaders ships them; the copies are generated
# (.gitignore) and follow the OMS checkout.
if(ORG_MODULE_SERVICES_SHADER_DIR)
    file(GLOB _oms_pass_shaders "${ORG_MODULE_SERVICES_SHADER_DIR}/*.hlsl" "${ORG_MODULE_SERVICES_SHADER_DIR}/*.hlsli")
    file(COPY ${_oms_pass_shaders} DESTINATION "${CMAKE_SOURCE_DIR}/package/Shaders/ORGModuleServices")
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_oms_pass_shaders})
endif()
# The Vulkan SDK's DXC: it has SPIR-V code generation (the Windows SDK's does not).
foreach(_dll IN ITEMS dxcompiler.dll dxil.dll)
    if(EXISTS "$ENV{VULKAN_SDK}/Bin/${_dll}")
        list(APPEND CS_DXC_RUNTIME_DLLS "$ENV{VULKAN_SDK}/Bin/${_dll}")
    endif()
endforeach()
if(NOT CS_DXC_RUNTIME_DLLS)
    message(WARNING "Render graph: VULKAN_SDK has no dxcompiler.dll; runtime SPIR-V compilation will be unavailable in game")
endif()

# The render-graph compute passes compile their HLSL at runtime through ORGModuleServices (src/RenderGraph/
# ComputeProgram.cpp), like Drawcall Limit Fix's Lighting builds; the sources ship with the other shaders.
target_link_libraries(${PROJECT_NAME} PRIVATE OpenRenderGraph::OpenRenderGraph OpenRenderGraph::ScheduleUI OpenRenderGraph::MemoryUI BasicRHI::BasicRHI)
# Unbounded lock-free queues (tbb::concurrent_queue): the render graph's compute submitter. Static, as DCLF's executor's.
if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE TBB::tbb)
# CS's executor (src/RenderGraph/Async): the frame's lanes, graph and program builds, and DCLF's jobs run on it.
include(${CMAKE_CURRENT_LIST_DIR}/FrameExecutor.cmake)
target_link_libraries(${PROJECT_NAME} PRIVATE FrameExecutor)
if(CS_ORG_D3D12)
    # d3d12.h and the DXGI headers of the DirectX-Headers package come with BasicRHI (Microsoft::DirectX-Headers).
    target_compile_definitions(${PROJECT_NAME} PRIVATE CS_ORG_HAS_D3D12=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d12 dxgi)
endif()
# Debug info in the render-graph libraries, so hang and crash dumps name their frames and locals (they build without it).
if(MSVC)
    foreach(_org_target IN ITEMS OpenRenderGraph BasicRHI)
        if(TARGET ${_org_target})
            target_compile_options(${_org_target} PRIVATE /Z7)
        endif()
    endforeach()
endif()

message(STATUS "Render graph: OpenRenderGraph from ${CS_ORG_ROOT}")

# Drawcall Limit Fix (src/Features/DrawcallLimitFix/), on ORGModuleServices.
get_target_property(_cs_dclf_plugin_sources ${PROJECT_NAME} SOURCES)
# The source glob also finds the executor; it is compiled only in its own library (FrameExecutor), without the game's PCH.
list(FILTER _cs_dclf_plugin_sources EXCLUDE REGEX "/src/RenderGraph/Async/Executor\.cpp$")
include("${CS_ORG_ROOT}/ORGModuleServices/cmake/AsyncPrimitives.cmake")
target_link_libraries(${PROJECT_NAME} PRIVATE ORGModuleServices::AsyncPrimitives)
# DCLF's CPU publication graph shares ORGModuleServices' scheduler/runtime; it is linked explicitly and can be exercised
# without a live frame switch.
include(${CMAKE_CURRENT_LIST_DIR}/DclfSceneGraph.cmake)
target_link_libraries(${PROJECT_NAME} PRIVATE DclfSceneGraph)
# The engine's scene writer hooks are SceneWriterFramework's (extern/SceneWriterFramework), shared with every plugin that
# embeds it or installs its standalone DLL. CS's copy is one candidate in its provider election (DCLF::SceneWriter).
include(${CMAKE_SOURCE_DIR}/extern/SceneWriterFramework/cmake/SWFEmbed.cmake)
target_link_libraries(${PROJECT_NAME} PRIVATE swf::embedded)
# The plugin's source glob also finds these two; they are compiled only in their independently testable libraries, without
# the game's PCH.
list(FILTER _cs_dclf_plugin_sources EXCLUDE REGEX "/SceneGraph\.cpp$")
# The CPU cull's frustum kernels and CPU detection: ORGModuleServices' sphere cull. Its kernels get the plugin's floating-point and
# optimisation options, so they build as they did inside the plugin.
include("${CS_ORG_ROOT}/ORGModuleServices/cmake/SphereCull.cmake")
if(MSVC)
    target_compile_options(ORGSphereCull PRIVATE "$<$<CONFIG:DEBUG>:${SC_DEBUG_OPTS}>" "$<$<CONFIG:RELEASE>:${SC_RELEASE_OPTS}>")
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ORGModuleServices::SphereCull)
# The CPU cull's paraboloid and portal kernels (Core/Render/Direct/CullSimd.h): each file built for its instruction set alone, without
# the game's PCH (built for the baseline), and chosen at run time by what the CPU supports.
set(_cs_dclf_simd ${CMAKE_SOURCE_DIR}/src/Features/DrawcallLimitFix/Core/Render/Direct)
if(MSVC)
    set(_cs_dclf_avx2 /arch:AVX2)
    set(_cs_dclf_avx512 /arch:AVX512)
else()
    set(_cs_dclf_avx2 -mavx2)
    set(_cs_dclf_avx512 -mavx512f -mavx512bw -mavx512vl)
endif()
set_source_files_properties(${_cs_dclf_simd}/CullAvx2.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "${_cs_dclf_avx2}")
set_source_files_properties(${_cs_dclf_simd}/CullAvx512.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "${_cs_dclf_avx512}")
message(STATUS "Drawcall Limit Fix: built")
set_property(TARGET ${PROJECT_NAME} PROPERTY SOURCES ${_cs_dclf_plugin_sources})
