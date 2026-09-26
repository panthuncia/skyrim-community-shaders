# Standalone end-to-end test of the render graph on DXVK's device (no game needed).
#
#   cmake -S tests/OrgDxvkInterop -B build/OrgDxvkInterop \
#         -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ...
#
# The libraries come from the extern/ submodules unless CS_ORG_ROOT points elsewhere.
#   build/OrgDxvkInterop/<config>/OrgDxvkInteropTest.exe extern/dxvk/build/src/d3d11
#
# The DXVK directory must hold both dxvk_d3d11.dll and dxvk_dxgi.dll (tools/build-dxvk.ps1
# builds them into separate folders; copy dxvk_dxgi.dll next to dxvk_d3d11.dll).
cmake_minimum_required(VERSION 3.21)
project(OrgDxvkInterop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(_cs_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CS_ORG_ROOT "${_cs_root}/extern" CACHE PATH "Directory holding BasicRHI, BasicTelemetry, OpenRenderGraph and volk")

# Same configuration as cmake/RenderGraph.cmake uses for the plugin.
set(BASICRHI_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_STREAMLINE OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_PIX OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_TRACY_GPU_PROFILING OFF CACHE BOOL "" FORCE)
set(BASICTELEMETRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BASICTELEMETRY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BASICTELEMETRY_BUILD_ARTIFACTS OFF CACHE BOOL "" FORCE)
set(OPENRENDERGRAPH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CS_ORG_ROOT}/OpenRenderGraph" "${CMAKE_BINARY_DIR}/OpenRenderGraph" EXCLUDE_FROM_ALL)

include("${CS_ORG_ROOT}/BasicRHI/cmake/BasicRHIShaderFlags.cmake")
set(_spv "${CMAKE_BINARY_DIR}/WriteValues.spv")
basicrhi_compile_spirv(OUTPUT "${_spv}" SOURCE "${CMAKE_CURRENT_LIST_DIR}/WriteValues.hlsl" ENTRY main PROFILE cs_6_6)

add_executable(OrgDxvkInteropTest OrgDxvkInteropTest.cpp ImageHandoffTests.cpp ImageHandoffTests.h "${_spv}")
target_include_directories(OrgDxvkInteropTest PRIVATE "${_cs_root}/src")
target_compile_definitions(OrgDxvkInteropTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN ORG_DXVK_TEST_SPV="${_spv}")
if(MSVC)
    target_compile_options(OrgDxvkInteropTest PRIVATE /utf-8)
endif()
target_link_libraries(OrgDxvkInteropTest PRIVATE OpenRenderGraph::OpenRenderGraph BasicRHI::BasicRHI d3dcompiler)

enable_testing()
add_executable(ExternalAccessLedgerTests ExternalAccessLedgerTests.cpp)
target_compile_features(ExternalAccessLedgerTests PRIVATE cxx_std_20)
add_test(NAME ExternalAccessLedgerTests COMMAND ExternalAccessLedgerTests)
set(ORG_DXVK_TEST_DXVK_DIR "${_cs_root}/extern/dxvk/build/src/d3d11" CACHE PATH "Directory with dxvk_d3d11.dll and dxvk_dxgi.dll")
add_test(NAME OrgDxvkInteropTest.lock COMMAND OrgDxvkInteropTest "${ORG_DXVK_TEST_DXVK_DIR}" lock)
add_test(NAME OrgDxvkInteropTest.stream COMMAND OrgDxvkInteropTest "${ORG_DXVK_TEST_DXVK_DIR}" stream)

add_test(NAME OrgDxvkInteropTest.resources COMMAND OrgDxvkInteropTest "${ORG_DXVK_TEST_DXVK_DIR}" resources)

add_test(NAME OrgDxvkInteropTest.scoped COMMAND OrgDxvkInteropTest "${ORG_DXVK_TEST_DXVK_DIR}" scoped)

# Explicit opt-in: requires the Khronos validation layer installed locally.
option(ORG_DXVK_TEST_SYNCHRONIZATION_VALIDATION "Run scoped GPU handoff with required synchronization validation" OFF)
if(ORG_DXVK_TEST_SYNCHRONIZATION_VALIDATION)
    add_test(NAME OrgDxvkInteropTest.scoped-validation COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:OrgDxvkInteropTest>"
        "-DDXVK_DIRECTORY=${ORG_DXVK_TEST_DXVK_DIR}"
        "-DOUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/scoped-validation"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RunSynchronizationValidation.cmake")
endif()
set_tests_properties(OrgDxvkInteropTest.lock OrgDxvkInteropTest.stream
    OrgDxvkInteropTest.resources OrgDxvkInteropTest.scoped PROPERTIES RUN_SERIAL TRUE)

add_test(NAME OrgDxvkInteropTest.leased COMMAND OrgDxvkInteropTest "${ORG_DXVK_TEST_DXVK_DIR}" leased)
set_tests_properties(OrgDxvkInteropTest.leased PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)

if(ORG_DXVK_TEST_SYNCHRONIZATION_VALIDATION)
    add_test(NAME OrgDxvkInteropTest.leased-validation COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:OrgDxvkInteropTest>"
        "-DDXVK_DIRECTORY=${ORG_DXVK_TEST_DXVK_DIR}"
        "-DOUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/leased-validation"
        -DTEST_MODE=leased -P "${CMAKE_CURRENT_SOURCE_DIR}/RunSynchronizationValidation.cmake")
    set_tests_properties(OrgDxvkInteropTest.leased-validation PROPERTIES RUN_SERIAL TRUE TIMEOUT 70)
endif()

add_test(NAME OrgDxvkInteropTest.native-buffer COMMAND OrgDxvkInteropTest "${ORG_DXVK_TEST_DXVK_DIR}" native-buffer)
add_test(NAME OrgDxvkInteropTest.native-image COMMAND OrgDxvkInteropTest "${ORG_DXVK_TEST_DXVK_DIR}" native-image)
set_tests_properties(OrgDxvkInteropTest.native-image PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
set_tests_properties(OrgDxvkInteropTest.native-buffer PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
if(ORG_DXVK_TEST_SYNCHRONIZATION_VALIDATION)
    add_test(NAME OrgDxvkInteropTest.native-buffer-validation COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:OrgDxvkInteropTest>"
        "-DDXVK_DIRECTORY=${ORG_DXVK_TEST_DXVK_DIR}"
        "-DOUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/native-buffer-validation"
        -DTEST_MODE=native-buffer -P "${CMAKE_CURRENT_SOURCE_DIR}/RunSynchronizationValidation.cmake")
    set_tests_properties(OrgDxvkInteropTest.native-buffer-validation PROPERTIES RUN_SERIAL TRUE TIMEOUT 70)
    add_test(NAME OrgDxvkInteropTest.native-image-validation COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:OrgDxvkInteropTest>"
        "-DDXVK_DIRECTORY=${ORG_DXVK_TEST_DXVK_DIR}"
        "-DOUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/native-image-validation"
        -DTEST_MODE=native-image -P "${CMAKE_CURRENT_SOURCE_DIR}/RunSynchronizationValidation.cmake")
    set_tests_properties(OrgDxvkInteropTest.native-image-validation PROPERTIES RUN_SERIAL TRUE TIMEOUT 70)
endif()
