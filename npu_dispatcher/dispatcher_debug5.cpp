#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <opencv2/opencv.hpp>
#include <vip_lite.h>

static int frame_count = 0;
static vip_network g_network = NULL;

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
    printf("[CB] frame %d START\n", ++frame_count); fflush(stdout);

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) { printf("[CB] no sample\n"); fflush(stdout); return GST_FLOW_ERROR; }
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMemory *mem = gst_buffer_peek_memory(buf, 0);
    if (!gst_is_dmabuf_memory(mem)) {
        printf("[CB] not dmabuf\n"); fflush(stdout);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    int fd = gst_dmabuf_memory_get_fd(mem);
    gsize size = gst_memory_get_sizes(mem, NULL, NULL);
    printf("[CB] frame %d dmabuf fd=%d size=%zu\n", frame_count, fd, size); fflush(stdout);

    // === mmap ===
    printf("[CB] frame %d before mmap\n", frame_count); fflush(stdout);
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap"); gst_sample_unref(sample); return GST_FLOW_ERROR; }
    printf("[CB] frame %d after mmap\n", frame_count); fflush(stdout);

    // === YV12 → BGR ===
    printf("[CB] frame %d before cvtColor\n", frame_count); fflush(stdout);
    cv::Mat yv12(1088 * 3 / 2, 1920, CV_8UC1, ptr);
    cv::Mat bgr;
    cv::cvtColor(yv12, bgr, cv::COLOR_YUV2BGR_YV12);
    printf("[CB] frame %d after cvtColor (bgr=%dx%d)\n", frame_count, bgr.cols, bgr.rows); fflush(stdout);

    // === resize ===
    printf("[CB] frame %d before resize\n", frame_count); fflush(stdout);
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(640, 640));
    printf("[CB] frame %d after resize\n", frame_count); fflush(stdout);

    munmap(ptr, size);

    // === NPU ===
    printf("[CB] frame %d before vip_create_buffer\n", frame_count); fflush(stdout);
    // ... (только create_buffer_from_fd, без run)
    vip_buffer_create_params_t ip = {0};
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_DATA_FORMAT, &ip.data_format);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_QUANT_FORMAT, &ip.quant_format);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &ip.num_of_dims);
    vip_query_input(g_network, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, ip.sizes);
    ip.memory_type = VIP_BUFFER_MEMORY_TYPE_HOST;
    size_t in_size = 1;
    for (int i = 0; i < ip.num_of_dims; i++) in_size *= ip.sizes[i];
    in_size = (in_size + 63) & ~63;
    vip_buffer input_buf = NULL;
    vip_status_e status = vip_create_buffer_from_fd(&ip, fd, in_size, &input_buf);
    printf("[CB] frame %d after vip_create_buffer: status=%d\n", frame_count, status); fflush(stdout);
    if (status == VIP_SUCCESS) vip_destroy_buffer(input_buf);

    gst_sample_unref(sample);
    printf("[CB] frame %d END\n", frame_count); fflush(stdout);
    return GST_FLOW_OK;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    printf("=== Debug 5 ===\n"); fflush(stdout);

    vip_init();
    FILE *f = fopen("/dev/shm/yolo26s_6_pcq_a733.nb", "rb");
    fseek(f, 0, SEEK_END); size_t sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *data = malloc(sz);
    fread(data, 1, sz, f); fclose(f);
    vip_create_network(data, sz, VIP_CREATE_NETWORK_FROM_MEMORY, &g_network);
    vip_prepare_network(g_network);

    GstElement *pipeline = gst_parse_launch(
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=10 ! "
        "image/jpeg ! omxmjpegvideodec ! "
        "appsink name=npu_sink emit-signals=true sync=false max-buffers=1 drop=true",
        NULL);

    GstAppSink *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "npu_sink"));
    g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessageType types = (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND, types);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = NULL; gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            printf("ERROR: %s\n%s\n", err->message, dbg ? dbg : "");
        } else { printf("EOS\n"); }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    printf("Frames: %d\n", frame_count);
    printf("DONE\n");
    return 0;
}
