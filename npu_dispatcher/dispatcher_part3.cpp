// npu_dispatcher — Часть 3: постобработка + JSON
// Логика: GStreamer → VPU → C++ (YV12→BGR→letterbox→RGB) → NPU → post → JSON

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
#include <linux/dma-heap.h>
#include <opencv2/opencv.hpp>
#include <vip_lite.h>

#include "model_config.h"

#define ALIGN 64
#define INPUT_W 1920
#define INPUT_H 1088
#define OUTPUT_W 640
#define OUTPUT_H 640

// От detect_yolo26_6_post из yolo26_6_post.cpp
struct Object {
    cv::Rect_<float> rect;
    int label;
    float prob;
};

extern int detect_yolo26_6_post(const cv::Mat& bgr, std::vector<Object>& detections, float **output);

struct DmaBufSlot {
    int fd;
    void *ptr;
    size_t size;
};

static DmaBufSlot g_slots[4];
static int g_write_idx = 0;
static int g_frame_count = 0;

static vip_network g_network = NULL;
static vip_buffer g_output_bufs[10];
static int g_num_outputs = 0;

// Метрики
static double g_sum_mmap = 0, g_sum_cvt = 0, g_sum_letterbox = 0;
static double g_sum_write = 0, g_sum_npu = 0, g_sum_post = 0, g_sum_total = 0;

// Последний JSON (для Python)
static char g_last_json[65536] = {0};

static int load_model(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return -1; }
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);
    printf("Model loaded: %zu bytes\n", sz);

    vip_status_e status = vip_create_network(data, sz, VIP_CREATE_NETWORK_FROM_MEMORY, &g_network);
    if (status != VIP_SUCCESS) { printf("vip_create_network failed: %d\n", status); return -1; }

    status = vip_prepare_network(g_network);
    if (status != VIP_SUCCESS) { printf("vip_prepare_network failed: %d\n", status); return -1; }
    printf("Network prepared\n");

    vip_query_network(g_network, VIP_NETWORK_PROP_OUTPUT_COUNT, &g_num_outputs);
    printf("Outputs: %d\n", g_num_outputs);
    for (int i = 0; i < g_num_outputs; i++) {
        vip_buffer_create_params_t op = {0};
        vip_query_output(g_network, i, VIP_BUFFER_PROP_DATA_FORMAT, &op.data_format);
        vip_query_output(g_network, i, VIP_BUFFER_PROP_QUANT_FORMAT, &op.quant_format);
        vip_query_output(g_network, i, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &op.num_of_dims);
        vip_query_output(g_network, i, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, op.sizes);
        op.memory_type = VIP_BUFFER_MEMORY_TYPE_DEFAULT;
        status = vip_create_buffer(&op, sizeof(op), &g_output_bufs[i]);
        if (status != VIP_SUCCESS) { printf("create output %d failed: %d\n", i, status); return -1; }
        status = vip_set_output(g_network, i, g_output_bufs[i]);
        if (status != VIP_SUCCESS) { printf("set output %d failed: %d\n", i, status); return -1; }
    }
    return 0;
}

static int create_dma_buf(size_t size, void **ptr_out) {
    int heap_fd = open("/dev/dma_heap/reserved", O_RDWR);
    if (heap_fd < 0) { perror("open dma_heap"); return -1; }
    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    struct dma_heap_allocation_data data = {
        .len = size, .fd_flags = O_RDWR | O_CLOEXEC, .heap_flags = 0,
    };
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) { perror("ALLOC"); close(heap_fd); return -1; }
    close(heap_fd);
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap"); close(data.fd); return -1; }
    *ptr_out = ptr;
    return data.fd;
}

static int init_ring_buffer() {
    size_t size = OUTPUT_W * OUTPUT_H * 3;
    for (int i = 0; i < 4; i++) {
        g_slots[i].fd = create_dma_buf(size, &g_slots[i].ptr);
        if (g_slots[i].fd < 0) return -1;
        g_slots[i].size = (size + ALIGN - 1) & ~(ALIGN - 1);
        printf("Slot %d: fd=%d\n", i, g_slots[i].fd);
    }
    return 0;
}

