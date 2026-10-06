cmake_minimum_required(VERSION 2.8)

project(yolo11_demo)

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
# Для yolo11_demo_a733 исключаем npu_server.cpp (у него свой main)
set(DEMO_SRCS "")
foreach(src ${DIRSRCS})
    if(NOT src MATCHES "npu_(server|pose_server)\\.cpp$")
        list(APPEND DEMO_SRCS ${src})
    endif()
endforeach()
add_executable(${PROJECT_NAME}_${TARGET_LOWER} ${DEMO_SRCS} ${NPULIB_DIRSRCS})
target_link_libraries(${PROJECT_NAME}_${TARGET_LOWER} ${OpenCV_LIBRARIES} ${NPU_RT_LIBS} -lm -ldl)


# === NPU server (наш) ===
# Используем ${DIRSRCS} — он содержит все .cpp из текущей папки (включая npu_server.cpp, yolo11_6_post.cpp, yolo11_6_pre.cpp, main.cpp)
# Плюс ${NPULIB_DIRSRCS} — npulib.cpp, npu_util.cpp
# Исключаем main.cpp (там main() для yolo11_demo) из DIRSRCS для npu_server
set(NPU_SERVER_SRCS "")
foreach(src ${DIRSRCS})
    if(NOT src MATCHES "main\\.cpp$")
        list(APPEND NPU_SERVER_SRCS ${src})
    endif()
endforeach()
# Для npu_server собираем только YOLO26-специфичные файлы
# Исключаем: main.cpp (свой main), yolo11_6_post.cpp/yolo11_6_pre.cpp (старые, конфликтуют с YOLO26)
set(NPU_POSE_SRCS "")
foreach(src ${NPU_SERVER_SRCS})
    if(NOT src MATCHES "yolo11_pose_9_(post|pre)\\.cpp$")
        if(NOT src MATCHES "yolo11_6_(post|pre)\\.cpp$")
            if(NOT src MATCHES "npu_server\\.cpp$")
                if(NOT src MATCHES "npu_pose_server\\.cpp$")
                    list(APPEND NPU_POSE_SRCS ${src})
                endif()
            endif()
        endif()
    endif()
endforeach()
# + добавляем YOLO26 файлы
list(APPEND NPU_POSE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/yolo11_pose_9_post.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/yolo11_pose_9_pre.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npu_pose_server.cpp
)
add_executable(npu_pose_server ${NPU_POSE_SRCS} ${NPULIB_DIRSRCS})
target_link_libraries(npu_pose_server
    ${OpenCV_LIBRARIES}
    ${NPU_RT_LIBS}
    -lpthread
    -lm -ldl
)

# install
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${PROJECT_NAME}_${SYSTEM_LOWER}_${TARGET_LOWER})
message (STATUS "CMAKE_INSTALL_PREFIX = ${CMAKE_INSTALL_PREFIX}")
install(TARGETS ${PROJECT_NAME}_${TARGET_LOWER} DESTINATION ./)

if(SYS_ARCH STREQUAL "android")
    install(FILES ${NPU_RT_LIBS} DESTINATION ./lib/)
    install(FILES ${NPU_RT_DIR}/lib_${SYS_ARCH}/libomp.so DESTINATION ./lib/)
endif()

if(SYS_ARCH STREQUAL "android" AND ANDROID_STL STREQUAL "c++_shared")
    get_filename_component(STL_LIB_PATH ${CMAKE_CXX_COMPILER} DIRECTORY)
    set(STL_SHARED_LIB "${STL_LIB_PATH}/../sysroot/usr/lib/aarch64-linux-android/libc++_shared.so")

    if(EXISTS ${STL_SHARED_LIB})
        install(FILES ${STL_SHARED_LIB} DESTINATION lib)
    else()
        message(WARNING "libc++_shared.so not found at: ${STL_SHARED_LIB}")
    endif()
endif()


file(GLOB MODEL_FILE "model/*_${TARGET_LOWER}.nb")
file(GLOB TEST_IMAGE_FILES "model/*.jpg")
install(FILES ${MODEL_FILE} DESTINATION ./model/)
install(FILES ${TEST_IMAGE_FILES} DESTINATION ./model/)

