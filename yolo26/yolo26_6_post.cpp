/*
 * Company:    AW
 * Author:     Penng
 * Date:    2026/01/15
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
    int label;
    float prob;
};


static inline float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}


static void generate_proposals_6(int stride, const float* feat_grid, const float* feat_score, float prob_threshold, std::vector<Object>& objects,
                                int letterbox_cols, int letterbox_rows)
{
    const int num_grid_x = letterbox_cols / stride;
    const int num_grid_y = letterbox_rows / stride;
    const int num_grid_size = num_grid_x * num_grid_y;

    const int num_class = CLASS_NUM; // number of classes. 80 for COCO

    cv::Mat out_grid  = cv::Mat(        4, num_grid_size, CV_32FC1, (float*)feat_grid);
    cv::Mat out_score = cv::Mat(num_class, num_grid_size, CV_32FC1, (float*)feat_score);

    cv::transpose(out_grid,  out_grid);     // num_grid_size x 4
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

                score = sigmoid(score);
            }

            if (score >= prob_threshold)
            {
                const float *pred_grid = (float*)out_grid.data + num_grid_idx * 4;

                float x0 = x + 0.5f - pred_grid[0];
                float y0 = y + 0.5f - pred_grid[1];
                float x1 = x + 0.5f + pred_grid[2];
                float y1 = y + 0.5f + pred_grid[3];

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

                objects.push_back(obj);
            }
        }
    }
}

int detect_yolo26_6_post(const cv::Mat& bgr, std::vector<Object> &detections, float **output)
{
    std::chrono::steady_clock::time_point Tbegin, Tend;
    Tbegin = std::chrono::steady_clock::now();

    const float  *p8_data_0_ptr = output[0];        /* grid  */
    const float *p16_data_0_ptr = output[1];
    const float *p32_data_0_ptr = output[2];
    const float  *p8_data_1_ptr = output[3];        /* score */
    const float *p16_data_1_ptr = output[4];
    const float *p32_data_1_ptr = output[5];

    int img_w = bgr.cols;
    int img_h = bgr.rows;

    // set default letterbox size
    int letterbox_rows = LETTERBOX_ROWS;
    int letterbox_cols = LETTERBOX_COLS;

    /* postprocess */
    const float prob_threshold = SCORE_THRESHOLD;

    std::vector<Object> proposals;
    std::vector<Object> objects8;
    std::vector<Object> objects16;
    std::vector<Object> objects32;

    {
        generate_proposals_6(8, p8_data_0_ptr, p8_data_1_ptr, prob_threshold, objects8, letterbox_cols, letterbox_rows);
        proposals.insert(proposals.end(), objects8.begin(), objects8.end());
    }

    {
        generate_proposals_6(16, p16_data_0_ptr, p16_data_1_ptr, prob_threshold, objects16, letterbox_cols, letterbox_rows);
        proposals.insert(proposals.end(), objects16.begin(), objects16.end());
    }

    {
        generate_proposals_6(32, p32_data_0_ptr, p32_data_1_ptr, prob_threshold, objects32, letterbox_cols, letterbox_rows);
        proposals.insert(proposals.end(), objects32.begin(), objects32.end());
    }


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

    int count = proposals.size();

    detections.resize(count);
    for (int i = 0; i < count; i++)
    {
        detections[i] = proposals[i];

        // adjust offset to original unpadded
        float x0 = (detections[i].rect.x - (wpad / 2)) * ratio_x;
        float y0 = (detections[i].rect.y - (hpad / 2)) * ratio_y;
        float x1 = (detections[i].rect.x + detections[i].rect.width  - (wpad / 2)) * ratio_x;
        float y1 = (detections[i].rect.y + detections[i].rect.height - (hpad / 2)) * ratio_y;

        // clip
        x0 = std::max(std::min(x0, (float)(img_w - 1)), 0.f);
        y0 = std::max(std::min(y0, (float)(img_h - 1)), 0.f);
        x1 = std::max(std::min(x1, (float)(img_w - 1)), 0.f);
        y1 = std::max(std::min(y1, (float)(img_h - 1)), 0.f);

        detections[i].rect.x = x0;
        detections[i].rect.y = y0;
        detections[i].rect.width = x1 - x0;
        detections[i].rect.height = y1 - y0;
    }

    // sort objects by area
    struct
    {
        bool operator()(const Object& a, const Object& b) const
        {
            return a.rect.area() > b.rect.area();
        }
    } objects_area_greater;
    std::sort(detections.begin(), detections.end(), objects_area_greater);


    Tend = std::chrono::steady_clock::now();
    float f = std::chrono::duration_cast <std::chrono::milliseconds> (Tend - Tbegin).count();

    std::cout << "postprocess time : " << f << " ms" << std::endl;

    fprintf(stderr, "detection num: %lu\n", detections.size());

    return 0;
}

static void draw_objects(const cv::Mat& bgr, const std::vector<Object>& objects, const char *imagepath)
{
    cv::Mat image = bgr.clone();

    for (size_t i = 0; i < objects.size(); i++)
    {
        const Object& obj = objects[i];

        if (obj.prob > 1.0) {
            fprintf(stderr, "%2d: %3.0f%%, [%4.0f, %4.0f, %4.0f, %4.0f], score is illegal ........ \n", obj.label, obj.prob * 100, obj.rect.x,
                	obj.rect.y, obj.rect.x + obj.rect.width, obj.rect.y + obj.rect.height);
            continue;
        }

        fprintf(stderr, "%2d: %3.0f%%, [%4.0f, %4.0f, %4.0f, %4.0f], %s\n", obj.label, obj.prob * 100, obj.rect.x,
                obj.rect.y, obj.rect.x + obj.rect.width, obj.rect.y + obj.rect.height, g_classes_name[obj.label].c_str());

        cv::rectangle(image, obj.rect, cv::Scalar(255, 0, 0));

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
    
	cv::imwrite("out_yolo26.png", image);
}

int yolo26_postprocess(const char *imagepath, float **output)
{
    cv::Mat m = cv::imread(imagepath, 1);
    if (m.empty()) {
        fprintf(stderr, "cv::imread %s failed\n", imagepath);
        return -1;
    }

	std::vector<Object> objects;

    detect_yolo26_6_post(m, objects, output);

    draw_objects(m, objects, imagepath);

    return 0;
}
