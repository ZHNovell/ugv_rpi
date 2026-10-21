#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <cstdio>
#include <cstdlib>
#include <vip_lite.h>

static int frame_count = 0;
static vip_network g_network = NULL;

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMemory *mem = gst_buffer_peek_memory(buf, 0);
    if (gst_is_dmabuf_memory(mem)) {
        int fd = gst_dmabuf_memory_get_fd(mem);
        printf("[CB] frame %d fd=%d\n", ++frame_count, fd);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    gst_init(&argc, &argv);

    printf("=== Debug 6: vip_init + load_model THEN pipeline ===\n");

    // NPU init ДО pipeline
    vip_status_e st = vip_init();
    printf("vip_init: %d\n", st);

    FILE *f = fopen("/dev/shm/yolo26s_6_pcq_a733.nb", "rb");
    fseek(f, 0, SEEK_END); size_t sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *data = malloc(sz);
    fread(data, 1, sz, f); fclose(f);

    st = vip_create_network(data, sz, VIP_CREATE_NETWORK_FROM_MEMORY, &g_network);
    printf("vip_create_network: %d\n", st);

    st = vip_prepare_network(g_network);
    printf("vip_prepare_network: %d\n", st);

    // GStreamer
    GstElement *pipeline = gst_parse_launch(
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=10 ! "
        "image/jpeg ! omxmjpegvideodec ! "
        "tee name=t "
        "t. ! queue ! appsink name=npu_sink emit-signals=true sync=false max-buffers=1 drop=true "
        "t. ! queue ! videoconvert ! video/x-raw,format=BGR ! "
        "appsink name=ui_sink emit-signals=true sync=false max-buffers=1 drop=true",
        NULL);
    printf("pipeline: %p\n", pipeline);

    GstAppSink *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "npu_sink"));
    g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    printf("PLAYING...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessageType types = (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 15 * GST_SECOND, types);

    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = NULL; gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            printf("ERROR: %s\n%s\n", err->message, dbg ? dbg : "");
        } else {
            printf("EOS\n");
        }
        gst_message_unref(msg);
    } else {
        printf("TIMEOUT (no EOS, no callback?)\n");
    }

    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("Frames: %d\n", frame_count);
    printf("DONE\n");
    return 0;
}
