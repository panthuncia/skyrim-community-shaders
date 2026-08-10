cmake_minimum_required(VERSION 3.25)
project(CSRenderGraphRegistryTests LANGUAGES CXX)

add_executable(cs_render_graph_source_policy_tests RegistryTests.cpp)
target_compile_features(cs_render_graph_source_policy_tests PRIVATE cxx_std_20)
target_compile_definitions(cs_render_graph_source_policy_tests PRIVATE
	CS_TEST_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
enable_testing()
add_test(NAME cs_render_graph_source_policy_tests COMMAND cs_render_graph_source_policy_tests)
