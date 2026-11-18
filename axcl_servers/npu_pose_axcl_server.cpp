/*
 * NPU Server для YOLO26-pose на AX-M1 (AXCL).
 * Принимает RAW BGR, возвращает JSON: bbox + 17 keypoints (COCO).
 *
 * Протокол:
 *   Клиент -> Сервер: [4B width BE][4B height BE][4B channels BE][RAW BGR]
 *   Сервер -> Клиент: [4B длина JSON BE][JSON]
 *
 * Постпроцесс портирован из ax_infer.py (YOLO26-pose): ltrb-декодирование,
 * 9 выходов (3 масштаба x [box, cls, kpt]), left-top letterbox (127),
 * keypoints: (kpt_xy + anchor) * stride, conf через sigmoid.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <arpa/inet.h>
#include <math.h>
#include <vector>
#include <string>
#include <float.h>
#include <algorithm>
#include <opencv2/opencv.hpp>
#include "ax_model_runner/ax_model_runner_axcl.hpp"
#include <axcl.h>

using namespace std;

#define SOCKET_PATH "/tmp/npu_pose_axcl.sock"
#define INPUT_W 640
#define INPUT_H 640
#define NUM_CLASS 1
#define NUM_POINT 17
#define KPT_DIM (NUM_POINT * 3)
#define SCORE_THRES 0.25f
#define NMS_THRES 0.7f

struct PoseObj {
    cv::Rect_<float> rect;       // xyxy в координатах модели
    float prob;
    float kpts[NUM_POINT][3];    // x, y, conf (в координатах модели)
};

// Left-top aligned letterbox, padding 127 (как в ax_infer.py)
static void preprocess_letterbox_lt(const cv::Mat& sample, unsigned char* input_data, int model_h, int model_w)
{
    cv::Mat img;
    cv::cvtColor(sample, img, cv::COLOR_BGR2RGB);

    float scale = std::min((float)model_h / img.rows, (float)model_w / img.cols);
    int new_w = (int)(img.cols * scale);
    int new_h = (int)(img.rows * scale);

    cv::resize(img, img, cv::Size(new_w, new_h));

    cv::Mat padded(model_h, model_w, CV_8UC3, cv::Scalar(127, 127, 127));
    img.copyTo(padded(cv::Rect(0, 0, new_w, new_h)));

    memcpy(input_data, padded.data, (size_t)model_h * model_w * 3);
}

// IoU NMS (сортировка по prob)
static vector<int> nms_boxes(const vector<PoseObj>& objs, float thres)
{
    vector<int> picked;
    int n = (int)objs.size();
    if (n == 0) return picked;

    vector<float> areas(n);
    for (int i = 0; i < n; i++) areas[i] = objs[i].rect.area();

    vector<int> order(n);
    for (int i = 0; i < n; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return objs[a].prob > objs[b].prob; });

    for (int i : order) {
        bool keep = true;
        for (int j : picked) {
            cv::Rect_<float> inter = objs[i].rect & objs[j].rect;
            float inter_area = inter.area();
            if (inter_area <= 0) continue;
            float iou = inter_area / (areas[i] + areas[j] - inter_area);
            if (iou > thres) { keep = false; break; }
        }
        if (keep) picked.push_back(i);
    }
    return picked;
}

static string detections_to_json(const vector<PoseObj>& objs)
{
    string s = "[";
    for (size_t i = 0; i < objs.size(); i++) {
        const PoseObj& o = objs[i];
        if (i > 0) s += ",";
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"class_id\":0,\"class\":\"person\",\"confidence\":%.4f,\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"keypoints\":[",
            o.prob, o.rect.x, o.rect.y, o.rect.x + o.rect.width, o.rect.y + o.rect.height);
        s += buf;
        for (int k = 0; k < NUM_POINT; k++) {
            if (k > 0) s += ",";
            char kb[64];
            snprintf(kb, sizeof(kb), "[%.1f,%.1f,%.3f]", o.kpts[k][0], o.kpts[k][1], o.kpts[k][2]);
            s += kb;
        }
        s += "]}";
    }
    s += "]";
    return s;
}

static bool read_n(int fd, void* buf, size_t n)
{
    char* p = (char*)buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

static bool write_n(int fd, const void* buf, size_t n)
{
    const char* p = (const char*)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

int main(int argc, char** argv)
{
    const char* model_file = "/root/ugv_rpi/models_axcl/yolo26m-pose_npu3.axmodel";
    if (argc >= 2) model_file = argv[1];

    fprintf(stderr, "[npu_pose_axcl_server] Loading model: %s\n", model_file);

    if (auto ret = axclInit(0); 0 != ret) {
        fprintf(stderr, "[npu_pose_axcl_server] Init AXCL failed{0x%x}\n", ret);
        return -1;
    }
    axclrtDeviceList lst;
    if (auto ret = axclrtGetDeviceList(&lst); 0 != ret || 0 == lst.num) {
        fprintf(stderr, "[npu_pose_axcl_server] Get AXCL device failed\n");
        return -1;
    }
    if (auto ret = axclrtSetDevice(lst.devices[0]); 0 != ret) {
        fprintf(stderr, "[npu_pose_axcl_server] Set AXCL device failed\n");
        return -1;
    }
    if (auto ret = axclrtEngineInit(AXCL_VNPU_DISABLE); 0 != ret) {
        fprintf(stderr, "[npu_pose_axcl_server] axclrtEngineInit failed %d\n", ret);
        return ret;
    }

    ax_runner_axcl runner;
    if (auto ret = runner.init(model_file); ret != 0) {
        fprintf(stderr, "[npu_pose_axcl_server] runner.init failed\n");
        return -1;
    }
    fprintf(stderr, "[npu_pose_axcl_server] Model loaded, ready.\n");

    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return -1; }
    if (listen(srv, 4) < 0) { perror("listen"); return -1; }
    fprintf(stderr, "[npu_pose_axcl_server] Listening on %s\n", SOCKET_PATH);

    uint8_t* input_buffer_ptr = (uint8_t*)runner.get_input(0).pVirAddr;

    while (true) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) { perror("accept"); continue; }

        while (true) {
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

            preprocess_letterbox_lt(frame, input_buffer_ptr, INPUT_H, INPUT_W);

            if (runner.inference() != 0) {
                fprintf(stderr, "[npu_pose_axcl_server] inference failed\n");
                continue;
            }

            auto* output = runner.get_outputs_ptr(0);
            int nOut = runner.get_num_outputs();
            if (nOut < 9) {
                fprintf(stderr, "[npu_pose_axcl_server] unexpected outputs: %d\n", nOut);
                continue;
            }

            // 1. Декодирование (ltrb) по 3 масштабам
            vector<PoseObj> proposals;
            float conf_raw = -logf(1.0f / SCORE_THRES - 1.0f);

            for (int s = 0; s < 3; s++) {
                int stride = 8 << s;               // 8, 16, 32
                int grid = INPUT_W / stride;       // 80, 40, 20
                int bi = s * 3, ci = s * 3 + 1, ki = s * 3 + 2;

                float* box_data = (float*)output[bi].pVirAddr;   // (grid,grid,4) ltrb
                float* cls_data = (float*)output[ci].pVirAddr;   // (grid,grid,1)
                float* kpt_data = (float*)output[ki].pVirAddr;   // (grid,grid,51)

                int HW = grid * grid;
                for (int i = 0; i < HW; i++) {
                    float raw_cls = cls_data[i];
                    if (raw_cls < conf_raw) continue;

                    float prob = 1.0f / (1.0f + expf(-raw_cls));

                    int gy = i / grid, gx = i % grid;
                    float ax = gx + 0.5f, ay = gy + 0.5f;

                    PoseObj o;
                    float x0 = (ax - box_data[i * 4 + 0]) * stride;
                    float y0 = (ay - box_data[i * 4 + 1]) * stride;
                    float x1 = (ax + box_data[i * 4 + 2]) * stride;
                    float y1 = (ay + box_data[i * 4 + 3]) * stride;
                    o.rect = cv::Rect_<float>(x0, y0, x1 - x0, y1 - y0);
                    o.prob = prob;

                    for (int k = 0; k < NUM_POINT; k++) {
                        float kx = (kpt_data[i * KPT_DIM + k * 3 + 0] + ax) * stride;
                        float ky = (kpt_data[i * KPT_DIM + k * 3 + 1] + ay) * stride;
                        float kc = 1.0f / (1.0f + expf(-kpt_data[i * KPT_DIM + k * 3 + 2]));
                        o.kpts[k][0] = kx;
                        o.kpts[k][1] = ky;
                        o.kpts[k][2] = kc;
                    }
                    proposals.push_back(o);
                }
            }

            // 2. NMS
            vector<int> picked = nms_boxes(proposals, NMS_THRES);

            // 3. Масштаб в orig-координаты
            float scale = std::min((float)INPUT_H / frame.rows, (float)INPUT_W / frame.cols);

            vector<PoseObj> objects;
            for (int idx : picked) {
                PoseObj& o = proposals[idx];
                PoseObj out;
                float x0 = std::min(std::max(o.rect.x / scale, 0.0f), (float)frame.cols);
                float y0 = std::min(std::max(o.rect.y / scale, 0.0f), (float)frame.rows);
                float x1 = std::min(std::max((o.rect.x + o.rect.width) / scale, 0.0f), (float)frame.cols);
                float y1 = std::min(std::max((o.rect.y + o.rect.height) / scale, 0.0f), (float)frame.rows);
                out.rect = cv::Rect_<float>(x0, y0, x1 - x0, y1 - y0);
                out.prob = o.prob;
                for (int k = 0; k < NUM_POINT; k++) {
                    out.kpts[k][0] = std::min(std::max(o.kpts[k][0] / scale, 0.0f), (float)frame.cols);
                    out.kpts[k][1] = std::min(std::max(o.kpts[k][1] / scale, 0.0f), (float)frame.rows);
                    out.kpts[k][2] = o.kpts[k][2];
                }
                objects.push_back(out);
            }

            // 4. JSON-ответ
            string json = detections_to_json(objects);
            uint32_t len = (uint32_t)json.size();
            uint32_t len_be = htonl(len);
            write_n(cli, &len_be, 4);
            write_n(cli, json.data(), len);
        }
        close(cli);
    }

    return 0;
}
