#!/usr/bin/env python3
"""Listen for ArtTimeCode packets on UDP 6454 and print each one with
a monotonic timestamp. Designed for diffing two senders.

Usage:
    python3 tools/sniff_artnet_tc.py [seconds]

Prints lines like:
    0.0000  +0.000  192.168.1.4   00:00:01:23  type=3  19 bytes
              ^delta from prev    ^source ip   ^TC      ^Art-Net Type byte
"""
import socket
import struct
import sys
import time

ART_NET_HEADER = b"Art-Net\x00"
ART_TIMECODE_OPCODE = 0x9700  # little-endian on the wire = 0x00 0x97


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 0  # 0 = forever
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            pass
    s.bind(("", 6454))
    s.settimeout(1.0)
    print(f"# listening on UDP 6454 for ArtTimeCode (Ctrl+C to stop)")

    start = None
    last = None
    last_tc = None
    count = 0
    skipped = 0
    repeated = 0

    try:
        while True:
            try:
                data, (addr, _) = s.recvfrom(2048)
            except socket.timeout:
                if duration and time.monotonic() - (start or time.monotonic()) > duration:
                    break
                continue

            now = time.monotonic()
            if start is None:
                start = now
            t = now - start
            dt = (now - last) * 1000.0 if last is not None else 0.0
            last = now

            # Filter ArtTimeCode
            if len(data) < 19 or data[:8] != ART_NET_HEADER:
                continue
            opcode = struct.unpack_from("<H", data, 8)[0]
            if opcode != ART_TIMECODE_OPCODE:
                continue

            frames, secs, mins, hours, ftype = data[14], data[15], data[16], data[17], data[18]
            tc = (hours, mins, secs, frames)

            note = ""
            if last_tc is not None:
                # Convert to absolute frame index assuming 30fps for a quick monotonicity check.
                a = ((last_tc[0] * 60 + last_tc[1]) * 60 + last_tc[2]) * 30 + last_tc[3]
                b = ((tc[0] * 60 + tc[1]) * 60 + tc[2]) * 30 + tc[3]
                delta = b - a
                if delta == 0:
                    repeated += 1
                    note = "  REPEAT"
                elif delta < 0:
                    note = f"  REWIND ({delta})"
                elif delta > 1:
                    skipped += 1
                    note = f"  SKIP +{delta}"

            last_tc = tc
            count += 1
            print(f"{t:7.4f}  +{dt:6.2f}ms  {addr:<15}  "
                  f"{hours:02d}:{mins:02d}:{secs:02d}:{frames:02d}  type={ftype}  {len(data)}B{note}")
    except KeyboardInterrupt:
        pass

    if count:
        elapsed = (last - start) if (last and start) else 1.0
        print(f"\n# {count} packets in {elapsed:.2f}s "
              f"({count/elapsed:.2f}/s) "
              f"repeats={repeated} skips={skipped}")


if __name__ == "__main__":
    main()
