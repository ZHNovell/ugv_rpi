#!/bin/bash
#
# Нативная сборка NPU pose-сервера для YOLO11_pose на Orange Pi 4 Pro (A733).
#
# Требования:
#   - Model Zoo v0.9.0 или v1.1.0 в /root/awnpu_model_zoo-v0.9.0-*/
#   - common/npuruntime/ (libNBGlinker.so, libVIPhal.so)
#   - Системный OpenCV 4.x (libopencv-dev pkg-config)
#   - cmake, gcc, g++, make
#
set -e

MODEL_ZOO=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b
SRC_DIR=${MODEL_ZOO}/examples/yolo11_pose
BUILD_DIR=${SRC_DIR}/build
REPO_DIR=$(dirname "$0")

echo "=== 1. Проверка зависимостей ==="
which cmake gcc g++ make pkg-config
pkg-config --modversion opencv4

echo "=== 2. Копируем наши файлы в Model Zoo ==="
mkdir -p "${SRC_DIR}"
cp "${REPO_DIR}/CMakeLists.txt.patched" "${SRC_DIR}/CMakeLists.txt"
cp "${REPO_DIR}/npu_pose_server.cpp" "${SRC_DIR}/npu_pose_server.cpp"
cp "${REPO_DIR}/yolo11_pose_9_post.cpp" "${SRC_DIR}/yolo11_pose_9_post.cpp"
cp "${REPO_DIR}/yolo11_pose_9_pre.cpp" "${SRC_DIR}/yolo11_pose_9_pre.cpp"
cp "${REPO_DIR}/model_config.h" "${SRC_DIR}/model_config.h"

echo "=== 3. Чистим и создаём build/ ==="
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "=== 4. CMake ==="
cmake -DTARGET_NAME=A733 -DCMAKE_BUILD_TYPE=Release .. 2>&1 | tail -10

echo "=== 5. Make npu_pose_server ==="
make npu_pose_server -j4 2>&1 | tail -15

echo "=== 6. Копируем бинарник в репозиторий ==="
cp "${BUILD_DIR}/npu_pose_server" "${REPO_DIR}/npu_pose_server"
echo "✅ Готово: ${REPO_DIR}/npu_pose_server"
