cmake_minimum_required(VERSION 3.25)
project(CSRenderGraphABI LANGUAGES C)

add_executable(cs_render_graph_c_header HeaderCompile.c)
target_include_directories(cs_render_graph_c_header PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include")
set_property(TARGET cs_render_graph_c_header PROPERTY C_STANDARD 11)
enable_testing()
add_test(NAME cs_render_graph_c_header COMMAND cs_render_graph_c_header)
