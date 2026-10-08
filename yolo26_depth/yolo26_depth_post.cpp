/*
 *   1. NPU 输出 (OUTPUT_H, OUTPUT_W) 深度图 → 裁剪 letterbox padding + 双线性缩放回原图
 *   2.  disparity(1/d) + 2/98 百分位 + JET 热力图
 */

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <opencv2/opencv.hpp>
#include <sys/time.h>

#include "model_config.h"


/*
 *   从模型输出 (OUTPUT_H, OUTPUT_W) 裁剪 letterbox padding 后, 双线性缩放到原图尺寸。
 *   gain = min(im1_h/im0_h, im1_w/im0_w); 反推 pad_w/pad_h; 均分; 裁剪; 缩放 */
static cv::Mat crop_resize_depth(const cv::Mat& depth, const cv::Size& orig)
{
    int im1_h = depth.rows, im1_w = depth.cols;
    int im0_h = orig.height, im0_w = orig.width;
    if (im1_h == im0_h && im1_w == im0_w)
        return depth.clone();

    float gain = std::min((float)im1_h / im0_h, (float)im1_w / im0_w);  // gain = old / new
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


/*
 *   v = 1/d (disparity); lo/hi = percentile(v[valid], 2/98)
 *   dn = clip((v-lo)/(hi-lo), 0, 1); applyColorMap(JET); 无效(d<=0)像素置黑
 *   返回 BGR uint8 热力图
 */
static cv::Mat colorize_depth_disparity(const cv::Mat& depth)
{
    cv::Mat d;
    depth.convertTo(d, CV_32F);
    const int total = (int)d.total();
    const float* dptr = d.ptr<float>();  // 裸指针, 避免 at<float>(i) 索引开销

    // 单次遍历: v = 1/d (valid 之外为 0), 同时收集 valid 像素
    cv::Mat v(d.size(), CV_32F, cv::Scalar(0.0f));
    float* vptr = v.ptr<float>();
    std::vector<float> pool;
    pool.reserve(total);
    for (int i = 0; i < total; ++i) {
        float dv = dptr[i];
        if (dv > 0) {
            vptr[i] = 1.0f / dv;
            pool.push_back(vptr[i]);
        }
    }

    // 2/98 百分位
    float lo = 0.0f, hi = 1.0f;
    if (!pool.empty()) {
        size_t idx_lo = std::min<size_t>(pool.size() - 1, (size_t)(pool.size() * 0.02));
        size_t idx_hi = std::min<size_t>(pool.size() - 1, (size_t)(pool.size() * 0.98));

        // std::nth_element 部分排序 (O(n)
        std::nth_element(pool.begin(), pool.begin() + idx_hi, pool.end());
        hi = pool[idx_hi];
        std::nth_element(pool.begin(), pool.begin() + idx_lo, pool.begin() + idx_hi);
        lo = pool[idx_lo];

        if (hi <= lo) hi = lo + 1e-6f;
    }
    const float inv_range = 1.0f / (hi - lo);

    // 单次遍历: idx = clip((v-lo)*inv_range, 0, 1)*255 → uint8 (合并 dn+convertTo)
    cv::Mat idx(d.size(), CV_8U);
    uchar* idxptr = idx.ptr<uchar>();
    for (int i = 0; i < total; ++i) {
        float t = (vptr[i] - lo) * inv_range;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        idxptr[i] = (uchar)(t * 255.0f);
    }

    cv::Mat heat;
    cv::applyColorMap(idx, heat, cv::COLORMAP_JET);  // BGR

    // 无效 (d<=0) 像素置黑
    cv::Vec3b* hptr = heat.ptr<cv::Vec3b>();
    for (int i = 0; i < total; ++i) {
        if (dptr[i] <= 0)
            hptr[i] = cv::Vec3b(0, 0, 0);
    }
    return heat;
}


int yolo26_depth_postprocess(const char *imagepath, float **output)
{
    cv::Mat m = cv::imread(imagepath, 1);
    if (m.empty()) {
        fprintf(stderr, "cv::imread %s failed\n", imagepath);
        return -1;
    }

    struct timeval tv1, tv2;
    gettimeofday(&tv1, NULL);

    // 1. NPU 输出 → 深度图 (OUTPUT_H, OUTPUT_W) float32, 单位: 米
    const float* npu_out = output[0];
    cv::Mat depth(OUTPUT_HEIGHT, OUTPUT_WIDTH, CV_32F);
    memcpy(depth.data, npu_out, OUTPUT_HEIGHT * OUTPUT_WIDTH * sizeof(float));

    // 2. 裁剪 letterbox padding + 双线性缩放回原图
    cv::Mat depth_map = crop_resize_depth(depth, m.size());

    // //深度统计
    // double d_min, d_max, d_mean;
    // cv::minMaxLoc(depth_map, &d_min, &d_max);
    // d_mean = cv::mean(depth_map)[0];
    // fprintf(stderr, "depth range: %.3f ~ %.3f m, mean: %.3f m\n", d_min, d_max, d_mean);


    gettimeofday(&tv2, NULL);
    double total_time_ms = (tv2.tv_sec - tv1.tv_sec) * 1000.0 + (tv2.tv_usec - tv1.tv_usec) / 1000.0;
    fprintf(stderr, "depth postprocess time: %.2f ms\n", total_time_ms);


    // 3. 可视化: 输出热力图
    cv::Mat heat = colorize_depth_disparity(depth_map);
    cv::imwrite(OUTPUT_HEATMAP_PATH, heat);
    fprintf(stderr, "saved heatmap: %s\n", OUTPUT_HEATMAP_PATH);

    return 0;
}
