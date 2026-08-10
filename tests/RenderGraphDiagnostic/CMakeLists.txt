cmake_minimum_required(VERSION 3.24)
project(CSRenderGraphDiagnostic LANGUAGES CXX)

add_library(CSRenderGraphProducer SHARED ProducerPlugin.cpp)
add_library(CSRenderGraphConsumer SHARED ConsumerPlugin.cpp)
foreach(_target CSRenderGraphProducer CSRenderGraphConsumer)
	target_compile_features(${_target} PRIVATE cxx_std_20)
	target_include_directories(${_target} PRIVATE ../../include ../../extern/OpenRenderGraph/include)
endforeach()
