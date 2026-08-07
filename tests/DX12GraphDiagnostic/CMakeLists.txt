cmake_minimum_required(VERSION 3.24)
project(CSDX12GraphDiagnostic LANGUAGES CXX)

add_library(CSDX12GraphDiagnostic SHARED DiagnosticPlugin.cpp)
target_compile_features(CSDX12GraphDiagnostic PRIVATE cxx_std_20)
target_include_directories(CSDX12GraphDiagnostic PRIVATE ../../include)
target_link_libraries(CSDX12GraphDiagnostic PRIVATE d3d12)
