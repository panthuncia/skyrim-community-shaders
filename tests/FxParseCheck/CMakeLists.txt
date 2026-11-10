# The FX parser's offline check (tools/fx-parse-check) as a test: it parses tools/fx-parse-check/testdata/constructs.fx,
# checks the parser's $Globals layout against FXC's reflection, and fails on any diagnostic. Windows only (D3DCompiler).
#
#   cmake -S tests/FxParseCheck -B build/FxParseCheck
#   cmake --build build/FxParseCheck --config Release
#   ctest --test-dir build/FxParseCheck -C Release
cmake_minimum_required(VERSION 3.21)
project(FxParseCheck LANGUAGES CXX)
enable_testing()

get_filename_component(_cs_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_fx "${_cs_root}/src/Features/Effects11/ORG")

add_executable(fx-parse-check
    "${_cs_root}/tools/fx-parse-check/FxParseCheck.cpp"
    "${_fx}/FxParser.cpp"
    "${_fx}/FxLower.cpp"
    "${_fx}/FxSourceText.cpp")
target_compile_features(fx-parse-check PRIVATE cxx_std_23)
target_include_directories(fx-parse-check PRIVATE "${_cs_root}/src")
# As the tool's header builds it.
if(MSVC)
    target_compile_options(fx-parse-check PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
endif()
target_link_libraries(fx-parse-check PRIVATE d3dcompiler dxguid)

add_test(NAME FxParseCheck.constructs COMMAND fx-parse-check "${_cs_root}/tools/fx-parse-check/testdata/constructs.fx")
