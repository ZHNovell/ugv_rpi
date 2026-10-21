// npu_dispatcher — Часть 1: GStreamer + dma-buf + конвертация
// Цель: принять YV12 dma-buf из VPU, конвертировать в BGR 640×640,
//       записать в свой dma-buf (для NPU).
// Пока без NPU.

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

#define ALIGN 64
#define INPUT_W 1920
#define INPUT_H 1088
#define OUTPUT_W 640
#define OUTPUT_H 640

// === Ring buffer из 4 dma-buf (для NPU) ===
struct DmaBufSlot {
    int fd;
    void *ptr;
    size_t size;
};

static DmaBufSlot g_slots[4];
static int g_write_idx = 0;
static int g_frame_count = 0;

// === Метрики ===
struct Metrics {
    double mmap_ms;
    double cvt_ms;
    double resize_ms;
    double write_ms;
    double total_ms;
};

static Metrics g_metrics = {0};

// === Создание своего dma-buf ===
static int create_dma_buf(size_t size, void **ptr_out) {
    int heap_fd = open("/dev/dma_heap/reserved", O_RDWR);
    if (heap_fd < 0) { perror("open dma_heap"); return -1; }

    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    struct dma_heap_allocation_data data = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
        .heap_flags = 0,
    };
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        close(heap_fd);
        return -1;
    }
    close(heap_fd);

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(data.fd);
        return -1;
    }
    *ptr_out = ptr;
    return data.fd;
}

// === Инициализация ring buffer (4 слота) ===
static int init_ring_buffer() {
    size_t size = OUTPUT_W * OUTPUT_H * 3;
    for (int i = 0; i < 4; i++) {
        g_slots[i].fd = create_dma_buf(size, &g_slots[i].ptr);
        if (g_slots[i].fd < 0) {
            printf("Failed to create slot %d\n", i);
            return -1;
        }
        g_slots[i].size = (size + ALIGN - 1) & ~(ALIGN - 1);
        printf("Slot %d: fd=%d, ptr=%p, size=%zu\n", i, g_slots[i].fd, g_slots[i].ptr, g_slots[i].size);
    }
    return 0;
}

// === Callback на новый кадр ===
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
    auto t0 = std::chrono::high_resolution_clock::now();

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMemory *mem = gst_buffer_peek_memory(buf, 0);

    if (!gst_is_dmabuf_memory(mem)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    int fd = gst_dmabuf_memory_get_fd(mem);
    gsize size = gst_memory_get_sizes(mem, NULL, NULL);

    // === 1. mmap dma-buf ===
    auto t1 = std::chrono::high_resolution_clock::now();
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    auto t2 = std::chrono::high_resolution_clock::now();
    if (ptr == MAP_FAILED) {
        perror("mmap");
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    // === 2. YV12 → BGR ===
    cv::Mat yv12(INPUT_H * 3 / 2, INPUT_W, CV_8UC1, ptr);
    cv::Mat bgr;
    cv::cvtColor(yv12, bgr, cv::COLOR_YUV2BGR_YV12);
    auto t3 = std::chrono::high_resolution_clock::now();

    // === 3. Resize 1920×1088 → 640×640 ===
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(OUTPUT_W, OUTPUT_H));
    auto t4 = std::chrono::high_resolution_clock::now();

    // === 4. Запись в свой dma-buf (slot) ===
    DmaBufSlot &slot = g_slots[g_write_idx];
    // BGR → RGB (NPU ждёт RGB)
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    memcpy(slot.ptr, rgb.data, OUTPUT_W * OUTPUT_H * 3);
    auto t5 = std::chrono::high_resolution_clock::now();

    // Метрики
    double mmap_ms   = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double cvt_ms    = std::chrono::duration<double, std::milli>(t3 - t2).count();
    double resize_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
    double write_ms  = std::chrono::duration<double, std::milli>(t5 - t4).count();
    double total_ms  = std::chrono::duration<double, std::milli>(t5 - t0).count();

    g_metrics.mmap_ms   += mmap_ms;
    g_metrics.cvt_ms    += cvt_ms;
    g_metrics.resize_ms += resize_ms;
    g_metrics.write_ms  += write_ms;
    g_metrics.total_ms  += total_ms;

    g_frame_count++;
    if (g_frame_count % 30 == 0) {
        printf("Frame %d: mmap=%.2f, cvt=%.2f, resize=%.2f, write=%.2f, total=%.2f ms\n",
               g_frame_count, mmap_ms, cvt_ms, resize_ms, write_ms, total_ms);
    }

    munmap(ptr, size);
    g_write_idx = (g_write_idx + 1) % 4;

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);

    printf("=== NPU Dispatcher Part 1 ===\n");
    printf("Input: %dx%d YV12 → Output: %dx%d RGB\n", INPUT_W, INPUT_H, OUTPUT_W, OUTPUT_H);

    // === Ring buffer для NPU ===
    if (init_ring_buffer() < 0) {
        printf("Failed to init ring buffer\n");
        return 1;
    }

    // === GStreamer pipeline ===
    const char *pipeline_str =
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=300 ! "
        "image/jpeg ! "
        "omxmjpegvideodec ! "
        "tee name=t "
        "t. ! queue leaky=downstream max-size-buffers=2 ! "
        "appsink name=npu_sink emit-signals=true sync=false max-buffers=1 drop=true "
        "t. ! queue leaky=downstream max-size-buffers=2 ! "
        "videoconvert ! video/x-raw,format=BGR ! "
        "appsink name=ui_sink emit-signals=true sync=false max-buffers=1 drop=true";

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_str, &error);
    if (!pipeline) {
        printf("Pipeline failed: %s\n", error ? error->message : "unknown");
        return 1;
    }

    GstAppSink *npu_sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "npu_sink"));
    if (!npu_sink) { printf("npu_sink not found\n"); return 1; }
    g_signal_connect(npu_sink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    printf("Starting pipeline...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // === Ждём EOS (300 кадров ≈ 5 сек при 60 fps) ===
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessageType types = (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND, types);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            printf("ERROR: %s\n%s\n", err->message, dbg ? dbg : "");
            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
        } else {
            printf("EOS\n");
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    // === Итоги ===
    if (g_frame_count > 0) {
        printf("\n=== Metrics (avg per frame) ===\n");
        printf("mmap:   %.2f ms\n", g_metrics.mmap_ms / g_frame_count);
        printf("cvt:    %.2f ms\n", g_metrics.cvt_ms / g_frame_count);
        printf("resize: %.2f ms\n", g_metrics.resize_ms / g_frame_count);
        printf("write:  %.2f ms\n", g_metrics.write_ms / g_frame_count);
        printf("total:  %.2f ms (%.1f FPS)\n",
               g_metrics.total_ms / g_frame_count,
               1000.0 / (g_metrics.total_ms / g_frame_count));
    }
    printf("Frames processed: %d\n", g_frame_count);

    // Cleanup
    for (int i = 0; i < 4; i++) {
        munmap(g_slots[i].ptr, g_slots[i].size);
        close(g_slots[i].fd);
    }

    printf("DONE\n");
    return 0;
}
