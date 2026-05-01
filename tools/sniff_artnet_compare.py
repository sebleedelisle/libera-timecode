#!/usr/bin/env python3
"""Capture incoming ArtTimeCode packets on port 6454 and dump full hex
plus inter-packet timing. Listens on both 127.0.0.1 and 0.0.0.0 so it
catches loopback unicast and broadcasts.
"""
import socket, struct, time, select, sys

ART_NET_HEADER = b"Art-Net\x00"
PORT = 6454
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0


def make_listener(addr):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except OSError:
        pass
    s.bind((addr, PORT))
    s.setblocking(False)
    return s


sockets = [make_listener("127.0.0.1"), make_listener("0.0.0.0")]
print(f"# listening for {DURATION}s on 127.0.0.1:6454 and 0.0.0.0:6454", flush=True)

start = None
last = None
last_tc = None
seen_packets = set()  # dedupe identical (src, payload) within ~1ms across two listeners
end = time.monotonic() + DURATION

while time.monotonic() < end:
    rds, _, _ = select.select(sockets, [], [], 0.5)
    for s in rds:
        try:
            data, (addr, sport) = s.recvfrom(2048)
        except BlockingIOError:
            continue
        now = time.monotonic()

        # de-dupe across our two listeners (one packet may arrive on both)
        key = (addr, sport, data)
        if key in seen_packets:
            continue
        seen_packets.add(key)
        # forget old keys to keep the set small
        if len(seen_packets) > 200:
            seen_packets.clear()

        if start is None:
            start = now
        t = now - start
        dt = (now - last) * 1000.0 if last is not None else 0.0
        last = now

        if len(data) < 19 or data[:8] != ART_NET_HEADER:
            print(f"{t:6.4f}  +{dt:6.2f}ms  non-ArtNet from {addr}:{sport}  {len(data)}B  {data[:32].hex()}", flush=True)
            continue
        op = struct.unpack_from("<H", data, 8)[0]
        if op != 0x9700:
            print(f"{t:6.4f}  +{dt:6.2f}ms  Art-Net op=0x{op:04x} from {addr}:{sport}  {len(data)}B", flush=True)
            continue

        ph, pl = data[10], data[11]
        f1, f2 = data[12], data[13]
        frames, secs, mins, hours, ftype = data[14], data[15], data[16], data[17], data[18]
        tc = (hours, mins, secs, frames)
        note = ""
        if last_tc is not None:
            a = ((last_tc[0] * 60 + last_tc[1]) * 60 + last_tc[2]) * 30 + last_tc[3]
            b = ((tc[0] * 60 + tc[1]) * 60 + tc[2]) * 30 + tc[3]
            d = b - a
            if d == 0: note = "  REPEAT"
            elif d < 0: note = f"  REWIND ({d})"
            elif d > 1: note = f"  SKIP +{d}"
        last_tc = tc
        # Show full hex of the 19-byte packet so we can byte-diff against another sender
        hex_str = data[:19].hex(' ', 1)
        print(f"{t:6.4f}  +{dt:6.2f}ms  src={addr:<15}:{sport:<5}  "
              f"{hours:02d}:{mins:02d}:{secs:02d}:{frames:02d}  "
              f"protver={ph}.{pl}  filler={f1:02x}{f2:02x}  type={ftype}  "
              f"len={len(data)}  hex=[{hex_str}]{note}", flush=True)
