#!/bin/bash
#
# Нативная сборка демо + сервера YOLO11_seg на Orange Pi 4 Pro (A733).
#
set -e

REPO_DIR=$(cd "$(dirname "$0")" && pwd)
MODEL_ZOO=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b
SRC_DIR=${REPO_DIR}
BUILD_DIR=${REPO_DIR}/build

echo "=== 1. Проверка зависимостей ==="
which cmake gcc g++ make pkg-config
pkg-config --modversion opencv4

echo "=== 2. Копируем CMakeLists.txt.patched -> CMakeLists.txt ==="
cp "${REPO_DIR}/CMakeLists.txt.patched" "${SRC_DIR}/CMakeLists.txt"

echo "=== 3. Чистим и создаём build/ ==="
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "=== 4. CMake ==="
cmake -DTARGET_NAME=A733 -DCMAKE_BUILD_TYPE=Release .. 2>&1 | tail -10

echo "=== 5. Make yolo11_seg_demo + npu_seg_server ==="
make yolo11_seg_demo npu_seg_server -j4 2>&1 | tail -20

echo "=== 6. Копируем бинарники в репозиторий ==="
cp "${BUILD_DIR}/yolo11_seg_demo" "${REPO_DIR}/yolo11_seg_demo"
cp "${BUILD_DIR}/npu_seg_server" "${REPO_DIR}/npu_seg_server"
echo "✅ Готово:"
echo "  ${REPO_DIR}/yolo11_seg_demo"
echo "  ${REPO_DIR}/npu_seg_server"
