"""RK3568 诊断：独立打开 evdev 记录原始边沿，不独占设备，不生成按键。"""
import fcntl
import os
import select
import struct
import sys
import time

device = sys.argv[1]
duration = float(sys.argv[2])
event = struct.Struct("@llHHi")
fd = os.open(device, os.O_RDONLY | os.O_NONBLOCK)
try:
    bitmap = bytearray(96)
    fcntl.ioctl(fd, 0x80004518 | (len(bitmap) << 16), bitmap, True)
    print("START code115_held={} input_event_bytes={}".format(
        bool(bitmap[115 // 8] & (1 << (115 % 8))), event.size), flush=True)
    end = time.monotonic() + duration
    while time.monotonic() < end:
        if not select.select([fd], [], [], min(0.2, max(0, end-time.monotonic())))[0]:
            continue
        raw = os.read(fd, event.size)
        if len(raw) != event.size:
            raise RuntimeError("Short input event")
        sec, usec, kind, code, value = event.unpack(raw)
        if kind == 1 or (kind == 0 and code == 3):
            print("mono_ms={} kernel={}.{:06d} type={} code={} value={}".format(
                time.monotonic_ns() // 1000000, sec, usec, kind, code, value), flush=True)
finally:
    os.close(fd)
