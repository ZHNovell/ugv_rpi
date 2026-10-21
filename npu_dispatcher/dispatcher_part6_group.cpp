// npu_dispatcher — Часть 6: 4 модели через vip_create_group + vip_run_group
// VIPLite запускает все 4 модели АТОМАРНО, без прерываний между ними.

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <opencv2/opencv.hpp>
#include <vip_lite.h>

#define ALIGN 64
#define INPUT_W 1920
#define INPUT_H 1088
#define QUEUE_MAX 2

struct DmaBufSlot { int fd; void *ptr; size_t size; };

struct Object { cv::Rect_<float> rect; int label; float prob; };
extern int detect_yolo26_6_post(const cv::Mat& bgr, std::vector<Object>& detections, float **output);

struct PoseKeyPoint { cv::Point2f p; float prob; };
struct PoseObject { cv::Rect_<float> rect; int label; float prob; std::vector<PoseKeyPoint> keypoints; };
extern int detect_pose_9_post(const cv::Mat& bgr, std::vector<PoseObject>& objects, float **output);

struct SegObject { cv::Rect_<float> rect; int label; float prob; cv::Mat mask; };
extern int detect_seg_10_post(const cv::Mat& bgr, std::vector<SegObject>& objects, float **output);

extern "C" int depth_post_json(const cv::Mat& depth_npu, int orig_w, int orig_h, char* json_out, size_t json_size);

struct ModelState {
    vip_network network;
    vip_buffer output_bufs[12];
    int num_outputs;
    DmaBufSlot slot;
};

static ModelState g_yolo26s, g_pose, g_seg, g_depth;

// === ГРУППА ===
static vip_group g_group = NULL;

struct FrameData { cv::Mat bgr; };

static std::queue<FrameData> g_queue;
static std::mutex g_queue_mutex;
static std::condition_variable g_queue_cv;
static bool g_running = true;

static int g_frame_count = 0;
static double g_sum_worker = 0;
static char g_json_objects[65536] = {0};
static char g_json_pose[65536] = {0};
static char g_json_seg[65536] = {0};
static char g_json_depth[65536] = {0};

static int create_dma_buf(size_t size, void **ptr_out) {
    int heap_fd = open("/dev/dma_heap/reserved", O_RDWR);
    if (heap_fd < 0) { perror("open dma_heap"); return -1; }
    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    struct dma_heap_allocation_data data = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC, .heap_flags = 0 };
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) { perror("ALLOC"); close(heap_fd); return -1; }
    close(heap_fd);
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap"); close(data.fd); return -1; }
    *ptr_out = ptr;
    return data.fd;
}

// === Загрузка модели БЕЗ prepare (prepare сделаем после add в группу) ===
static int load_model(const char *path, ModelState *state, int in_w, int in_h, const char *name) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return -1; }
    fseek(f, 0, SEEK_END); size_t sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *data = malloc(sz); fread(data, 1, sz, f); fclose(f);
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
        vip_create_buffer(&op, sizeof(op), &state->output_bufs[i]);
        vip_set_output(state->network, i, state->output_bufs[i]);
    }

    size_t in_size = in_w * in_h * 3;
    state->slot.fd = create_dma_buf(in_size, &state->slot.ptr);
    state->slot.size = (in_size + ALIGN - 1) & ~(ALIGN - 1);
    printf("[%s] Ready (slot fd=%d)\n", name, state->slot.fd);
    return 0;
}

static void letterbox_to_rgb(const cv::Mat& bgr, unsigned char* out_data, int lw, int lh) {
    float scale = std::min((float)lh / bgr.rows, (float)lw / bgr.cols);
    int rc = (int)round(scale * bgr.cols);
    int rr = (int)round(scale * bgr.rows);
    float dh = (lh - rr) / 2.0f, dw = (lw - rc) / 2.0f;
    cv::Mat resized, rgb;
    cv::resize(bgr, resized, cv::Size(rc, rr), 0, 0, cv::INTER_LINEAR);
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    cv::Mat img_new(lh, lw, CV_8UC3, out_data);
    int top = (int)(round(dh - 0.1)), bot = (int)(round(dh + 0.1));
    int left = (int)(round(dw - 0.1)), right = (int)(round(dw + 0.1));
    cv::copyMakeBorder(rgb, img_new, top, bot, left, right, cv::BORDER_CONSTANT, cv::Scalar(114,114,114));
}

// === Привязка input-буфера к сети (для группы) ===
static vip_buffer bind_input(ModelState *state) {
    vip_buffer_create_params_t ip = {0};
    vip_query_input(state->network, 0, VIP_BUFFER_PROP_DATA_FORMAT, &ip.data_format);
    vip_query_input(state->network, 0, VIP_BUFFER_PROP_QUANT_FORMAT, &ip.quant_format);
    vip_query_input(state->network, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &ip.num_of_dims);
    vip_query_input(state->network, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, ip.sizes);
    ip.memory_type = VIP_BUFFER_MEMORY_TYPE_HOST;
    size_t in_size = 1;
    for (int i = 0; i < ip.num_of_dims; i++) in_size *= ip.sizes[i];
    in_size = (in_size + ALIGN - 1) & ~(ALIGN - 1);
    vip_buffer buf = NULL;
    vip_create_buffer_from_fd(&ip, state->slot.fd, in_size, &buf);
    return buf;
}

