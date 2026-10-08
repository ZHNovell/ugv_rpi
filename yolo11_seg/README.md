# 概述

本文档描述yolo11-seg模型在NPU的部署过程，含模型转换与板端示例两部分，其中`convert_model`为模型转换的目录，其它文件为板端运行的示例代码等文件。

# 模型获取

## 模型来源

测试模型基于https://github.com/ultralytics/ultralytics.git 仓库的e15d6f50dc618d542c1cd9f3d968c76981c90c9f commit 导出、修改和验证。原始模型下载链接：https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11s-seg.pt

## 导出onnx模型

```
from ultralytics import YOLO
model = YOLO("./yolo11s-seg.pt")
model.export(
    format='onnx',
    imgsz=640,
    dynamic=False,
    simplify=True,
    opset=11,
    nms=False,
    batch=1,
    device='cpu'
)
```

## 模型修改内容

yolo11-seg网络的后处理部分8bit量化会产生较大的精度损失，通过onnx_extract.py对模型剪枝，修改输出结构，同时将后处理结构移至外部使用cpu进行相应的处理，最终模型输出差异如下，左边是官方模型，右边是修改后的模型；

![diff_img](figures/diff_img.png)


## 参考的onnx浮点模型下载地址

```
https://netstorage.allwinnertech.com:5001/sharing/csx561sLG
```

将下载的onnx模型保存到convert_model目录。



# 模型转换

模型转换主要包含原始模型导入、量化、导出为NPU可识别的模型格式等步骤。

```bash
cd ./convert_model/
```

修改config_yml.py文件的相关参数配置；

```python
# "database"
DATASET = '../../dataset/coco_12/dataset.txt'
DATASET_TYPE = "TEXT"
# mean, scale
MEAN    = [0, 0, 0]
SCALE   = [0.0039216, 0.0039216, 0.0039216]

# reverse_channel: True bgr, False rgb
REVERSE_CHANNEL = False
# add_preproc_node, True or False
ADD_PREPROC_NODE = True
# "preproc_type"
PREPROC_TYPE = "IMAGE_RGB"
# add_postproc_node, quant output -> float32 output
ADD_POSTPROC_NODE = True
```

模型导入、量化、导出等步骤：

```bash
# using xxx_env.sh to create softlink
./convert_model_env.sh

# 导入
# pegasus_import.sh <model_name>
./pegasus_import.sh yolo11s-seg_10

# 量化
# pegasus_quantize.sh <model_name> <quantize_type> <calibration_set_size>
./pegasus_quantize.sh yolo11s-seg_10 uint8 12

# 仿真（可选）
# pegasus_inference.sh <model_name> <quantize_type>
./pegasus_inference.sh yolo11s-seg_10 uint8

# 导出nb模型
# pegasus_export_ovx_nbg.sh <model_name> <quantize_type> <platform>
./pegasus_export_ovx_nbg.sh yolo11s-seg_10 uint8 t527

# 导出的模型文件存放在../model目录
# 例如 ../model/yolo11s-seg_10_uint8_t527.nb
```



# 板端demo

含demo编译及运行说明。

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



### Android

下载Android NDK，下载地址： https://developer.android.google.cn/ndk/downloads?hl=zh-cn

将下载的NDK放到编译机器目录，例如：./0-toolchains/ ;

请根据下载的版本修改`cmake_toolchain`目录的`android_ndk_build_env.sh` 文件。

使用unzip命令解压

## build && run

### Linux

在Linux系统下测试。编译用法如下：

```bash
# 途径一：在yolo11_seg目录编译
cd ../examples/yolo11_seg/
./../build_linux.sh -t <platform> [-s <system>]
# 途径二：在examples目录，再选择yolo11_seg目录编译
cd ../examples
./build_linux.sh -t <platform> -p yolo11_seg [-s <system>]
```

以下说明以T527平台为例；

```bash
cd ../examples/yolo11_seg/
./../build_linux.sh -t t527
```

> 若是T527平台debian系统，则是以下命令：
>
> ```bash
> cd ../examples/yolo11_seg/
> ./../build_linux.sh -t t527 -s debian11
> ```

push 可执行文件、模型文件、输入图片到板端目录（建议推到tf卡目录，空间充足）；

```bash
adb push .\install\yolo11_seg_demo_linux_t527 /mnt/UDISK/
```

运行；

```bash
adb shell
cd /mnt/UDISK/yolo11_seg_demo_linux_t527

# 可选
export LD_LIBRARY_PATH=./lib

# 运行可执行文件
chmod +x ./yolo11_seg_demo_t527
./yolo11_seg_demo_t527 -nb model/yolo11s-seg_10_pcq_t527.nb -i model/dog.jpg
```

运行后，打印log输出，能看到检测信息输出和输出解析后保存的图片（同下文）。



### Android

在Android 64bit系统下测试。编译用法如下：

```bash
# 途径一：在yolo11_seg目录编译
cd ../examples/yolo11_seg/
./../build_android.sh -t <platform>
# 途径二：在examples目录，再选择yolo11_seg目录编译
cd ../examples
./build_android.sh -t <platform> -p yolo11_seg
```

以下说明以A733平台为例；

```bash
cd ../examples/yolo11_seg/
./../build_android.sh -t a733
```

修改权限；

```bash
adb root
adb remount
```

push 可执行文件、模型文件、输入图片到`/data/local/`目录；

```bash
adb push install\yolo11_seg_demo_android_a733 /data/local/
```

运行；

```bash
adb shell
cd /data/local/yolo11_seg_demo_android_a733

export LD_LIBRARY_PATH=./lib

# 运行可执行文件
chmod +x ./yolo11_seg_demo_a733
./yolo11_seg_demo_a733 -nb model/yolo11s-seg_10_pcq_a733.nb -i model/dog.jpg
```

运行后，打印log输出，能看到检测信息输出；

```tex
detection num: 3
 1:  95%, [ 125,  130,  568,  419], bicycle
16:  94%, [ 132,  221,  310,  541], dog
 2:  81%, [ 467,   76,  688,  172], car
destory npu finished.
~NpuUint.
```

输出解析后保存的图片：

![out_yolo11_seg_pcq](figures/out_yolo11_seg_pcq.png)