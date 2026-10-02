import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstApp', '1.0')
from gi.repository import Gst, GstApp
import threading
import time
import numpy as np

Gst.init(None)


class GstStream:
    def __init__(self, device='/dev/video0', width=1280, height=720, fps=30):
        self.device = device
        self.width = width
        self.height = height
        self.fps = fps
        self.pipeline = None
        self.appsink = None
        self.tee = None
        self.recording_branch = None
        self.latest_frame = None
        self.frame_lock = threading.Lock()
        self.pull_thread = None
        self.running = False
        self.recording = False

    def build_pipeline(self):
        pipeline_str = (
            f"v4l2src device={self.device} ! "
            f"image/jpeg,width={self.width},height={self.height},framerate={self.fps}/1 ! "
            f"jpegdec ! videoconvert ! video/x-raw,format=BGR ! "
            f"tee name=t "
            f"t. ! queue ! "
            f"appsink name=appsink max-buffers=1 drop=true sync=false "
            f"t. ! queue ! fakesink"
        )
        print(f"[GstStream] Building pipeline")
        self.pipeline = Gst.parse_launch(pipeline_str)
        self.appsink = self.pipeline.get_by_name('appsink')
        self.tee = self.pipeline.get_by_name('t')

    def _pull_loop(self):
        while self.running:
            try:
                sample = self.appsink.try_pull_sample(100000000)
                if sample:
                    buffer = sample.get_buffer()
                    success, map_info = buffer.map(Gst.MapFlags.READ)
                    if success:
                        arr = np.frombuffer(map_info.data, dtype=np.uint8)
                        arr = arr.reshape((self.height, self.width, 3)).copy()
                        with self.frame_lock:
                            self.latest_frame = arr
                        buffer.unmap(map_info)
            except Exception as e:
                print(f"[GstStream] pull error: {e}")
                time.sleep(0.01)

    def get_frame(self):
        with self.frame_lock:
            return self.latest_frame

    def start(self):
        if self.running:
            return
        self.build_pipeline()
        self.pipeline.set_state(Gst.State.PLAYING)
        self.running = True
        self.pull_thread = threading.Thread(target=self._pull_loop, daemon=True)
        self.pull_thread.start()
        print("[GstStream] Started")

    def stop(self):
        if not self.running:
            return
        self.stop_recording()
        self.running = False
        if self.pull_thread:
            self.pull_thread.join(timeout=1)
        self.pipeline.set_state(Gst.State.NULL)
        print("[GstStream] Stopped")

    def start_recording(self, record_file):
        if self.recording or not self.pipeline:
            return
        print(f"[GstStream] Creating recording branch for {record_file}")
        queue = Gst.ElementFactory.make("queue", "rec_queue")
        conv = Gst.ElementFactory.make("videoconvert", "rec_conv")
        encoder = Gst.ElementFactory.make("omxh264videoenc", "rec_encoder")
        parser = Gst.ElementFactory.make("h264parse", "rec_parser")
        muxer = Gst.ElementFactory.make("mp4mux", "rec_muxer")
        sink = Gst.ElementFactory.make("filesink", "rec_sink")
        sink.set_property("location", record_file)

        self.recording_branch = Gst.Bin.new("recording_branch")
        for el in [queue, conv, encoder, parser, muxer, sink]:
            self.recording_branch.add(el)

        queue.link(conv)
        conv.link(encoder)
        encoder.link(parser)
        parser.link(muxer)
        muxer.link(sink)

        sink_pad = queue.get_static_pad("sink")
        ghost_pad = Gst.GhostPad.new("sink", sink_pad)
        self.recording_branch.add_pad(ghost_pad)

        self.pipeline.add(self.recording_branch)
        tee_src_pad = self.tee.request_pad_simple("src_%u")
        tee_src_pad.link(ghost_pad)

        self.recording_branch.sync_state_with_parent()
        self.recording = True
        print(f"[GstStream] Recording branch added.")

    def stop_recording(self):
        if not self.recording or not self.recording_branch:
            return
        print("[GstStream] Stopping recording branch...")
        self.recording_branch.send_event(Gst.Event.new_eos())
        bus = self.pipeline.get_bus()
        bus.timed_pop_filtered(2 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
        ghost_pad = self.recording_branch.get_static_pad("sink")
        if ghost_pad:
            tee_pad = ghost_pad.get_peer()
            if tee_pad:
                tee_pad.unlink(ghost_pad)
                self.tee.release_request_pad(tee_pad)
        self.recording_branch.set_state(Gst.State.NULL)
        self.pipeline.remove(self.recording_branch)
        self.recording_branch = None
        self.recording = False
        print("[GstStream] Recording branch removed.")


if __name__ == '__main__':
    stream = GstStream()
    stream.start()
    time.sleep(2)
    frame = stream.get_frame()
    if frame is not None:
        print(f"Frame shape: {frame.shape}, dtype: {frame.dtype}")
    else:
        print("No frame")
    stream.stop()
