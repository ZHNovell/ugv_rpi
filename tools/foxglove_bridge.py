#!/usr/bin/env python3
"""
foxglove_bridge.py — Foxglove Studio WebSocket bridge for the UGV SLAM system.

Reads Cartographer SLAM output from /dev/shm (read-only) and publishes to
Foxglove Studio over the "foxglove.websocket.v1" subprotocol:

  /map         foxglove.Grid          occupancy grid (1000x1000, 0.05 m/cell)
  /pose        foxglove.PoseInFrame   robot pose
  /trajectory  foxglove.Path          robot trajectory

All channels use the "cdr" (ROS 2 CDR) encoding.

CDR notes (matching Foxglove's decoder — @foxglove/cdr /
@foxglove/rosmsg2-serialization):
  * Each message payload begins with the 4-byte RTPS encapsulation header
    00 01 00 00 (PLAIN_CDR, little-endian, a.k.a. CDR_LE).
  * 64-bit primitives (uint64/float64) are aligned to 8 bytes; 32-bit
    primitives to 4 bytes; 8-bit primitives have no alignment.
  * foxglove.Grid / foxglove.PoseInFrame / foxglove.Path use
    `builtin_interfaces/Time` (int32 sec + uint32 nanosec), not a single
    uint64 nanoseconds field.
"""

import asyncio
import base64
import hashlib
import json
import math
import struct
import time

try:
    from websockets.asyncio.server import serve  # websockets >= 13
except ImportError:  # pragma: no cover - older websockets
    from websockets import serve

HOST = "0.0.0.0"
PORT = 8765

MAP_PATH = "/dev/shm/ugv_slam_map.raw"
POSE_PATH = "/dev/shm/ugv_slam_pose.json"

GRID_COLUMNS = 1000
GRID_ROWS = 1000
MAP_DATA_LEN = GRID_COLUMNS * GRID_ROWS  # 1,000,000 bytes
RESOLUTION = 0.05
GRID_ORIGIN_X = -25.0  # bottom-left corner (gx=0 -> x = -25 m)
GRID_ORIGIN_Y = -25.0

TICK_SECONDS = 0.2       # 5 Hz
MAP_MIN_INTERVAL = 1.0   # republish the map at least once per second
MAX_PATH_POINTS = 500

# ---------------------------------------------------------------------------
# Schema definitions (self-contained ROS 2 .msg text, base64-encoded in the
# advertise message). These match the official foxglove-sdk schemas.
# ---------------------------------------------------------------------------
GRID_SCHEMA = (
    "builtin_interfaces/Time timestamp\n"
    "string frame_id\n"
    "geometry_msgs/Pose pose\n"
    "uint32 column_count\n"
    "foxglove_msgs/Vector2 cell_size\n"
    "uint32 row_stride\n"
    "uint32 cell_stride\n"
    "foxglove_msgs/PackedElementField[] fields\n"
    "uint8[] data\n"
)

POSE_IN_FRAME_SCHEMA = (
    "builtin_interfaces/Time timestamp\n"
    "string frame_id\n"
    "geometry_msgs/Pose pose\n"
)

PATH_SCHEMA = (
    "builtin_interfaces/Time timestamp\n"
    "string frame_id\n"
    "geometry_msgs/Pose[] poses\n"
)

CHANNELS = [
    {
        "id": 1,
        "topic": "/map",
        "encoding": "cdr",
        "schemaName": "foxglove_msgs/Grid",
        "schema": base64.b64encode(GRID_SCHEMA.encode("utf-8")).decode("ascii"),
    },
    {
        "id": 2,
        "topic": "/pose",
        "encoding": "cdr",
        "schemaName": "foxglove_msgs/PoseInFrame",
        "schema": base64.b64encode(POSE_IN_FRAME_SCHEMA.encode("utf-8")).decode("ascii"),
    },
    {
        "id": 3,
        "topic": "/trajectory",
        "encoding": "cdr",
        "schemaName": "foxglove_msgs/Path",
        "schema": base64.b64encode(PATH_SCHEMA.encode("utf-8")).decode("ascii"),
    },
]

SERVER_INFO = {
    "op": "serverInfo",
    "name": "ugv-foxglove-bridge",
    "capabilities": [],
    "supportedEncodings": ["cdr"],
}

