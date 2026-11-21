#!/usr/bin/env python3
"""
foxglove_test_client.py — verifies the foxglove_bridge WebSocket server.

Connects with subprotocol foxglove.websocket.v1, reads serverInfo + advertise,
subscribes to the three channels, reads one binary "Message Data" frame per
channel, and prints a summary.
"""

import asyncio
import json
import struct

import websockets

URL = "ws://127.0.0.1:8765"
SUBPROTOCOL = "foxglove.websocket.v1"


async def main():
    async with websockets.connect(URL, subprotocols=[SUBPROTOCOL], max_size=2**24) as ws:
        assert ws.subprotocol == SUBPROTOCOL, f"subprotocol mismatch: {ws.subprotocol!r}"
        print(f"connected, subprotocol={ws.subprotocol!r}")

        info = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
        adv = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
        assert info["op"] == "serverInfo", f"expected serverInfo, got {info}"
        assert adv["op"] == "advertise", f"expected advertise, got {adv}"

        print("serverInfo name:", info.get("name"))
        print("supportedEncodings:", info.get("supportedEncodings"))
        print("channels:")
        schema_ok = True
        for c in adv["channels"]:
            schema_text = c.get("schema", "")
            first_line = schema_text.splitlines()[0] if schema_text else ""
            print(
                f"  id={c['id']} topic={c['topic']} schemaName={c['schemaName']} "
                f"encoding={c['encoding']} schemaEncoding={c.get('schemaEncoding')} "
                f"schema_len={len(schema_text)} first_line={first_line!r}"
            )
            # Schema must be raw ros2msg text (starts with a type token, not base64).
            if (
                c.get("schemaEncoding") != "ros2msg"
                or not first_line.startswith("builtin_interfaces")
            ):
                schema_ok = False

        await ws.send(json.dumps({
            "op": "subscribe",
            "subscriptions": [
                {"id": 0, "channelId": 1},
                {"id": 1, "channelId": 2},
                {"id": 2, "channelId": 3},
            ],
        }))

        sub_to_channel = {0: 1, 1: 2, 2: 3}
        received = {}

        deadline = asyncio.get_event_loop().time() + 15
        while len(received) < 3 and asyncio.get_event_loop().time() < deadline:
            try:
                message = await asyncio.wait_for(ws.recv(), timeout=5)
            except asyncio.TimeoutError:
                continue
            if not isinstance(message, bytes):
                continue
            opcode = message[0]
            sub_id = struct.unpack("<I", message[1:5])[0]
            ts_ns = struct.unpack("<Q", message[5:13])[0]
            payload = message[13:]
            channel = sub_to_channel.get(sub_id)
            received[channel] = (opcode, sub_id, ts_ns, len(payload))

        print("binary messages received:")
        ok = schema_ok
        for channel in (1, 2, 3):
            r = received.get(channel)
            print(f"  channel {channel}: {r}")
            if r is None:
                ok = False
                continue
            opcode, sub_id, ts_ns, plen = r
            if opcode != 0x01 or plen <= 0:
                ok = False

        print("RESULT:", "SUCCESS" if ok else "FAIL")
        return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
