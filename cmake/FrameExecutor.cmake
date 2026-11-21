include_guard(GLOBAL)
# CS's executor is OMS's (extern/ORGModuleServices/include/ORGModuleServices/Async/Executor.h), built without the game's PCH.
# FrameExecutor adds CS's src folder for the forwarding header RenderGraph/Async/Executor.h, so its tests link it alone.
include(${CMAKE_CURRENT_LIST_DIR}/../extern/ORGModuleServices/cmake/AsyncExecutor.cmake)
add_library(FrameExecutor INTERFACE)
target_include_directories(FrameExecutor INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../src)
target_link_libraries(FrameExecutor INTERFACE ORGModuleServices::AsyncExecutor)
