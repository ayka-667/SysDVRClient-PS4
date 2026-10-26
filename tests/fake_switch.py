#!/usr/bin/env python3
"""Fake Switch that mimics the SysDVR sysmodule in TCP Bridge mode (protocol 02/03).

Used to test the PS4 client (or the native harness) without a console.
Usage: python3 fake_switch.py file.h264 [--proto 02|03] [--no-broadcast]
"""
import argparse
import math
import socket
import struct
import threading
import time

HEADER = struct.Struct("<IIQBB")  # Magic, DataSize, Timestamp, MetaData, ReplaySlot
REQ = struct.Struct("<IHBBBB6s")


def split_access_units(data):
    """Splits an Annex B H.264 stream into access units (one frame per packet, like grc:d)."""
    starts = []
    i = 0
    while True:
        i = data.find(b"\x00\x00\x01", i)
        if i < 0:
            break
        s = i - 1 if i > 0 and data[i - 1] == 0 else i
        starts.append((s, data[i + 3] & 0x1F))
        i += 3
    aus, cur_start, have_slice = [], None, False
    for idx, (pos, nal) in enumerate(starts):
        if cur_start is None:
            cur_start = pos
        is_slice = nal in (1, 5)
        if is_slice:
            first_mb_zero = data[pos + (4 if data[pos + 2] == 0 else 3) + 1] & 0x80
            if have_slice and first_mb_zero:
                aus.append(data[cur_start:pos])
                cur_start = pos
            have_slice = True
        elif nal in (7, 8, 6, 9) and have_slice:
            aus.append(data[cur_start:pos])
            cur_start, have_slice = pos, False
    aus.append(data[cur_start:])
    return [a for a in aus if a]


def handshake(conn, proto, expect_meta):
    conn.sendall(b"SysDVR|" + proto.encode() + b"\x00")
    req = b""
    while len(req) < 16:
        chunk = conn.recv(16 - len(req))
        if not chunk:
            raise ConnectionError("client left during the handshake")
        req += chunk
    magic, ver, meta, vflags, abatch, feat, _ = REQ.unpack(req)
    ok = magic == 0xAAAAAAAA and ver == (ord(proto[0]) | ord(proto[1]) << 8) and meta == expect_meta
    print(f"[fake] handshake meta={meta} vflags={vflags} abatch={abatch} feat={feat} -> {'OK' if ok else 'REJECTED'}")
    code = 6 if ok else 4
    if proto == "03":
        conn.sendall(struct.pack("<I", code) + struct.pack("<I", 0xFFFFFFFF) + b"\x00" * 64)
    else:
        conn.sendall(struct.pack("<I", code))
    return ok, abatch


def serve_video(port, aus, proto):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    while True:
        conn, addr = srv.accept()
        print(f"[fake] video: client {addr}")
        try:
            ok, _ = handshake(conn, proto, 1)
            if not ok:
                conn.close()
                continue
            t0 = time.time()
            n = 0
            while True:
                au = aus[n % len(aus)]
                conn.sendall(HEADER.pack(0xCCCCCCCC, len(au), int((time.time() - t0) * 1e6), 0x01 | 0x04, 0xFF) + au)
                n += 1
                delay = t0 + n / 30 - time.time()
                if delay > 0:
                    time.sleep(delay)
        except (ConnectionError, OSError) as e:
            print(f"[fake] video: disconnected ({e})")
        finally:
            conn.close()


def serve_audio(port, proto):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    while True:
        conn, addr = srv.accept()
        print(f"[fake] audio: client {addr}")
        try:
            ok, batch = handshake(conn, proto, 2)
            if not ok:
                conn.close()
                continue
            frames = 1024 * (batch + 1)
            t0 = time.time()
            n = 0
            phase = 0
            while True:
                pcm = bytearray()
                for _ in range(frames):
                    v = int(8000 * math.sin(phase))
                    phase += 2 * math.pi * 440 / 48000
                    pcm += struct.pack("<hh", v, v)
                conn.sendall(HEADER.pack(0xCCCCCCCC, len(pcm), int((time.time() - t0) * 1e6), 0x02 | 0x04, 0xFF) + pcm)
                n += frames
                delay = t0 + n / 48000 - time.time()
                if delay > 0:
                    time.sleep(delay)
        except (ConnectionError, OSError) as e:
            print(f"[fake] audio: disconnected ({e})")
        finally:
            conn.close()


def broadcast(proto):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    msg = f"SysDVR|6.3|{proto}|FAKE-SWITCH-0001".encode() + b"\x00" * 8
    while True:
        for dst in ("255.255.255.255", "127.0.0.1"):
            try:
                s.sendto(msg, (dst, 19999))
            except OSError:
                pass
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("h264")
    ap.add_argument("--proto", default="03", choices=["02", "03"])
    ap.add_argument("--no-broadcast", action="store_true")
    args = ap.parse_args()

    with open(args.h264, "rb") as f:
        aus = split_access_units(f.read())
    print(f"[fake] {len(aus)} frames loaded, protocol {args.proto}")

    threading.Thread(target=serve_video, args=(9911, aus, args.proto), daemon=True).start()
    threading.Thread(target=serve_audio, args=(9922, args.proto), daemon=True).start()
    if not args.no_broadcast:
        threading.Thread(target=broadcast, args=(args.proto,), daemon=True).start()
    while True:
        time.sleep(1)


if __name__ == "__main__":
    main()
