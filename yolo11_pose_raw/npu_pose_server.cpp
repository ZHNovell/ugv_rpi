/*
 * NPU Server для YOLO11s на Orange Pi 4 Pro (A733).
 * Держит модель загруженной в NPU, принимает JPEG-кадры через UNIX-сокет,
 * возвращает JSON со списком детекций.
 *
 * Протокол:
 *   Клиент -> Сервер: [4 байта: длина JPEG big-endian] [JPEG bytes]
 *   Сервер -> Клиент: [4 байта: длина JSON big-endian] [JSON bytes]
 *
 * JSON формат: [{"class_id": 16, "class": "dog", "confidence": 0.92, "bbox": [x0,y0,x1,y1]}, ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <arpa/inet.h>
#include <signal.h>
#include <vector>
#include <string>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include "npulib.h"
#include "model_config.h"

using namespace std;

#define SOCKET_PATH "/tmp/npu_pose_raw.sock"

// Внешние функции из yolo11_6_post.cpp и yolo11_6_pre.cpp
struct KeyPoint {
    cv::Point2f p;
    float prob;
};

struct Object {
    cv::Rect_<float> rect;
    int label;
    float prob;
    std::vector<KeyPoint> keypoints;
};
extern int detect_yolo11_pose_9_post(const cv::Mat& bgr, std::vector<Object>& objects, float **output);

// Класс для пересылки
struct Detection {
    int class_id;
    string class_name;
    float confidence;
    float x0, y0, x1, y1;
    vector<pair<float,float>> keypoints;   // (x, y)
    vector<float> kp_probs;                // confidences
};

// COCO классы
static const char* COCO_CLASSES[] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light",
    "fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow",
    "elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
    "skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard",
    "tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
    "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch",
    "potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone",
    "microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear",
    "hair drier","toothbrush"
};

// Заполнение input-буфера из cv::Mat (letterbox)
static void preprocess_mat(const cv::Mat& sample, unsigned char* input_data, int letterbox_rows, int letterbox_cols)
{
    cv::Mat img;
    cv::cvtColor(sample, img, cv::COLOR_BGR2RGB);

    float scale_letterbox;
    if ((letterbox_rows * 1.0 / img.rows) < (letterbox_cols * 1.0 / img.cols))
        scale_letterbox = letterbox_rows * 1.0 / img.rows;
    else
        scale_letterbox = letterbox_cols * 1.0 / img.cols;

    int resize_cols = int(round(scale_letterbox * img.cols));
    int resize_rows = int(round(scale_letterbox * img.rows));

    float dh = (float)(letterbox_rows - resize_rows) / 2.0f;
    float dw = (float)(letterbox_cols - resize_cols) / 2.0f;

    cv::resize(img, img, cv::Size(resize_cols, resize_rows));

    cv::Mat img_new(letterbox_rows, letterbox_cols, CV_8UC3, input_data);
    int top = (int)(round(dh - 0.1)), bot = (int)(round(dh + 0.1));
    int left = (int)(round(dw - 0.1)), right = (int)(round(dw + 0.1));

    cv::copyMakeBorder(img, img_new, top, bot, left, right, cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));
}

// Формирование JSON из detections
static string detections_to_json(const vector<Detection>& dets)
{
    string s = "[";
    for (size_t i = 0; i < dets.size(); i++) {
        if (i > 0) s += ",";
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"class_id\":%d,\"class\":\"%s\",\"confidence\":%.4f,\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"keypoints\":[",
            dets[i].class_id, dets[i].class_name.c_str(), dets[i].confidence,
            dets[i].x0, dets[i].y0, dets[i].x1, dets[i].y1);
        s += buf;
        for (size_t k = 0; k < dets[i].keypoints.size(); k++) {
            if (k > 0) s += ",";
            char kb[64];
            snprintf(kb, sizeof(kb), "[%.1f,%.1f,%.3f]",
                dets[i].keypoints[k].first, dets[i].keypoints[k].second,
                dets[i].kp_probs[k]);
            s += kb;
        }
        s += "]}";
    }
    s += "]";
    return s;
}

// Чтение N байт из сокета
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

// Запись N байт в сокет
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
    const char* model_file = "/dev/shm/yolo11s_6_uint8_a733.nb";
    if (argc >= 2) model_file = argv[1];

    fprintf(stderr, "[npu_pose_raw_server] Loading model: %s\n", model_file);

    // 1. NPU init
    NpuUint npu_uint;
    if (npu_uint.npu_init() != 0) {
        fprintf(stderr, "[npu_pose_raw_server] npu_init failed\n");
        return -1;
    }

    // 2. Network create & prepare
    NetworkItem net;
    unsigned int net_id = 0;
    if (net.network_create((char*)model_file, net_id) != 0) {
        fprintf(stderr, "[npu_pose_raw_server] network_create failed\n");
        return -1;
    }
    if (net.network_prepare() != 0) {
        fprintf(stderr, "[npu_pose_raw_server] network_prepare failed\n");
        return -1;
    }
    fprintf(stderr, "[npu_pose_raw_server] Model loaded, ready.\n");

    // 3. Создаём UNIX-сокет
    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return -1; }
    if (listen(srv, 4) < 0) { perror("listen"); return -1; }
    fprintf(stderr, "[npu_pose_raw_server] Listening on %s\n", SOCKET_PATH);

    // Получаем указатель на input-буфер
    void* input_buffer_ptr = nullptr;
    unsigned int input_buffer_size = 0;
    net.get_network_input_buff_info(0, &input_buffer_ptr, &input_buffer_size);
    fprintf(stderr, "[npu_pose_raw_server] input buffer ptr=%p size=%u\n", input_buffer_ptr, input_buffer_size);

    int output_cnt = net.get_output_cnt();
    float** output_data = new float*[output_cnt]();

    // 4. Главный цикл
    while (true) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) { perror("accept"); continue; }
        fprintf(stderr, "[npu_pose_raw_server] Client connected\n");

        while (true) {
            // Читаем заголовок raw BGR: width, height, channels (по 4 байта BE)
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

            // Preprocess: Mat -> input_buffer
            preprocess_mat(frame, (unsigned char*)input_buffer_ptr, LETTERBOX_ROWS, LETTERBOX_COLS);

            // Run network
            net.network_input_output_set();
            net.network_run();

            // Получаем output
            output_info_s outputs_info[output_cnt];
            net.get_output_nocopy(outputs_info);
            for (int t = 0; t < output_cnt; t++) {
                output_data[t] = (float*)outputs_info[t].ptr;
            }

            // Postprocess -> detections
            vector<Object> objects;
            detect_yolo11_pose_9_post(frame, objects, output_data);

            // Конвертим в JSON
            vector<Detection> dets;
            for (const auto& obj : objects) {
                Detection d;
                d.class_id = obj.label;
                d.class_name = (obj.label == 0) ? "person" : "unknown";
                d.confidence = obj.prob;
                d.x0 = obj.rect.x;
                d.y0 = obj.rect.y;
                d.x1 = obj.rect.x + obj.rect.width;
                d.y1 = obj.rect.y + obj.rect.height;
                for (const auto& kp : obj.keypoints) {
                    d.keypoints.push_back({kp.p.x, kp.p.y});
                    d.kp_probs.push_back(kp.prob);
                }
                dets.push_back(d);
            }
            string json = detections_to_json(dets);

            // Отправляем длину JSON + JSON
            uint32_t json_len_be = htonl((uint32_t)json.size());
            if (!write_n(cli, &json_len_be, 4)) break;
            if (!write_n(cli, json.data(), json.size())) break;
        }

        close(cli);
        fprintf(stderr, "[npu_pose_raw_server] Client disconnected\n");
    }

    delete[] output_data;
    close(srv);
    unlink(SOCKET_PATH);
    return 0;
}
