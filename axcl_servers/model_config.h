#ifndef _MODEL_CONFIG_H_
#define _MODEL_CONFIG_H_

#include <opencv2/opencv.hpp>

// 模型输入尺寸
#define INPUT_WIDTH     768
#define INPUT_HEIGHT    768
#define INPUT_CHANNEL   3

#define PAD_VALUE       114     // 填充值

// NPU 输出 = 输入尺寸 (1,1,768,768), 单位为米
#define OUTPUT_WIDTH    768
#define OUTPUT_HEIGHT   768

#define OUTPUT_HEATMAP_PATH   "output_depth_heatmap.jpg"

/*----------------------- 函数声明 -----------------------*/
int yolo26_depth_preprocess(const char* imagepath, void* buff_ptr, unsigned int buff_size);
int yolo26_depth_postprocess(const char *imagepath, float **output);

#endif