ADVERTISE = {"op": "advertise", "channels": CHANNELS}


# ---------------------------------------------------------------------------
# CDR writer (CDR1 little-endian). The message *body* is built with alignment
# relative to body offset 0; the 4-byte CDR_LE header is prepended at the end.
# This is equivalent to @foxglove/cdr aligning against origin=4.
# ---------------------------------------------------------------------------
class CdrWriter:
    __slots__ = ("buf",)

    def __init__(self):
        self.buf = bytearray()

    def _align(self, n):
        pad = (-len(self.buf)) % n
        if pad:
            self.buf.extend(b"\x00" * pad)

    def uint8(self, value):
        self.buf.append(int(value) & 0xFF)

    def uint32(self, value):
        self._align(4)
        self.buf.extend(struct.pack("<I", int(value) & 0xFFFFFFFF))

    def int32(self, value):
        self._align(4)
        self.buf.extend(struct.pack("<i", int(value)))

    def uint64(self, value):
        self._align(8)
        self.buf.extend(struct.pack("<Q", int(value) & 0xFFFFFFFFFFFFFFFF))

    def float64(self, value):
        self._align(8)
        self.buf.extend(struct.pack("<d", float(value)))

    def string(self, value):
        raw = value.encode("utf-8")
        self.uint32(len(raw) + 1)  # length includes the null terminator
        self.buf.extend(raw)
        self.buf.append(0x00)

    def sequence_length(self, count):
        self.uint32(count)

    def finish(self):
        # RTPS encapsulation header: PLAIN_CDR little-endian (CDR_LE = 0x0001).
        return b"\x00\x01\x00\x00" + bytes(self.buf)


def cdr_time(w, ts_ns):
    """builtin_interfaces/Time == int32 sec + uint32 nanosec."""
    sec = ts_ns // 1_000_000_000
    nanosec = ts_ns % 1_000_000_000
    w.int32(sec)
    w.uint32(nanosec)


def cdr_pose(w, x, y, z, qx, qy, qz, qw):
    """geometry_msgs/Pose == Point position (3x float64) + Quaternion (4x float64)."""
    w.float64(x)
    w.float64(y)
    w.float64(z)
    w.float64(qx)
    w.float64(qy)
    w.float64(qz)
    w.float64(qw)


def build_grid_payload(ts_ns, cell_data):
    w = CdrWriter()
    cdr_time(w, ts_ns)
    w.string("map")
    # Grid pose = position of the grid's corner closest to the origin (bottom-left).
    cdr_pose(w, GRID_ORIGIN_X, GRID_ORIGIN_Y, 0.0, 0.0, 0.0, 0.0, 1.0)
    w.uint32(GRID_COLUMNS)          # column_count
    w.float64(RESOLUTION)           # cell_size.x
    w.float64(RESOLUTION)           # cell_size.y
    w.uint32(GRID_COLUMNS)          # row_stride (bytes per row)
    w.uint32(1)                     # cell_stride (bytes per cell)
    # fields: sequence<PackedElementField> with one field
    #         {name="uint8", offset=0, type=UINT8(1)}
    w.uint32(1)
    w.string("uint8")
    w.uint32(0)
    w.uint8(1)
    # data: sequence<uint8>
    w.uint32(len(cell_data))
    w.buf.extend(cell_data)
    return w.finish()


def build_pose_payload(ts_ns, x, y, theta):
    half = theta / 2.0
    w = CdrWriter()
    cdr_time(w, ts_ns)
    w.string("map")
    cdr_pose(w, x, y, 0.0, 0.0, 0.0, math.sin(half), math.cos(half))
    return w.finish()


def build_path_payload(ts_ns, points):
    w = CdrWriter()
    cdr_time(w, ts_ns)
    w.string("map")
    w.uint32(len(points))
    for lx, ly in points:
        cdr_pose(w, lx, ly, 0.0, 0.0, 0.0, 0.0, 1.0)
    return w.finish()


def message_data(subscription_id, ts_ns, payload):
    """Binary "Message Data" frame: opcode 0x01, uint32 sub id, uint64 ts, payload."""
    return (
        b"\x01"
        + struct.pack("<I", int(subscription_id) & 0xFFFFFFFF)
        + struct.pack("<Q", int(ts_ns) & 0xFFFFFFFFFFFFFFFF)
        + payload
    )


