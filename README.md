# Orange Pi 4 Pro UGV Robot
[🇷🇺 Русская версия](README.ru.md)

![GitHub top language](https://img.shields.io/github/languages/top/ZHNovell/ugv_rpi)
![GitHub language count](https://img.shields.io/github/languages/count/ZHNovell/ugv_rpi)
![GitHub code size in bytes](https://img.shields.io/github/languages/code-size/ZHNovell/ugv_rpi)
![GitHub repo size](https://img.shields.io/github/repo-size/ZHNovell/ugv_rpi)
![GitHub](https://img.shields.io/github/license/ZHNovell/ugv_rpi)
![GitHub last commit](https://img.shields.io/github/last-commit/ZHNovell/ugv_rpi)

<img width="1088" height="721" alt="UGV Robot" src="https://github.com/user-attachments/assets/368fbf34-2087-4e6b-b819-b5fca5f83acd" />

**UGV robot** based on **Orange Pi 4 Pro** (Allwinner A733, NPU 3 TOPS) with an optional **Radxa AICore AX-M1** (AX650N) accelerator and **4 NPU models**: detection, pose, depth, segmentation. It runs **autonomously** (SPI + NVMe) and is controlled via a **web interface**.
## 📋 Table of Contents

1. [Hardware platform](#-hardware-platform)
2. [Architecture](#-architecture)
3. [Building Armbian](#-building-armbian)
4. [Installing the OS](#-installing-the-os)
5. [Storage: eMMC and NVMe](#-storage-emmc-and-nvme)
6. [eMMC CQE bug](#-emmc-cqe-bug)
7. [Interface setup](#-interface-setup)
8. [Dependencies](#-dependencies)
9. [MediaPipe (HAND GS)](#-mediapipe-hand-gs)
10. [Motion (Object Detection via YOLO26s NPU)](#-motion-object-detection-via-yolo26s-npu)
11. [MP POSE (Elbow angle + Adaptive PT)](#-mp-pose-elbow-angle--adaptive-pt)
12. [MP FACE (Nod / Turn)](#-mp-face-nod--turn)
13. [Emergency Stop](#-emergency-stop-emergency-stop)
14. [NPU servers](#-npu-servers)
15. [Code adaptation](#-code-adaptation)
16. [Web interface](#-web-interface)
17. [Autostart](#-autostart)
18. [JSON commands](#-json-commands-esp32)
19. [USB camera](#-usb-camera-uvc)
20. [Video recording](#-video-recording)
21. [Stress tests](#-stress-tests)
22. [Changelog](#-changelog)
23. [Links](#-links)
## 🔧 Hardware platform

- **Orange Pi 4 Pro** — Allwinner A733, 8 cores (4×A76 + 4×A55), 12 GB RAM, NPU 3 TOPS INT8.
- **General Driver for Robots** — Waveshare, ESP32-WROOM-32UE, 2×MX1919, INA219, OLED SSD1306 (0x3C).
- **2-Axis Pan-Tilt Camera Module** — ST3215 servos, UART.
- **WAVE ROVER / UGV Rover chassis** — 4 motors, 4 encoders, 3S Li-Ion UPS.
- **USB camera** — navigation (UVC).
- **CSI camera** — optional (overview).
- **Radxa AICore AX-M1** — PCIe accelerator on the AX650N (optional, for pose/seg/depth).

**Pinout (40-pin):**

| Orange Pi Pin | Function | General Driver Pin |
|---|---|---|
| 8 | UART7 TX | RX (ESP32) |
| 10 | UART7 RX | TX (ESP32) |
| 1 | 3.3V | 3.3V |
| 2, 4 | 5V | 5V |
| 6, 9, 14... | GND | GND |

**Important:** OLED, INA219, and the servos are controlled by the **ESP32** via **internal buses**. Orange Pi does **not touch** them.

## 📋 Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     Orange Pi 4 Pro                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │
│  │  app.py     │  │ cv_ctrl.py  │  │   os_info.py    │  │
│  │  (Flask)    │──│ (CV ctrl)   │──│  (monitoring)   │  │
│  └──────┬──────┘  └──────┬──────┘  └─────────────────┘  │
│         │                │                              │
│         │       ┌────────┴────────┐                     │
│         │       │  4 NPU clients  │                     │
│         │       │  (UNIX sockets) │                     │
│         │       └────────┬────────┘                     │
│         │                │                              │
│  ┌──────┴──────┐  ┌──────┴──────┐  ┌─────────────────┐  │
│  │ gst_stream  │  │ npu_server  │  │ npu_pose_server │  │
│  │ (GStreamer) │  │ (YOLO26s)   │  │ (YOLO11_pose)   │  │
│  └─────────────┘  └─────────────┘  └─────────────────┘  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │
│  │ npu_seg_    │  │ npu_depth_  │  │ aruco_client    │  │
│  │ server      │  │ server      │  │ (OpenCV CPU)    │  │
│  │ (YOLO11_seg)│  │ (YOLO26n_   │  │                 │  │
│  │             │  │  depth)     │  │                 │  │
│  └─────────────┘  └─────────────┘  └─────────────────┘  │
└─────────────────────────────────────────────────────────┘
                           │ UART7 (JSON)
                 ┌─────────┴─────────┐
                 │      ESP32        │
                 │  (General Driver) │
                 │ motors/servos/IMU │
                 └───────────────────┘
```

**Key decisions:**
- **4 NPU servers** via UNIX sockets (RAW BGR).
- **ArUco** — via OpenCV (CPU), not the NPU.
- Video via GStreamer `tee` — preview and recording simultaneously.
- CV modes are switched via JSON commands (`{"T":10304}` = OBJECTS, `{"T":10309}` = MP POSE, `{"T":10310}` = DEPTH, `{"T":10311}` = SEG, `{"T":10312}` = ARUCO).
## 📁 Project structure

```
ugv_rpi/
├── app.py                 # entry point (Flask + Socket.IO)
├── config.yaml            # configuration
├── requirements.txt
├── core/                  # core modules
│   ├── cv_ctrl.py         # CV controller
│   ├── base_ctrl.py       # UART → ESP32
│   ├── gst_stream.py      # GStreamer pipeline
│   ├── os_info.py         # monitoring
│   ├── audio_ctrl.py
│   └── pt_stabilizer.py
├── modules/               # NPU clients (UNIX sockets)
│   ├── npu_client.py      # detection
│   ├── npu_pose_client.py
│   ├── npu_seg_client.py
│   ├── npu_depth_client.py
│   └── aruco_client.py
├── scripts/               # shell scripts
├── tools/                 # utilities (calibration, tests)
├── axcl_servers/          # C++ AXCL servers (AX-M1)
├── templates/             # web interface
├── controllers/           # joystick, RoArm
├── models/                # models (MediaPipe, calibration)
├── models_axcl/           # AX-M1 models (.axmodel)
├── yolo*/                 # A733 NPU servers + conversion
└── ugv-env/               # virtual environment
```

## 🛠️ Building Armbian

**Build repository:** [armbian/build](https://github.com/armbian/build) — Orange Pi 4 Pro support is already in the main branch.

**Build commands:**

```bash
git clone https://github.com/armbian/build.git
cd build

./compile.sh BOARD=orangepi4pro BRANCH=vendor RELEASE=trixie \
  BUILD_DESKTOP=no BUILD_MINIMAL=yes \
  KERNEL_CONFIGURE=no KERNEL_BTF=no KERNEL_GIT=shallow
```

**Result:** `Armbian-unofficial_26.11.0-trunk_Orangepi4pro_trixie_vendor_6.6.98_minimal.img`

**What's included:**
- Kernel 6.6.98-vendor-sun60iw2.
- DTB: sun60i-a733-orangepi-4-pro.dtb.
- GPU PowerVR BXM-4-64 (EGL/GLES/GBM/Vulkan/OpenCL).
- NPU VIPLite (`/dev/vipcore`, `libVIPhal.so`, `libNBGlinker.so`).
- VPU CedarC (`/dev/cedar_dev`, `/dev/cedar_dev_ve2`).
- ISP (libAWIspApi) — for CSI cameras.
- GStreamer OMX (`gstreamer1.0-omx-allwinner`).

## 💾 Installing the OS

**Writing to an SD card:**

```bash
unxz Armbian-...-minimal.img.xz
sudo dd if=Armbian-unofficial_26.11.0-trunk_Orangepi4pro_trixie_vendor_6.6.98_minimal.img of=/dev/sdX bs=4M status=progress
sync
```

**First boot:**
1. Insert the SD card into the Orange Pi 4 Pro.
2. Connect Ethernet + HDMI.
3. Boot.
4. Complete `armbian-firstlogin` (via HDMI + keyboard).
5. Change the root password, create a user.
## 💾 Storage: eMMC and NVMe

**Choosing the storage:**
- **eMMC** — built-in module (32 GB). Speed ~205 MB/s. Easier to install.
- **NVMe SSD** — M.2 M-Key (128 GB). Speed ~570 MB/s. Requires SPI Flash + a U-Boot patch.

**General boot order:**
```
BootROM: SD -> eMMC -> SPI -> USB
U-Boot (boot_targets): SD -> eMMC -> NVMe -> USB
```

### Option A: Installing on eMMC (manual copy)

**Important:** `armbian-install` is buggy on the Orange Pi 4 Pro — it copies `boot_package` incorrectly. Use **manual** copying.

```bash
# 1. Unmount eMMC
sudo umount /mnt/emmc 2>/dev/null

# 2. Partition eMMC (same partition table as on SD)
sudo sfdisk -d /dev/mmcblk1 | sudo sfdisk /dev/mmcblk0

# 3. Format eMMC
sudo mkfs.ext4 -F /dev/mmcblk0p1

# 4. Mount eMMC
sudo mkdir -p /mnt/emmc
sudo mount /dev/mmcblk0p1 /mnt/emmc

# 5. Copy the system from SD to eMMC (5-10 minutes)
sudo rsync -aAXHv --exclude={/dev/*,/proc/*,/sys/*,/tmp/*,/run/*,/mnt/*,/media/*,/lost+found,/var/log.hdd/*} / /mnt/emmc/

# 6. Configure armbianEnv.txt (new eMMC UUID)
sudo blkid /dev/mmcblk0p1  # note the UUID
sudo sed -i 's/^rootdev=.*/rootdev=UUID=<NEW_UUID>/' /mnt/emmc/boot/armbianEnv.txt

# 7. Configure fstab (new eMMC UUID)
sudo sed -i 's/^UUID=.* \/ ext4/UUID=<NEW_UUID> \/ ext4/' /mnt/emmc/etc/fstab

# 8. Disable auto-resize on eMMC
sudo mkdir -p /mnt/emmc/etc/systemd/system
sudo ln -sf /dev/null /mnt/emmc/etc/systemd/system/armbian-resize-filesystem.service

# 9. Copy the bootloader (U-Boot + boot_package) — 20 MB
sudo dd if=/dev/mmcblk1 of=/dev/mmcblk0 bs=1M count=20 conv=notrunc

# 10. Sync and unmount
sudo sync
sudo umount /mnt/emmc

# 11. Power off, remove the SD card, boot from eMMC
sudo shutdown -h now
```

**After the first boot — expand the partition to 29 GB:**

```bash
sudo parted /dev/mmcblk0 resizepart 1 100%
sudo resize2fs /dev/mmcblk0p1
df -h /
```
### Option B: Installing on NVMe (SPI + U-Boot patch)

**Required:** SPI Flash + U-Boot with the `dc1sw1` patch (see below).

**1. Building U-Boot with the patch (VirtualBox):**

```bash
cd ~/orangepi-build
git clone https://github.com/TblP/orangepi-uboot-fix.git /tmp/uboot-fix
mkdir -p userpatches/u-boot/u-boot-sunxi/
cp /tmp/uboot-fix/patches/0001-orangepi4pro-pcie-3v3-dc1sw1.patch userpatches/u-boot/u-boot-sunxi/
sudo ./build.sh BOARD=orangepi4pro BRANCH=current RELEASE=jammy BUILD_OPT=u-boot KERNEL_CONFIGURE=no CLEAN_LEVEL=make
```

**2. Installing on the Orange Pi:**

```bash
sudo dpkg -r linux-u-boot-orangepi4pro-vendor
sudo dpkg -i linux-u-boot-current-orangepi4pro_1.1.0_arm64.deb
```

**3. Flashing SPI:**

```bash
sudo dd if=/dev/mtd0 of=/root/spi_backup_$(date +%Y%m%d_%H%M%S).bin bs=1M count=16
sudo flash_erase /dev/mtd0 0 50
sudo mtd_debug write /dev/mtd0 0 $(stat --format="%s" /usr/lib/linux-u-boot-current-orangepi4pro_1.1.0_arm64/boot0_spinor_a733.fex) /usr/lib/linux-u-boot-current-orangepi4pro_1.1.0_arm64/boot0_spinor_a733.fex
sudo mtd_debug write /dev/mtd0 262144 $(stat --format="%s" /usr/lib/linux-u-boot-current-orangepi4pro_1.1.0_arm64/boot_package.fex) /usr/lib/linux-u-boot-current-orangepi4pro_1.1.0_arm64/boot_package.fex
sudo sync
```

**4. Moving the system to NVMe:**

```bash
sudo parted /dev/nvme0n1 mklabel gpt
sudo parted /dev/nvme0n1 mkpart primary ext4 0% 100%
sudo mkfs.ext4 -F /dev/nvme0n1p1
sudo mkdir -p /mnt/nvme && sudo mount /dev/nvme0n1p1 /mnt/nvme
sudo rsync -aAXHv --exclude={/dev/*,/proc/*,/sys/*,/tmp/*,/run/*,/mnt/*,/media/*,/lost+found} / /mnt/nvme/
sudo sed -i 's|^rootdev=.*|rootdev=UUID=<UUID_NVME> rootdelay=5|' /mnt/nvme/boot/armbianEnv.txt
sudo sed -i 's|<UUID_EMMC>|<UUID_NVME>|g' /mnt/nvme/etc/fstab
sudo sync && sudo umount /mnt/nvme
```

**5. Power off, remove the eMMC, power on.**

**Result:**

```
$ findmnt -no SOURCE,UUID /
/dev/nvme0n1p1 e4384603-9cf2-4342-ba68-bb64e3e921e4

$ cat /sys/bus/pci/devices/0000:01:00.0/current_link_speed
8.0 GT/s PCIe

$ df -h /
/dev/nvme0n1p1 117G 11G 101G 10% /
```

**Speed:** 570 MB/s (NVMe Gen3 x1) vs 205 MB/s (eMMC).

**Why U-Boot did not see the NVMe:** U-Boot switched on the wrong 3.3 V rail (`dc1sw2` instead of `dc1sw1`). Patch: `pcie3v3_supply = "dc1sw1"` in `board-uboot.dts`.
## ⚠️ eMMC CQE bug

**Problem:** the `sunxi-mmc` driver on the A733 has a bug with CQE (Command Queue Engine). At high frequency (200 MHz) the CQE cannot halt (`Failed to halt`); the driver disables CQE and drops the frequency to 50 MHz.

**Solution:** use **HS400 @ 150 MHz**.

```bash
DTB=/boot/dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb
NODE=/soc@3000000/sdmmc@4022000

# Check
sudo dtc -I dtb -O dts $DTB 2>/dev/null | grep -A 30 "mmc@4022000" | \
    grep -E "compatible|max-frequency|sunxi-dly"

# Should be:
#   compatible = "allwinner,sunxi-mmc-v5p3x";
#   max-frequency = <0x8f0d180>;   (150 MHz)
#   sunxi-dly-208M = <0xff 0x01 0xff 0xff 0xff 0xff>;

# If 200 MHz — lower it to 150
sudo fdtput -t i $DTB $NODE max-frequency 150000000
sudo reboot
```

**Result:** 150 MHz (HS400), speed ~205 MB/s, no CQE errors.

## ⚙️ Interface setup

### UART7 (pins 8/10)

```bash
sudo armbian-config
# System -> Kernel -> Manage device tree overlays
# Enable: uart7
# Save, exit, reboot

ls -la /dev/ttyS7
# crw-rw---- 1 root dialout 241, 7 ... /dev/ttyS7
```

**Loopback test (short pins 8 and 10):**

```bash
picocom -b 115200 /dev/ttyS7
# Type characters -> they should echo back
# Exit: Ctrl+A, Ctrl+Q
```

### I2C2 (pins 19/23)

```bash
sudo armbian-config
# System -> Kernel -> Manage device tree overlays
# Enable: i2c2
# Save, exit, reboot

ls -la /dev/i2c-2
# crw------- 1 root root 89, 2 ... /dev/i2c-2

apt install -y i2c-tools
i2cdetect -y 2
```

**armbianEnv.txt (after activation):**

```
overlays=i2c2 uart7
```
## 📦 Dependencies

### System packages

```bash
sudo apt update
sudo apt install -y \
    git python3-pip python3-venv python3-dev \
    cmake build-essential \
    libopenblas-dev liblapack-dev libhdf5-dev \
    libjpeg-dev libtiff-dev libpng-dev \
    libavcodec-dev libavformat-dev libswscale-dev \
    libv4l-dev libxvidcore-dev libx264-dev \
    libgtk-3-dev libcanberra-gtk3-module \
    gfortran libfreetype-dev libharfbuzz-dev \
    libfribidi-dev libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    i2c-tools picocom unzip espeak-ng libespeak1
```

### Python dependencies (venv)

```bash
cd ~/ugv_rpi
python3 -m venv ugv-env
source ugv-env/bin/activate
pip install --upgrade pip
```

**requirements.txt (Python 3.13):**

```
Flask==3.0.3
Flask-SocketIO==5.3.6
Werkzeug==3.0.3
Jinja2==3.1.4
itsdangerous==2.2.0
blinker==1.8.2
python-socketio==5.11.2
python-engineio==4.9.1
aiortc==1.15.0
av==17.1.0
simplejpeg==1.9.0
aioice==0.10.2
pyee==14.0.0
pylibsrtp==1.0.0
cryptography==50.0.1
numpy==1.26.4
PyYAML==6.0.1
requests==2.32.3
psutil==5.9.8
pyserial==3.5
opencv-python-headless==4.10.0.84
Pillow==11.3.0
imutils==0.5.4
```

```bash
pip install -r requirements.txt
pip install imageio pygame-ce pyttsx3 netifaces
```

**Key points:**
- **Pillow 11.3.0** (not 10.3.0 — it doesn't work with Python 3.13).
- **pygame-ce** (not pygame — there is no wheel for Python 3.13 + ARM64).
- **av 17.1.0** (not 12.3.0).
- **aiortc 1.15.0** (not 1.8.0).

## 🖐️ MediaPipe (HAND GS)

Wheel for Orange Pi 4 Pro (aarch64, Python 3.13):
https://github.com/ZHNovell/ugv_rpi/releases/download/mediapipe-0.10.14-aarch64/mediapipe-0.10.14.dev0-cp313-cp313-linux_aarch64.whl

Installation:
```
pip3 install --break-system-packages --ignore-installed --force-reinstall https://github.com/ZHNovell/ugv_rpi/releases/download/mediapipe-0.10.14-aarch64/mediapipe-0.10.14.dev0-cp313-cp313-linux_aarch64.whl
```

Verification:
```
python3 -c "import mediapipe as mp; print(mp.__version__)"
```

Hand landmarker model:
```
wget -O /tmp/hand_landmarker.task https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task
```
### 🎯 Motion (Object Detection via YOLO26s NPU)

The **Motion** mode (Simple Detection Type) now uses **YOLO26s on the NPU** (`/tmp/npu11.sock`), not `absdiff` or `efficientdet`. This provides:

- **Accuracy:** finds **people** (class `person`), not abstract motion.
- **Speed:** ~53 ms per frame (NPU), boxes update quickly.
- **Stability:** it doesn't "go crazy" when the camera moves (unlike `absdiff`).
- **Dead Zone:** if the object is in the center (60 pixels), the servo does not twitch.
- **Lighting:** on detection it turns on the spotlight (IO5) in AUTO mode.

**Code:** `cv_detect_movition` in `cv_ctrl.py` (calling `self.npu_client.detect(img)`).

### 📷 WebRTC Resolution Switcher

The **WebRTC** button has been replaced with a **1080P / 720P** dropdown list. Switching is instant (via `set_camera_mode()` and `GstStream.switch_mode()`).

**Code:** `set_camera_mode()` in `cv_ctrl.py`, commands `cam_1080` (10108) and `cam_720` (10109) in `config.yaml`.

## 🧠 NPU servers

**Comparison of the two model placement options:**

| Model | Built-in NPU (A733) | Radxa AICore AX-M1 |
|---|---|---|
| Detection | YOLO26s — `/tmp/npu11.sock` | — |
| Pose | YOLO11s-pose — `/tmp/npu_pose_raw.sock` | YOLO26m-pose — `/tmp/npu_pose_axcl.sock` |
| Segmentation | YOLO11s-seg — `/tmp/npu_seg_raw.sock` | YOLO26m-seg — `/tmp/npu_seg_axcl.sock` |
| Depth | YOLO26n-depth — `/tmp/npu_depth_raw.sock` | YOLO26m-depth — `/tmp/npu_depth_axcl.sock` |

**Protocol (RAW BGR)** — the same for both options:

```
Client -> Server: [4 BE bytes: width] [4 BE bytes: height] [4 BE bytes: channels] [raw BGR]
Server -> Client: [4 BE bytes: JSON length] [JSON bytes]
```

### Option 1: built-in NPU (A733)

All 4 models run on the built-in A733 NPU. The Python clients are in `modules/`:
- `npu_client.py` — YOLO26s
- `npu_pose_client.py` — YOLO11_pose
- `npu_seg_client.py` — YOLO11_seg
- `npu_depth_client.py` — YOLO26n_depth

#### YOLO26s (object detection)

**Conversion pipeline:**

1. Download `yolo26s.pt` (Ultralytics).
2. ONNX export: `yolo export model=yolo26s.pt format=onnx simplify=True dynamic=False opset=16`.
3. Cut out the 6 outputs via `onnx_extract.py`:
   - `/model.23/Reshape_output_0` (`[1, 4, 6400]`)
   - `/model.23/Reshape_1_output_0` (`[1, 4, 1600]`)
   - `/model.23/Reshape_2_output_0` (`[1, 4, 400]`)
   - `/model.23/Reshape_3_output_0` (`[1, 80, 6400]`)
   - `/model.23/Reshape_4_output_0` (`[1, 80, 1600]`)
   - `/model.23/Reshape_5_output_0` (`[1, 80, 400]`)
4. Docker ACUITY (`ubuntu-npu:v2.0.10.2`):
   - `./pegasus_import.sh yolo26s_6`
   - `./pegasus_quantize.sh yolo26s_6 pcq 12`
   - `./pegasus_export_ovx_nbg.sh yolo26s_6 pcq a733`
5. Result: `yolo26s_6_pcq_a733.nb` (9 MB).

**Native build:**

```bash
cd /root/ugv_rpi/yolo26_raw/
./build_native.sh
```

**Key fixes in `CMakeLists.txt.patched`:**
1. `SYS_ARCH=linux_aarch64`.
2. OpenCV via `pkg-config` (not `find_package`).
3. Remove `_GLIBCXX_USE_CXX11_ABI=0`.
4. `MODEL_ZOO_HOME_DIR` — an absolute path.
5. Exclude the extra `.cpp` files (main, yolo11_6_*).
#### YOLO11_pose (pose)

**Pipeline:**
1. `yolo11s-pose.pt` -> ONNX (opset 15).
2. `onnx_extract.py` — cut out the 9 outputs (cv2/cv3/cv4 × 3 levels).
3. Docker ACUITY:
   - `./pegasus_import.sh yolo11s-pose_9`
   - `./pegasus_quantize.sh yolo11s-pose_9 uint8 12`
   - `./pegasus_export_ovx_nbg.sh yolo11s-pose_9 uint8 a733`
4. Result: `yolo11s-pose_9_uint8_a733.nb` (7 MB).

**Model:**
- 9 outputs: bbox (cv2) + class (cv3) + keypoints (cv4) × 3 levels.
- 1 class — person.
- 17 keypoints (COCO): nose, eyes, ears, shoulders, elbows, wrists, hips, knees, ankles.

**Native build:**

```bash
cd /root/ugv_rpi/yolo11_pose_raw/
./build_native.sh
```

#### YOLO11_seg (segmentation)

**Pipeline:**
1. Download `yolo11s-seg_10.onnx` (Allwinner, 40 MB) — already trimmed.
2. Docker ACUITY:
   - `./pegasus_import.sh yolo11s-seg_10`
   - `./pegasus_quantize.sh yolo11s-seg_10 uint8 12`
   - `./pegasus_export_ovx_nbg.sh yolo11s-seg_10 uint8 a733`
3. Result: `yolo11s-seg_10_uint8_a733.nb` (7.3 MB).

**Model:**
- 10 outputs: bbox + class + mask coeffs + protos × 3 levels.
- 80 classes (COCO).
- Instance segmentation (pixel masks).

**Native build:**

```bash
cd /root/ugv_rpi/yolo11_seg_raw/
./build_native.sh
```

#### YOLO26n_depth (depth map)

**Pipeline:**
1. Download `yolo26n-depth.pt` (Ultralytics, 12.4 MB).
2. ONNX export: `imgsz=768, opset=14, simplify=True`.
3. Docker ACUITY:
   - `./pegasus_import.sh yolo26n-depth`
   - `./pegasus_quantize.sh yolo26n-depth pcq 10`
   - `./pegasus_export_ovx_nbg.sh yolo26n-depth pcq a733`
4. Result: `yolo26n-depth_pcq_a733.nb` (10.4 MB).

**Model:**
- Input: 768×768 RGB (mean=0, scale=1/255).
- Output: 768×768 float32 (meters).

**Important:** nano + PCQ is the best balance (65 ms, ~11 FPS). s + PCQ is noisy due to `Exp()` at the output.

**Native build:**

```bash
cd /root/ugv_rpi/yolo26_depth_raw/
./build_native.sh
```
#### Process of migrating the server to RAW BGR

For each of the 4 servers:

**1. Copy the server folder to `_raw`:**

```bash
cp -r yolo26 yolo26_raw
cd yolo26_raw
```

**2. Edit `npu_server.cpp` — change JPEG input to raw BGR.**

Before (JPEG):
```cpp
uint32_t jpeg_len_be;
if (!read_n(cli, &jpeg_len_be, 4)) break;
uint32_t jpeg_len = ntohl(jpeg_len_be);
vector<unsigned char> jpeg_buf(jpeg_len);
if (!read_n(cli, jpeg_buf.data(), jpeg_len)) break;
cv::Mat frame = cv::imdecode(jpeg_buf, cv::IMREAD_COLOR);
```

After (RAW):
```cpp
uint32_t width_be, height_be, channels_be;
if (!read_n(cli, &width_be, 4)) break;
if (!read_n(cli, &height_be, 4)) break;
if (!read_n(cli, &channels_be, 4)) break;

uint32_t width = ntohl(width_be);
uint32_t height = ntohl(height_be);
uint32_t channels = ntohl(channels_be);

if (width == 0 || height == 0 || channels != 3) break;
if (width > 4096 || height > 4096) break;

uint32_t raw_size = width * height * channels;
if (raw_size > 20 * 1024 * 1024) break;

vector<unsigned char> raw_buf(raw_size);
if (!read_n(cli, raw_buf.data(), raw_size)) break;

cv::Mat frame = cv::Mat(height, width, CV_8UC3, raw_buf.data()).clone();
```

**Important:** `.clone()` is mandatory, otherwise the `raw_buf` data is destroyed after leaving the block.

**3. Change SOCKET_PATH to `_raw`:**

```cpp
#define SOCKET_PATH "/tmp/npu11_raw.sock"
```

**4. Edit `build_native.sh`:**

```bash
SRC_DIR=${REPO_DIR}
mkdir -p "${SRC_DIR}"
rm -f "${SRC_DIR}/yolov26_6_post.cpp" "${SRC_DIR}/yolov26_6_pre.cpp"
```

**5. Build:**

```bash
./build_native.sh
cp -f build/npu_server ./npu_server
```

**6. Edit `npu_client.py` — send raw BGR.**

Before (JPEG):
```python
ok, jpeg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
jpeg_bytes = jpeg.tobytes()
self.sock.sendall(struct.pack('>I', len(jpeg_bytes)))
self.sock.sendall(jpeg_bytes)
```

After (RAW):
```python
h, w = frame.shape[:2]
self.sock.sendall(struct.pack('>I', w))
self.sock.sendall(struct.pack('>I', h))
self.sock.sendall(struct.pack('>I', 3))
self.sock.sendall(frame.tobytes())
```

**7. Change SOCKET_PATH in the client:**

```python
SOCKET_PATH = "/tmp/npu11_raw.sock"
```

**8. Add a timeout to `_connect` (10 s):**

```python
attempts = 0
while attempts < 20:
    try:
        ...
        return
    except (FileNotFoundError, ConnectionRefusedError) as e:
        attempts += 1
        time.sleep(0.5)
raise ConnectionError(f"[NPUClient] Server {self.socket_path} not available after 10s")
```

**9. Update the systemd service:**

```bash
sudo sed -i 's|/root/ugv_rpi/yolo26/|/root/ugv_rpi/yolo26_raw/|g' /etc/systemd/system/npu-server.service
sudo systemctl daemon-reload
sudo systemctl restart npu-server.service
```

**10. Verify:**

```bash
ls -la /tmp/npu11_raw.sock
ps aux | grep npu_server | grep -v grep
```
### Option 2: Radxa AICore AX-M1

Detection stays on the A733, while pose/seg/depth run on the **AX-M1 (AX650N)** via C++ AXCL servers in `axcl_servers/`.

| Service | Model (`models_axcl/`) | Socket |
|---|---|---|
| `npu-pose-axcl-server.service` | `yolo26m-pose_npu3.axmodel` | `/tmp/npu_pose_axcl.sock` |
| `npu-seg-axcl-server.service` | `yolo26m-seg.axmodel` | `/tmp/npu_seg_axcl.sock` |
| `npu-depth-axcl-server.service` | `yolo26m-depth.axmodel` | `/tmp/npu_depth_axcl.sock` |

**Build:**

```bash
cd /root/ugv_rpi/axcl_servers
./build.sh
```

The models (`.axmodel`, in `models_axcl/`) are downloaded from Hugging Face (the AXERA-TECH organization). More details — in `axcl_servers/README.md`.

The AX-M1 connects via PCIe; the AXCL kernel modules (`ax_pcie_host_dev`, `axcl_host`, etc.) are loaded via `ExecStartPre` in the services.

## 🔧 Code adaptation

### `base_ctrl.py`

**Change:** UART port.

```python
# Before (Raspberry Pi):
base = BaseController('/dev/ttyAMA0', 115200)

# After (Orange Pi 4 Pro):
base = BaseController('/dev/ttyS7', 115200)
```

```bash
sed -i "s|/dev/ttyAMA0|/dev/ttyS7|g" base_ctrl.py
```

### `app.py`

**Change 1:** UART port + removal of the Raspberry Pi check block.

```python
# Before:
def is_raspberry_pi5():
    ...

if is_raspberry_pi5():
    base = BaseController('/dev/ttyAMA0', 115200)
else:
    base = BaseController('/dev/serial0', 115200)

# After:
base = BaseController('/dev/ttyS7', 115200)
```

**Change 2:** `eth0_ip` -> `end0_ip`.

```bash
sed -i 's/si\.eth0_ip/si.end0_ip/g' app.py
```

**Change 3:** Added a `SIGTERM` handler.

```python
import signal
import sys

def cleanup_handler(signum, frame):
    print(f"[app] Received signal {signum}, cleaning up...", flush=True)
    try:
        if hasattr(cvf, "camera") and cvf.camera:
            cvf.camera.release()
    except Exception as e:
        print(f"[app] Camera release error: {e}", flush=True)
    try:
        if hasattr(cvf, "writer") and cvf.writer:
            cvf.writer.release()
    except Exception as e:
        print(f"[app] Writer release error: {e}", flush=True)
    try:
        cvf.cv_event.set()
    except Exception:
        pass
    print("[app] Cleanup done, exiting", flush=True)
    sys.exit(0)

signal.signal(signal.SIGTERM, cleanup_handler)
signal.signal(signal.SIGINT, cleanup_handler)
```
### `cv_ctrl.py`

**Change 1:** Wrap the imports in `try/except`.

```python
try:
    import mediapipe as mp
    MEDIAPIPE_AVAILABLE = True
except ImportError:
    MEDIAPIPE_AVAILABLE = False
    print("mediapipe not available — face/hand/pose detection disabled")
    mp = None

try:
    from picamera2 import Picamera2
    CSI_CAMERA_AVAILABLE = True
except ImportError:
    CSI_CAMERA_AVAILABLE = False
    print("picamera2 not available — CSI camera disabled")

try:
    import depthai as dai
    OAK_CAMERA_AVAILABLE = True
except ImportError:
    OAK_CAMERA_AVAILABLE = False
    print("depthai not available — OAK camera disabled")
```

**Change 2:** The `mediapipe` blocks are wrapped in `if MEDIAPIPE_AVAILABLE:`.

**Change 3:** The CSI/OAK blocks — checks for `CSI_CAMERA_AVAILABLE`, `OAK_CAMERA_AVAILABLE`.

**Change 4:** NPU initialization (instead of `cv2.dnn`).

```python
from npu_client import NPUClient
from npu_pose_client import NPUPoseClient
from npu_seg_client import NPUSegClient
from npu_depth_client import NPUDepthClient

def _try_client(cls, name):
    try:
        client = cls()
        print(f"[cv_ctrl] {name} connected", flush=True)
        return client
    except Exception as e:
        print(f"[cv_ctrl] {name} not available: {e}", flush=True)
        return None

self.npu_client = _try_client(NPUClient, 'NPUClient')
self.npu_pose_client = _try_client(NPUPoseClient, 'NPUPoseClient')
self.npu_seg_client = _try_client(NPUSegClient, 'NPUSegClient')
self.npu_depth_client = _try_client(NPUDepthClient, 'NPUDepthClient')
```

### `os_info.py`

**Change 1:** `vcgencmd` -> `sysfs`.

```python
with open('/sys/class/thermal/thermal_zone0/temp', 'r') as f:
    temperature_str = f.read().strip()
temperature = float(temperature_str) / 1000.0
return round(temperature, 1)
```

**Change 2:** `iwconfig` -> `iw`.

```python
result = subprocess.check_output(['/usr/sbin/iw', 'dev', 'wlan0', 'info'],
                                 encoding='utf-8', stderr=subprocess.DEVNULL)

output = subprocess.check_output(["/usr/sbin/iw", "dev", interface, "link"],
                                 encoding="utf-8", stderr=subprocess.DEVNULL)

signal_strength = re.search(r"signal:\s*(-\d+)", output)
if signal_strength:
    return int(signal_strength.group(1))
return 0
```

**Change 3:** `eth0` -> `end0`.

```bash
sed -i 's/'"'"'eth0'"'"'/'"'"'end0'"'"'/g' os_info.py
sed -i 's/self\.eth0_ip/self.end0_ip/g' os_info.py
```
## 🌐 Web interface

### `templates/index.html`

**Buttons `OBJECTS`, `COLOR`, `HAND GS`:**

```html
<div><button onclick="sendCmdObjs();" class="ctl_btn">OBJECTS</button></div>
<div><button onclick="sendCmdClor();" class="ctl_btn">COLOR</button></div>
<div><button onclick="sendCmdHand();" class="ctl_btn">HAND GS</button></div>
```

**NPU Depth/Seg block:**

```html
<div>
    <h2 class="sc_title feed_ct_tt">NPU Depth/Seg</h2>
    <div id="npu_depth_seg_btn">
        <div><button onclick="sendCmdDepth();" class="ctl_btn">DEPTH</button></div>
        <div><button onclick="sendCmdSeg();" class="ctl_btn">SEG</button></div>
        <div><button onclick="sendCmdAruco();" class="ctl_btn">ARUCO</button></div>
    </div>
</div>
```

**`control.js` versioning:**

```html
<script src="./control.js?v=8"></script>
```

### `templates/control.js`

**Functions:**

```javascript
function sendCmdObjs() {
    sendCommand('base -c {"T":' + cv_objs + '}');
}
function sendCmdClor() {
    sendCommand('base -c {"T":' + cv_clor + '}');
}
function sendCmdHand() {
    sendCommand('base -c {"T":' + mp_hand + '}');
}
function sendCmdDepth() {
    sendCommand('base -c {"T":' + cv_depth + '}');
}
function sendCmdSeg() {
    sendCommand('base -c {"T":' + cv_seg + '}');
}
function sendCmdAruco() {
    sendCommand('base -c {"T":' + cv_aruco + '}');
}

function sendCommand(command) {
    fetch('/send_command', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'command=' + encodeURIComponent(command)
    })
    .then(response => response.json())
    .then(data => console.log('Command sent:', command, data))
    .catch(error => console.error('Error:', error));
}
```

**Button handlers (in `socket.on('update')`):**

```javascript
FButtons[0].onclick = function() { sendCommand('base -c {"T":' + cv_objs + '}'); };
FButtons[1].onclick = function() { sendCommand('base -c {"T":' + cv_clor + '}'); };
FButtons[2].onclick = function() { sendCommand('base -c {"T":' + mp_hand + '}'); };

var npuDSBtn = document.getElementById("npu_depth_seg_btn");
if (npuDSBtn) {
    var DSButtons = npuDSBtn.getElementsByTagName("button");
    removeButtonsClass(DSButtons);
    DSButtons[0].onclick = function() { sendCommand('base -c {"T":' + cv_depth + '}'); };
    DSButtons[1].onclick = function() { sendCommand('base -c {"T":' + cv_seg + '}'); };
    if (DSButtons[2]) DSButtons[2].onclick = function() { sendCommand('base -c {"T":' + cv_aruco + '}'); };
}
```

### `templates/style.css`

```css
.video {
    width: 960px;
    height: 540px;
    overflow: hidden;
}
.video img {
    width: 100%;
    height: 100%;
    border-radius: 4px;
    object-fit: contain;
}
main { width: 1600px; margin: auto; }
.box1 .section_video { width: 1000px; margin-right: 10px; }
```
## 🛑 Emergency Stop (Emergency stop)

A red **STOP** button has been added to the web interface.
It sends the command `{"T":0}` directly to the ESP32 (via `/api/stop`), bypassing the joystick and heartbeat.

**Usage:**
1. Press **STOP** in the web interface.
2. The robot will stop instantly (even if you keep holding a movement key).

**Note:** For the button to work, the ESP32 must be flashed with support for the `{"T":0}` command (see the `ugv_base_general` repository).

## 🎭 MP FACE (Nod / Turn)

New **MP FACE** mode (MediaPipe Face Landmarker):
*   **Nod (Yaw):** The robot blinks IO5 twice (0.3s/0.3s/0.3s) and **nods back** (PT down-up).
*   **Turn (Pitch):** The robot blinks IO5 twice and **mirrors the turn** (PT).

**Debounce protection:**
*   The nod triggers only when `dyaw > 3°` and `dpitch > 2°` (so as not to confuse it with a turn).
*   After a gesture, a **3-second** pause is set so the camera can "settle".

**Implementation:** `mp_detect_face` in `cv_ctrl.py`, model `face_landmarker.task`.

## 🎯 MP POSE (Elbow angle + Adaptive PT)

The **MP POSE** mode (YOLO11_pose on the NPU) now uses the **elbow angle** to control the chassis:
*   **FOLLOW (come here):** The arm is **straight** (angle < 30°). The robot drives forward, steering to follow the person.
*   **HOLD (stop):** The arm is **bent** (angle > 60°). The robot stops.

**Adaptive PT lift:**
*   On approach (bbox > 60% of the frame) the PT lifts in steps (15° → 17.5° → 20° → 22.5° → 25°).
*   On moving away (bbox < 20%) the PT lowers to 15°.
*   Debounce protection: a 2-second pause after each PT movement.

**Implementation:** `_npu_pose_worker` in `cv_ctrl.py`.

## 🚀 Autostart

### `npu-server.service`

```ini
[Unit]
Description=NPU Server for YOLO26s (A733)
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/yolo26_raw
Environment=LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733
ExecStartPre=/root/ugv_rpi/scripts/prepare_shm.sh
ExecStart=/root/ugv_rpi/yolo26_raw/npu_server /dev/shm/yolo26s_6_pcq_a733.nb
Restart=always
RestartSec=3
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

### `npu-pose-axcl-server.service`

```ini
[Unit]
Description=NPU Pose Server on AX-M1 (AXCL) - YOLO26m-pose
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/axcl-samples/build/examples/axcl
ExecStartPre=/sbin/modprobe ax_pcie_host_dev
ExecStartPre=/sbin/modprobe axcl_host
ExecStartPre=/sbin/modprobe ax_pcie_mmb
ExecStartPre=/sbin/modprobe ax_pcie_msg
ExecStartPre=/sbin/modprobe ax_pcie_net_host
ExecStartPre=/sbin/modprobe ax_pcie_p2p_rc
ExecStart=/root/ugv_rpi/axcl-samples/build/examples/axcl/axcl_npu_pose_server /root/ugv_rpi/models_axcl/yolo26m-pose_npu3.axmodel
Restart=always
RestartSec=3
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

### `npu-seg-axcl-server.service`

```ini
[Unit]
Description=NPU Seg Server on AX-M1 (AXCL) - YOLO26m-seg
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/axcl-samples/build/examples/axcl
ExecStartPre=/sbin/modprobe ax_pcie_host_dev
ExecStartPre=/sbin/modprobe axcl_host
ExecStartPre=/sbin/modprobe ax_pcie_mmb
ExecStartPre=/sbin/modprobe ax_pcie_msg
ExecStartPre=/sbin/modprobe ax_pcie_net_host
ExecStartPre=/sbin/modprobe ax_pcie_p2p_rc
ExecStart=/root/ugv_rpi/axcl-samples/build/examples/axcl/axcl_npu_seg_server /root/ugv_rpi/models_axcl/yolo26m-seg.axmodel
Restart=always
RestartSec=3
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

### `npu-depth-axcl-server.service`

```ini
[Unit]
Description=NPU Depth Server on AX-M1 (AXCL) - YOLO26m-depth
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/axcl-samples/build/examples/axcl
ExecStartPre=/sbin/modprobe ax_pcie_host_dev
ExecStartPre=/sbin/modprobe axcl_host
ExecStartPre=/sbin/modprobe ax_pcie_mmb
ExecStartPre=/sbin/modprobe ax_pcie_msg
ExecStartPre=/sbin/modprobe ax_pcie_net_host
ExecStartPre=/sbin/modprobe ax_pcie_p2p_rc
ExecStart=/root/ugv_rpi/axcl-samples/build/examples/axcl/axcl_npu_depth_server /root/ugv_rpi/models_axcl/yolo26m-depth.axmodel
Restart=always
RestartSec=3
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

### `ugv.service`

```ini
[Unit]
Description=UGV Robot App
After=network.target npu-server.service npu-pose-axcl-server.service npu-seg-axcl-server.service npu-depth-axcl-server.service
Requires=npu-server.service npu-pose-axcl-server.service npu-seg-axcl-server.service npu-depth-axcl-server.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi
ExecStartPre=/root/ugv_rpi/scripts/prepare_shm.sh
ExecStartPre=/bin/sh -c "v4l2-ctl -d /dev/video0 -c focus_automatic_continuous=0 && v4l2-ctl -d /dev/video0 -c focus_absolute=671"
ExecStart=/root/ugv_rpi/ugv-env/bin/python /root/ugv_rpi/app.py
Restart=on-failure
RestartSec=5
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

**Activation:**

```bash
sudo systemctl daemon-reload
sudo systemctl enable npu-server.service npu-pose-axcl-server.service npu-seg-axcl-server.service npu-depth-axcl-server.service ugv.service
sudo systemctl start npu-server.service npu-pose-axcl-server.service npu-seg-axcl-server.service npu-depth-axcl-server.service ugv.service
```

**Logs:**

```bash
journalctl -u ugv.service -f
```
## 📡 JSON commands (ESP32)

### Control commands (via UART7)

| Command | JSON | Description |
|---|---|---|
| Motion | `{"T":1,"L":0.5,"R":0.5}` | L/R: -1.0 to 1.0 |
| PWM direct | `{"T":11,"L":164,"R":164}` | L/R: -255 to 255 |
| Speed (rate) | `{"T":138,"L":0.3,"R":0.3}` | Speed scaling |
| OLED | `{"T":3,"lineNum":0,"Text":"..."}` | Text output |
| Module | `{"T":4,"cmd":0}` | 0=Null, 1=RoArm, 2=PT |
| PT servos | `{"T":133,"X":0,"Y":0,"SPD":0,"ACC":0}` | Pan-tilt |
| Light | `{"T":132,"IO4":255,"IO5":255}` | IO4/IO5: 0-255 |
| Telemetry | `{"T":130}` | Data request |
| Telemetry stream | `{"T":131,"cmd":1}` | On/off |
| Telemetry interval | `{"T":142,"cmd":0}` | ms |

### CV commands

| Command | T | Description |
|---|---|---|
| None | 10301 | Disable detection |
| Motion | 10302 | Motion detection |
| Faces | 10303 | Face detection |
| Objects | 10304 | NPU YOLO26s |
| Color | 10305 | Color detection |
| Hand GS | 10306 | Hand gestures |
| Auto | 10307 | Auto mode |
| MP Face | 10308 | MediaPipe Face Landmarker (nod/turn) |
| MP Pose | 10309 | NPU YOLO11_pose |
| Depth | 10310 | NPU YOLO26n_depth |
| Seg | 10311 | NPU YOLO11_seg |
| ArUco | 10312 | ArUco markers |
## 📷 USB camera (UVC)

**Connection:**
- The camera plugs into a USB port on the Orange Pi 4 Pro.
- It is detected as `/dev/video0` and `/dev/video1`.
- The `uvcvideo` module loads automatically.

**Verification:**

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
fswebcam -d /dev/video0 --no-banner -r 1920x1080 -S 5 ./test.jpg
```

**Universal detection:**

```python
def usb_camera_detection(self):
    import glob
    video_devices = glob.glob('/dev/video*')
    if not video_devices:
        print("USB Camera not connected (no /dev/video*)")
        return False
    try:
        cap = cv2.VideoCapture(0)
        if cap.isOpened():
            ret, frame = cap.read()
            cap.release()
            if ret and frame is not None:
                print(f"USB Camera connected: {video_devices}, frame: {frame.shape}")
                return True
        cap.release()
    except Exception as e:
        print(f"USB Camera detection error: {e}")
    print("USB Camera not connected (OpenCV failed)")
    return False
```

### Universal auto-detect FPS

**Problem:** different cameras support different FPS for different resolutions.

**Solution:** when switching modes, the camera itself determines the maximum FPS for the selected resolution via `v4l2-ctl --list-formats-ext`.

**`GstStream.detect_max_fps(width, height, device)`:**
- Parses the output of `v4l2-ctl --list-formats-ext`.
- Finds the MJPG section.
- Looks for the required resolution.
- Returns the maximum FPS.
- **Limitation:** 1080p — depends on the camera (up to 60 fps).

**`GstStream.switch_mode(width, height, fps=None)`:**
- If `fps` is not specified — calls `detect_max_fps`.
- Recreates the pipeline with the found FPS.

**`cv_ctrl.toggle_camera_mode()`:**
- Determines the target resolution.
- Calls `switch_mode(new_w, new_h)` without fps.
- Builds `self.camera_mode` dynamically: `f"{height}p{real_fps}"`.
## 🎥 Video recording

**Requirement:** the live stream must not be interrupted during recording.

**Architecture (GStreamer `tee`):**

```
v4l2src -> image/jpeg (MJPG, 1920x1080@60) -> omxmjpegvideodec -> videorate (30 FPS) -> videoconvert -> BGR -> tee
├─ queue (leaky) -> appsink (live stream)
└─ queue -> videoconvert -> videoscale -> 1280x720 I420
   -> avenc_mjpeg -> matroskamux -> filesink (recording)
```

**Key decisions:**

1. **`tee` after `jpegdec`** — the camera is read once. The live stream is not interrupted.
2. **`.mkv` container (matroskamux), not `.avi`** — the camera produces VFR (FPS drops in the dark). AVI does not store VFR correctly. Matroska preserves the real PTS.
3. **Encoder `avenc_mjpeg`** (libav), not `jpegenc`.
4. **Proper teardown of the recording branch:**
   - Unlink `rec_tee_pad` from `rec_queue`.
   - Send EOS through the sink pad `rec_queue`.
   - `set_state(NULL)` -> `pipeline.remove()`.
5. **`v4l2src do-timestamp=true`** — PTS from the system clock.
6. **Hardware MJPEG decode** — `omxmjpegvideodec` (VPU CedarC) + `videorate` 30 FPS instead of software `jpegdec`. CPU: **267% → 133%** (~50%).

**Parameters:**
- Recording resolution: **1280x720**.
- MJPEG bitrate: **8 Mbit/s**, `qmin=3`, `qmax=10`.
- File size: ~10 MB per 10 seconds.

**Limitation:** `.mkv` does not play in the browser. To view it — VLC / MPC-HC / ffmpeg.
## 🎯 ArUco markers

**Result:** ArUco detection via OpenCV (CPU). Accuracy — **±1 cm at 1.35 m** (~0.7%). The **ARUCO** button is in the web interface.

**Parameters:**

| Parameter | Value |
|---|---|
| Dictionary | `DICT_4X4_50` |
| Marker size | 14.6 cm (black square) |
| Allowed IDs | 0, 1, 2, 3 |
| Resolution | 1280×720 |
| Calibration | `camera_calib.npz` (RMS=0.3475) |
| Accuracy | ±1 cm at 1.35 m (~0.7%) |

**Files:**
- `aruco_client.py` — client (detection + pose + drawing).
- `aruco_detect.py` — demo script.
- `camera_calibrate_v2.py` — calibration without a GUI.
- `camera_calib.npz` — saved calibration.

**Camera calibration:**

```bash
python3 camera_calibrate_v2.py
```

Capture 25 frames with a chessboard (5×8 internal corners, 30 mm). Result: `camera_calib.npz` (RMS=0.3475).

**Auto-scaling to resolution:**

```python
if (self.calib_w != frame_w) or (self.calib_h != frame_h):
    scale_x = frame_w / self.calib_w
    scale_y = frame_h / self.calib_h
    self.camera_matrix[0, 0] *= scale_x  # fx
    self.camera_matrix[1, 1] *= scale_y  # fy
    self.camera_matrix[0, 2] *= scale_x  # cx
    self.camera_matrix[1, 2] *= scale_y  # cy
```
## 📊 Stress tests

**Conditions:** 4 NPU models, a real 720p60 video stream, 70 s per model + 70 s in parallel. CPU/RAM/TEMP monitoring every 2 s.

### RAW BGR (current)

**One at a time (70 s per model):**

| Model | avg | FPS | Frames |
|---|---|---|---|
| YOLO26s | 67.3 ms | **14.9** | 703 |
| YOLO11_pose | 53.3 ms | **18.8** | 720 |
| YOLO11_seg | 90.0 ms | **11.1** | 632 |
| YOLO26n_depth | 94.4 ms | **10.6** | 580 |

**In parallel (all 4):**

| Metric | Value |
|---|---|
| Total camera FPS | **12.5** |
| Each model | **5.5 FPS** |
| CPU avg | **14.3%** |
| CPU max | **26.4%** |
| RAM | 5.3% |
| TEMP max | **36.3 °C** |

**Note:** the FPS in the table is the model's inference FPS (how many frames the NPU processes). The on-screen camera FPS (in the OSD) may differ due to the drop during parallel work.

### JPEG vs RAW comparison

| Model | JPEG (avg / FPS) | RAW (avg / FPS) | Gain |
|---|---|---|---|
| YOLO26s | 90.9 ms / 11.0 | **67.3 ms / 14.9** | +35% |
| YOLO11_pose | 83.7 ms / 12.0 | **53.3 ms / 18.8** | +57% |
| YOLO11_seg | 113.8 ms / 8.8 | **90.0 ms / 11.1** | +26% |
| YOLO26n_depth | 117.3 ms / 8.5 | **94.4 ms / 10.6** | +25% |

**CPU max:** 100% -> 26.4% (-74%).

**GIL:** in parallel mode RAW gives 12.5 camera FPS (JPEG — 15.0). The cause — the GIL in Python (copying large buffers). Solution (for the future): `multiprocessing` or a C++ dispatcher.

## 🚧 What's left / Roadmap

### Done

### Done

- [x] **Rebuilt Armbian with PR #10835** — GPU, VPU, H.264, CSI.
- [x] **eMMC CQE bug** — HS400 @ 150 MHz (205 MB/s).
- [x] **eMMC expansion** to 29 GB.
- [x] **`TimeoutStopSec=5`**, **`KillMode=control-group`**, **`SendSIGKILL=yes`**.
- [x] **`SIGTERM` handler** in `app.py`.
- [x] **Video recording with a parallel stream** — GStreamer `tee` + `avenc_mjpeg` + `.mkv`.
- [x] **NVMe SSD + SPI Flash** — booting from NVMe (570 MB/s) without eMMC.
- [x] **Universal auto-detect FPS** for the camera.
- [x] **YOLO26s** — detection (PCQ, RAW, ~53 ms).
- [x] **YOLO11_pose** — 17 keypoints (uint8, RAW, ~42 ms).
- [x] **YOLO11_seg** — segmentation (uint8, RAW, ~70 ms).
- [x] **YOLO26n_depth** — depth map (PCQ, RAW, ~77 ms).
- [x] **4 NPU services** + 4 Python clients (RAW BGR).
- [x] **DEPTH/SEG/ARUCO buttons** in the web interface.
- [x] **ArUco markers** — detection (OpenCV CPU, ±1 cm at 1.35 m).
- [x] **Camera calibration** — RMS=0.3475.
- [x] **Camera focus lock** (671).
- [x] **Stress test** — 4 models, 720p60, CPU 14.3% avg, TEMP 36.3 °C.
- [x] **MediaPipe HAND GS** — hand gestures (MediaPipe 0.10.14).
- [x] **MP POSE (elbow angle)** — FOLLOW/HOLD via YOLO11_pose.
- [x] **MP FACE (nod/turn)** — MediaPipe Face Landmarker.
- [x] **Emergency Stop** — STOP button on the Orange Pi and ESP32.
- [x] **Battery indication** — 4 colors (green/yellow/orange/red).



### Upcoming

- [x] **GIL** — bottleneck of parallel RAW
- [x] **Multi-client for the NPU server** (currently 1 client at a time).
- [x] **CSI camera** — second camera (overview, on the PT).
- [x] **GPU** — OpenCV acceleration (OpenCL).
- [x] **Camera switcher** — USB / CSI in the web interface.
- [x] **ArUco markers** — parking logic.
- [x] **D500 LiDAR** — SLAM + navigation.
- [x] **ESP32 (IR, sonars)** — code for flashing.

### Long-term (from ROADMAP)

- [x] **MediaPipe Pose -> YOLO11_pose (NPU)** — done.
- [x] **MediaPipe Face** — replace with NPU (by analogy with pose).
- [x] **D500 LiDAR** — driver directly in `cv_ctrl.py` (without ROS2).
- [x] **Sensor Fusion** — LiDAR + depth in a Python worker.
- [x] **SLAM** — Cartographer (ROS2, malinka stack 1:1) + rviz on PC via MobaXterm.
- [ ] **Navigation** — ROS2 Nav2 (AMCL/emcl2 + TEB) + map saving.
- [x] **LiDAR protection** — stop < 0.4 m.
- [ ] **Autopilot** — visual odometry.
- [ ] **Depth camera** — Orbbec Gemini 335 (once the project gets going).
- [ ] **Voice control** — via `pyttsx3` + recognition.

**Detailed plan:** [ROADMAP.md](https://github.com/ZHNovell/ugv_rpi/blob/main/ROADMAP.md)




## 📅 Changelog

> **Full project history:** [PROJECT.md](https://github.com/ZHNovell/ugv_rpi/blob/main/PROJECT.md)

- **2026-09-28:** First launch of Orange Pi 4 Pro, UART7, I2C2, NPU.
- **2026-10-01:** Manual copy to eMMC, CQE fix (150 MHz).
- **2026-10-02:** GStreamer `tee` — recording without interruption.
- **2026-10-03:** 1080p30/720p60 switching.
- **2026-10-05:** YOLO11s INT8 on the NPU.
- **2026-10-06:** YOLO26s (PCQ), YOLO11_pose.
- **2026-10-07:** CQE bug defeated (150 MHz, 205 MB/s).
- **2026-10-08:** Camera auto-detect FPS, YOLO26_depth, YOLO11_seg.
- **2026-10-09:** 4 NPU services, NVMe + SPI Flash.
- **2026-10-10:** Stress test, camera calibration, RAW BGR for the 4 servers.
- **2026-10-24:** MediaPipe 0.10.14 (HAND GS), MP POSE (elbow angle), MP FACE (nod/turn), Emergency Stop.
- **2026-10-25:** Adaptive PT lift, battery indication (4 colors), STOP button on the Orange Pi and ESP32.
- **2026-11-16:** Moving seg/depth to the Radxa AICore AX-M1 (AXCL servers).
- **2026-11-18:** Moving pose to the AX-M1, 1080p@60 camera fix, project restructuring (core/modules/scripts/tools/).
- **2026-11-19:** Hardware MJPEG decode (`omxmjpegvideodec`) + 30 FPS cap (CPU 267% → 133%).


## 🔗 Links

**GitHub:**
- Fork: [ZHNovell/ugv_rpi](https://github.com/ZHNovell/ugv_rpi)
- Original: [waveshareteam/ugv_rpi](https://github.com/waveshareteam/ugv_rpi)

**Hardware:**
- [Orange Pi 4 Pro](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-4-Pro.html)
- [Waveshare WAVE ROVER](https://www.waveshare.com/wiki/WAVE_ROVER)
- [General Driver for Robots](https://www.waveshare.com/wiki/General_Driver_for_Robots)
- [2-Axis Pan-Tilt](https://www.waveshare.com/wiki/2-Axis_Pan-Tilt_Camera_Module)

**Armbian:**
- [armbian/build](https://github.com/armbian/build) — Orange Pi 4 Pro support in the main branch

**NPU / Ultralytics:**
- [Allwinner Model Zoo v1.1.0](https://www.aw-ol.com/)
- [Radxa YOLO26 Docs](https://docs.radxa.com/en/cubie/a7a/app-dev/npu-dev/model-zoo/yolo26)
- [Ultralytics YOLO26](https://docs.ultralytics.com/models/yolo26/)
- [COCO Keypoints](https://cocodataset.org/#keypoints-2020)

**JSON commands:**
- [Waveshare JSON Instruction Set](https://www.waveshare.com/wiki/08_Slave_Device_JSON_Instruction_Set)

## 👥 Authors

- **ZHNovell** — adaptation for Orange Pi 4 Pro, NPU, web interface.
- **Waveshare** — the original `ugv_rpi`.
- **deece** — A733 support in Armbian.
- **ijiki16** — GPU modules on the A733.
- **Allwinner** — Model Zoo v1.1.0, ACUITY Toolkit.
- **AXERA-TECH** — axcl-samples (AXCL examples).

---

**Last update:** 2026-11-19

**PROJECT:** [PROJECT.md](https://github.com/ZHNovell/ugv_rpi/blob/main/PROJECT.md)
**ROADMAP:** [ROADMAP.md](https://github.com/ZHNovell/ugv_rpi/blob/main/ROADMAP.md)

Copyright (C) 2024 [Waveshare](https://www.waveshare.com/)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/gpl-3.0.txt>.
