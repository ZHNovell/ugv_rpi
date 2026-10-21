// npu_seg_server_final — seg через NetworkItem (как в оригинале) + приём fd
// Используем npulib.h напрямую — то же, что в оригинальном рабочем сервере.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <opencv2/opencv.hpp>

#include "npulib.h"
#include "model_config_seg.h"

#define SOCKET_PATH "/tmp/npu_seg_fd.sock"
#define INPUT_W 640
#define INPUT_H 640

struct SegObject {
    cv::Rect_<float> rect;
    int label;
    float prob;
    int gindex;
    cv::Mat mask;
    std::vector<float> mask_feat;
};

extern int detect_seg_10_post(const cv::Mat& bgr, std::vector<SegObject>& objects, float **output);

static NetworkItem g_net;
static void *g_input_ptr = nullptr;
static unsigned int g_input_size = 0;
static int g_output_cnt = 0;
static float **g_output_data = nullptr;

// === RLE для маски ===
static std::string mask_to_rle(const cv::Mat& mask) {
    if (mask.empty()) return "[]";
    std::string s = "[";
    int prev = -1, count = 0;
    const uchar* p = mask.ptr<uchar>();
    int total = (int)mask.total();
    for (int i = 0; i < total; i++) {
        int v = (p[i] > 127) ? 1 : 0;
        if (v == prev) count++;
        else {
            if (prev != -1) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%s[%d,%d]", (s.size() > 1) ? "," : "", prev, count);
                s += buf;
            }
            prev = v; count = 1;
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

static std::string detections_to_json(const std::vector<SegObject>& objects) {
    std::string s = "[";
    for (size_t i = 0; i < objects.size(); i++) {
        const SegObject& obj = objects[i];
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

static int recv_fd(int sock, int *out_fd) {
    char buf[1];
    struct iovec io = { .iov_base = buf, .iov_len = 1 };
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf; msg.msg_controllen = sizeof(cmsg_buf);
    if (recvmsg(sock, &msg, 0) <= 0) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;
    memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
    return 0;
}

static bool write_n(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n > 0) { ssize_t w = write(fd, p, n); if (w <= 0) return false; p += w; n -= w; }
    return true;
}

// Letterbox: BGR → RGB → input_ptr (как в оригинале)
static void preprocess_mat(const cv::Mat& sample, unsigned char* input_data, int lb_rows, int lb_cols) {
    cv::Mat img;
    cv::cvtColor(sample, img, cv::COLOR_BGR2RGB);
    float scale;
    if ((lb_rows * 1.0 / img.rows) < (lb_cols * 1.0 / img.cols))
        scale = lb_rows * 1.0 / img.rows;
    else
        scale = lb_cols * 1.0 / img.cols;
    int rc = int(round(scale * img.cols));
    int rr = int(round(scale * img.rows));
    float dh = (float)(lb_rows - rr) / 2.0f;
    float dw = (float)(lb_cols - rc) / 2.0f;
    cv::resize(img, img, cv::Size(rc, rr));
    cv::Mat img_new(lb_rows, lb_cols, CV_8UC3, input_data);
    int top = (int)(round(dh - 0.1)), bot = (int)(round(dh + 0.1));
    int left = (int)(round(dw - 0.1)), right = (int)(round(dw + 0.1));
    cv::copyMakeBorder(img, img_new, top, bot, left, right, cv::BORDER_CONSTANT, cv::Scalar(114,114,114));
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "=== NPU Seg Server (NetworkItem + fd) ===\n");

    const char* model_file = "/dev/shm/yolo11s-seg_10_uint8_a733.nb";
    if (argc >= 2) model_file = argv[1];

    NpuUint npu_uint;
    if (npu_uint.npu_init() != 0) { fprintf(stderr, "npu_init failed\n"); return -1; }

    unsigned int net_id = 0;
    if (g_net.network_create((char*)model_file, net_id) != 0) {
        fprintf(stderr, "network_create failed\n"); return -1;
    }
    if (g_net.network_prepare() != 0) {
        fprintf(stderr, "network_prepare failed\n"); return -1;
    }
    g_net.get_network_input_buff_info(0, &g_input_ptr, &g_input_size);
    fprintf(stderr, "input buffer ptr=%p size=%u\n", g_input_ptr, g_input_size);

    g_output_cnt = g_net.get_output_cnt();
    g_output_data = new float*[g_output_cnt]();

    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    bind(srv, (struct sockaddr*)&addr, sizeof(addr));
    listen(srv, 4);
    fprintf(stderr, "Listening on %s\n", SOCKET_PATH);

    while (true) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) continue;
        fprintf(stderr, "Client connected\n");

        while (true) {
            // Принимаем fd
            int fd = -1;
            if (recv_fd(cli, &fd) < 0) break;

            size_t bgr_size = INPUT_W * INPUT_H * 3;
            void *ptr = mmap(NULL, bgr_size, PROT_READ, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED) { perror("mmap"); close(fd); break; }

            cv::Mat frame(INPUT_H, INPUT_W, CV_8UC3, ptr);

            // preprocess → input_ptr
            preprocess_mat(frame, (unsigned char*)g_input_ptr, LETTERBOX_ROWS, LETTERBOX_COLS);

            // network_input_output_set + run (как оригинал)
            g_net.network_input_output_set();
            g_net.network_run();

            output_info_s outputs_info[g_output_cnt];
            g_net.get_output_nocopy(outputs_info);
            for (int t = 0; t < g_output_cnt; t++) {
                g_output_data[t] = (float*)outputs_info[t].ptr;
            }

            std::vector<SegObject> objects;
            detect_seg_10_post(frame, objects, g_output_data);

            std::string json = detections_to_json(objects);

            uint32_t json_len_be = htonl((uint32_t)json.size());
            write_n(cli, &json_len_be, 4);
            write_n(cli, json.data(), json.size());

            munmap(ptr, bgr_size);
            close(fd);
        }

        close(cli);
        fprintf(stderr, "Client disconnected\n");
    }
    return 0;
}
