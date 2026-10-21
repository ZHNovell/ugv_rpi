// npu_dispatcher — Часть 4: 2 модели в одном процессе
// YOLO26s (детекция объектов) + YOLO11_pose (скелеты)
// Один GStreamer pipeline, один VPU, два NPU-инференса.

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
#include "model_config_pose.h"

#define ALIGN 64
#define INPUT_W 1920
#define INPUT_H 1088
#define OUTPUT_W 640
#define OUTPUT_H 640

// === Общие структуры ===
struct DmaBufSlot {
    int fd;
    void *ptr;
    size_t size;
};

// === YOLO26s ===
struct Object {
    cv::Rect_<float> rect;
    int label;
    float prob;
};

extern int detect_yolo26_6_post(const cv::Mat& bgr, std::vector<Object>& detections, float **output);

// === YOLO11_pose ===
struct PoseKeyPoint {
    cv::Point2f p;
    float prob;
};

struct PoseObject {
    cv::Rect_<float> rect;
    int label;
    float prob;
    std::vector<PoseKeyPoint> keypoints;
};

extern int detect_pose_9_post(const cv::Mat& bgr, std::vector<PoseObject>& objects, float **output);

// === Состояние для каждой модели ===
struct ModelState {
    vip_network network;
    vip_buffer input_buf;
    vip_buffer output_bufs[12];
    int num_outputs;
    DmaBufSlot slot;   // свой dma-buf для input
    int frame_count;
    double sum_npu, sum_post;
};

static ModelState g_yolo26s;
static ModelState g_pose;

static int g_write_idx = 0;
static int g_frame_count = 0;

// Метрики
static double g_sum_mmap = 0, g_sum_cvt = 0, g_sum_letterbox = 0;
static double g_sum_total = 0;

// Последние JSON
static char g_last_json_objects[65536] = {0};
static char g_last_json_pose[65536] = {0};

// === Создание dma-buf ===
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

// === Загрузка модели ===
static int load_model(const char *path, ModelState *state, const char *name) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return -1; }
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);
    printf("[%s] Model loaded: %zu bytes\n", name, sz);

    vip_status_e status = vip_create_network(data, sz, VIP_CREATE_NETWORK_FROM_MEMORY, &state->network);
    if (status != VIP_SUCCESS) { printf("[%s] create_network failed: %d\n", name, status); return -1; }

    status = vip_prepare_network(state->network);
    if (status != VIP_SUCCESS) { printf("[%s] prepare_network failed: %d\n", name, status); return -1; }

    vip_query_network(state->network, VIP_NETWORK_PROP_OUTPUT_COUNT, &state->num_outputs);
    printf("[%s] Outputs: %d\n", name, state->num_outputs);

    for (int i = 0; i < state->num_outputs; i++) {
        vip_buffer_create_params_t op = {0};
        vip_query_output(state->network, i, VIP_BUFFER_PROP_DATA_FORMAT, &op.data_format);
        vip_query_output(state->network, i, VIP_BUFFER_PROP_QUANT_FORMAT, &op.quant_format);
        vip_query_output(state->network, i, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &op.num_of_dims);
        vip_query_output(state->network, i, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, op.sizes);
        op.memory_type = VIP_BUFFER_MEMORY_TYPE_DEFAULT;
        status = vip_create_buffer(&op, sizeof(op), &state->output_bufs[i]);
        if (status != VIP_SUCCESS) { printf("[%s] create output %d failed\n", name, i); return -1; }
        vip_set_output(state->network, i, state->output_bufs[i]);
    }

    // Свой dma-buf для input
    size_t in_size = OUTPUT_W * OUTPUT_H * 3;
    state->slot.fd = create_dma_buf(in_size, &state->slot.ptr);
    if (state->slot.fd < 0) { printf("[%s] create_dma_buf failed\n", name); return -1; }
    state->slot.size = (in_size + ALIGN - 1) & ~(ALIGN - 1);
    state->frame_count = 0;
    state->sum_npu = 0;
    state->sum_post = 0;
    printf("[%s] Ready (slot fd=%d)\n", name, state->slot.fd);
    return 0;
}

// === Letterbox: BGR → 640×640 RGB (в out_data) ===
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

