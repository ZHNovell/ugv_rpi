cmake_minimum_required(VERSION 3.16)
project(carto_standalone CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Eigen3 REQUIRED)
find_package(Threads REQUIRED)

file(GLOB ABSL_STATIC_LIBS "/usr/local/lib/libabsl_*.a")

add_executable(carto_standalone carto_standalone.cpp)

target_include_directories(carto_standalone PRIVATE
  ${EIGEN3_INCLUDE_DIR}
  /usr/include/lua5.2
  /usr/include/cairo)

target_compile_options(carto_standalone PRIVATE -include/root/tsan_fix.h)

target_link_libraries(carto_standalone PRIVATE
  "-Wl,--start-group"
  /usr/local/lib/libcartographer.a
  ${ABSL_STATIC_LIBS}
  /usr/local/lib/libceres.a
  /usr/local/lib/libprotobuf.a
  "-Wl,--end-group"
  lua5.2
  boost_iostreams
  gflags
  glog
  cairo
  lapack
  blas
  gomp
  Threads::Threads
  z)
