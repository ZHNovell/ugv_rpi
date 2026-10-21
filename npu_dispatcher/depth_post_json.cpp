// depth_post_json.cpp — обёртка для depth
// Использует crop_resize_depth (из yolo26_depth_post.cpp) + depth_to_json (из npu_depth_server.cpp)

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <opencv2/opencv.hpp>

#include "model_config_depth.h"

#define GRID_W 16
#define GRID_H 16
#define NEAR_THRESHOLD 2.0f

using namespace std;

// === Из yolo26_depth_post.cpp ===
static cv::Mat crop_resize_depth(const cv::Mat& depth, const cv::Size& orig)
{
    int im1_h = depth.rows, im1_w = depth.cols;
    int im0_h = orig.height, im0_w = orig.width;
    if (im1_h == im0_h && im1_w == im0_w)
        return depth.clone();

    float gain = std::min((float)im1_h / im0_h, (float)im1_w / im0_w);
    float pad_w = im1_w - std::round(im0_w * gain);
    float pad_h = im1_h - std::round(im0_h * gain);
    pad_w /= 2.0f;
    pad_h /= 2.0f;

    int top = (int)std::round(pad_h - 0.1f);
    int left = (int)std::round(pad_w - 0.1f);
    int bottom = im1_h - (int)std::round(pad_h + 0.1f);
    int right = im1_w - (int)std::round(pad_w + 0.1f);

    cv::Mat cropped = depth(cv::Rect(left, top, right - left, bottom - top)).clone();
    cv::Mat resized;
    cv::resize(cropped, resized, cv::Size(im0_w, im0_h), 0, 0, cv::INTER_LINEAR);
    return resized;
}

// === Из npu_depth_server.cpp ===
static string depth_to_json(const cv::Mat& depth_map, int orig_w, int orig_h)
{
    cv::Mat small;
    cv::resize(depth_map, small, cv::Size(GRID_W, GRID_H), 0, 0, cv::INTER_AREA);

    string s = "{\"grid_size\":[";
    char buf[64];
    snprintf(buf, sizeof(buf), "%d,%d],\"grid\":[", GRID_W, GRID_H);
    s += buf;

    const float* sp = small.ptr<float>();
    for (int i = 0; i < GRID_W * GRID_H; i++) {
        if (i > 0) s += ",";
        float v = sp[i];
        if (v < 0) v = 0;
        snprintf(buf, sizeof(buf), "%.2f", v);
        s += buf;
    }
    s += "],";

    cv::Mat near_mask(depth_map.rows, depth_map.cols, CV_8U, cv::Scalar(0));
    uchar* mp = near_mask.ptr<uchar>();
    const float* dp = depth_map.ptr<float>();
    int total = (int)depth_map.total();
    for (int i = 0; i < total; i++) {
        float v = dp[i];
        if (v > 0 && v < NEAR_THRESHOLD)
            mp[i] = 1;
    }

    snprintf(buf, sizeof(buf), "\"near_threshold\":%.2f,\"near_mask_size\":[%d,%d],\"near_rle\":[",
             NEAR_THRESHOLD, near_mask.cols, near_mask.rows);
    s += buf;

    int prev = -1, count = 0;
    bool first = true;
    for (int i = 0; i < total; i++) {
        int v = mp[i];
        if (v == prev) {
            count++;
        } else {
            if (prev != -1 && prev == 1) {
                snprintf(buf, sizeof(buf), "%s[%d,%d]", first ? "" : ",", prev, count);
                s += buf;
                first = false;
            }
            prev = v;
            count = 1;
        }
    }
    if (prev == 1) {
        snprintf(buf, sizeof(buf), "%s[%d,%d]", first ? "" : ",", prev, count);
        s += buf;
    }
    s += "]}";
    return s;
}

// === Публичная функция ===
// depth_npu: cv::Mat 768×768 CV_32F (из NPU)
// orig_w, orig_h: исходный кадр
// json_out: буфер для JSON
// json_size: размер буфера
// Возврат: длина JSON или -1
extern "C" int depth_post_json(const cv::Mat& depth_npu, int orig_w, int orig_h,
                                char* json_out, size_t json_size) {
    cv::Mat depth_map = crop_resize_depth(depth_npu, cv::Size(orig_w, orig_h));
    string json = depth_to_json(depth_map, orig_w, orig_h);
    if (json.size() >= json_size) return -1;
    memcpy(json_out, json.data(), json.size());
    json_out[json.size()] = '\0';
    return (int)json.size();
}
