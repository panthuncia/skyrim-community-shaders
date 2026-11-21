include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/../extern/ORGModuleServices/cmake/AsyncPrimitives.cmake)
find_package(Threads REQUIRED)
if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()
# CS's executor (src/RenderGraph/Async/Executor.cpp): built without the game's PCH, so its tests link it alone.
add_library(FrameExecutor STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/RenderGraph/Async/Executor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/RenderGraph/Async/Executor.h)
target_compile_features(FrameExecutor PUBLIC cxx_std_20)
target_include_directories(FrameExecutor PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../src)
target_link_libraries(FrameExecutor PUBLIC ORGModuleServices::AsyncPrimitives Threads::Threads PRIVATE TBB::tbb)
