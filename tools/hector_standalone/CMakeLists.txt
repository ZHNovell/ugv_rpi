cmake_minimum_required(VERSION 3.0)
project(hector_standalone)
set(CMAKE_CXX_STANDARD 14)
find_package(Eigen3 REQUIRED)
find_package(Boost REQUIRED COMPONENTS thread)
add_executable(hector_standalone hector_standalone.cpp)
target_include_directories(hector_standalone PRIVATE
  /root/hector_slam/hector_mapping/include
  ${EIGEN3_INCLUDE_DIRS})
target_link_libraries(hector_standalone ${Boost_LIBRARIES} pthread)
