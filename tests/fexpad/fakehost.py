#!/usr/bin/env python3
"""Stand-in for the app's pad bridge (app/src/main/cpp/io/pad_bridge.c) for the
native-track shim test (tools/fexpad/libfexpad.c).

usage: fakehost.py <dir> <node> [script]
       fakehost.py --nxp2 <socket-path>

Binds <dir>/event<node> like the app does, reads the "FXP1" hello the guest
sends, and then sends the event packets listed in <script> (one packet per
line, "type:code:value" triples separated by spaces, e.g. "1:0x130:1").
After the script is exhausted the host closes the pad, which the guest sees as
the pad being unplugged.

SOCK_SEQPACKET where the platform has it (Linux), SOCK_DGRAM elsewhere: both
keep message boundaries, which is all the protocol needs.
"""
import os
import socket
import struct
import sys
import time

SEQ = getattr(socket, "SOCK_SEQPACKET", None)
SOCK_TYPE = SEQ if SEQ is not None else socket.SOCK_DGRAM

EV_SIZE = 24  # struct input_event on 64-bit Linux
HELLO_MAGIC = 0x31505846
NXP2_MAGIC = b"NXP2"
NXP2_VERSION = 2
NXP2_HEADER_SIZE = 28
NXP2_PLAYER_RECORD_SIZE = 48


def bind_server(path):
    """SOCK_SEQPACKET where the platform has it (Linux), SOCK_DGRAM elsewhere:
    both keep message boundaries, which is all the protocol needs."""
    candidates = [t for t in (SEQ, socket.SOCK_DGRAM) if t is not None]
    last = None
    for kind in candidates:
        server = None
        try:
            server = socket.socket(socket.AF_UNIX, kind)
            server.bind(path)
            return server, kind
        except OSError as error:
            last = error
            if server is not None:
                server.close()
    raise last


def parse_group(line):
    out = []
    for token in line.split():
        parts = token.split(":")
        if len(parts) != 3:
            continue
        out.append((int(parts[0], 0), int(parts[1], 0), int(parts[2], 0)))
    return out


def nxp2_frame(message_type, generation, sequence, payload=b""):
    return struct.pack("<4sHHIQQ", NXP2_MAGIC, NXP2_VERSION, message_type, len(payload),
                       generation, sequence) + payload


def nxp2_header(packet):
    if len(packet) < NXP2_HEADER_SIZE:
        raise RuntimeError(f"short NXP2 packet: {packet.hex()}")
    return struct.unpack("<4sHHIQQ", packet[:NXP2_HEADER_SIZE])


def nxp2_record(player, active=True, buttons=0, axes=(0, 0, 0, 0), triggers=(0, 0),
                touches=(), click=0):
    record = bytearray(NXP2_PLAYER_RECORD_SIZE)
    record[0] = player
    record[1] = 1 if active else 0
    if not active:
        return record
    struct.pack_into("<I", record, 4, buttons)
    struct.pack_into("<hhhh", record, 8, *axes)
    struct.pack_into("<HH", record, 16, *triggers)
    record[20] = len(touches)
    record[21] = click
    for index, (touch_id, x, y) in enumerate(touches):
        struct.pack_into("<HHff", record, 24 + index * 12, touch_id, 0, x, y)
    return record


class Nxp2Peer:
    def __init__(self, server, kind):
        self.server = server
        self.kind = kind
        self.conn = None
        self.peer = None

    def receive(self):
        if self.kind == SEQ:
            return self.conn.recv(4096)
        packet, self.peer = self.server.recvfrom(4096)
        return packet

    def send(self, packet):
        if self.kind == SEQ:
            self.conn.sendall(packet)
        else:
            self.server.sendto(packet, self.peer)

    def close_connection(self):
        if self.conn is not None:
            self.conn.close()
            self.conn = None


def nxp2_accept(server, kind):
    peer = Nxp2Peer(server, kind)
    if kind == SEQ:
        peer.conn, _ = server.accept()
        packet = peer.receive()
    else:
        packet = peer.receive()
    header = nxp2_header(packet)
    magic, version, message_type, payload_size, generation, sequence = header
    if (magic, version, message_type, payload_size, generation, sequence) != (
            NXP2_MAGIC, NXP2_VERSION, 1, 12, 0, 1):
        raise RuntimeError(f"bad NXP2 HELLO: {header}")
    hello = struct.unpack("<HHIB3s", packet[NXP2_HEADER_SIZE:])
    if hello[0] != NXP2_VERSION or hello[1] != NXP2_VERSION or hello[2] != 7 or hello[3] != 4:
        raise RuntimeError(f"unexpected NXP2 capabilities: {hello}")
    print(f"NXP2 HELLO capabilities={hello[2]:#x} players={hello[3]}", flush=True)
    return peer


def nxp2_welcome(peer, generation):
    payload = struct.pack("<HHIBBH16s", NXP2_VERSION, 0, 7, 2, 2, 0,
                          bytes([generation & 0xFF]) * 16)
    peer.send(nxp2_frame(2, generation, 2, payload))
    ready = peer.receive()
    header = nxp2_header(ready)
    expected = (NXP2_MAGIC, NXP2_VERSION, 3, 8, generation, 2)
    response = struct.unpack("<IBBH", ready[NXP2_HEADER_SIZE:])
    if header != expected or response != (7, 2, 2, 0):
        raise RuntimeError(f"bad NXP2 READY: {header} {response}")
    print(f"NXP2 READY generation={generation:#x} capabilities={response[0]:#x}",
          flush=True)


