cmake_minimum_required(VERSION 3.13)
project(ugv_axcl_servers C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

# AXCL SDK (Radxa AICore / AX650N)
set(AXCL_INCLUDE_DIR /usr/include/axcl)
set(AXCL_LIB_DIR /usr/lib/axcl)

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${AXCL_INCLUDE_DIR})
link_directories(${AXCL_LIB_DIR})

function(add_axcl_server target source)
    add_executable(${target} ${source} ax_model_runner/ax_model_runner_axcl.cpp)
    target_link_libraries(${target} PRIVATE ${OpenCV_LIBS} axcl_rt ${CMAKE_THREAD_LIBS_INIT})
    target_compile_options(${target} PRIVATE -O3)
endfunction()

add_axcl_server(axcl_npu_pose_server  npu_pose_axcl_server.cpp)
add_axcl_server(axcl_npu_seg_server   npu_seg_axcl_server.cpp)
add_axcl_server(axcl_npu_depth_server npu_depth_axcl_server.cpp)
