// npu_seg_server_fd2 — seg с копированием dma-buf → внутренний буфер NPU
// (потому что seg не работает с _from_fd)

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

#define SOCKET_PATH "/tmp/npu_seg_fd.sock"
#define ALIGN 64
#define INPUT_W 640
#define INPUT_H 640

struct SegObject {
    cv::Rect_<float> rect;
    int label;
    float prob;
    cv::Mat mask;
};

extern int detect_seg_10_post(const cv::Mat& bgr, std::vector<SegObject>& objects, float **output);

static vip_network g_network = NULL;
static vip_buffer g_output_bufs[12];
static int g_num_outputs = 0;
static vip_buffer g_input_buf = NULL;   // ВНУТРЕННИЙ буфер NPU

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

    // Устанавливаем таймаут (20 сек)
    int timeout_ms = 20000;
    if (vip_set_network(g_network, VIP_NETWORK_PROP_SET_TIME_OUT, &timeout_ms) == VIP_SUCCESS)
        printf("Set network timeout: %d ms\n", timeout_ms);

    // 1) СНАЧАЛА создаём буферы (БЕЗ set_input/output)
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
    }

    vip_buffer_create_params_t ip = {0};
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_DATA_FORMAT, &ip.data_format);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_QUANT_FORMAT, &ip.quant_format);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &ip.num_of_dims);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, ip.sizes);
    ip.memory_type = VIP_BUFFER_MEMORY_TYPE_DEFAULT;
    if (vip_create_buffer(&ip, sizeof(ip), &g_input_buf) != VIP_SUCCESS) {
        printf("create input buffer failed\n"); return -1;
    }

    // 2) prepare ПОСЛЕ создания буферов
    if (vip_prepare_network(g_network) != VIP_SUCCESS) {
        printf("prepare_network failed\n"); return -1;
    }

    printf("Buffers created, network prepared\n");
    return 0;
}

static int handle_client(int cli) {
    int frame_count = 0;
    double sum_npu = 0, sum_post = 0;

    while (true) {
        fprintf(stderr, "[DBG] waiting for fd\n");
        int fd = -1;
        if (recv_fd(cli, &fd) < 0) { fprintf(stderr, "[DBG] recv_fd failed\n"); break; }
        fprintf(stderr, "[DBG] got fd=%d\n", fd);

        size_t bgr_size = INPUT_W * INPUT_H * 3;
        void *ptr = mmap(NULL, bgr_size, PROT_READ, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) { perror("mmap"); close(fd); break; }
        fprintf(stderr, "[DBG] mmap ok ptr=%p\n", ptr);

        // Копируем BGR из dma-buf во ВНУТРЕННИЙ буфер NPU
        fprintf(stderr, "[DBG] before vip_map_buffer\n");
        void *npu_in = vip_map_buffer(g_input_buf);
        fprintf(stderr, "[DBG] npu_in=%p\n", npu_in);
        if (npu_in) memcpy(npu_in, ptr, bgr_size);
        fprintf(stderr, "[DBG] memcpy done\n");

        // set_input/output КАЖДЫЙ РАЗ (как в оригинале network_input_output_set)
        vip_set_input(g_network, 0, g_input_buf);
        for (int i = 0; i < g_num_outputs; i++) {
            vip_set_output(g_network, i, g_output_bufs[i]);
        }

        // FLUSH input cache
        vip_flush_buffer(g_input_buf, VIP_BUFFER_OPER_TYPE_FLUSH);

        auto tn1 = std::chrono::high_resolution_clock::now();
        vip_run_network(g_network);
        auto tn2 = std::chrono::high_resolution_clock::now();

        // INVALIDATE output cache
        for (int i = 0; i < g_num_outputs; i++) {
            vip_flush_buffer(g_output_bufs[i], VIP_BUFFER_OPER_TYPE_INVALIDATE);
        }

        float* outputs[12];
        for (int i = 0; i < g_num_outputs; i++) outputs[i] = (float*)vip_map_buffer(g_output_bufs[i]);

        // cv::Mat для post
        cv::Mat bgr(INPUT_H, INPUT_W, CV_8UC3, ptr);

        auto tp1 = std::chrono::high_resolution_clock::now();
        std::vector<SegObject> segs;
        detect_seg_10_post(bgr, segs, outputs);
        auto tp2 = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < g_num_outputs; i++) vip_unmap_buffer(g_output_bufs[i]);

        // Ограничиваем количество детекций (защита от мусора)
        if (segs.size() > 100) {
            printf("[WARN] too many segs: %zu, truncating to 100\n", segs.size());
            segs.resize(100);
        }

        // JSON (только валидные: prob > 0.3, label 0..79)
        char json[65536];
        int off = snprintf(json, sizeof(json), "[");
        int valid = 0;
        for (size_t i = 0; i < segs.size() && off < (int)sizeof(json) - 200; i++) {
            const auto& s = segs[i];
            if (s.label < 0 || s.label > 79) continue;
            if (s.prob < 0.3f || s.prob > 1.0f) continue;
            if (s.rect.x < 0 || s.rect.x > 640 || s.rect.y < 0 || s.rect.y > 640) continue;
            valid++;
            off += snprintf(json + off, sizeof(json) - off,
                "%s{\"class_id\":%d,\"confidence\":%.3f,\"bbox\":[%.1f,%.1f,%.1f,%.1f]}",
                valid > 1 ? "," : "", s.label, s.prob,
                s.rect.x, s.rect.y, s.rect.x+s.rect.width, s.rect.y+s.rect.height);
        }
        snprintf(json + off, sizeof(json) - off, "]");

        uint32_t json_len_be = htonl((uint32_t)strlen(json));
        write_n(cli, &json_len_be, 4);
        write_n(cli, json, strlen(json));

        double npu_ms = std::chrono::duration<double, std::milli>(tn2 - tn1).count();
        double post_ms = std::chrono::duration<double, std::milli>(tp2 - tp1).count();
        sum_npu += npu_ms; sum_post += post_ms;
        frame_count++;

        if (frame_count % 10 == 0)
            printf("Frame %d: npu=%.2f post=%.2f valid=%d json_len=%zu\n",
                   frame_count, npu_ms, post_ms, valid, strlen(json));

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
    printf("=== NPU Seg Server (fd2, DEFAULT input) ===\n");

    vip_init();
    if (load_model("/dev/shm/yolo11s-seg_10_uint8_a733.nb") < 0) return 1;

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