def nxp2_snapshot(peer, generation, sequence, records, delay=0.12):
    payload = bytes([len(records), 0, 0, 0]) + b"".join(records)
    peer.send(nxp2_frame(4, generation, sequence, payload))
    ack = peer.receive()
    header = nxp2_header(ack)
    if header != (NXP2_MAGIC, NXP2_VERSION, 5, 8, generation, sequence):
        raise RuntimeError(f"bad NXP2 ACK header: {header}")
    if struct.unpack("<Q", ack[NXP2_HEADER_SIZE:])[0] != sequence:
        raise RuntimeError(f"bad NXP2 ACK payload: {ack.hex()}")
    time.sleep(delay)


def run_nxp2(endpoint):
    try:
        os.unlink(endpoint)
    except FileNotFoundError:
        pass
    server, kind = bind_server(endpoint)
    if kind == SEQ:
        server.listen(4)
    server.settimeout(8)
    try:
        first = nxp2_accept(server, kind)
        generation = 0x10203040
        nxp2_welcome(first, generation)
        nxp2_snapshot(first, generation, 3, [
            nxp2_record(0, buttons=1 << 8,
                        touches=((101, 0.20, 0.30), (202, 0.70, 0.80))),
            nxp2_record(1),
        ])
        nxp2_snapshot(first, generation, 4, [
            nxp2_record(0, buttons=1 << 8,
                        touches=((202, 0.65, 0.75), (101, 0.25, 0.35)), click=1),
            nxp2_record(1, buttons=1),
        ])
        nxp2_snapshot(first, generation, 5, [
            nxp2_record(0, touches=((202, 0.60, 0.70),)),
            nxp2_record(1),
        ])
        nxp2_snapshot(first, generation, 6, [nxp2_record(0)])
        nxp2_snapshot(first, generation, 7, [nxp2_record(0, click=1)], delay=0.08)
        nxp2_snapshot(first, generation, 8, [nxp2_record(0)], delay=0.08)

        bad_generation = bytes([1, 0, 0, 0]) + nxp2_record(0)
        first.send(nxp2_frame(4, generation - 1, 9, bad_generation))
        error = first.receive()
        header = nxp2_header(error)
        error_payload = struct.unpack("<HH", error[NXP2_HEADER_SIZE:])
        if header[2] != 6 or error_payload != (8, 4):
            raise RuntimeError(f"bad generation error: {header} {error_payload}")
        print("NXP2 stale generation rejected and input cleared", flush=True)
        first.close_connection()

        second = nxp2_accept(server, kind)
        generation = 0x50607080
        nxp2_welcome(second, generation)
        nxp2_snapshot(second, generation, 3, [
            nxp2_record(0, buttons=1),
            nxp2_record(1),
        ])
        if kind == SEQ:
            second.close_connection()
            print("NXP2 transport disconnected", flush=True)
        else:
            second.send(nxp2_frame(6, generation, 4, struct.pack("<HH", 6, 6)))
            error = second.receive()
            header = nxp2_header(error)
            error_payload = struct.unpack("<HH", error[NXP2_HEADER_SIZE:])
            if header[2] != 6 or error_payload != (6, 6):
                raise RuntimeError(f"bad unsupported-message error: {header} {error_payload}")
            print("NXP2 abnormal message rejected and input cleared", flush=True)
            second.close_connection()

        third = nxp2_accept(server, kind)
        generation = 0x90A0B0C0
        nxp2_welcome(third, generation)
        print("NXP2 reconnect HELLO received", flush=True)
        time.sleep(4.5)
        third.close_connection()
        return 0
    finally:
        server.close()
        try:
            os.unlink(endpoint)
        except FileNotFoundError:
            pass


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--nxp2":
        if len(sys.argv) != 3:
            raise SystemExit("usage: fakehost.py --nxp2 <socket-path>")
        return run_nxp2(sys.argv[2])

    directory = sys.argv[1]
    node = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    script = sys.argv[3] if len(sys.argv) > 3 else None
    stay = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0

    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, f"event{node}")
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass

    server, kind = bind_server(path)
    seq = kind == SEQ
    if seq:
        server.listen(4)
    print("listening on", path, flush=True)

    if seq:
        conn, _ = server.accept()
        peer = None
    else:
        conn = None
        peer = None

    hello = b""
    if conn is not None:
        hello = conn.recv(64)
    else:
        # The guest's connect() shows up as one datagram: the hello.
        server.settimeout(10)
        hello, peer = server.recvfrom(64)
    magic, size = struct.unpack("<II", hello[:8])
    print(f"hello magic={magic:#x} event_size={size}", flush=True)
    if magic != HELLO_MAGIC:
        print("bad hello", flush=True)
        return 1

    groups = []
    if script:
        with open(script, "r", encoding="ascii") as handle:
            for line in handle:
                line = line.split("#", 1)[0].strip()
                if line:
                    groups.append(parse_group(line))

    now = time.time()
    sec, usec = int(now), int((now % 1) * 1e6)
    for group in groups:
        packet = b"".join(struct.pack("<qqHHi", sec, usec, t, c, v) for t, c, v in group)
        if conn is not None:
            conn.send(packet)
        else:
            server.sendto(packet, peer)
        print("sent", " ".join(f"{t}:{c:#x}:{v}" for t, c, v in group), flush=True)
        time.sleep(0.15)

    # Keep the pad connected a while (a merge test holds one slot while another
    # closes), then the pad goes away: the guest must drop its virtual gamepad.
    if stay > 0:
        time.sleep(stay)
    if conn is not None:
        conn.close()
    else:
        server.close()
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    print("closed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
