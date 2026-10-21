// npu_dispatcher_full — C++ диспетчер:
// GStreamer → VPU → dma-buf → BGR → letterbox ×4 → fd → 4 NPU-сервера → JSON
// Плюс: BGR 1920×1088 в SHM для Python overlay.

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <linux/dma-heap.h>
#include <arpa/inet.h>
#include <opencv2/opencv.hpp>

#define ALIGN 64
#define INPUT_W 1920
#define INPUT_H 1088
#define BGR_SIZE (INPUT_W * INPUT_H * 3)
#define SHM_SLOTS 4
#define SHM_NAME "/ugv_bgr_shm"
#define HEADER_SIZE 64

struct ShmHeader {
    volatile uint64_t frame_id;
    volatile uint32_t width;
    volatile uint32_t height;
    volatile uint32_t channels;
    volatile uint32_t write_idx;
    volatile uint32_t reserved[11];
};

// 4 модели
struct ModelSocket {
    const char *name;
    const char *sock_path;
    int sock;
    int out_w, out_h;
    int dma_fd;
    void *dma_ptr;
};

static ModelSocket g_models[4] = {
    {"YOLO26s", "/tmp/npu_yolo_fd.sock",  -1, 640, 640, -1, NULL},
    {"Pose",    "/tmp/npu_pose_fd.sock",  -1, 640, 640, -1, NULL},
    {"Seg",     "/tmp/npu_seg_fd.sock",   -1, 640, 640, -1, NULL},
    {"Depth",   "/tmp/npu_depth_fd.sock", -1, 768, 768, -1, NULL},
};

static void *g_shm_ptr = NULL;
static int g_shm_fd = -1;
static int g_frame_count = 0;
static int g_write_idx = 0;

// === dma-buf ===
static int create_dma_buf(size_t size, void **ptr_out) {
    int heap_fd = open("/dev/dma_heap/reserved", O_RDWR);
    if (heap_fd < 0) { perror("open dma_heap"); return -1; }
    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    struct dma_heap_allocation_data data = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) { perror("ALLOC"); close(heap_fd); return -1; }
    close(heap_fd);
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap"); close(data.fd); return -1; }
    *ptr_out = ptr;
    return data.fd;
}

// === SHM ===
static int init_shm() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return -1; }
    size_t size = HEADER_SIZE + SHM_SLOTS * BGR_SIZE;
    if (ftruncate(fd, size) < 0) { perror("ftruncate"); close(fd); return -1; }
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap"); close(fd); return -1; }

    struct ShmHeader *hdr = (struct ShmHeader*)ptr;
    hdr->width = INPUT_W; hdr->height = INPUT_H; hdr->channels = 3;
    hdr->write_idx = 0; hdr->frame_id = 0;

    g_shm_ptr = ptr;
    return fd;
}

// === Передача fd через SCM_RIGHTS ===
static int send_fd(int sock, int fd) {
    char buf[1] = {'F'};
    struct iovec io = { .iov_base = buf, .iov_len = 1 };
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf; msg.msg_controllen = sizeof(cmsg_buf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0);
}

static bool read_n(int fd, void *buf, size_t n) {
    char *p = (char*)buf;
    while (n > 0) { ssize_t r = read(fd, p, n); if (r <= 0) return false; p += r; n -= r; }
    return true;
}

static bool recv_json(int sock, char *out, size_t out_size) {
    uint32_t len_be;
    if (!read_n(sock, &len_be, 4)) return false;
    uint32_t len = ntohl(len_be);
    if (len >= out_size) return false;
    if (!read_n(sock, out, len)) return false;
    out[len] = '\0';
    return true;
}

// === Letterbox: BGR → RGB в dma-buf ===
static void letterbox_to_dma(const cv::Mat& bgr, void *dma_ptr, int lw, int lh) {
    float scale = std::min((float)lh / bgr.rows, (float)lw / bgr.cols);
    int rc = (int)round(scale * bgr.cols);
    int rr = (int)round(scale * bgr.rows);
    float dh = (lh - rr) / 2.0f, dw = (lw - rc) / 2.0f;

    cv::Mat resized, rgb;
    cv::resize(bgr, resized, cv::Size(rc, rr), 0, 0, cv::INTER_LINEAR);
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    cv::Mat img_new(lh, lw, CV_8UC3, dma_ptr);
    int top = (int)(round(dh - 0.1)), bot = (int)(round(dh + 0.1));
    int left = (int)(round(dw - 0.1)), right = (int)(round(dw + 0.1));
    cv::copyMakeBorder(rgb, img_new, top, bot, left, right, cv::BORDER_CONSTANT, cv::Scalar(114,114,114));
}

