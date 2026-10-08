/*
 * NPU Server для YOLO11s-seg на Orange Pi 4 Pro (A733).
 * Принимает JPEG-кадры через UNIX-сокет, возвращает JSON с детекциями + масками (RLE).
 *
 * Протокол:
 *   Клиент -> Сервер: [4 байта: длина JPEG big-endian] [JPEG bytes]
 *   Сервер -> Клиент: [4 байта: длина JSON big-endian] [JSON bytes]
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

#define SOCKET_PATH "/tmp/npu_seg.sock"

struct Object {
    cv::Rect_<float> rect;
    int label;
    float prob;
    int gindex;
    cv::Mat mask;             // uint8 (0/255), размер = bbox
    std::vector<float> mask_feat;
};

extern int detect_yolo11_seg_10_post(const cv::Mat& bgr, std::vector<Object>& objects, float **output);

// Сериализация одной маски в RLE (uint8, 0/255)
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

static string detections_to_json(const vector<Object>& objects)
{
    string s = "[";
    for (size_t i = 0; i < objects.size(); i++) {
        const Object& obj = objects[i];
        if (i > 0) s += ",";

        const char* cls = (obj.label >= 0 && obj.label < (int)g_classes_name.size())
                          ? g_classes_name[obj.label].c_str() : "unknown";

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

// Letterbox (аналогично pose-серверу)
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

int main(int argc, char** argv)
{
    const char* model_file = "/dev/shm/yolo11s-seg_10_uint8_a733.nb";
    if (argc >= 2) model_file = argv[1];

    fprintf(stderr, "[npu_seg_server] Loading model: %s\n", model_file);

    NpuUint npu_uint;
    if (npu_uint.npu_init() != 0) {
        fprintf(stderr, "[npu_seg_server] npu_init failed\n");
        return -1;
    }

    NetworkItem net;
    unsigned int net_id = 0;
    if (net.network_create((char*)model_file, net_id) != 0) {
        fprintf(stderr, "[npu_seg_server] network_create failed\n");
        return -1;
    }
    if (net.network_prepare() != 0) {
        fprintf(stderr, "[npu_seg_server] network_prepare failed\n");
        return -1;
    }
    fprintf(stderr, "[npu_seg_server] Model loaded, ready.\n");

    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return -1; }
    if (listen(srv, 4) < 0) { perror("listen"); return -1; }
    fprintf(stderr, "[npu_seg_server] Listening on %s\n", SOCKET_PATH);

    void* input_buffer_ptr = nullptr;
    unsigned int input_buffer_size = 0;
    net.get_network_input_buff_info(0, &input_buffer_ptr, &input_buffer_size);
    fprintf(stderr, "[npu_seg_server] input buffer ptr=%p size=%u\n", input_buffer_ptr, input_buffer_size);

    int output_cnt = net.get_output_cnt();
    float** output_data = new float*[output_cnt]();

    while (true) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) { perror("accept"); continue; }
        fprintf(stderr, "[npu_seg_server] Client connected\n");

        while (true) {
            uint32_t jpeg_len_be;
            if (!read_n(cli, &jpeg_len_be, 4)) break;
            uint32_t jpeg_len = ntohl(jpeg_len_be);
            if (jpeg_len == 0 || jpeg_len > 10 * 1024 * 1024) break;

            vector<unsigned char> jpeg_buf(jpeg_len);
            if (!read_n(cli, jpeg_buf.data(), jpeg_len)) break;

            cv::Mat frame = cv::imdecode(jpeg_buf, cv::IMREAD_COLOR);
            if (frame.empty()) {
                fprintf(stderr, "[npu_seg_server] imdecode failed\n");
                uint32_t zero = 0;
                write_n(cli, &zero, 4);
                continue;
            }

            preprocess_mat(frame, (unsigned char*)input_buffer_ptr, LETTERBOX_ROWS, LETTERBOX_COLS);

            net.network_input_output_set();
            net.network_run();

            output_info_s outputs_info[output_cnt];
            net.get_output_nocopy(outputs_info);
            for (int t = 0; t < output_cnt; t++) {
                output_data[t] = (float*)outputs_info[t].ptr;
            }

            vector<Object> objects;
            detect_yolo11_seg_10_post(frame, objects, output_data);

            string json = detections_to_json(objects);

            uint32_t json_len_be = htonl((uint32_t)json.size());
            if (!write_n(cli, &json_len_be, 4)) break;
            if (!write_n(cli, json.data(), json.size())) break;
        }

        close(cli);
        fprintf(stderr, "[npu_seg_server] Client disconnected\n");
    }

    delete[] output_data;
    close(srv);
    unlink(SOCKET_PATH);
    return 0;
}
