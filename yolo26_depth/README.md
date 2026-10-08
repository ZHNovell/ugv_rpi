# 概述

本文简要介绍yolo26-depth模型的部署流程。

## 模型来源

Ultralytics YOLO26 可通过官方 Ultralytics YOLO 包使用，支持目标检测、实例分割、语义分割、深度估计、图像分类、姿态估计、旋转目标检测和跟踪，并提供快速、准确、易用的 Python 与 CLI 工作流。

开源项目地址：

```
https://github.com/ultralytics/ultralytics
```



# yolo26-depth

下载github上的Ultralytics源码， 调用源码的DepthPredictor进行推理，推理效果如下：

![result_pt](figures/result_pt.jpg)

## 导出onnx模型

ultralytics提供了模型导出器，位置于ultralytics/engine/exporter.py

执行export_onnx.py，导出yolo26s-depth.onnx

```python
import argparse
import sys
from pathlib import Path

# 使用项目源码
_LOCAL_ULTralYTICS = Path(__file__).resolve().parent / "ultralytics"
if str(_LOCAL_ULTralYTICS) not in sys.path:
    sys.path.insert(0, str(_LOCAL_ULTralYTICS))

from ultralytics import YOLO
def main():
    parser = argparse.ArgumentParser(description="导出 YOLO26-depth 为 ONNX")
    parser.add_argument("--model", type=str, default="yolo26s-depth.pt",
                        help="模型权重路径 (.pt)")
    parser.add_argument("--imgsz", type=int, default=768,
                        help="输入尺寸 (与训练一致, 默认 768)")
    parser.add_argument("--opset", type=int, default=14,
                        help="ONNX opset 版本 ")
    parser.add_argument("--simplify", action="store_true",
                        help="使用 onnxslim 简化模型")

    args = parser.parse_args()

    model = YOLO(args.model)
    print(f"[INFO] task: {model.task}")

    export_kwargs = dict(
        format="onnx",
        imgsz=args.imgsz,
        opset=args.opset,
        simplify=args.simplify,
        dynamic=args.dynamic,
    )
    path = model.export(**export_kwargs)
    print(f"[OK] ONNX 导出完成: {path}")


if __name__ == "__main__":
    main()
```



## onnx模型简化

使用onnxsim工具检测并裁剪yolo26s-depth.onnx模型的多余计算节点。

```bash
python3 -m onnxsim yolo26s-depth.onnx yolo26s-depth.onnx
```

打印输出如下，裁剪了两个多余算子。

```bash
┏━━━━━━━━━━━━━━━┳━━━━━━━━━━━━━━━━┳━━━━━━━━━━━━━━━━━━┓
┃               ┃ Original Model ┃ Simplified Model ┃
┡━━━━━━━━━━━━━━━╇━━━━━━━━━━━━━━━━╇━━━━━━━━━━━━━━━━━━┩
│ Add           │ 22             │ 22               │
│ Clip          │ 1              │ 1                │
│ Concat        │ 19             │ 19               │
│ Constant      │ 192            │ 191              │
│ Conv          │ 88             │ 88               │
│ ConvTranspose │ 1              │ 1                │
│ Exp           │ 1              │ 1                │
│ MatMul        │ 4              │ 4                │
│ MaxPool       │ 3              │ 3                │
│ Mul           │ 81             │ 81               │
│ Pow           │ 1              │ 0                │
│ Reshape       │ 6              │ 6                │
│ Resize        │ 5              │ 5                │
│ Sigmoid       │ 78             │ 78               │
│ Softmax       │ 2              │ 2                │
│ Split         │ 11             │ 11               │
│ Transpose     │ 4              │ 4                │
│ Model Size    │ 46.0MiB        │ 46.0MiB          │
└───────────────┴────────────────┴──────────────────┘
```

用Netron软件查看ONNX模型结构如下：

![Netron](figures/Netron.png)



# 参考的onnx浮点模型下载地址

```bash
http://netstorage.allwinnertech.com:5000/sharing/jV6YQDAv2
```



# 数据集来源

yolo26-depth深度估计模型使用的NYU Depth V2数据集来进行的训练和评估，这是一个室内场景深度数据集，覆盖复杂室内环境（办公室、家庭等），数据集链接：https://cs.nyu.edu/~fergus/datasets/nyu_depth_v2.html，选取10张训练集图片作为量化数据集，存放在../../dataset/NYU_Depth_V2文件夹中。



# 模型转换

模型转换主要包含原始模型导入、量化、导出为NPU可识别的模型格式等步骤。