// === Запуск одной модели ===
static void run_model(ModelState *state, const cv::Mat& bgr, float **outputs_out) {
    vip_buffer_create_params_t ip = {0};
    vip_query_input(state->network, 0, VIP_BUFFER_PROP_DATA_FORMAT, &ip.data_format);
    vip_query_input(state->network, 0, VIP_BUFFER_PROP_QUANT_FORMAT, &ip.quant_format);
    vip_query_input(state->network, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &ip.num_of_dims);
    vip_query_input(state->network, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, ip.sizes);
    ip.memory_type = VIP_BUFFER_MEMORY_TYPE_HOST;
    size_t in_size = 1;
    for (int i = 0; i < ip.num_of_dims; i++) in_size *= ip.sizes[i];
    in_size = (in_size + ALIGN - 1) & ~(ALIGN - 1);

    vip_buffer input_buf = NULL;
    vip_status_e status = vip_create_buffer_from_fd(&ip, state->slot.fd, in_size, &input_buf);
    if (status != VIP_SUCCESS) { printf("create_buffer_from_fd failed: %d\n", status); return; }
    vip_set_input(state->network, 0, input_buf);

    vip_run_network(state->network);
    vip_destroy_buffer(input_buf);

    for (int i = 0; i < state->num_outputs; i++) {
        outputs_out[i] = (float*)vip_map_buffer(state->output_bufs[i]);
    }
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

    auto t1 = std::chrono::high_resolution_clock::now();
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    auto t2 = std::chrono::high_resolution_clock::now();
    if (ptr == MAP_FAILED) { perror("mmap"); gst_sample_unref(sample); return GST_FLOW_ERROR; }

    cv::Mat yv12(INPUT_H * 3 / 2, INPUT_W, CV_8UC1, ptr);
    cv::Mat bgr;
    cv::cvtColor(yv12, bgr, cv::COLOR_YUV2BGR_YV12);
    auto t3 = std::chrono::high_resolution_clock::now();

    // Letterbox для YOLO26s
    letterbox_to_rgb(bgr, (unsigned char*)g_yolo26s.slot.ptr, OUTPUT_W, OUTPUT_H);
    // Letterbox для pose
    letterbox_to_rgb(bgr, (unsigned char*)g_pose.slot.ptr, OUTPUT_W, OUTPUT_H);
    auto t4 = std::chrono::high_resolution_clock::now();

    munmap(ptr, size);

    // === YOLO26s ===
    auto tn1 = std::chrono::high_resolution_clock::now();
    float* outputs_yolo[12];
    run_model(&g_yolo26s, bgr, outputs_yolo);
    auto tn2 = std::chrono::high_resolution_clock::now();

    std::vector<Object> objects;
    auto tp1 = std::chrono::high_resolution_clock::now();
    detect_yolo26_6_post(bgr, objects, outputs_yolo);
    auto tp2 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < g_yolo26s.num_outputs; i++) vip_unmap_buffer(g_yolo26s.output_bufs[i]);

    // === Pose ===
    auto tp3 = std::chrono::high_resolution_clock::now();
    float* outputs_pose[12];
    run_model(&g_pose, bgr, outputs_pose);
    auto tp4 = std::chrono::high_resolution_clock::now();

    std::vector<PoseObject> poses;
    auto tp5 = std::chrono::high_resolution_clock::now();
    detect_pose_9_post(bgr, poses, outputs_pose);
    auto tp6 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < g_pose.num_outputs; i++) vip_unmap_buffer(g_pose.output_bufs[i]);

    // === JSON для YOLO26s ===
    int off = snprintf(g_last_json_objects, sizeof(g_last_json_objects), "[");
    for (size_t i = 0; i < objects.size() && off < (int)sizeof(g_last_json_objects) - 200; i++) {
        const auto& o = objects[i];
        off += snprintf(g_last_json_objects + off, sizeof(g_last_json_objects) - off,
            "%s{\"class_id\":%d,\"class\":\"%s\",\"confidence\":%.3f,\"bbox\":[%.1f,%.1f,%.1f,%.1f]}",
            i > 0 ? "," : "", o.label,
            o.label < (int)g_classes_name.size() ? g_classes_name[o.label].c_str() : "?",
            o.prob, o.rect.x, o.rect.y, o.rect.x + o.rect.width, o.rect.y + o.rect.height);
    }
    snprintf(g_last_json_objects + off, sizeof(g_last_json_objects) - off, "]");

    // === JSON для Pose ===
    off = snprintf(g_last_json_pose, sizeof(g_last_json_pose), "[");
    for (size_t i = 0; i < poses.size() && off < (int)sizeof(g_last_json_pose) - 800; i++) {
        const auto& p = poses[i];
        off += snprintf(g_last_json_pose + off, sizeof(g_last_json_pose) - off,
            "%s{\"confidence\":%.3f,\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"keypoints\":[",
            i > 0 ? "," : "", p.prob,
            p.rect.x, p.rect.y, p.rect.x + p.rect.width, p.rect.y + p.rect.height);
        for (size_t k = 0; k < p.keypoints.size() && off < (int)sizeof(g_last_json_pose) - 50; k++) {
            off += snprintf(g_last_json_pose + off, sizeof(g_last_json_pose) - off,
                "%s[%.1f,%.1f,%.2f]", k > 0 ? "," : "",
                p.keypoints[k].p.x, p.keypoints[k].p.y, p.keypoints[k].prob);
        }
        off += snprintf(g_last_json_pose + off, sizeof(g_last_json_pose) - off, "]}");
    }
    snprintf(g_last_json_pose + off, sizeof(g_last_json_pose) - off, "]");

    // Метрики
    double mmap_ms      = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double cvt_ms       = std::chrono::duration<double, std::milli>(t3 - t2).count();
    double letterbox_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
    double npu_yolo_ms  = std::chrono::duration<double, std::milli>(tn2 - tn1).count();
    double post_yolo_ms = std::chrono::duration<double, std::milli>(tp2 - tp1).count();
    double npu_pose_ms  = std::chrono::duration<double, std::milli>(tp4 - tp3).count();
    double post_pose_ms = std::chrono::duration<double, std::milli>(tp6 - tp5).count();
    double total_ms     = std::chrono::duration<double, std::milli>(tp6 - t0).count();

    g_sum_mmap += mmap_ms; g_sum_cvt += cvt_ms; g_sum_letterbox += letterbox_ms;
    g_sum_total += total_ms;
    g_yolo26s.sum_npu += npu_yolo_ms;
    g_yolo26s.sum_post += post_yolo_ms;
    g_pose.sum_npu += npu_pose_ms;
    g_pose.sum_post += post_pose_ms;

    g_frame_count++;

    if (g_frame_count % 30 == 0) {
        printf("Frame %d: mmap=%.2f cvt=%.2f lb=%.2f | yolo: npu=%.2f post=%.2f | pose: npu=%.2f post=%.2f | total=%.2f ms\n",
               g_frame_count, mmap_ms, cvt_ms, letterbox_ms,
               npu_yolo_ms, post_yolo_ms, npu_pose_ms, post_pose_ms, total_ms);
        printf("  → objects: %zu, poses: %zu\n", objects.size(), poses.size());
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    gst_init(&argc, &argv);
    printf("=== NPU Dispatcher Part 4 (2 models) ===\n");

    vip_status_e vstatus = vip_init();
    if (vstatus != VIP_SUCCESS) { printf("vip_init failed\n"); return 1; }

    if (load_model("/dev/shm/yolo26s_6_pcq_a733.nb", &g_yolo26s, "YOLO26s") < 0) return 1;
    if (load_model("/dev/shm/yolo11s-pose_9_uint8_a733.nb", &g_pose, "Pose") < 0) return 1;

    const char *pipeline_str =
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=100 ! "
        "image/jpeg ! omxmjpegvideodec ! "
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
        printf("mmap:       %.2f ms\n", g_sum_mmap / g_frame_count);
        printf("cvt:        %.2f ms\n", g_sum_cvt / g_frame_count);
        printf("letterbox:  %.2f ms\n", g_sum_letterbox / g_frame_count);
        printf("YOLO26s: npu=%.2f post=%.2f\n",
               g_yolo26s.sum_npu / g_frame_count, g_yolo26s.sum_post / g_frame_count);
        printf("Pose:    npu=%.2f post=%.2f\n",
               g_pose.sum_npu / g_frame_count, g_pose.sum_post / g_frame_count);
        printf("total:      %.2f ms (%.1f FPS)\n",
               g_sum_total / g_frame_count, 1000.0 / (g_sum_total / g_frame_count));
    }
    printf("Frames: %d\n", g_frame_count);
    printf("Objects JSON: %s\n", g_last_json_objects);
    printf("Pose JSON: %s\n", g_last_json_pose);

    for (int i = 0; i < g_yolo26s.num_outputs; i++) vip_destroy_buffer(g_yolo26s.output_bufs[i]);
    vip_destroy_network(g_yolo26s.network);
    for (int i = 0; i < g_pose.num_outputs; i++) vip_destroy_buffer(g_pose.output_bufs[i]);
    vip_destroy_network(g_pose.network);
    vip_destroy();
    munmap(g_yolo26s.slot.ptr, g_yolo26s.slot.size);
    close(g_yolo26s.slot.fd);
    munmap(g_pose.slot.ptr, g_pose.slot.size);
    close(g_pose.slot.fd);
    printf("DONE\n");
    return 0;
}