# ---------------------------------------------------------------------------
# SHM readers
# ---------------------------------------------------------------------------
def read_map_cells():
    with open(MAP_PATH, "rb") as f:
        raw = f.read()
    # 0=free, 255=wall stay as-is; 128 (unknown) -> 0 (free) for a clean display.
    data = raw.replace(b"\x80", b"\x00")
    if len(data) < MAP_DATA_LEN:
        data = data + b"\x00" * (MAP_DATA_LEN - len(data))
    elif len(data) > MAP_DATA_LEN:
        data = data[:MAP_DATA_LEN]
    return data


def read_pose():
    with open(POSE_PATH, "r", encoding="utf-8") as f:
        d = json.load(f)
    x = float(d["x"])
    y = float(d["y"])
    theta = float(d["theta"])
    traj = d.get("traj", [])
    return x, y, theta, traj


def downsample_trajectory(points, limit=MAX_PATH_POINTS):
    pts = []
    for p in points:
        try:
            pts.append((float(p[0]), float(p[1])))
        except (TypeError, ValueError, IndexError):
            continue
    if len(pts) <= limit:
        return pts
    out = []
    n = len(pts)
    for i in range(limit):
        idx = int(round(i * (n - 1) / (limit - 1)))
        out.append(pts[idx])
    return out


# ---------------------------------------------------------------------------
# WebSocket server
# ---------------------------------------------------------------------------
# websocket -> {channel_id: subscription_id}
CLIENTS = {}


async def tick_loop():
    last_map_hash = None
    last_map_time = 0.0
    while True:
        ts_ns = int(time.time() * 1e9)

        grid_payload = None
        pose_payload = None
        path_payload = None

        try:
            cell_data = read_map_cells()
            h = hashlib.sha1(cell_data).digest()
            now = time.time()
            if h != last_map_hash or (now - last_map_time) >= MAP_MIN_INTERVAL:
                grid_payload = build_grid_payload(ts_ns, cell_data)
                last_map_hash = h
                last_map_time = now
        except (OSError, IOError, ValueError):
            grid_payload = None

        try:
            x, y, theta, traj = read_pose()
            pose_payload = build_pose_payload(ts_ns, x, y, theta)
            path_payload = build_path_payload(ts_ns, downsample_trajectory(traj))
        except (OSError, IOError, ValueError, KeyError, TypeError):
            pose_payload = None
            path_payload = None

        for ws, state in list(CLIENTS.items()):
            for channel_id, sub_id in list(state.items()):
                payload = None
                if channel_id == 1:
                    payload = grid_payload
                elif channel_id == 2:
                    payload = pose_payload
                elif channel_id == 3:
                    payload = path_payload
                if payload is not None:
                    try:
                        await ws.send(message_data(sub_id, ts_ns, payload))
                    except Exception:
                        pass

        await asyncio.sleep(TICK_SECONDS)


async def handler(websocket):
    await websocket.send(json.dumps(SERVER_INFO))
    await websocket.send(json.dumps(ADVERTISE))
    CLIENTS[websocket] = {}
    try:
        async for message in websocket:
            if isinstance(message, bytes):
                continue
            try:
                msg = json.loads(message)
            except (ValueError, TypeError):
                continue
            op = msg.get("op")
            if op == "subscribe":
                for sub in msg.get("subscriptions", []):
                    channel_id = sub.get("channelId")
                    sub_id = sub.get("id")
                    if channel_id in (1, 2, 3):
                        CLIENTS[websocket][channel_id] = sub_id
            elif op == "unsubscribe":
                ids = set(msg.get("subscriptionIds", []))
                state = CLIENTS[websocket]
                for ch in [c for c, s in state.items() if s in ids]:
                    state.pop(ch, None)
            # "advertise"/"unadvertise"/unknown ops are ignored gracefully.
    finally:
        CLIENTS.pop(websocket, None)


async def main():
    asyncio.get_running_loop().create_task(tick_loop())
    print(f"[foxglove_bridge] listening on ws://0.0.0.0:{PORT}", flush=True)
    async with serve(
        handler,
        HOST,
        PORT,
        subprotocols=["foxglove.websocket.v1"],
        ping_interval=20,
        ping_timeout=20,
    ):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
