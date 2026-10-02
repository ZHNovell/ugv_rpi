import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstApp', '1.0')
from gi.repository import Gst, GstApp, GLib
import threading
import time
import numpy as np

Gst.init(None)


class GstStream:
    def __init__(self, device='/dev/video0', width=1920, height=1080, fps=30):
        self.device = device
        self.width = width
        self.height = height
        self.fps = fps

        self.pipeline = None
        self.appsink = None
        self.tee = None

        # recording branch elements (None when not recording)
        self.rec_queue = None
        self.rec_convert = None
        self.rec_scale = None
        self.rec_caps = None
        self.rec_encoder = None
        self.rec_mux = None
        self.rec_sink = None
        self.rec_elements = []
        self.rec_tee_pad = None

        self.latest_frame = None
        self.frame_lock = threading.Lock()
        self.loop = None
        self.loop_thread = None
        self.running = False
        self.recording = False
        self.rec_lock = threading.Lock()

    # ---------- pipeline ----------
    def build_pipeline(self):
        pipeline_str = (
            f"v4l2src device={self.device} ! "
            f"image/jpeg,width={self.width},height={self.height},framerate={self.fps}/1 ! "
            f"jpegdec ! tee name=t "
            f"t. ! queue max-size-buffers=2 leaky=downstream ! "
            f"videoconvert ! video/x-raw,format=BGR ! "
            f"appsink name=appsink max-buffers=1 drop=true emit-signals=true sync=false"
        )
        print("[GstStream] Building pipeline (with tee)")
        self.pipeline = Gst.parse_launch(pipeline_str)
        self.appsink = self.pipeline.get_by_name('appsink')
        self.tee = self.pipeline.get_by_name('t')
        self.appsink.connect('new-sample', self._on_new_sample)

    def _on_new_sample(self, sink):
        sample = sink.emit('pull-sample')
        if sample:
            buffer = sample.get_buffer()
            success, map_info = buffer.map(Gst.MapFlags.READ)
            if success:
                try:
                    arr = np.frombuffer(map_info.data, dtype=np.uint8)
                    arr = arr.reshape((self.height, self.width, 3)).copy()
                    with self.frame_lock:
                        self.latest_frame = arr
                finally:
                    buffer.unmap(map_info)
        return Gst.FlowReturn.OK

    def get_frame(self):
        with self.frame_lock:
            return self.latest_frame

    def _run_loop(self):
        self.loop = GLib.MainLoop()
        self.loop.run()

    def start(self):
        if self.running:
            return
        self.build_pipeline()
        self.pipeline.set_state(Gst.State.PLAYING)
        self.loop_thread = threading.Thread(target=self._run_loop, daemon=True)
        self.loop_thread.start()
        self.running = True
        print("[GstStream] Started")

    def stop(self):
        if not self.running:
            return
        if self.recording:
            try:
                self.stop_recording()
            except Exception as e:
                print(f"[GstStream] stop_recording during stop failed: {e}")
        self.pipeline.set_state(Gst.State.NULL)
        if self.loop:
            self.loop.quit()
        self.running = False
        print("[GstStream] Stopped")

    # ---------- recording ----------
    def start_recording(self, path):
        """Dynamically add a recording branch to the live pipeline.
        The live appsink branch continues running — viewing is NOT interrupted."""
        with self.rec_lock:
            if self.recording:
                print("[GstStream] already recording")
                return
            if self.pipeline is None:
                raise RuntimeError("pipeline not started")

            print(f"[GstStream] start_recording -> {path}")

            self.rec_queue   = Gst.ElementFactory.make("queue", "rec_queue")
            self.rec_queue.set_property("max-size-buffers", 30)
            self.rec_queue.set_property("max-size-bytes", 0)
            self.rec_queue.set_property("max-size-time", 0)
            self.rec_convert = Gst.ElementFactory.make("videoconvert", "rec_convert")
            self.rec_scale   = Gst.ElementFactory.make("videoscale", "rec_scale")
            self.rec_caps    = Gst.ElementFactory.make("capsfilter", "rec_caps")
            self.rec_encoder = Gst.ElementFactory.make("avenc_mjpeg", "rec_encoder")
            self.rec_parse   = None  # not needed for jpeg
            self.rec_mux     = Gst.ElementFactory.make("matroskamux", "rec_mux")
            self.rec_sink    = Gst.ElementFactory.make("filesink", "rec_sink")

            if not all([self.rec_queue, self.rec_convert, self.rec_scale,
                        self.rec_caps, self.rec_encoder, self.rec_mux, self.rec_sink]):
                raise RuntimeError("failed to create one of recording elements")

            # scale to 1280x720 and use I420 for jpegenc (soft encoder)
            caps = Gst.Caps.from_string(
                f"video/x-raw,format=I420,width=1280,height=720,framerate={self.fps}/1"
            )
            self.rec_caps.set_property("caps", caps)

            # avenc_mjpeg: target bitrate ~ 8 Mbit/s для 720p30, + quality 80
            try:
                self.rec_encoder.set_property("bitrate", 8000000)
            except Exception as e:
                print(f"[GstStream] set avenc_mjpeg bitrate failed: {e}", flush=True)
            try:
                self.rec_encoder.set_property("qmin", 3)
                self.rec_encoder.set_property("qmax", 10)
            except Exception:
                pass

            self.rec_sink.set_property("location", path)
            self.rec_sink.set_property("sync", False)
            self.rec_sink.set_property("async", False)

            self.rec_elements = [self.rec_queue, self.rec_convert, self.rec_scale,
                                 self.rec_caps, self.rec_encoder, self.rec_mux, self.rec_sink]

            for el in self.rec_elements:
                self.pipeline.add(el)

            if not self.rec_queue.link(self.rec_convert):
                raise RuntimeError("link rec_queue->rec_convert failed")
            if not self.rec_convert.link(self.rec_scale):
                raise RuntimeError("link rec_convert->rec_scale failed")
            if not self.rec_scale.link(self.rec_caps):
                raise RuntimeError("link rec_scale->rec_caps failed")
            if not self.rec_caps.link(self.rec_encoder):
                raise RuntimeError("link rec_caps->rec_encoder failed")
            if not self.rec_encoder.link(self.rec_mux):
                raise RuntimeError("link rec_encoder->rec_mux failed")
            if not self.rec_mux.link(self.rec_sink):
                raise RuntimeError("link rec_mux->rec_sink failed")

            # request pad from tee and link to rec_queue
            self.rec_tee_pad = self.tee.request_pad_simple("src_%u")
            if self.rec_tee_pad is None:
                raise RuntimeError("tee.request_pad_simple returned None")
            queue_sink_pad = self.rec_queue.get_static_pad("sink")
            if self.rec_tee_pad.link(queue_sink_pad) != Gst.PadLinkReturn.OK:
                raise RuntimeError("tee->rec_queue link failed")

            # sync state with pipeline
            for el in self.rec_elements:
                el.sync_state_with_parent()

            self.recording = True
            # дать videoconvert+encoder время завершить negotiation
            time.sleep(0.5)
            print("[GstStream] recording started")

    def stop_recording(self):
        """Send EOS to recording branch, wait, tear it down. Live branch unaffected."""
        with self.rec_lock:
            if not self.recording:
                print("[GstStream] not recording")
                return

            print("[GstStream] stopping recording...")

            # === 1. Сначала ОТЛИНКОВАТЬ ветку записи от tee ===
            try:
                if self.rec_tee_pad is not None and self.rec_queue is not None:
                    self.rec_tee_pad.unlink(self.rec_queue.get_static_pad("sink"))
                    self.tee.release_request_pad(self.rec_tee_pad)
                    print("[GstStream] tee pad released", flush=True)
            except Exception as e:
                print(f"[GstStream] release tee pad failed: {e}", flush=True)

            # === 2. Теперь безопасно отправить EOS в ветку записи ===
            # Отправляем через SINK-pad rec_queue, чтобы EOS гарантированно пошёл вниз по ветке.
            try:
                if self.rec_queue is not None:
                    rec_queue_sink = self.rec_queue.get_static_pad("sink")
                    if rec_queue_sink is not None:
                        rec_queue_sink.send_event(Gst.Event.new_eos())
                        print("[GstStream] EOS sent via rec_queue sink pad", flush=True)
                    else:
                        self.rec_queue.send_event(Gst.Event.new_eos())
                        print("[GstStream] EOS sent via rec_queue element", flush=True)
            except Exception as e:
                print(f"[GstStream] EOS send failed: {e}", flush=True)

            # === 3. Дождаться EOS на bus (max 5 сек) ===
            bus = self.pipeline.get_bus()
            deadline = time.time() + 5.0
            got_eos = False
            while time.time() < deadline:
                msg = bus.timed_pop_filtered(100 * Gst.MSECOND,
                                             Gst.MessageType.EOS | Gst.MessageType.ERROR)
                if msg is None:
                    continue
                if msg.type == Gst.MessageType.EOS:
                    got_eos = True
                    break
                if msg.type == Gst.MessageType.ERROR:
                    err, dbg = msg.parse_error()
                    print(f"[GstStream] bus ERROR during stop: {err} / {dbg}", flush=True)
                    break
            print(f"[GstStream] EOS received: {got_eos}", flush=True)

            # === 4. Убрать элементы ветки в NULL ===
            for el in self.rec_elements:
                try:
                    el.set_state(Gst.State.NULL)
                except Exception:
                    pass

            # === 5. Удалить из pipeline ===
            for el in self.rec_elements:
                try:
                    self.pipeline.remove(el)
                except Exception:
                    pass

            # reset refs
            self.rec_queue = self.rec_convert = self.rec_scale = None
            self.rec_caps = self.rec_encoder = self.rec_mux = self.rec_sink = None
            self.rec_elements = []
            self.rec_tee_pad = None
            self.recording = False
            print("[GstStream] recording stopped")


if __name__ == '__main__':
    stream = GstStream()
    stream.start()
    time.sleep(2)
    frame = stream.get_frame()
    if frame is not None:
        print(f"Frame shape: {frame.shape}")
    else:
        print("No frame")
    stream.start_recording('/tmp/test_rec.mkv')
    time.sleep(5)
    stream.stop_recording()
    time.sleep(1)
    stream.stop()