// Letterbox: YV12 → BGR → 640×640 RGB (в input_data)
static void letterbox_to_rgb(const cv::Mat& bgr, unsigned char* out_data,
                              int letterbox_cols, int letterbox_rows) {
    float scale_letterbox = 1.f;
    if ((letterbox_rows * 1.0 / bgr.rows) < (letterbox_cols * 1.0 / bgr.cols)) {
        scale_letterbox = letterbox_rows * 1.0 / bgr.rows;
    } else {
        scale_letterbox = letterbox_cols * 1.0 / bgr.cols;
    }
    int resize_cols = int(round(scale_letterbox * bgr.cols));
    int resize_rows = int(round(scale_letterbox * bgr.rows));

    float dh = (float)(letterbox_rows - resize_rows);
    float dw = (float)(letterbox_cols - resize_cols);
    dh /= 2.0f;
    dw /= 2.0f;

    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(resize_cols, resize_rows), 0, 0, cv::INTER_LINEAR);

    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    cv::Mat img_new(letterbox_rows, letterbox_cols, CV_8UC3, out_data);
    int top   = (int)(round(dh - 0.1));
    int bot   = (int)(round(dh + 0.1));
    int left  = (int)(round(dw - 0.1));
    int right = (int)(round(dw + 0.1));

    cv::copyMakeBorder(rgb, img_new, top, bot, left, right,
                       cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));
}

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
    auto t0 = std::chrono::high_resolution_clock::now();
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMemory *mem = gst_buffer_peek_memory(buf, 0);
    if (!gst_is_dmabuf_memory(mem)) { gst_sample_unref(sample); return GST_FLOW_OK; }
    int fd = gst_dmabuf_memory_get_fd(mem);
    gsize size = gst_memory_get_sizes(mem, NULL, NULL);

    auto t1 = std::chrono::high_resolution_clock::now();
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    auto t2 = std::chrono::high_resolution_clock::now();
    if (ptr == MAP_FAILED) { perror("mmap"); gst_sample_unref(sample); return GST_FLOW_ERROR; }

    // YV12 → BGR (исходное 1920×1088 — сохраняем для постобработки)
    cv::Mat yv12(INPUT_H * 3 / 2, INPUT_W, CV_8UC1, ptr);
    cv::Mat bgr;
    cv::cvtColor(yv12, bgr, cv::COLOR_YUV2BGR_YV12);
    auto t3 = std::chrono::high_resolution_clock::now();

    // Letterbox → 640×640 RGB
    DmaBufSlot &slot = g_slots[g_write_idx];
    letterbox_to_rgb(bgr, (unsigned char*)slot.ptr, OUTPUT_W, OUTPUT_H);
    auto t4 = std::chrono::high_resolution_clock::now();

    munmap(ptr, size);

    // === NPU ===
    vip_buffer_create_params_t ip = {0};
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_DATA_FORMAT, &ip.data_format);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_QUANT_FORMAT, &ip.quant_format);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &ip.num_of_dims);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, ip.sizes);
    ip.memory_type = VIP_BUFFER_MEMORY_TYPE_HOST;
    size_t in_size = 1;
    for (int i = 0; i < ip.num_of_dims; i++) in_size *= ip.sizes[i];
    in_size = (in_size + ALIGN - 1) & ~(ALIGN - 1);

    vip_buffer input_buf = NULL;
    vip_status_e status = vip_create_buffer_from_fd(&ip, slot.fd, in_size, &input_buf);
    if (status != VIP_SUCCESS) {
        printf("create_buffer_from_fd failed: %d\n", status);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    vip_set_input(g_network, 0, input_buf);

    auto tn1 = std::chrono::high_resolution_clock::now();
    status = vip_run_network(g_network);
    auto tn2 = std::chrono::high_resolution_clock::now();
    if (status != VIP_SUCCESS) printf("vip_run_network failed: %d\n", status);

    vip_destroy_buffer(input_buf);

    // === Постобработка ===
    auto tp1 = std::chrono::high_resolution_clock::now();
    float* outputs[10];
    for (int i = 0; i < g_num_outputs; i++) {
        outputs[i] = (float*)vip_map_buffer(g_output_bufs[i]);
    }

    std::vector<Object> objects;
    detect_yolo26_6_post(bgr, objects, outputs);

    for (int i = 0; i < g_num_outputs; i++) {
        vip_unmap_buffer(g_output_bufs[i]);
    }
    auto tp2 = std::chrono::high_resolution_clock::now();

    // === JSON ===
    int off = snprintf(g_last_json, sizeof(g_last_json), "[");
    for (size_t i = 0; i < objects.size() && off < (int)sizeof(g_last_json) - 200; i++) {
        const auto& o = objects[i];
        off += snprintf(g_last_json + off, sizeof(g_last_json) - off,
            "%s{\"class_id\":%d,\"class\":\"%s\",\"confidence\":%.3f,\"bbox\":[%.1f,%.1f,%.1f,%.1f]}",
            i > 0 ? "," : "",
            o.label,
            o.label < (int)g_classes_name.size() ? g_classes_name[o.label].c_str() : "?",
            o.prob,
            o.rect.x, o.rect.y, o.rect.x + o.rect.width, o.rect.y + o.rect.height);
    }
    snprintf(g_last_json + off, sizeof(g_last_json) - off, "]");

    double mmap_ms      = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double cvt_ms       = std::chrono::duration<double, std::milli>(t3 - t2).count();
    double letterbox_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
    double write_ms     = 0;  // letterbox включает запись
    double npu_ms       = std::chrono::duration<double, std::milli>(tn2 - tn1).count();
    double post_ms      = std::chrono::duration<double, std::milli>(tp2 - tp1).count();
    double total_ms     = std::chrono::duration<double, std::milli>(tp2 - t0).count();

    g_sum_mmap += mmap_ms; g_sum_cvt += cvt_ms; g_sum_letterbox += letterbox_ms;
    g_sum_write += write_ms; g_sum_npu += npu_ms; g_sum_post += post_ms; g_sum_total += total_ms;
    g_frame_count++;

    if (g_frame_count % 30 == 0) {
        printf("Frame %d: mmap=%.2f cvt=%.2f letterbox=%.2f npu=%.2f post=%.2f total=%.2f ms | %zu objects\n",
               g_frame_count, mmap_ms, cvt_ms, letterbox_ms, npu_ms, post_ms, total_ms, objects.size());
    }

    g_write_idx = (g_write_idx + 1) % 4;
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    gst_init(&argc, &argv);
    printf("=== NPU Dispatcher Part 3 ===\n");

    vip_status_e vstatus = vip_init();
    if (vstatus != VIP_SUCCESS) { printf("vip_init failed\n"); return 1; }
    if (load_model("/dev/shm/yolo26s_6_pcq_a733.nb") < 0) return 1;

    if (init_ring_buffer() < 0) { printf("ring buffer failed\n"); return 1; }

    const char *pipeline_str =
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=100 ! "
        "image/jpeg ! "
        "omxmjpegvideodec ! "
        "appsink name=npu_sink emit-signals=true sync=false max-buffers=1 drop=true";

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_str, &error);
    if (!pipeline) { printf("Pipeline failed: %s\n", error ? error->message : "unknown"); return 1; }

    GstAppSink *npu_sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "npu_sink"));
    g_signal_connect(npu_sink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    printf("Starting pipeline...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessageType types = (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND, types);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = NULL; gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            printf("ERROR: %s\n%s\n", err->message, dbg ? dbg : "");
        } else { printf("EOS\n"); }
        gst_message_unref(msg);
    } else { printf("TIMEOUT\n"); }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (g_frame_count > 0) {
        printf("\n=== Metrics (avg per frame) ===\n");
        printf("mmap:      %.2f ms\n", g_sum_mmap / g_frame_count);
        printf("cvt:       %.2f ms\n", g_sum_cvt / g_frame_count);
        printf("letterbox: %.2f ms\n", g_sum_letterbox / g_frame_count);
        printf("npu:       %.2f ms\n", g_sum_npu / g_frame_count);
        printf("post:      %.2f ms\n", g_sum_post / g_frame_count);
        printf("total:     %.2f ms (%.1f FPS)\n",
               g_sum_total / g_frame_count, 1000.0 / (g_sum_total / g_frame_count));
    }
    printf("Frames processed: %d\n", g_frame_count);
    printf("Last JSON: %s\n", g_last_json);

    for (int i = 0; i < g_num_outputs; i++) vip_destroy_buffer(g_output_bufs[i]);
    vip_destroy_network(g_network);
    vip_destroy();
    for (int i = 0; i < 4; i++) {
        munmap(g_slots[i].ptr, g_slots[i].size);
        close(g_slots[i].fd);
    }
    printf("DONE\n");
    return 0;
}