static void json_objects(const std::vector<Object>& objects) {
    int off = snprintf(g_json_objects, sizeof(g_json_objects), "[");
    for (size_t i = 0; i < objects.size() && off < (int)sizeof(g_json_objects) - 200; i++) {
        const auto& o = objects[i];
        off += snprintf(g_json_objects + off, sizeof(g_json_objects) - off,
            "%s{\"class_id\":%d,\"confidence\":%.3f,\"bbox\":[%.1f,%.1f,%.1f,%.1f]}",
            i>0?",":"", o.label, o.prob,
            o.rect.x, o.rect.y, o.rect.x+o.rect.width, o.rect.y+o.rect.height);
    }
    snprintf(g_json_objects + off, sizeof(g_json_objects) - off, "]");
}

static void json_pose(const std::vector<PoseObject>& poses) {
    int off = snprintf(g_json_pose, sizeof(g_json_pose), "[");
    for (size_t i = 0; i < poses.size() && off < (int)sizeof(g_json_pose) - 800; i++) {
        const auto& p = poses[i];
        off += snprintf(g_json_pose + off, sizeof(g_json_pose) - off,
            "%s{\"confidence\":%.3f,\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"keypoints\":[",
            i>0?",":"", p.prob,
            p.rect.x, p.rect.y, p.rect.x+p.rect.width, p.rect.y+p.rect.height);
        for (size_t k = 0; k < p.keypoints.size() && off < (int)sizeof(g_json_pose) - 50; k++) {
            off += snprintf(g_json_pose + off, sizeof(g_json_pose) - off,
                "%s[%.1f,%.1f,%.2f]", k>0?",":"",
                p.keypoints[k].p.x, p.keypoints[k].p.y, p.keypoints[k].prob);
        }
        off += snprintf(g_json_pose + off, sizeof(g_json_pose) - off, "]}");
    }
    snprintf(g_json_pose + off, sizeof(g_json_pose) - off, "]");
}

static void json_seg(const std::vector<SegObject>& segs) {
    int off = snprintf(g_json_seg, sizeof(g_json_seg), "[");
    for (size_t i = 0; i < segs.size() && off < (int)sizeof(g_json_seg) - 300; i++) {
        const auto& s = segs[i];
        off += snprintf(g_json_seg + off, sizeof(g_json_seg) - off,
            "%s{\"class_id\":%d,\"confidence\":%.3f,\"bbox\":[%.1f,%.1f,%.1f,%.1f]}",
            i>0?",":"", s.label, s.prob,
            s.rect.x, s.rect.y, s.rect.x+s.rect.width, s.rect.y+s.rect.height);
    }
    snprintf(g_json_seg + off, sizeof(g_json_seg) - off, "]");
}

