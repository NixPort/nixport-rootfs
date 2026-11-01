#!/usr/bin/env python3
"""Stand-in for the app's pad bridge (app/src/main/cpp/io/pad_bridge.c) for PC tests.

usage: fakepad.py <dir> [node number]
Listens on <dir>/event<N> (SOCK_SEQPACKET), reads each client's hello and sends it three
events: BTN_A down, ABS_X = 12345, SYN_REPORT, in the layout the hello asked for.
"""
import os, socket, struct, sys, time

d = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 200
path = os.path.join(d, f"event{n}")
os.makedirs(d, exist_ok=True)
try:
    os.unlink(path)
except FileNotFoundError:
    pass
srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
srv.bind(path)
srv.listen(4)
print("listening on", path, flush=True)
while True:
    c, _ = srv.accept()
    hello = c.recv(64)
    magic, size = struct.unpack("<II", hello[:8])
    print(f"hello magic={magic:#x} event_size={size}", flush=True)
    now = time.time()
    sec, usec = int(now), int((now % 1) * 1e6)
    fmt = "<qqHHi" if size == 24 else "<IIHHi"
    evs = [(1, 0x130, 1), (3, 0x00, 12345), (0, 0, 0)]
    time.sleep(0.2)
    c.send(b"".join(struct.pack(fmt, sec, usec, t, code, v) for t, code, v in evs))
    time.sleep(0.5)
    c.close()
