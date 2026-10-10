/*
 *   1. LetterBox: 等比例缩放到 768 + (114) padding
 *   2. BGR -> RGB
 *   3. 数据写入 NPU 输入 buffer (uint8 )
 */

#include <opencv2/opencv.hpp>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "model_config.h"


static cv::Mat letterbox(const cv::Mat& img, int target_w, int target_h)
{
    int h = img.rows, w = img.cols;
    float r = std::min((float)target_h / h, (float)target_w / w);  // 缩放比例 (new/old)

    int new_w = (int)std::round(w * r);
    int new_h = (int)std::round(h * r);

    float dw = (target_w - new_w) / 2.0f;
    float dh = (target_h - new_h) / 2.0f;

    int top = (int)std::round(dh - 0.1f);
    int bottom = (int)std::round(dh + 0.1f);
    int left = (int)std::round(dw - 0.1f);
    int right = (int)std::round(dw + 0.1f);

    // resize
    cv::Mat resized;
    if (w != new_w || h != new_h)
        cv::resize(img, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
    else
        resized = img.clone();

    // padding (value=(114,114,114))
    cv::Mat padded;
    cv::copyMakeBorder(resized, padded, top, bottom, left, right,
                       cv::BORDER_CONSTANT, cv::Scalar(PAD_VALUE, PAD_VALUE, PAD_VALUE));
    return padded;
}


void get_input_data(const char* image_file, unsigned char* input_data, int Resize_H, int Resize_W)
{
    // 1. 读取图片
    cv::Mat sample = cv::imread(image_file, 1);
    if (sample.empty()) {
        fprintf(stderr, "cv::imread %s failed\n", image_file);
        // 空图: 全灰填充
        memset(input_data, PAD_VALUE, Resize_H * Resize_W * INPUT_CHANNEL);
        return;
    }

    // 2. LetterBox
    cv::Mat boxed = letterbox(sample, Resize_W, Resize_H);

    // 3. BGR -> RGB
    cv::Mat rgb;
    cv::cvtColor(boxed, rgb, cv::COLOR_BGR2RGB);

    // 4b. uint8 RGB 拷贝
    memcpy(input_data, rgb.data, Resize_H * Resize_W * INPUT_CHANNEL);
}


int yolo26_depth_preprocess(const char* imagepath, void* buff_ptr, unsigned int buff_size)
{
    int img_c = INPUT_CHANNEL;
    int Height = INPUT_HEIGHT;
    int Width = INPUT_WIDTH;
    int img_size = Height * Width * img_c;
    unsigned int data_size = img_size * sizeof(uint8_t);

    if (data_size > buff_size) {
        printf("data size (%u) > buff size (%u), please check code.\n", data_size, buff_size);
        return -1;
    }

    get_input_data(imagepath, (unsigned char*)buff_ptr, Height, Width);

    return 0;
}
