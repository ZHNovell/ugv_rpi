#!/bin/bash
#
# Нативная сборка YOLO11 демо для NPU A733 на Orange Pi 4 Pro.
#
# Требования:
#   - Model Zoo v0.9.0 (или v1.1.0) распакован в /root/awnpu_model_zoo-v0.9.0-*/
#   - common/npuruntime/ скопирован из v1.1.0 (libNBGlinker.so, libVIPhal.so)
#   - Системный OpenCV 4.x (sudo apt install libopencv-dev pkg-config)
#   - cmake, gcc, g++, make
#
# Использование:
#   cd /root/ugv_rpi/yolo11/
#   ./build_native.sh
#
set -e

MODEL_ZOO=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b
SRC_DIR=${MODEL_ZOO}/examples/yolo11
BUILD_DIR=${SRC_DIR}/build

echo "=== 1. Проверка зависимостей ==="
which cmake gcc g++ make pkg-config
pkg-config --modversion opencv4

echo "=== 2. Копируем наш CMakeLists.txt (patched) ==="
cp "$(dirname "$0")/CMakeLists.txt.patched" "${SRC_DIR}/CMakeLists.txt"

echo "=== 3. Чистим и создаём build/ ==="
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "=== 4. CMake ==="
cmake -DTARGET_NAME=A733 -DCMAKE_BUILD_TYPE=Release .. 2>&1 | tail -10

echo "=== 5. Make ==="
make -j4 2>&1 | tail -15

echo "=== 6. Install ==="
make install 2>&1 | tail -10

echo "=== 7. Копируем бинарник в репозиторий ==="
INSTALL_DIR="${SRC_DIR}/install/yolo11_demo_linux_a733"
cp "${INSTALL_DIR}/yolo11_demo_a733" "$(dirname "$0")/yolo11_demo_a733"
echo "✅ Готово: $(dirname "$0")/yolo11_demo_a733"
