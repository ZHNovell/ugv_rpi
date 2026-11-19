# Hector SLAM standalone (ROS-free)

Standalone wrapper around hector_mapping (Hector SLAM), compiled without ROS.

## Build (on the robot)

```bash
# 1. clone hector_slam (once)
git clone --depth 1 https://github.com/tu-darmstadt-ros-pkg/hector_slam.git /root/hector_slam

# 2. patch the ROS dependency (tf) out of UtilFunctions.h
python3 patch_hector.py

# 3. build
mkdir -p /root/hector_standalone
cp hector_standalone.cpp CMakeLists.txt /root/hector_standalone/
cd /root/hector_standalone
cmake -S . -B build
cmake --build build
```

Dependencies: `libeigen3-dev libboost-all-dev build-essential cmake`.

## Protocol

- stdin: one line per scan = `yaw x1 y1 x2 y2 ...` (yaw = IMU heading prior in radians, points in meters in robot frame).
- stdout: one line per scan = `x y yaw` (pose in meters + radians, scan-to-map matched).
