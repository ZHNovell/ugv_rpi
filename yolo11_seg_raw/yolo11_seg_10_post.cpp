/*
 * Company:    AW
 * Author:     Penng
 * Date:    2025/10/11
 */

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/dnn.hpp>
#include <iostream>
#include <stdio.h>
#include <vector>
#include <cmath>

#include "model_config.h"


using namespace std;


struct Object
{
    cv::Rect_<float> rect;
    int         label;
    float       prob;
    int         gindex;
    cv::Mat     mask;
    std::vector<float> mask_feat;
};


static inline float intersection_area(const Object& a, const Object& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

static void qsort_descent_inplace(std::vector<Object>& objects, int left, int right)
{
    int i = left;
    int j = right;
    float p = objects[(left + right) / 2].prob;

    while (i <= j)
    {
        while (objects[i].prob > p)
            i++;

        while (objects[j].prob < p)
            j--;

        if (i <= j)
        {
            // swap
            std::swap(objects[i], objects[j]);

            i++;
            j--;
        }
    }

#pragma omp parallel sections
    {
#pragma omp section
        {
            if (left < j) qsort_descent_inplace(objects, left, j);
        }
#pragma omp section
        {
            if (i < right) qsort_descent_inplace(objects, i, right);
        }
    }
}

static void qsort_descent_inplace(std::vector<Object>& objects)
{
    if (objects.empty())
        return;

    qsort_descent_inplace(objects, 0, objects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<Object>& objects, std::vector<int>& picked, float nms_threshold, bool agnostic = false)
{
    picked.clear();

    const int n = objects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
    {
        areas[i] = objects[i].rect.area();
    }

    for (int i = 0; i < n; i++)
    {
        const Object& a = objects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++)
        {
            const Object& b = objects[picked[j]];

//            if (!agnostic && a.label != b.label)
//                continue;

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            // float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold)
                keep = 0;
        }

        if (keep)
            picked.push_back(i);
    }
}

static inline float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

static inline float desigmoid(float x)
{
    return static_cast<float>(-log(1.f/x - 1.f));
}

static float softmax(
    const float* src,
    float* dst,
    int length
)
{
    float alpha = -FLT_MAX;
    for (int c = 0; c < length; c++)
    {
        float score = src[c];
        if (score > alpha)
        {
            alpha = score;
        }
    }

    float denominator = 0;
    float dis_sum = 0;
    for (int i = 0; i < length; ++i)
    {
        dst[i] = expf(src[i] - alpha);
        denominator += dst[i];
    }
    for (int i = 0; i < length; ++i)
    {
        dst[i] /= denominator;
        dis_sum += i * dst[i];
    }
    return dis_sum;
}


static void generate_proposals_9(int stride, const float* feat_grid, const float* feat_score, const float* feat_mask, float prob_threshold,
                                std::vector<Object>& objects, int letterbox_cols, int letterbox_rows)
{
    const int num_grid_x = letterbox_cols / stride;
    const int num_grid_y = letterbox_rows / stride;
    const int num_grid_size = num_grid_x * num_grid_y;

    const int reg_max_1 = 16;
    const int num_class = CLASS_NUM; // number of classes, 80 for COCO
    const int mask_channel = MASK_PROTOS_C;
    float dst[16];

    float deprob_threshold = desigmoid(prob_threshold);

    cv::Mat out_score = cv::Mat(num_class,    num_grid_size, CV_32FC1, (float*)feat_score);

    cv::transpose(out_score, out_score);    // num_grid_size x num_class


    for (int y = 0; y < num_grid_y; y++)
    {
        for (int x = 0; x < num_grid_x; x++)
        {
            int num_grid_idx = y * num_grid_x + x;

            // find label with max score
            int label = -1;
            float score = -FLT_MAX;
            {
                const float *pred_score = (float*)out_score.data + num_grid_idx * num_class;

                for (int k = 0; k < num_class; k++)
                {
                    float s = *(pred_score + k);
                    if (s > score)
                    {
                        label = k;
                        score = s;
                    }
                }
                if (score < deprob_threshold) {
                    continue;
                }
                score = sigmoid(score);
            }

            if (score >= prob_threshold)
            {
                const float* cur_pred_grid = feat_grid + num_grid_idx;

                float pred_grid[reg_max_1 * 4] = { 0.0f };
                for (int i = 0; i < reg_max_1 * 4; i++) {
                    pred_grid[i] = *(cur_pred_grid + i * num_grid_size);
                }

                float x0 = x + 0.5f - softmax(pred_grid, dst, 16);
                float y0 = y + 0.5f - softmax(pred_grid + 16, dst, 16);
                float x1 = x + 0.5f + softmax(pred_grid + 2 * 16, dst, 16);
                float y1 = y + 0.5f + softmax(pred_grid + 3 * 16, dst, 16);

                x0 *= stride;
                y0 *= stride;
                x1 *= stride;
                y1 *= stride;

                Object obj;
                obj.rect.x = x0;
                obj.rect.y = y0;
                obj.rect.width = x1 - x0;
                obj.rect.height = y1 - y0;
                obj.label = label;
                obj.prob = score;
                obj.gindex = num_grid_idx;

                const float* cur_pred_mask = feat_mask + num_grid_idx;

                std::vector<float> mask_feat(mask_channel);
                for (int i = 0; i < mask_channel; i++) {
                    mask_feat[i] = *(cur_pred_mask + i * num_grid_size);
                }

                // mask_feat
                obj.mask_feat.resize(mask_channel);
                memcpy(obj.mask_feat.data(), mask_feat.data(), sizeof(float) * mask_channel);

                objects.push_back(obj);
            }

        }
    }
}

int detect_yolo11_seg_10_post(const cv::Mat& bgr, std::vector<Object>& objects, float **output)
{
    std::chrono::steady_clock::time_point Tbegin, Tend;

    Tbegin = std::chrono::steady_clock::now();


    const float  *p8_data_0_ptr = output[0];        /* grid  */
    const float  *p8_data_1_ptr = output[1];        /* score */
    const float  *p8_data_2_ptr = output[2];        /* the number of masks */
    const float *p16_data_0_ptr = output[3];
    const float *p16_data_1_ptr = output[4];
    const float *p16_data_2_ptr = output[5];        /* the number of masks */
    const float *p32_data_0_ptr = output[6];
    const float *p32_data_1_ptr = output[7];
    const float *p32_data_2_ptr = output[8];        /* the number of masks */
    const float *protos_ptr = output[9];            /* protos */

    int img_w = bgr.cols;
    int img_h = bgr.rows;

    // set default letterbox size
    int letterbox_rows = LETTERBOX_ROWS;
    int letterbox_cols = LETTERBOX_COLS;

    /* postprocess */
    const float prob_threshold = SCORE_THRESHOLD;
    const float nms_threshold  = NMS_THRESHOLD;
    const float mask_threshold = MASK_THRESHOLD;

    std::vector<Object> proposals;
    std::vector<Object> objects8;
    std::vector<Object> objects16;
    std::vector<Object> objects32;

    {
        generate_proposals_9(8, p8_data_0_ptr, p8_data_1_ptr, p8_data_2_ptr, prob_threshold, objects8, letterbox_cols, letterbox_rows);
        proposals.insert(proposals.end(), objects8.begin(), objects8.end());
    }

    {
        generate_proposals_9(16, p16_data_0_ptr, p16_data_1_ptr, p16_data_2_ptr, prob_threshold, objects16, letterbox_cols, letterbox_rows);
        proposals.insert(proposals.end(), objects16.begin(), objects16.end());
    }

    {
        generate_proposals_9(32, p32_data_0_ptr, p32_data_1_ptr, p32_data_2_ptr, prob_threshold, objects32, letterbox_cols, letterbox_rows);
        proposals.insert(proposals.end(), objects32.begin(), objects32.end());
    }


    // sort all proposals by score from highest to lowest
    qsort_descent_inplace(proposals);

    // apply nms with nms_threshold
    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, nms_threshold);


    float scale_letterbox = 1.0f;
    if ((letterbox_rows * 1.0 / bgr.rows) < (letterbox_cols * 1.0 / bgr.cols))
    {
        scale_letterbox = letterbox_rows * 1.0 / bgr.rows;
    }
    else
    {
        scale_letterbox = letterbox_cols * 1.0 / bgr.cols;
    }
    int resize_cols = int(round(scale_letterbox * bgr.cols));
    int resize_rows = int(round(scale_letterbox * bgr.rows));

    int hpad = (letterbox_rows - resize_rows);
    int wpad = (letterbox_cols - resize_cols);

    float ratio_y = (float)bgr.rows / resize_rows;
    float ratio_x = (float)bgr.cols / resize_cols;

    // mask proto
    int mask_proto_dim = MASK_PROTOS_C;
    int mask_stride  = MASK_PROTOS_STRIDE;
    int mask_proto_h = int(letterbox_rows / mask_stride);
    int mask_proto_w = int(letterbox_cols / mask_stride);


    int count = picked.size();
    if (count == 0)
        return 0;

    std::vector<vector<float>> objects_mask_feat;
    objects_mask_feat.resize(count);

    objects.resize(count);
    for (int i = 0; i < count; i++)
    {
        objects[i] = proposals[picked[i]];

        float x0 = (objects[i].rect.x);
        float y0 = (objects[i].rect.y);
        float x1 = (objects[i].rect.x + objects[i].rect.width);
        float y1 = (objects[i].rect.y + objects[i].rect.height);

        int hstart = std::floor(objects[i].rect.y / mask_stride);
        int hend   = std::ceil(objects[i].rect.y  / mask_stride + objects[i].rect.height / mask_stride);
        int wstart = std::floor(objects[i].rect.x / mask_stride);
        int wend   = std::ceil(objects[i].rect.x  / mask_stride + objects[i].rect.width  / mask_stride);

        hstart = std::min(std::max(hstart, 0), mask_proto_h);
        wstart = std::min(std::max(wstart, 0), mask_proto_w);
        hend   = std::min(std::max(hend, 0), mask_proto_h);
        wend   = std::min(std::max(wend, 0), mask_proto_w);

        int mask_w = wend - wstart;
        int mask_h = hend - hstart;

        cv::Mat mask = cv::Mat(mask_h, mask_w, CV_32FC1);
        if (mask_w > 0 && mask_h > 0)
        {
            std::vector<cv::Range> roi_ranges;
            roi_ranges.push_back(cv::Range(0, 1));
            roi_ranges.push_back(cv::Range::all());
            roi_ranges.push_back(cv::Range(hstart, hend));
            roi_ranges.push_back(cv::Range(wstart, wend));

            cv::Mat mask_protos = cv::Mat(mask_proto_dim, mask_proto_h * mask_proto_w, CV_32FC1, (float*)protos_ptr);
            int protos_size[] = {1, mask_proto_dim, mask_proto_h, mask_proto_w};
            cv::Mat mask_protos_reshape = mask_protos.reshape(1, 4, protos_size);
            cv::Mat protos = mask_protos_reshape(roi_ranges).clone().reshape(0, {mask_proto_dim, mask_w * mask_h});
            cv::Mat mask_proposals = cv::Mat(1, mask_proto_dim, CV_32FC1, (float*)objects[i].mask_feat.data());
            cv::Mat masks_feature = (mask_proposals * protos);

            /* sigmoid */
            cv::exp(-masks_feature.reshape(1, {mask_h, mask_w}), mask);
            mask = 1.0 / (1.0 + mask);
        }


        // adjust offset to original unpadded
        x0 = (objects[i].rect.x - (wpad / 2)) * ratio_x;
        y0 = (objects[i].rect.y - (hpad / 2)) * ratio_y;
        x1 = (objects[i].rect.x + objects[i].rect.width  - (wpad / 2)) * ratio_x;
        y1 = (objects[i].rect.y + objects[i].rect.height - (hpad / 2)) * ratio_y;

        // clip
        x0 = std::max(std::min(x0, (float)(img_w - 1)), 0.f);
        y0 = std::max(std::min(y0, (float)(img_h - 1)), 0.f);
        x1 = std::max(std::min(x1, (float)(img_w - 1)), 0.f);
        y1 = std::max(std::min(y1, (float)(img_h - 1)), 0.f);

        objects[i].rect.x = x0;
        objects[i].rect.y = y0;
        objects[i].rect.width = x1 - x0;
        objects[i].rect.height = y1 - y0;

        cv::resize(mask, mask, cv::Size((int)objects[i].rect.width, (int)objects[i].rect.height));
        objects[i].mask = mask > mask_threshold;
    }

    // sort objects by area
    struct
    {
        bool operator()(const Object& a, const Object& b) const
        {
            return a.rect.area() > b.rect.area();
        }
    } objects_area_greater;
    std::sort(objects.begin(), objects.end(), objects_area_greater);


    Tend = std::chrono::steady_clock::now();
    float f = std::chrono::duration_cast <std::chrono::milliseconds> (Tend - Tbegin).count();

    std::cout << "post process time : " << f << " ms" << std::endl;

    fprintf(stderr, "detection num: %d\n", count);

    return 0;
}


