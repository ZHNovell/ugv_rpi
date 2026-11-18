#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"

mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"

echo "Built binaries in $(pwd):"
ls -la axcl_npu_pose_server axcl_npu_seg_server axcl_npu_depth_server
