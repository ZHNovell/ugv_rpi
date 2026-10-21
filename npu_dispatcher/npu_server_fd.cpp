// npu_server_fd — YOLO26s сервер с приёмом fd dma-buf через SCM_RIGHTS
// Клиент (C++ dispatcher) передаёт fd dma-buf с готовым BGR 640×640.
// Сервер делает vip_create_buffer_from_fd → inference → JSON.

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

#define SOCKET_PATH "/tmp/npu_yolo_fd.sock"
#define ALIGN 64
#define INPUT_W 640
#define INPUT_H 640

struct Object {
    cv::Rect_<float> rect;
    int label;
    float prob;
};

extern int detect_yolo26_6_post(const cv::Mat& bgr, std::vector<Object>& detections, float **output);

static const std::vector<std::string> g_classes_name = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic_light",
    "fire_hydrant", "stop_sign", "parking_meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports_ball", "kite", "baseball_bat", "baseball_glove", "skateboard", "surfboard",
    "tennis_racket", "bottle", "wine_glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot_dog", "pizza", "donut", "cake", "chair", "couch",
    "potted_plant", "bed", "dining_table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell_phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy_bear",
    "hair_drier", "toothbrush"
};

static vip_network g_network = NULL;
static vip_buffer g_output_bufs[12];
static int g_num_outputs = 0;

// === Приём fd через SCM_RIGHTS ===
static int recv_fd(int sock, int *out_fd) {
    char buf[1];
    struct iovec io = { .iov_base = buf, .iov_len = 1 };
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n <= 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;
    memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
    return 0;
}

// === Отправка JSON ===
static bool write_n(int fd, const void* buf, size_t n) {
    const char *p = (const char*)buf;
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
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

static int handle_client(int cli) {
    int frame_count = 0;
    double sum_npu = 0, sum_post = 0;

    while (true) {
        // Принимаем fd
        int fd = -1;
        if (recv_fd(cli, &fd) < 0) break;

        // mmap dma-buf для доступа к BGR
        size_t bgr_size = INPUT_W * INPUT_H * 3;
        void *ptr = mmap(NULL, bgr_size, PROT_READ, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) { perror("mmap"); close(fd); break; }

        cv::Mat bgr(INPUT_H, INPUT_W, CV_8UC3, ptr);

        // NPU
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

        // Собираем выходы
        float* outputs[12];
        for (int i = 0; i < g_num_outputs; i++) outputs[i] = (float*)vip_map_buffer(g_output_bufs[i]);

        auto tp1 = std::chrono::high_resolution_clock::now();
        std::vector<Object> objects;
        detect_yolo26_6_post(bgr, objects, outputs);
        auto tp2 = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < g_num_outputs; i++) vip_unmap_buffer(g_output_bufs[i]);

        // JSON
        char json[65536];
        int off = snprintf(json, sizeof(json), "[");
        for (size_t i = 0; i < objects.size() && off < (int)sizeof(json) - 200; i++) {
            const auto& o = objects[i];
            off += snprintf(json + off, sizeof(json) - off,
                "%s{\"class_id\":%d,\"class\":\"%s\",\"confidence\":%.3f,\"bbox\":[%.1f,%.1f,%.1f,%.1f]}",
                i>0?",":"", o.label,
                o.label < (int)g_classes_name.size() ? g_classes_name[o.label].c_str() : "?",
                o.prob, o.rect.x, o.rect.y, o.rect.x+o.rect.width, o.rect.y+o.rect.height);
        }
        snprintf(json + off, sizeof(json) - off, "]");

        // Отправляем JSON
        uint32_t json_len_be = htonl((uint32_t)strlen(json));
        write_n(cli, &json_len_be, 4);
        write_n(cli, json, strlen(json));

        double npu_ms = std::chrono::duration<double, std::milli>(tn2 - tn1).count();
        double post_ms = std::chrono::duration<double, std::milli>(tp2 - tp1).count();
        sum_npu += npu_ms; sum_post += post_ms;
        frame_count++;

        if (frame_count % 20 == 0) {
            printf("Frame %d: npu=%.2f post=%.2f objects=%zu\n",
                   frame_count, npu_ms, post_ms, objects.size());
        }

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
    printf("=== NPU Server (fd) — YOLO26s ===\n");

    vip_init();
    if (load_model("/dev/shm/yolo26s_6_pcq_a733.nb") < 0) return 1;

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
        break;  // Пока один клиент
    }

    close(srv);
    printf("DONE\n");
    return 0;
}