// === Worker thread ===
static void worker_thread() {
    while (true) {
        FrameData frame;
        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait(lock, [] { return !g_queue.empty() || !g_running; });
            if (!g_running && g_queue.empty()) break;
            frame = g_queue.front();
            g_queue.pop();
        }
        auto tw0 = std::chrono::high_resolution_clock::now();

        // Letterbox ×4
        letterbox_to_rgb(frame.bgr, (unsigned char*)g_yolo26s.slot.ptr, 640, 640);
        // pose letterbox disabled
        letterbox_to_rgb(frame.bgr, (unsigned char*)g_seg.slot.ptr, 640, 640);
        // depth letterbox disabled

        // Привязка входов
        vip_buffer in_yolo = bind_input(&g_yolo26s);
        vip_buffer in_pose = NULL;
        vip_buffer in_seg = bind_input(&g_seg);
        vip_buffer in_depth = NULL;

        vip_set_input(g_yolo26s.network, 0, in_yolo);
        // pose set_input disabled
        vip_set_input(g_seg.network,     0, in_seg);
        // depth set_input disabled

        // === ГРУППОВОЙ ЗАПУСК 4 МОДЕЛЕЙ ===
        vip_status_e st = vip_run_group(g_group, 2);
        if (st != VIP_SUCCESS) printf("vip_run_group failed: %d\n", st);
        st = vip_wait_group(g_group);
        if (st != VIP_SUCCESS) printf("vip_wait_group failed: %d\n", st);

        vip_destroy_buffer(in_yolo);
        vip_destroy_buffer(in_pose);
        vip_destroy_buffer(in_seg);
        if (in_depth) vip_destroy_buffer(in_depth);

        // Собираем выходы
        float* out_yolo[12]; float* out_pose[12]; float* out_seg[12]; float* out_depth[12];
        for (int i = 0; i < g_yolo26s.num_outputs; i++) out_yolo[i] = (float*)vip_map_buffer(g_yolo26s.output_bufs[i]);
        // pose map disabled
        for (int i = 0; i < g_seg.num_outputs; i++)     out_seg[i]  = (float*)vip_map_buffer(g_seg.output_bufs[i]);
        // depth map disabled

        std::vector<Object> objects;
        detect_yolo26_6_post(frame.bgr, objects, out_yolo);
        for (int i = 0; i < g_yolo26s.num_outputs; i++) vip_unmap_buffer(g_yolo26s.output_bufs[i]);

        std::vector<PoseObject> poses;  // pose disabled

        std::vector<SegObject> segs;
        detect_seg_10_post(frame.bgr, segs, out_seg);
        for (int i = 0; i < g_seg.num_outputs; i++) vip_unmap_buffer(g_seg.output_bufs[i]);

// depth post disabled

        json_objects(objects);
        json_pose(poses);
        json_seg(segs);

        auto tw1 = std::chrono::high_resolution_clock::now();
        double worker_ms = std::chrono::duration<double, std::milli>(tw1 - tw0).count();
        g_sum_worker += worker_ms;

        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            g_frame_count++;
            if (g_frame_count % 5 == 0) {
                printf("Frame %d: worker=%.1f ms | objects=%zu poses=%zu segs=%zu\n",
                       g_frame_count, worker_ms, objects.size(), poses.size(), segs.size());
            }
        }
    }
    printf("[worker] exiting\n");
}

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
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

    munmap(ptr, size);
    gst_sample_unref(sample);

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (g_queue.size() >= QUEUE_MAX) g_queue.pop();
        g_queue.push({bgr.clone()});
    }
    g_queue_cv.notify_one();
    return GST_FLOW_OK;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    gst_init(&argc, &argv);
    printf("=== NPU Dispatcher Part 6 (group) ===\n");

    vip_init();
    if (load_model("/dev/shm/yolo26s_6_pcq_a733.nb", &g_yolo26s, 640, 640, "YOLO26s") < 0) return 1;
    if (load_model("/dev/shm/yolo11s-pose_9_uint8_a733.nb", &g_pose, 640, 640, "Pose") < 0) return 1;
    if (load_model("/dev/shm/yolo11s-seg_10_uint8_a733.nb", &g_seg, 640, 640, "Seg") < 0) return 1;
    if (load_model("/dev/shm/yolo26n-depth_pcq_a733.nb", &g_depth, 768, 768, "Depth") < 0) return 1;

    // === ГРУППА ===
    vip_status_e gst = vip_create_group(4, &g_group);
    printf("vip_create_group: %d\n", gst);
    if (gst != VIP_SUCCESS) return 1;

    // Set device ID перед add
    int device_id = 0;
    gst = vip_set_group(g_group, VIP_GROUP_PROP_SET_DEVICE_ID, &device_id);
    printf("vip_set_group (DEVICE_ID=0): %d\n", gst);

    // Set timeout
    int timeout_ms = 5000;
    gst = vip_set_group(g_group, VIP_GROUP_PROP_SET_TIME_OUT, &timeout_ms);
    printf("vip_set_group (TIME_OUT=5000): %d\n", gst);

    gst = vip_add_network(g_group, g_yolo26s.network); printf("add yolo: %d\n", gst);
    // POSE DISABLED FROM GROUP
    gst = vip_add_network(g_group, g_seg.network);     printf("add seg:  %d\n", gst);
    // DEPTH DISABLED FROM GROUP

    std::thread worker(worker_thread);

    const char *pipeline_str =
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=100 ! "
        "image/jpeg ! omxmjpegvideodec ! "
        "appsink name=npu_sink emit-signals=true sync=false max-buffers=2 drop=true";

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

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_running = false;
    }
    g_queue_cv.notify_all();
    worker.join();

    printf("\n=== Metrics ===\n");
    printf("Frames: %d\n", g_frame_count);
    if (g_frame_count > 0)
        printf("Avg worker: %.2f ms (%.1f FPS)\n",
               g_sum_worker / g_frame_count, 1000.0 / (g_sum_worker / g_frame_count));
    printf("Objects: %s\n", g_json_objects);
    printf("Pose:    %s\n", g_json_pose);
    printf("Seg:     %s\n", g_json_seg);
    printf("Depth:   %s\n", g_json_depth);

    vip_destroy_group(g_group);
    for (int i = 0; i < g_yolo26s.num_outputs; i++) vip_destroy_buffer(g_yolo26s.output_bufs[i]);
    vip_destroy_network(g_yolo26s.network);
    for (int i = 0; i < g_pose.num_outputs; i++) vip_destroy_buffer(g_pose.output_bufs[i]);
    vip_destroy_network(g_pose.network);
    for (int i = 0; i < g_seg.num_outputs; i++) vip_destroy_buffer(g_seg.output_bufs[i]);
    vip_destroy_network(g_seg.network);
    for (int i = 0; i < g_depth.num_outputs; i++) vip_destroy_buffer(g_depth.output_bufs[i]);
    vip_destroy_network(g_depth.network);
    vip_destroy();
    printf("DONE\n");
    return 0;
}
