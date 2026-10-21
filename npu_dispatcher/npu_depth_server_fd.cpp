// npu_depth_server_fd — YOLO26n_depth сервер с приёмом fd dma-buf через SCM_RIGHTS

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
#include <vip_lite.h>

#define SOCKET_PATH "/tmp/npu_depth_fd.sock"
#define ALIGN 64
#define INPUT_W 768
#define INPUT_H 768

#define GRID_W 16
#define GRID_H 16
#define NEAR_THRESHOLD 2.0f

static vip_network g_network = NULL;
static vip_buffer g_output_bufs[12];
static int g_num_outputs = 0;

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
    const char *p = (const char*)buf;
    while (n > 0) { ssize_t r = write(fd, p, n); if (r <= 0) return false; p += r; n -= r; }
    return true;
}

static int load_model(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return -1; }
    fseek(f, 0, SEEK_END); size_t sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *data = malloc(sz); fread(data, 1, sz, f); fclose(f);
    printf("Model loaded: %zu bytes\n", sz);
    if (vip_create_network(data, sz, VIP_CREATE_NETWORK_FROM_MEMORY, &g_network) != VIP_SUCCESS) return -1;
    if (vip_prepare_network(g_network) != VIP_SUCCESS) return -1;
    vip_query_network(g_network, VIP_NETWORK_PROP_OUTPUT_COUNT, &g_num_outputs);
    printf("Outputs: %d\n", g_num_outputs);
    for (int i = 0; i < g_num_outputs; i++) {
        vip_buffer_create_params_t op = {0};
        vip_query_output(g_network, i, VIP_BUFFER_PROP_DATA_FORMAT, &op.data_format);
        vip_query_output(g_network, i, VIP_BUFFER_PROP_QUANT_FORMAT, &op.quant_format);
        vip_query_output(g_network, i, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &op.num_of_dims);
        vip_query_output(g_network, i, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, op.sizes);
        op.memory_type = VIP_BUFFER_MEMORY_TYPE_DEFAULT;
        vip_create_buffer(&op, sizeof(op), &g_output_bufs[i]);
        vip_set_output(g_network, i, g_output_bufs[i]);
    }
    return 0;
}

// Crop + resize depth
static cv::Mat crop_resize_depth(const cv::Mat& depth, const cv::Size& orig) {
    int im1_h = depth.rows, im1_w = depth.cols;
    int im0_h = orig.height, im0_w = orig.width;
    if (im1_h == im0_h && im1_w == im0_w) return depth.clone();
    float gain = std::min((float)im1_h / im0_h, (float)im1_w / im0_w);
    float pad_w = (im1_w - std::round(im0_w * gain)) / 2.0f;
    float pad_h = (im1_h - std::round(im0_h * gain)) / 2.0f;
    int top = (int)std::round(pad_h - 0.1f);
    int left = (int)std::round(pad_w - 0.1f);
    int bottom = im1_h - (int)std::round(pad_h + 0.1f);
    int right = im1_w - (int)std::round(pad_w + 0.1f);
    cv::Mat cropped = depth(cv::Rect(left, top, right - left, bottom - top)).clone();
    cv::Mat resized;
    cv::resize(cropped, resized, cv::Size(im0_w, im0_h), 0, 0, cv::INTER_LINEAR);
    return resized;
}

static std::string depth_to_json(const cv::Mat& depth_map, int orig_w, int orig_h) {
    cv::Mat small;
    cv::resize(depth_map, small, cv::Size(GRID_W, GRID_H), 0, 0, cv::INTER_AREA);

    std::string s = "{\"grid_size\":[";
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
        if (v > 0 && v < NEAR_THRESHOLD) mp[i] = 1;
    }

    snprintf(buf, sizeof(buf), "\"near_threshold\":%.2f,\"near_mask_size\":[%d,%d],\"near_rle\":[",
             NEAR_THRESHOLD, near_mask.cols, near_mask.rows);
    s += buf;

    int prev = -1, count = 0;
    bool first = true;
    for (int i = 0; i < total; i++) {
        int v = mp[i];
        if (v == prev) { count++; }
        else {
            if (prev != -1 && prev == 1) {
                snprintf(buf, sizeof(buf), "%s[%d,%d]", first ? "" : ",", prev, count);
                s += buf; first = false;
            }
            prev = v; count = 1;
        }
    }
    if (prev == 1) {
        snprintf(buf, sizeof(buf), "%s[%d,%d]", first ? "" : ",", prev, count);
        s += buf;
    }
    s += "]}";
    return s;
}