// === Callback ===
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
    auto t0 = std::chrono::high_resolution_clock::now();
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMemory *mem = gst_buffer_peek_memory(buf, 0);
    if (!gst_is_dmabuf_memory(mem)) { gst_sample_unref(sample); return GST_FLOW_OK; }
    int fd = gst_dmabuf_memory_get_fd(mem);
    gsize size = gst_memory_get_sizes(mem, NULL, NULL);

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap"); gst_sample_unref(sample); return GST_FLOW_ERROR; }

    cv::Mat yv12(INPUT_H * 3 / 2, INPUT_W, CV_8UC1, ptr);
    cv::Mat bgr;
    cv::cvtColor(yv12, bgr, cv::COLOR_YUV2BGR_YV12);
    auto t1 = std::chrono::high_resolution_clock::now();

    // SHM: BGR 1920×1088
    struct ShmHeader *hdr = (struct ShmHeader*)g_shm_ptr;
    uint32_t slot = g_write_idx % SHM_SLOTS;
    unsigned char *shm_bgr = (unsigned char*)g_shm_ptr + HEADER_SIZE + slot * BGR_SIZE;
    memcpy(shm_bgr, bgr.data, BGR_SIZE);
    hdr->write_idx = slot;
    hdr->frame_id++;
    auto t2 = std::chrono::high_resolution_clock::now();

    munmap(ptr, size);
    gst_sample_unref(sample);

    // Letterbox ×4
    auto tlb0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 4; i++) {
        letterbox_to_dma(bgr, g_models[i].dma_ptr, g_models[i].out_w, g_models[i].out_h);
    }
    auto tlb1 = std::chrono::high_resolution_clock::now();

    // send_fd ×4
    for (int i = 0; i < 4; i++) {
        send_fd(g_models[i].sock, g_models[i].dma_fd);
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    double lb_only_ms = std::chrono::duration<double, std::milli>(tlb1 - tlb0).count();

    // Принимаем JSON ×4
    char json[4][65536];
    for (int i = 0; i < 4; i++) {
        if (!recv_json(g_models[i].sock, json[i], sizeof(json[i]))) {
            printf("[%s] recv_json failed\n", g_models[i].name);
        }
    }
    auto t4 = std::chrono::high_resolution_clock::now();

    g_frame_count++;
    g_write_idx++;

    if (g_frame_count % 20 == 0) {
        double cvt_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double shm_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        double send_ms = std::chrono::duration<double, std::milli>(t3 - tlb1).count();
        double lb_ms_only = lb_only_ms;
        double recv_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
        double total_ms = std::chrono::duration<double, std::milli>(t4 - t0).count();
        printf("Frame %d: cvt=%.1f shm=%.1f lb=%.1f send=%.1f recv=%.1f total=%.1f ms | "
               "yolo=%zu pose=%zu seg=%zu depth=%zu\n",
               g_frame_count, cvt_ms, shm_ms, lb_ms_only, send_ms, recv_ms, total_ms,
               strlen(json[0]), strlen(json[1]), strlen(json[2]), strlen(json[3]));
    }

    return GST_FLOW_OK;
}

// === Соединение с сервером ===
static int connect_to_server(const char *path) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    gst_init(&argc, &argv);
    printf("=== NPU Dispatcher Full ===\n");

    // SHM
    g_shm_fd = init_shm();
    if (g_shm_fd < 0) return 1;
    printf("SHM: %s\n", SHM_NAME);

    // 4 dma-buf
    for (int i = 0; i < 4; i++) {
        size_t sz = g_models[i].out_w * g_models[i].out_h * 3;
        g_models[i].dma_fd = create_dma_buf(sz, &g_models[i].dma_ptr);
        printf("[%s] dma-buf fd=%d, size=%zu\n", g_models[i].name, g_models[i].dma_fd, sz);
    }

    // Соединение с 4 серверами
    for (int i = 0; i < 4; i++) {
        g_models[i].sock = connect_to_server(g_models[i].sock_path);
        if (g_models[i].sock < 0) {
            printf("[%s] connect to %s failed\n", g_models[i].name, g_models[i].sock_path);
            return 1;
        }
        printf("[%s] connected to %s\n", g_models[i].name, g_models[i].sock_path);
    }

    const char *pipeline_str =
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=100 ! "
        "image/jpeg ! omxmjpegvideodec ! "
        "appsink name=npu_sink emit-signals=true sync=false max-buffers=1 drop=true";

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_str, &error);
    if (!pipeline) { printf("Pipeline failed\n"); return 1; }

    GstAppSink *npu_sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "npu_sink"));
    g_signal_connect(npu_sink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    printf("Starting pipeline...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessageType types = (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 90 * GST_SECOND, types);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = NULL; gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            printf("ERROR: %s\n", err->message);
        } else printf("EOS\n");
        gst_message_unref(msg);
    } else printf("TIMEOUT\n");
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("Frames: %d\n", g_frame_count);

    // Закрываем
    for (int i = 0; i < 4; i++) {
        close(g_models[i].sock);
        munmap(g_models[i].dma_ptr, g_models[i].out_w * g_models[i].out_h * 3);
        close(g_models[i].dma_fd);
    }

    printf("DONE\n");
    return 0;
}