static void draw_objects(const cv::Mat& bgr, const std::vector<Object>& objects, const char *imagepath)
{
    cv::Mat image = bgr.clone();
    cv::Mat mask  = bgr.clone();

    static const cv::Scalar colors[] = {
        cv::Scalar(233, 30, 99),
        cv::Scalar(156, 39, 176),
        cv::Scalar(103, 58, 183),
        cv::Scalar(63, 81, 181),
        cv::Scalar(3, 169, 244),
        cv::Scalar(0, 188, 212),
        cv::Scalar(255, 193, 7),
        cv::Scalar(255, 152, 0),
        cv::Scalar(255, 87, 34),
        cv::Scalar(96, 125, 139)
    };

    for (size_t i = 0; i < objects.size(); i++)
    {
        const Object& obj = objects[i];

        const cv::Scalar& color = colors[i % 10];

        if (obj.prob > 1.0) {
            fprintf(stderr, "%2d: %3.0f%%, [%4.0f, %4.0f, %4.0f, %4.0f], score is illegal ........ \n", obj.label, obj.prob * 100, obj.rect.x,
                	obj.rect.y, obj.rect.x + obj.rect.width, obj.rect.y + obj.rect.height);
            continue;
        }

        fprintf(stderr, "%2d: %3.0f%%, [%4.0f, %4.0f, %4.0f, %4.0f], %s\n", obj.label, obj.prob * 100, obj.rect.x,
                obj.rect.y, obj.rect.x + obj.rect.width, obj.rect.y + obj.rect.height, g_classes_name[obj.label].c_str());


        mask(cv::Rect((int)obj.rect.x, (int)obj.rect.y, (int)objects[i].rect.width, (int)objects[i].rect.height)).setTo(color, objects[i].mask);

        cv::rectangle(image, obj.rect, color);

        char text[256];
        sprintf(text, "%s %.1f%%", g_classes_name[obj.label].c_str(), obj.prob * 100);

        int baseLine = 0;
        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

        int x = obj.rect.x;
        int y = obj.rect.y - label_size.height - baseLine;
        if (y < 0)
            y = 0;
        if (x + label_size.width > image.cols)
            x = image.cols - label_size.width;

        cv::rectangle(image, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
        cv::Scalar(255, 255, 255), -1);

        cv::putText(image, text, cv::Point(x, y + label_size.height),
        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
    }

    image = 0.5 * mask + 0.5 * image;

	cv::imwrite("out_yolo11_seg.png", image);
}

int yolo11_seg_postprocess(const char *imagepath, float **output)
{
    cv::Mat m = cv::imread(imagepath, 1);
    if (m.empty()) {
        fprintf(stderr, "cv::imread %s failed\n", imagepath);
        return -1;
    }

	std::vector<Object> objects;
    detect_yolo11_seg_10_post(m, objects, output);


    draw_objects(m, objects, imagepath);

//    printf("yolo11_seg_postprocess finished. \n");

    return 0;
}
