#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#define OUTPUT_W 640
#define OUTPUT_H 640

struct SegObject { cv::Rect_<float> rect; int label; float prob; cv::Mat mask; };
extern int detect_seg_10_post(const cv::Mat& bgr, std::vector<SegObject>& objects, float **output);

static vip_network g_network;
static vip_buffer g_output_bufs[12];
static int g_num_outputs;
static int g_slot_fd;
static void *g_slot_ptr;
static int g_frames = 0;

static int create_dma_buf(size_t size, void **ptr_out) {
    int heap_fd = open("/dev/dma_heap/reserved", O_RDWR);
    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    struct dma_heap_allocation_data data = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) return -1;
    close(heap_fd);
    *ptr_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
    return data.fd;
}

static int load_seg() {
    FILE *f = fopen("/dev/shm/yolo11s-seg_10_uint8_a733.nb", "rb");
    if (!f) { perror("fopen"); return -1; }
    fseek(f, 0, SEEK_END); size_t sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *data = malloc(sz); fread(data, 1, sz, f); fclose(f);

    if (vip_create_network(data, sz, VIP_CREATE_NETWORK_FROM_MEMORY, &g_network) != VIP_SUCCESS) return -1;
    if (vip_prepare_network(g_network) != VIP_SUCCESS) return -1;

    vip_query_network(g_network, VIP_NETWORK_PROP_OUTPUT_COUNT, &g_num_outputs);
    printf("Seg outputs: %d\n", g_num_outputs);
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

    size_t in_size = OUTPUT_W * OUTPUT_H * 3;
    g_slot_fd = create_dma_buf(in_size, &g_slot_ptr);
    printf("Slot fd=%d\n", g_slot_fd);
    return 0;
}

static void letterbox(const cv::Mat& bgr, unsigned char* out) {
    float scale = std::min(640.0f/bgr.rows, 640.0f/bgr.cols);
    int rc = (int)round(scale * bgr.cols), rr = (int)round(scale * bgr.rows);
    int top = (640-rr)/2, left = (640-rc)/2;
    cv::Mat resized, rgb, img_new(640, 640, CV_8UC3, out);
    img_new.setTo(cv::Scalar(114,114,114));
    cv::resize(bgr, resized, cv::Size(rc, rr));
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    rgb.copyTo(img_new(cv::Rect(left, top, rc, rr)));
}

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMemory *mem = gst_buffer_peek_memory(buf, 0);
    if (!gst_is_dmabuf_memory(mem)) { gst_sample_unref(sample); return GST_FLOW_OK; }
    int fd = gst_dmabuf_memory_get_fd(mem);
    gsize size = gst_memory_get_sizes(mem, NULL, NULL);
    void *ptr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { gst_sample_unref(sample); return GST_FLOW_ERROR; }

    cv::Mat yv12(INPUT_H * 3 / 2, INPUT_W, CV_8UC1, ptr);
    cv::Mat bgr;
    cv::cvtColor(yv12, bgr, cv::COLOR_YUV2BGR_YV12);
    munmap(ptr, size);

    letterbox(bgr, (unsigned char*)g_slot_ptr);

    vip_buffer_create_params_t ip = {0};
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_DATA_FORMAT, &ip.data_format);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_QUANT_FORMAT, &ip.quant_format);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &ip.num_of_dims);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, ip.sizes);
    ip.memory_type = VIP_BUFFER_MEMORY_TYPE_HOST;
    size_t in_size = 1;
    for (int i = 0; i < ip.num_of_dims; i++) in_size *= ip.sizes[i];
    in_size = (in_size + ALIGN - 1) & ~(ALIGN - 1);
    vip_buffer in_buf = NULL;
    vip_create_buffer_from_fd(&ip, g_slot_fd, in_size, &in_buf);
    vip_set_input(g_network, 0, in_buf);
    vip_run_network(g_network);
    vip_destroy_buffer(in_buf);

    float* outputs[12];
    for (int i = 0; i < g_num_outputs; i++) outputs[i] = (float*)vip_map_buffer(g_output_bufs[i]);

    std::vector<SegObject> segs;
    detect_seg_10_post(bgr, segs, outputs);

    fprintf(stderr, "[DBG] before unmap\n");
    for (int i = 0; i < g_num_outputs; i++) vip_unmap_buffer(g_output_bufs[i]);
    fprintf(stderr, "[DBG] after unmap\n");

    g_frames++;
    if (g_frames % 10 == 0)
        printf("Frame %d: segs=%zu\n", g_frames, segs.size());

    fprintf(stderr, "[DBG] before unref (frame %d)\n", g_frames);
    gst_sample_unref(sample);
    fprintf(stderr, "[DBG] after unref\n");
    return GST_FLOW_OK;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    gst_init(&argc, &argv);
    printf("=== SEG ONLY CLEAN ===\n");

    vip_init();
    if (load_seg() < 0) { printf("load_seg failed\n"); return 1; }

    const char *pipeline_str =
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=50 ! "
        "image/jpeg ! omxmjpegvideodec ! "
        "appsink name=npu_sink emit-signals=true sync=false max-buffers=1 drop=true";

    GstElement *pipeline = gst_parse_launch(pipeline_str, NULL);
    GstAppSink *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "npu_sink"));
    g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    printf("Starting...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND,
        (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
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

    printf("Frames: %d\n", g_frames);
    printf("DONE\n");
    return 0;
}
