cmake_minimum_required(VERSION 2.8)

project(yolo11_seg_demo)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_SYSTEM_PROCESSOR arm)



if(EXTERN_DEFINE_TARGET)
    message (STATUS "USE EXTERN DEFINE TARGET")
else()
    #set(TARGET_NAME T527)
    #set(TARGET_NAME T536)
endif()

# ------------------------------- FLAGS -----------------------------
set(MODEL_ZOO_HOME_DIR /root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b)

if(USE_EXTERN_TOOLCHAIN)
    message (STATUS "USE EXTERN TOOLCHAIN BUILD")
else()
    #set(CMAKE_C_COMPILER ${MODEL_ZOO_HOME_DIR}/0-toolchains/arm-openwrt-linux-muslgnueabi/bin/arm-openwrt-linux-muslgnueabi-gcc)
    #set(CMAKE_CXX_COMPILER ${MODEL_ZOO_HOME_DIR}/0-toolchains/arm-openwrt-linux-muslgnueabi/bin/arm-openwrt-linux-muslgnueabi-g++)
    #set(CMAKE_C_COMPILER ${MODEL_ZOO_HOME_DIR}/0-toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc)
    #set(CMAKE_CXX_COMPILER ${MODEL_ZOO_HOME_DIR}/0-toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++)
endif()


message (STATUS "CMAKE_C_COMPILER = ${CMAKE_C_COMPILER}")
message (STATUS "CMAKE_SYSTEM_NAME = ${CMAKE_SYSTEM_NAME}")
message (STATUS "TARGET_NAME = ${TARGET_NAME}")

# NATIVE BUILD ON A733 (aarch64) — forced
# Using NEW ABI (default) for compatibility with system OpenCV 4.10
set(SYS_ARCH linux_aarch64)

if(SYS_ARCH STREQUAL "linux_armhf")
    FIND_PACKAGE(OpenMP REQUIRED)
elseif(SYS_ARCH STREQUAL "android")
    FIND_PACKAGE(OpenMP REQUIRED)
endif()
if(OPENMP_FOUND)
    message("OPENMP FOUND")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

# NATIVE BUILD: use pkg-config for system OpenCV (4.x)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OpenCV REQUIRED opencv4)
message(STATUS "OpenCV via pkg-config: ${OpenCV_LIBRARIES}")
message(STATUS "OpenCV includes: ${OpenCV_INCLUDE_DIRS}")


set(NPU_RT_DIR ${MODEL_ZOO_HOME_DIR}/common/npuruntime)
if(SYS_ARCH STREQUAL "android")
    FIle(GLOB NPU_RT_LIBS "${NPU_RT_DIR}/lib_${SYS_ARCH}/${TARGET_NAME}/${ANDROID_ABI}/*.so*")
else()
    FIle(GLOB NPU_RT_LIBS "${NPU_RT_DIR}/lib_${SYS_ARCH}/${TARGET_NAME}/*.so*")
endif()


# Add the source in project root directory
aux_source_directory(. DIRSRCS)
aux_source_directory(${NPU_RT_DIR} NPULIB_DIRSRCS)

# Add header file include directories
if(SYS_ARCH STREQUAL "linux_armhf")
    include_directories(./ ${NPU_RT_DIR} ${NPU_RT_DIR}/include_85x)
else()
    include_directories(./ ${NPU_RT_DIR} ${NPU_RT_DIR}/include ${OpenCV_INCLUDE_DIRS})
endif()

# Add block directories


string(TOLOWER "${TARGET_NAME}" TARGET_LOWER)
string(TOLOWER "${CMAKE_SYSTEM_NAME}" SYSTEM_LOWER)
# Target

# === YOLO26_depth demo: собираем только 3 файла ===
set(DEMO_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/yolo11_seg_10_pre.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/yolo11_seg_10_post.cpp
)

add_executable(yolo11_seg_demo ${DEMO_SRCS} ${NPULIB_DIRSRCS})
target_link_libraries(yolo11_seg_demo
    ${OpenCV_LIBRARIES}
    ${NPU_RT_LIBS}
    -lpthread
    -lm -ldl
)

# === npu_seg_server: сервер через UNIX-сокет ===
set(SERVER_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/npu_seg_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/yolo11_seg_10_pre.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/yolo11_seg_10_post.cpp
)

add_executable(npu_seg_server ${SERVER_SRCS} ${NPULIB_DIRSRCS})
target_link_libraries(npu_seg_server
    ${OpenCV_LIBRARIES}
    ${NPU_RT_LIBS}
    -lpthread
    -lm -ldl
)

# install
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${PROJECT_NAME})
message (STATUS "CMAKE_INSTALL_PREFIX = ${CMAKE_INSTALL_PREFIX}")
install(TARGETS yolo11_seg_demo npu_seg_server DESTINATION ./)

# Устанавливаем модель и тестовое изображение
file(GLOB MODEL_FILE "model/*.nb")
file(GLOB TEST_IMAGE_FILES "model/*.jpg" "model/*.png")
install(FILES ${MODEL_FILE} DESTINATION ./model/)
install(FILES ${TEST_IMAGE_FILES} DESTINATION ./model/)
