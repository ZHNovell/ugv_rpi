# AXCL NPU Servers (Radxa AICore / AX650N)

C++ NPU servers running YOLO models on the Radxa AICore (AX-M1, AX650N) via AXCL.
Each server holds the model in memory and exposes a UNIX socket that the Python
clients in the project root talk to.

| Server | Model | Socket |
|---|---|---|
| `axcl_npu_pose_server` | yolo26m-pose_npu3.axmodel | `/tmp/npu_pose_axcl.sock` |
| `axcl_npu_seg_server` | yolo26m-seg.axmodel | `/tmp/npu_seg_axcl.sock` |
| `axcl_npu_depth_server` | yolo26m-depth.axmodel | `/tmp/npu_depth_axcl.sock` |

## Build

Requirements: AXCL SDK (`/usr/include/axcl`, `/usr/lib/axcl`), OpenCV 4, CMake.

```bash
./build.sh
```

Binaries are written to `build/`.

## Protocol

- Client -> Server: `[4 bytes BE width][4 bytes BE height][4 bytes BE channels][raw BGR]`
- Server -> Client: `[4 bytes BE json length][json bytes]`

## Notes

- `ax_model_runner/` is adapted from AXERA-TECH/axcl-samples (BSD-3-Clause).
- Models live in `../models_axcl/` and are not committed (see `.gitignore`).
