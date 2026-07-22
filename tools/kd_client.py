#!/usr/bin/env python3
"""
Minimal NT "KD" (kernel debugger) wire-protocol client.

Talks directly to GEMU's -serial tcp:HOST:PORT socket (the guest's COM1,
carrying the Windows kernel debug transport once /debug is set in
TXTSETUP.SIF's OsLoadOptions) well enough to synchronize the handshake and
decode state-change packets - the initial breakpoint/exception report the
target sends and keeps resending until something ACKs it. No WinDbg, no VM,
no Wine: this is a from-scratch implementation of just enough of the public,
long-reverse-engineered KD packet framing (field layout and constants
cross-checked against reactos/sdk/include/reactos/windbgkd.h) to be a
passive-but-participating listener.

Verified live against GEMU's Windows Server 2003 IA-64 Setup boot: ACKing
the initial STATE_CHANGE64 packet stops the target's resend loop and lets
execution continue well past the point it was stuck at before this existed.

Usage: python3 kd_client.py [host] [port]   (default 127.0.0.1:5555)
"""
import socket, struct, sys, time

PACKET_LEADER = 0x30303030          # data packets (state-change, etc.)
CONTROL_PACKET_LEADER = 0x69696969  # ack / resend / reset
PACKET_TRAILING_BYTE = 0xAA
INITIAL_PACKET_ID = 0x80800000
SYNC_PACKET_ID = 0x00000800

PACKET_TYPE_UNUSED = 0
PACKET_TYPE_KD_STATE_CHANGE32 = 1
PACKET_TYPE_KD_STATE_MANIPULATE = 2
PACKET_TYPE_KD_DEBUG_IO = 3
PACKET_TYPE_KD_ACKNOWLEDGE = 4
PACKET_TYPE_KD_RESEND = 5
PACKET_TYPE_KD_RESET = 6
PACKET_TYPE_KD_STATE_CHANGE64 = 7
PACKET_TYPE_KD_POLL_BREAKIN = 8
PACKET_TYPE_KD_TRACE_IO = 9
PACKET_TYPE_KD_CONTROL_REQUEST = 10
PACKET_TYPE_KD_FILE_IO = 11

TYPE_NAMES = {
    0: "UNUSED", 1: "STATE_CHANGE32", 2: "STATE_MANIPULATE", 3: "DEBUG_IO",
    4: "ACKNOWLEDGE", 5: "RESEND", 6: "RESET", 7: "STATE_CHANGE64",
    8: "POLL_BREAKIN", 9: "TRACE_IO", 10: "CONTROL_REQUEST", 11: "FILE_IO",
}

HEADER_FMT = "<IHHII"  # leader, type, bytecount, packetid, checksum
HEADER_LEN = struct.calcsize(HEADER_FMT)
assert HEADER_LEN == 16


def checksum(data: bytes) -> int:
    return sum(data) & 0xFFFFFFFF


class KdConn:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(2.0)
        self.buf = b""

    def _fill(self, n, deadline):
        while len(self.buf) < n:
            remaining = deadline - time.time()
            if remaining <= 0:
                return False
            self.sock.settimeout(min(remaining, 2.0))
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise ConnectionError("target closed the connection")
            self.buf += chunk
        return True

    def read_packet(self, timeout=30):
        """Returns (leader, ptype, packet_id, body_bytes) or None on timeout."""
        deadline = time.time() + timeout
        # scan for a valid 4-byte leader, byte by byte (frame sync)
        while True:
            if not self._fill(4, deadline):
                return None
            leader = struct.unpack_from("<I", self.buf, 0)[0]
            if leader in (PACKET_LEADER, CONTROL_PACKET_LEADER):
                break
            self.buf = self.buf[1:]  # slide window, resync
        if not self._fill(HEADER_LEN, deadline):
            return None
        leader, ptype, bytecount, packet_id, csum = struct.unpack_from(HEADER_FMT, self.buf, 0)
        self.buf = self.buf[HEADER_LEN:]
        body = b""
        if leader == PACKET_LEADER and bytecount:
            if not self._fill(bytecount + 1, deadline):  # +1 trailing byte
                return None
            body = self.buf[:bytecount]
            trailer = self.buf[bytecount]
            self.buf = self.buf[bytecount + 1:]
            if trailer != PACKET_TRAILING_BYTE:
                print(f"  [warn] bad trailing byte 0x{trailer:02X}", file=sys.stderr)
            actual = checksum(body)
            if actual != csum:
                print(f"  [warn] checksum mismatch: got 0x{csum:08X} computed 0x{actual:08X}", file=sys.stderr)
        return leader, ptype, packet_id, body

    def send_control(self, ptype, packet_id):
        pkt = struct.pack(HEADER_FMT, CONTROL_PACKET_LEADER, ptype, 0, packet_id, 0)
        self.sock.sendall(pkt)

    def send_ack(self, packet_id):
        # The kernel compares an incoming ACK's PacketId against its own
        # CurrentPacketId with the SYNC_PACKET_ID flag masked off - echoing
        # the flag back verbatim makes the ACK look like it doesn't match,
        # so the "resend" timer never gets cancelled.
        self.send_control(PACKET_TYPE_KD_ACKNOWLEDGE, packet_id & ~SYNC_PACKET_ID)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5555
    print(f"connecting to {host}:{port} ...")
    kd = KdConn(host, port)
    print("connected. listening for KD packets...")

    seen_ids = set()
    while True:
        pkt = kd.read_packet(timeout=60)
        if pkt is None:
            print("(timeout waiting for a packet)")
            continue
        leader, ptype, packet_id, body = pkt
        leader_name = "DATA" if leader == PACKET_LEADER else "CONTROL"
        tname = TYPE_NAMES.get(ptype, f"0x{ptype:X}")
        dup = " [RESEND]" if packet_id in seen_ids else ""
        print(f"<- {leader_name} type={tname} id=0x{packet_id:08X} len={len(body)}{dup}")
        if body:
            print("   body:", body[:64].hex(), "..." if len(body) > 64 else "")

        if leader == PACKET_LEADER:
            if packet_id not in seen_ids:
                seen_ids.add(packet_id)
            kd.send_ack(packet_id)
            print(f"-> ACK id=0x{packet_id:08X}")
        elif leader == CONTROL_PACKET_LEADER:
            if ptype == PACKET_TYPE_KD_RESET:
                print("   target sent RESET - re-acking")
                kd.send_ack(packet_id)


if __name__ == "__main__":
    main()
