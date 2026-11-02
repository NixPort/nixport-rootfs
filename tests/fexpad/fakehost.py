#!/usr/bin/env python3
"""Stand-in for the app's pad bridge (app/src/main/cpp/io/pad_bridge.c) for the
native-track shim test (tools/fexpad/libfexpad.c).

usage: fakehost.py <dir> <node> [script]
       fakehost.py --nxp2 <socket-path> [--version N]

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
# 版本协商：假宿主和垫片一样，最老说 2、最新说 3；HELLO 的帧头带 min，
# 载荷里给出区间，协商结果 = min(对端 max, 我方 max)。
# The version negotiation: the fake host speaks 2..3 like the shim. The HELLO
# frame carries min, the payload states the range, and the negotiated version
# is min(peer max, own max).
NXP2_VERSION_MIN = 2
NXP2_VERSION = 3
NXP2_RUMBLE = 7
NXP2_HEADER_SIZE = 28
NXP2_PLAYER_RECORD_SIZE = 48

# 客机发来的震动报文（宿主侧收下并记账，供测试断言）。
# The rumble frames the guest sends; the host side collects them for the test.
NXP2_RUMBLE_FRAMES = []


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


def nxp2_frame(message_type, generation, sequence, payload=b"", version=NXP2_VERSION):
    return struct.pack("<4sHHIQQ", NXP2_MAGIC, version, message_type, len(payload),
                       generation, sequence) + payload


def nxp2_header(packet):
    if len(packet) < NXP2_HEADER_SIZE:
        raise RuntimeError(f"short NXP2 packet: {packet.hex()}")
    return struct.unpack("<4sHHIQQ", packet[:NXP2_HEADER_SIZE])


def nxp2_expect(peer):
    """收一帧：震动报文（客机 → 宿主）可能插在中间，收下并记账后继续。
    Receives one frame: a rumble frame (guest -> host) may arrive in between;
    it is collected and the next frame is returned."""
    while True:
        packet = peer.receive()
        header = nxp2_header(packet)
        if header[2] == NXP2_RUMBLE:
            player, flags, strong, weak = struct.unpack("<BBHH",
                                                        packet[NXP2_HEADER_SIZE:])
            NXP2_RUMBLE_FRAMES.append((player, strong, weak))
            print(f"NXP2 RUMBLE player={player} strong={strong} weak={weak}", flush=True)
            continue
        return header, packet


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


def nxp2_accept(server, kind, host_version=None):
    peer = Nxp2Peer(server, kind)
    if kind == SEQ:
        peer.conn, _ = server.accept()
        packet = peer.receive()
    else:
        packet = peer.receive()
    header = nxp2_header(packet)
    magic, frame_version, message_type, payload_size, generation, sequence = header
    # HELLO 的帧头带的是发方还愿意说的最老版本。
    # The HELLO frame carries the oldest version the sender still speaks.
    if (magic, frame_version, message_type, payload_size, generation, sequence) != (
            NXP2_MAGIC, NXP2_VERSION_MIN, 1, 12, 0, 1):
        raise RuntimeError(f"bad NXP2 HELLO: {header}")
    hello = struct.unpack("<HHIB3s", packet[NXP2_HEADER_SIZE:])
    if hello[0] != NXP2_VERSION_MIN or hello[1] != NXP2_VERSION or hello[2] != 7 \
            or hello[3] != 4:
        raise RuntimeError(f"unexpected NXP2 capabilities: {hello}")
    # 假宿主可以只讲老版本（--version 2）：这时垫片必须退回 v2，不发震动。
    # The fake host can speak an older version only (--version 2): the shim
    # must fall back to v2 and send no rumble.
    negotiated = min(hello[1], NXP2_VERSION if host_version is None else host_version)
    print(f"NXP2 HELLO capabilities={hello[2]:#x} players={hello[3]} version={negotiated}",
          flush=True)
    peer.version = negotiated
    return peer


def nxp2_welcome(peer, generation):
    version = getattr(peer, "version", NXP2_VERSION)
    # WELCOME 的帧头与载荷头都带协商出来的版本。
    # The WELCOME frame and its payload both carry the negotiated version.
    payload = struct.pack("<HHIBBH16s", version, 0, 7, 2, 2, 0,
                          bytes([generation & 0xFF]) * 16)
    peer.send(nxp2_frame(2, generation, 2, payload, version))
    header, ready = nxp2_expect(peer)
    expected = (NXP2_MAGIC, version, 3, 8, generation, 2)
    response = struct.unpack("<IBBH", ready[NXP2_HEADER_SIZE:])
    if header != expected or response != (7, 2, 2, 0):
        raise RuntimeError(f"bad NXP2 READY: {header} {response}")
    print(f"NXP2 READY generation={generation:#x} capabilities={response[0]:#x} "
          f"version={version}", flush=True)


def nxp2_snapshot(peer, generation, sequence, records, delay=0.12):
    version = getattr(peer, "version", NXP2_VERSION)
    payload = bytes([len(records), 0, 0, 0]) + b"".join(records)
    peer.send(nxp2_frame(4, generation, sequence, payload, version))
    header, ack = nxp2_expect(peer)
    # ACK 帧自己的序号是客机的出站计数器（震动报文也占号），载荷里才是它
    # 确认的那一帧的序号。
    # The ACK frame carries the guest's own outbound counter (rumble frames
    # take numbers too); its payload holds the sequence it acknowledges.
    if header[0:2] != (NXP2_MAGIC, version) or header[2] != 5 or header[3] != 8 \
            or header[4] != generation:
        raise RuntimeError(f"bad NXP2 ACK header: {header}")
    if header[5] <= getattr(peer, "last_ack_sequence", 0):
        raise RuntimeError(f"NXP2 ACK sequence did not advance: {header}")
    peer.last_ack_sequence = header[5]
    if struct.unpack("<Q", ack[NXP2_HEADER_SIZE:])[0] != sequence:
        raise RuntimeError(f"bad NXP2 ACK payload: {ack.hex()}")
    time.sleep(delay)


def run_nxp2(endpoint, host_version=None):
    try:
        os.unlink(endpoint)
    except FileNotFoundError:
        pass
    server, kind = bind_server(endpoint)
    if kind == SEQ:
        server.listen(4)
    server.settimeout(8)
    try:
        first = nxp2_accept(server, kind, host_version)
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
        first.send(nxp2_frame(4, generation - 1, 9, bad_generation,
                              getattr(first, "version", NXP2_VERSION)))
        header, error = nxp2_expect(first)
        error_payload = struct.unpack("<HH", error[NXP2_HEADER_SIZE:])
        if header[2] != 6 or error_payload != (8, 4):
            raise RuntimeError(f"bad generation error: {header} {error_payload}")
        print("NXP2 stale generation rejected and input cleared", flush=True)
        first.close_connection()

        second = nxp2_accept(server, kind, host_version)
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
            second.send(nxp2_frame(6, generation, 4, struct.pack("<HH", 6, 6),
                                   getattr(second, "version", NXP2_VERSION)))
            header, error = nxp2_expect(second)
            error_payload = struct.unpack("<HH", error[NXP2_HEADER_SIZE:])
            if header[2] != 6 or error_payload != (6, 6):
                raise RuntimeError(f"bad unsupported-message error: {header} {error_payload}")
            print("NXP2 abnormal message rejected and input cleared", flush=True)
            second.close_connection()

        third = nxp2_accept(server, kind, host_version)
        generation = 0x90A0B0C0
        nxp2_welcome(third, generation)
        print("NXP2 reconnect HELLO received", flush=True)
        print(f"NXP2 RUMBLE FRAMES {len(NXP2_RUMBLE_FRAMES)} "
              f"{NXP2_RUMBLE_FRAMES}", flush=True)
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
        if len(sys.argv) < 3:
            raise SystemExit("usage: fakehost.py --nxp2 <socket-path> [--version N]")
        version = None
        if "--version" in sys.argv:
            version = int(sys.argv[sys.argv.index("--version") + 1])
        return run_nxp2(sys.argv[2], version)

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
