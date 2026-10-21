// Этап 1: GStreamer → YV12 → BGR → SHM (для Python overlay)
// Пока без NPU. Цель: Python читает BGR из SHM.

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
#include <sys/stat.h>
#include <linux/dma-heap.h>
#include <opencv2/opencv.hpp>

#define INPUT_W 1920
#define INPUT_H 1088
#define BGR_SIZE (INPUT_W * INPUT_H * 3)
#define SHM_SLOTS 4
#define SHM_NAME "/ugv_bgr_shm"
#define HEADER_SIZE 64  // frame_id (8) + width (4) + height (4) + channels (4) + write_idx (4) + padding

struct ShmHeader {
    volatile uint64_t frame_id;
    volatile uint32_t width;
    volatile uint32_t height;
    volatile uint32_t channels;
    volatile uint32_t write_idx;
    volatile uint32_t reserved[11];  // до 64 байт
};

static int g_frame_count = 0;
static int g_write_idx = 0;

// SHM: header + 4 слота × BGR_SIZE
static size_t shm_size() {
    return HEADER_SIZE + SHM_SLOTS * BGR_SIZE;
}

static int init_shm(void **ptr_out) {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return -1; }
    size_t size = shm_size();
    if (ftruncate(fd, size) < 0) { perror("ftruncate"); close(fd); return -1; }
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap"); close(fd); return -1; }

    struct ShmHeader *hdr = (struct ShmHeader*)ptr;
    hdr->width = INPUT_W;
    hdr->height = INPUT_H;
    hdr->channels = 3;
    hdr->write_idx = 0;
    hdr->frame_id = 0;

    *ptr_out = ptr;
    return fd;
}

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
    void *shm_ptr = user_data;
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

    munmap(ptr, size);
    gst_sample_unref(sample);

    // Пишем BGR в SHM
    struct ShmHeader *hdr = (struct ShmHeader*)shm_ptr;
    uint32_t slot = g_write_idx % SHM_SLOTS;
    unsigned char *shm_bgr = (unsigned char*)shm_ptr + HEADER_SIZE + slot * BGR_SIZE;

    memcpy(shm_bgr, bgr.data, BGR_SIZE);

    hdr->write_idx = slot;
    hdr->frame_id++;
    auto t4 = std::chrono::high_resolution_clock::now();

    g_frame_count++;
    g_write_idx++;

    double mmap_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double cvt_ms  = std::chrono::duration<double, std::milli>(t3 - t2).count();
    double shm_ms  = std::chrono::duration<double, std::milli>(t4 - t3).count();
    double total_ms= std::chrono::duration<double, std::milli>(t4 - t0).count();

    if (g_frame_count % 20 == 0) {
        printf("Frame %d: mmap=%.2f cvt=%.2f shm=%.2f total=%.2f ms | slot=%u frame_id=%lu\n",
               g_frame_count, mmap_ms, cvt_ms, shm_ms, total_ms, slot,
               (unsigned long)hdr->frame_id);
    }

    return GST_FLOW_OK;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    gst_init(&argc, &argv);
    printf("=== Stage 1: GStreamer → BGR → SHM ===\n");

    void *shm_ptr = NULL;
    int shm_fd = init_shm(&shm_ptr);
    if (shm_fd < 0) return 1;
    printf("SHM created: %s (size=%zu)\n", SHM_NAME, shm_size());

    const char *pipeline_str =
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=200 ! "
        "image/jpeg ! omxmjpegvideodec ! "
        "appsink name=npu_sink emit-signals=true sync=false max-buffers=1 drop=true";

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_str, &error);
    if (!pipeline) { printf("Pipeline failed\n"); return 1; }

    GstAppSink *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "npu_sink"));
    g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), shm_ptr);

    printf("Starting pipeline...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessageType types = (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND, types);
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
    printf("SHM: %s (Python может читать)\n", SHM_NAME);
    printf("DONE\n");
    return 0;
}