static int handle_client(int cli) {
    int frame_count = 0;
    double sum_npu = 0, sum_post = 0;

    while (true) {
        int fd = -1;
        if (recv_fd(cli, &fd) < 0) break;

        // Для depth: BGR 768×768×3 (буфер от диспетчера)
        size_t bgr_size = INPUT_W * INPUT_H * 3;
        void *ptr = mmap(NULL, bgr_size, PROT_READ, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) { perror("mmap"); close(fd); break; }

        vip_buffer_create_params_t ip = {0};
        vip_query_input(g_network, 0, VIP_BUFFER_PROP_DATA_FORMAT, &ip.data_format);
        vip_query_input(g_network, 0, VIP_BUFFER_PROP_QUANT_FORMAT, &ip.quant_format);
        vip_query_input(g_network, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &ip.num_of_dims);
        vip_query_input(g_network, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, ip.sizes);
        ip.memory_type = VIP_BUFFER_MEMORY_TYPE_HOST;
        size_t in_size = 1;
        for (int i = 0; i < ip.num_of_dims; i++) in_size *= ip.sizes[i];
        in_size = (in_size + ALIGN - 1) & ~(ALIGN - 1);

        auto tn1 = std::chrono::high_resolution_clock::now();
        vip_buffer input_buf = NULL;
        vip_create_buffer_from_fd(&ip, fd, in_size, &input_buf);
        vip_set_input(g_network, 0, input_buf);
        vip_run_network(g_network);
        vip_destroy_buffer(input_buf);
        auto tn2 = std::chrono::high_resolution_clock::now();

        // Depth: единственный выход 768×768 float32
        float* out = (float*)vip_map_buffer(g_output_bufs[0]);
        cv::Mat depth_npu(768, 768, CV_32F);
        memcpy(depth_npu.data, out, 768 * 768 * sizeof(float));
        vip_unmap_buffer(g_output_bufs[0]);

        auto tp1 = std::chrono::high_resolution_clock::now();
        cv::Mat depth_map = crop_resize_depth(depth_npu, cv::Size(INPUT_W, INPUT_H));
        std::string json = depth_to_json(depth_map, INPUT_W, INPUT_H);
        auto tp2 = std::chrono::high_resolution_clock::now();

        uint32_t json_len_be = htonl((uint32_t)json.size());
        write_n(cli, &json_len_be, 4);
        write_n(cli, json.data(), json.size());

        double npu_ms = std::chrono::duration<double, std::milli>(tn2 - tn1).count();
        double post_ms = std::chrono::duration<double, std::milli>(tp2 - tp1).count();
        sum_npu += npu_ms; sum_post += post_ms;
        frame_count++;

        if (frame_count % 20 == 0)
            printf("Frame %d: npu=%.2f post=%.2f json_len=%zu\n", frame_count, npu_ms, post_ms, json.size());

        munmap(ptr, bgr_size);
        close(fd);
    }

    printf("Client done: %d frames, avg npu=%.2f, avg post=%.2f\n",
           frame_count, sum_npu / std::max(1, frame_count), sum_post / std::max(1, frame_count));
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("=== NPU Depth Server (fd) ===\n");

    vip_init();
    if (load_model("/dev/shm/yolo26n-depth_pcq_a733.nb") < 0) return 1;

    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);
    bind(srv, (struct sockaddr*)&addr, sizeof(addr));
    listen(srv, 1);
    printf("Listening on %s\n", SOCKET_PATH);

    while (true) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) continue;
        printf("Client connected\n");
        handle_client(cli);
        close(cli);
        printf("Client disconnected\n");
        break;
    }

    close(srv);
    printf("DONE\n");
    return 0;
}