## yolo26_depth

```bash
cd ./convert_model/
```

修改config_yml.py文件的相关参数配置；

yolo26-depth模型是在[0, 1]范围的RGB值上训练的，没有做 mean/std 标准化

$$
x_{\text{out}} = \frac{x/255 - \text{mean}}{\text{std}}
$$

因此，数据的标准化操作为：mean=[0, 0, 0] x 255，scale=[1,1,1] / 255 = [0.0039216, 0.0039216, 0.0039216]

```python
# "database"
DATASET = ['../../dataset/NYU_Depth_V2/dataset.txt']
DATASET_TYPE = ["TEXT"]

# mean, scale	##根据模型训练中的图像预处理参数
MEAN = [0, 0, 0]
SCALE = [0.00392, 0.00392, 0.00392]

# reverse_channel: True bgr, False rgb
REVERSE_CHANNEL = False

# add_preproc_node, True or False
ADD_PREPROC_NODE = True
# "preproc_type"
PREPROC_TYPE = ["IMAGE_RGB"]

# add_postproc_node, quant output -> float32 output
ADD_POSTPROC_NODE = True
```

模型导入、量化、导出等步骤：

```bash
# using xxx_env.sh to create softlink
./convert_model_env.sh

# 导入
# pegasus_import.sh <model_name>
./pegasus_import.sh yolo26s-depth

# 量化
# pegasus_quantize.sh <model_name> <quantize_type> <calibration_set_size>
./pegasus_quantize.sh yolo26s-depth int16 10

# 仿真（可选）
# pegasus_inference.sh <model_name> <quantize_type>
./pegasus_inference.sh yolo26s-depth int16

# 导出nb模型
# pegasus_export_ovx_nbg.sh <model_name> <quantize_type> <platform>
./pegasus_export_ovx_nbg.sh yolo26s-depth int16 t736

# 导出的模型文件存放在../model目录
# 例如 ../model/yolo26s-depth_int16_t736.nb
```



# 板端demo

含demo编译及运行说明

## 解压opencv压缩包

```bash
# 进入目录
cd ../../../3rdparty/opencv/
# 解压，选择对应平台
# armhf, eg: V85x, R853
unzip opencv-3.4.16-gnueabihf-linux.zip
# linux aarch64, eg: T527/MR527/MR536/T536/A733/T736
unzip opencv-4.9.0-aarch64-linux-sunxi-glibc.zip
# android aarch64, eg: T527/A733/T736
unzip opencv-4.9.0-android.zip
```

## 准备交叉编译工具链

### Linux

```bash
# 进入目录
cd ../../0-toolchains/
# 解压
# armhf, V85x, R853
unzip arm-openwrt-linux-muslgnueabi.zip
chmod 777 -R ./arm-openwrt-linux-muslgnueabi
# aarch64, MR527, T527, MR536, T536, A733, T736
tar xvf gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu.tar.xz
# aarch64 for debian11, T527, A733, T736
tar vxf gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu.tar.xz
```

编译脚本会根据平台自动选择交叉编译工具链，若需使用其它路径的工具链，可在`cmake_toolchain`目录修改`.cmake`文件内容指定对应的交叉编译工具链路径。

## build && run

### Linux

在Linux系统下测试。编译用法如下：

```bash
# 途径一：在yolo26_depth目录编译
cd ../examples/yolo26_depth/
./../build_linux.sh -t <platform> [-s <system>]
# 途径二：在examples目录，再选择yolo26_depth目录编译
cd ../examples
./build_linux.sh -t <platform> -p yolo26_depth [-s <system>]
```

以下说明以T736平台为例，编译的文件在mediapipe/install/yolo26_depth_demo_linux_t736

```bash
cd ../examples
./build_linux.sh -t t736 -p yolo26_depth
```

push 可执行文件、模型文件、输入图片到板端目录；

```bash
adb push install/yolo26_depth_demo_linux_t736 /mnt/UDISK/
```

运行

```bash
adb shell
cd /mnt/UDISK/yolo26_depth_demo_linux_t736

# 可选
export LD_LIBRARY_PATH=./lib

# 运行可执行文件
# ./yollo26_depth_demo_t736 -h 查看执行示例说明
chmod +x ./yolo26_depth_demo_t736

./yolo26_depth_demo_t736  -nb model/yolo26s-depth_int16_t736.nb -i model/rgb_00285.jpg
```

运行后，检测结果保存为图片output_depth_heatmap.jpg。

int16量化推理结果如下：

![result_nb_int16](figures/result_nb_int16.jpg)
