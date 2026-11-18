/*
 * NPU Server для YOLO26-depth (nano) на Orange Pi 4 Pro (A733).
 * Принимает JPEG-кадры через UNIX-сокет, возвращает JSON с картой глубины.
 *
 * Формат ответа:
 *   - grid: сетка GRID_H x GRID_W средних значений глубины (метры).
 *   - near_rle: RLE-сжатая бинарная маска пикселей, где depth < NEAR_THRESHOLD.
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
#include "model_config.h"
#include "ax_model_runner/ax_model_runner_axcl.hpp"
#include <axcl.h>

using namespace std;

#define SOCKET_PATH "/tmp/npu_depth_axcl.sock"

// Параметры сетки и порога
#define GRID_W 16
#define GRID_H 16
#define NEAR_THRESHOLD 2.0f    // метры

// Letterbox (аналогично pose/seg)
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

// Обрезка letterbox padding из карты глубины (аналогично postprocess в демо)
static cv::Mat crop_letterbox_depth(const cv::Mat& depth, const cv::Size& orig)
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
    return cropped;
}

// Сетка средних значений + RLE близких пикселей
static string depth_to_json(const cv::Mat& depth_map, int orig_w, int orig_h)
{
    // depth_map: CV_32F, метры (после crop letterbox, размер ~ orig, но letterbox-соотношение)
    // 1. Ресайз до GRID (для средних)
    cv::Mat small;
    cv::resize(depth_map, small, cv::Size(GRID_W, GRID_H), 0, 0, cv::INTER_AREA);

    // 2. Формируем JSON
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

    // 3. RLE для близких пикселей (< NEAR_THRESHOLD)
    //    Используем depth_map после ресайза к оригинальному размеру кадра (например, 640x480)
    //    — чтобы избежать огромных масок 768x768.
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
            if (prev != -1 && prev == 1) {   // сохраняем только "1" (близкие)
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
    const char* model_file = "/dev/shm/yolo26n-depth_pcq_a733.nb";
    if (argc >= 2) model_file = argv[1];

    fprintf(stderr, "[npu_depth_raw_server] Loading model: %s\n", model_file);

    // AX-M1: инициализация
    if (auto ret = axclInit(0); 0 != ret) {
        fprintf(stderr, "[npu_depth_axcl_server] Init AXCL failed{0x%x}\n", ret);
        return -1;
    }
    axclrtDeviceList lst;
    if (auto ret = axclrtGetDeviceList(&lst); 0 != ret || 0 == lst.num) {
        fprintf(stderr, "[npu_depth_axcl_server] Get AXCL device failed{0x%x}\n", ret);
        return -1;
    }
    if (auto ret = axclrtSetDevice(lst.devices[0]); 0 != ret) {
        fprintf(stderr, "[npu_depth_axcl_server] Set AXCL device failed{0x%x}\n", ret);
        return -1;
    }
    if (auto ret = axclrtEngineInit(AXCL_VNPU_DISABLE); 0 != ret) {
        fprintf(stderr, "[npu_depth_axcl_server] axclrtEngineInit failed %d\n", ret);
        return ret;
    }

    // Загрузка модели
    ax_runner_axcl runner;
    if (auto ret = runner.init(model_file); ret != 0) {
        fprintf(stderr, "[npu_depth_axcl_server] runner.init failed\n");
        return -1;
    }
    fprintf(stderr, "[npu_depth_axcl_server] Model loaded, ready.\n");

    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return -1; }
    if (listen(srv, 4) < 0) { perror("listen"); return -1; }
    fprintf(stderr, "[npu_depth_raw_server] Listening on %s\n", SOCKET_PATH);

    // AX-M1: input buffer уже внутри runner (runner.get_input(0).pVirAddr)
    // output_data не нужен, получаем напрямую из runner.get_outputs_ptr(0)
    uint8_t* input_buffer_ptr = (uint8_t*)runner.get_input(0).pVirAddr;
    fprintf(stderr, "[npu_depth_axcl_server] input buffer ptr=%p\n", input_buffer_ptr);

    while (true) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) { perror("accept"); continue; }
        fprintf(stderr, "[npu_depth_raw_server] Client connected\n");

        while (true) {
            uint32_t jpeg_len_be;
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

            preprocess_mat(frame, (unsigned char*)input_buffer_ptr, INPUT_HEIGHT, INPUT_WIDTH);

            // AX-M1 инференс через ax_runner_axcl
            runner.inference();
            auto* out = runner.get_outputs_ptr(0);
            cv::Mat depth(OUTPUT_HEIGHT, OUTPUT_WIDTH, CV_32F);
            memcpy(depth.data, out->pVirAddr, OUTPUT_HEIGHT * OUTPUT_WIDTH * sizeof(float));

            // Обрезаем letterbox padding → оригинальный размер кадра
            cv::Mat depth_map = crop_letterbox_depth(depth, frame.size());

            string json = depth_to_json(depth_map, frame.cols, frame.rows);

            uint32_t json_len_be = htonl((uint32_t)json.size());
            if (!write_n(cli, &json_len_be, 4)) break;
            if (!write_n(cli, json.data(), json.size())) break;
        }

        close(cli);
        fprintf(stderr, "[npu_depth_raw_server] Client disconnected\n");
    }

    close(srv);
    unlink(SOCKET_PATH);
    return 0;
}
