/*
 * NPU Server для YOLO26-seg на AX-M1 (AXCL).
 * Принимает RAW BGR, возвращает JSON: детекции + RLE-маски (bbox-размера).
 *
 * Протокол:
 *   Клиент -> Сервер: [4B width BE][4B height BE][4B channels BE][RAW BGR]
 *   Сервер -> Клиент: [4B длина JSON BE][JSON]
 *
 * Постпроцесс портирован из ax_infer.py (YOLO26-seg): ltrb-декодирование,
 * 10 выходов (9 детекционных + 1 proto), left-top letterbox, порог 0.5 на маске.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <arpa/inet.h>
#include <vector>
#include <string>
#include <float.h>
#include <algorithm>
#include <opencv2/opencv.hpp>
#include "ax_model_runner/ax_model_runner_axcl.hpp"
#include <axcl.h>

using namespace std;

#define SOCKET_PATH "/tmp/npu_seg_axcl.sock"
#define INPUT_W 640
#define INPUT_H 640
#define NUM_CLASS 80
#define SCORE_THRES 0.25f
#define NMS_THRES 0.7f
#define MASK_PROTO_DIM 32

const char* CLASS_NAMES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"};

struct SegObj {
    cv::Rect_<float> rect;   // xyxy в координатах модели
    int label;
    float prob;
    vector<float> coeff;     // 32 коэффициента маски
    cv::Mat mask;            // бинарная маска (0/255), bbox-размер
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
static vector<int> nms_boxes(const vector<SegObj>& objs, float thres)
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

// Полная маска (orig-размер), портирована из process_mask ax_infer.py
static cv::Mat compute_mask(const float* proto, int dim, int proto_h, int proto_w,
                            const vector<float>& coeff, const cv::Rect_<float>& box_model,
                            int model_h, int model_w, const cv::Size& orig, float scale)
{
    // 1. coeffs @ proto -> (proto_h, proto_w)
    cv::Mat mask(proto_h, proto_w, CV_32F);
    for (int py = 0; py < proto_h; py++) {
        for (int px = 0; px < proto_w; px++) {
            float v = 0;
            for (int c = 0; c < dim; c++) {
                v += coeff[c] * proto[c * proto_h * proto_w + py * proto_w + px];
            }
            mask.at<float>(py, px) = v;
        }
    }

    // 2. Обнуляем вне bbox (proto-координаты)
    float wr = (float)proto_w / model_w;
    float hr = (float)proto_h / model_h;
    int bx1 = std::min(std::max((int)(box_model.x * wr), 0), proto_w);
    int by1 = std::min(std::max((int)(box_model.y * hr), 0), proto_h);
    int bx2 = std::min(std::max((int)((box_model.x + box_model.width) * wr), 0), proto_w);
    int by2 = std::min(std::max((int)((box_model.y + box_model.height) * hr), 0), proto_h);

    cv::Mat cropped = cv::Mat::zeros(proto_h, proto_w, CV_32F);
    if (bx2 > bx1 && by2 > by1) {
        mask(cv::Rect(bx1, by1, bx2 - bx1, by2 - by1)).copyTo(cropped(cv::Rect(bx1, by1, bx2 - bx1, by2 - by1)));
    }

    // 3. Upsample до model-размера
    cv::Mat up;
    cv::resize(cropped, up, cv::Size(model_w, model_h), 0, 0, cv::INTER_LINEAR);

    // 4. Кроп валидной области (left-top aligned)
    int new_h = std::min(std::max((int)(orig.height * scale), 1), model_h);
    int new_w = std::min(std::max((int)(orig.width * scale), 1), model_w);
    cv::Mat valid = up(cv::Rect(0, 0, new_w, new_h));

    // 5. Resize до orig-размера
    cv::Mat full;
    cv::resize(valid, full, orig, 0, 0, cv::INTER_LINEAR);

    // 6. Порог 0.5 -> бинарная (0/255)
    cv::Mat bin;
    cv::threshold(full, bin, 0.5, 255, cv::THRESH_BINARY);
    bin.convertTo(bin, CV_8U);
    return bin;
}

static string mask_to_rle(const cv::Mat& mask)
{
    if (mask.empty()) return "[]";
    string s = "[";
    int prev = -1;
    int count = 0;
    const uchar* p = mask.ptr<uchar>();
    int total = (int)mask.total();
    for (int i = 0; i < total; i++) {
        int v = (p[i] > 127) ? 1 : 0;
        if (v == prev) {
            count++;
        } else {
            if (prev != -1) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%s[%d,%d]", (s.size() > 1) ? "," : "", prev, count);
                s += buf;
            }
            prev = v;
            count = 1;
        }
    }
    if (prev != -1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s[%d,%d]", (s.size() > 1) ? "," : "", prev, count);
        s += buf;
    }
    s += "]";
    return s;
}

static string detections_to_json(const vector<SegObj>& objects)
{
    string s = "[";
    for (size_t i = 0; i < objects.size(); i++) {
        const SegObj& obj = objects[i];
        if (i > 0) s += ",";

        const char* cls = (obj.label >= 0 && obj.label < NUM_CLASS) ? CLASS_NAMES[obj.label] : "unknown";

        int x0 = (int)obj.rect.x;
        int y0 = (int)obj.rect.y;
        int x1 = (int)(obj.rect.x + obj.rect.width);
        int y1 = (int)(obj.rect.y + obj.rect.height);

        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"class_id\":%d,\"class\":\"%s\",\"confidence\":%.4f,"
            "\"bbox\":[%d,%d,%d,%d],\"mask_size\":[%d,%d],\"mask_rle\":",
            obj.label, cls, obj.prob, x0, y0, x1, y1,
            obj.mask.cols, obj.mask.rows);
        s += buf;
        s += mask_to_rle(obj.mask);
        s += "}";
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
    const char* model_file = "/root/ugv_rpi/models_axcl/yolo26m-seg.axmodel";
    if (argc >= 2) model_file = argv[1];

    fprintf(stderr, "[npu_seg_axcl_server] Loading model: %s\n", model_file);

    if (auto ret = axclInit(0); 0 != ret) {
        fprintf(stderr, "[npu_seg_axcl_server] Init AXCL failed{0x%x}\n", ret);
        return -1;
    }
    axclrtDeviceList lst;
    if (auto ret = axclrtGetDeviceList(&lst); 0 != ret || 0 == lst.num) {
        fprintf(stderr, "[npu_seg_axcl_server] Get AXCL device failed\n");
        return -1;
    }
    if (auto ret = axclrtSetDevice(lst.devices[0]); 0 != ret) {
        fprintf(stderr, "[npu_seg_axcl_server] Set AXCL device failed\n");
        return -1;
    }
    if (auto ret = axclrtEngineInit(AXCL_VNPU_DISABLE); 0 != ret) {
        fprintf(stderr, "[npu_seg_axcl_server] axclrtEngineInit failed %d\n", ret);
        return ret;
    }

    ax_runner_axcl runner;
    if (auto ret = runner.init(model_file); ret != 0) {
        fprintf(stderr, "[npu_seg_axcl_server] runner.init failed\n");
        return -1;
    }
    fprintf(stderr, "[npu_seg_axcl_server] Model loaded, ready.\n");

    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return -1; }
    if (listen(srv, 4) < 0) { perror("listen"); return -1; }
    fprintf(stderr, "[npu_seg_axcl_server] Listening on %s\n", SOCKET_PATH);

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
                fprintf(stderr, "[npu_seg_axcl_server] inference failed\n");
                continue;
            }

            auto* output = runner.get_outputs_ptr(0);
            int nOut = runner.get_num_outputs();
            if (nOut < 10) {
                fprintf(stderr, "[npu_seg_axcl_server] unexpected outputs: %d\n", nOut);
                continue;
            }

            // 1. Декодирование (ltrb) по 3 масштабам
            vector<SegObj> proposals;
            float conf_raw = -logf(1.0f / SCORE_THRES - 1.0f);

            for (int s = 0; s < 3; s++) {
                int stride = 8 << s;               // 8, 16, 32
                int grid = INPUT_W / stride;       // 80, 40, 20
                int bi = s * 3, ci = s * 3 + 1, mi = s * 3 + 2;

                float* box_data = (float*)output[bi].pVirAddr;   // (grid,grid,4)
                float* cls_data = (float*)output[ci].pVirAddr;   // (grid,grid,80)
                float* msk_data = (float*)output[mi].pVirAddr;   // (grid,grid,32)

                int HW = grid * grid;
                for (int i = 0; i < HW; i++) {
                    float maxs = -FLT_MAX;
                    int mid = 0;
                    for (int c = 0; c < NUM_CLASS; c++) {
                        float sc = cls_data[i * NUM_CLASS + c];
                        if (sc > maxs) { maxs = sc; mid = c; }
                    }
                    if (maxs < conf_raw) continue;

                    float prob = 1.0f / (1.0f + expf(-maxs));

                    int gy = i / grid, gx = i % grid;
                    float ax = gx + 0.5f, ay = gy + 0.5f;

                    float x0 = (ax - box_data[i * 4 + 0]) * stride;
                    float y0 = (ay - box_data[i * 4 + 1]) * stride;
                    float x1 = (ax + box_data[i * 4 + 2]) * stride;
                    float y1 = (ay + box_data[i * 4 + 3]) * stride;

                    SegObj o;
                    o.rect = cv::Rect_<float>(x0, y0, x1 - x0, y1 - y0);
                    o.label = mid;
                    o.prob = prob;
                    o.coeff.assign(msk_data + i * MASK_PROTO_DIM, msk_data + i * MASK_PROTO_DIM + MASK_PROTO_DIM);
                    proposals.push_back(o);
                }
            }

            // 2. NMS
            vector<int> picked = nms_boxes(proposals, NMS_THRES);

            // 3. Масштаб и маски
            float scale = std::min((float)INPUT_H / frame.rows, (float)INPUT_W / frame.cols);
            float* proto = (float*)output[9].pVirAddr;   // (32,160,160)
            int proto_h = INPUT_H / 4, proto_w = INPUT_W / 4;

            vector<SegObj> objects;
            for (int idx : picked) {
                SegObj& o = proposals[idx];

                // Бокс в orig-координатах
                float x0 = std::min(std::max(o.rect.x / scale, 0.0f), (float)frame.cols);
                float y0 = std::min(std::max(o.rect.y / scale, 0.0f), (float)frame.rows);
                float x1 = std::min(std::max((o.rect.x + o.rect.width) / scale, 0.0f), (float)frame.cols);
                float y1 = std::min(std::max((o.rect.y + o.rect.height) / scale, 0.0f), (float)frame.rows);

                SegObj out;
                out.rect = cv::Rect_<float>(x0, y0, x1 - x0, y1 - y0);
                out.label = o.label;
                out.prob = o.prob;

                // Полная маска (orig-размер) -> кроп до bbox
                cv::Mat full = compute_mask(proto, MASK_PROTO_DIM, proto_h, proto_w,
                                            o.coeff, o.rect, INPUT_H, INPUT_W,
                                            frame.size(), scale);

                int bx = (int)x0, by = (int)y0;
                int bw = (int)std::max(1.0f, x1 - x0);
                int bh = (int)std::max(1.0f, y1 - y0);
                bx = std::min(std::max(bx, 0), frame.cols - 1);
                by = std::min(std::max(by, 0), frame.rows - 1);
                int ex = std::min(bx + bw, frame.cols);
                int ey = std::min(by + bh, frame.rows);
                if (ex > bx && ey > by) {
                    out.mask = full(cv::Rect(bx, by, ex - bx, ey - by)).clone();
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
