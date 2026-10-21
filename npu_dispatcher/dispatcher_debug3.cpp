#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <cstdio>

static int npu_count = 0;
static int ui_count = 0;

static GstFlowReturn on_npu_sample(GstAppSink *sink, gpointer user_data) {
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMemory *mem = gst_buffer_peek_memory(buf, 0);
    if (gst_is_dmabuf_memory(mem)) {
        int fd = gst_dmabuf_memory_get_fd(mem);
        printf("[NPU] Frame %d: fd=%d\n", ++npu_count, fd);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static GstFlowReturn on_ui_sample(GstAppSink *sink, gpointer user_data) {
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstMemory *mem = gst_buffer_peek_memory(buf, 0);
    const char *kind = gst_is_dmabuf_memory(mem) ? "DMABUF" : "SYSTEM";
    printf("[UI ] Frame %d: %s, caps=%s\n", ++ui_count, kind, gst_caps_to_string(caps));
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);

    // Тест 2: tee + appsink (NPU) + videoconvert + appsink (UI)
    GstElement *pipeline = gst_parse_launch(
        "v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=30 ! "
        "image/jpeg ! "
        "omxmjpegvideodec ! "
        "tee name=t "
        "t. ! queue ! appsink name=npu_sink emit-signals=true sync=false max-buffers=1 drop=true "
        "t. ! queue ! videoconvert ! video/x-raw,format=BGR ! "
        "appsink name=ui_sink emit-signals=true sync=false max-buffers=1 drop=true",
        NULL);
    if (!pipeline) { printf("pipeline failed\n"); return 1; }

    GstAppSink *npu_sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "npu_sink"));
    GstAppSink *ui_sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "ui_sink"));
    g_signal_connect(npu_sink, "new-sample", G_CALLBACK(on_npu_sample), NULL);
    g_signal_connect(ui_sink, "new-sample", G_CALLBACK(on_ui_sample), NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessageType types = (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND, types);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            printf("ERROR: %s\n%s\n", err->message, dbg ? dbg : "");
        } else {
            printf("EOS\n");
        }
        gst_message_unref(msg);
    } else {
        printf("TIMEOUT\n");
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("NPU frames: %d, UI frames: %d\n", npu_count, ui_count);
    printf("DONE\n");
    return 0;
}
