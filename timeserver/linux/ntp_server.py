#!/usr/bin/env python3
"""LAN time server: a tiny NTP server for a network without internet.

It answers NTP (UDP 123) requests with this machine's own clock, so set the
server's clock correctly once and every device on the LAN can follow it.
Python 3 standard library only.

  serve:  sudo python3 ntp_server.py            (port 123 needs root/capability)
  test:   python3 ntp_server.py --query 192.168.1.10
"""
import argparse
import socket
import struct
import sys
import time

NTP_DELTA = 2208988800          # seconds from 1900-01-01 to 1970-01-01
STRATUM = 2                     # 0 would be read as "kiss of death"; 2 is accepted everywhere
REFID = b"LOCL"                 # reference = local clock
ROOT_DISPERSION = 0x00000290    # ~10 ms in 16.16 fixed point
LOG_EVERY_S = 3600              # log each client at most once per hour


def to_ntp(t):
    sec = int(t)
    frac = int((t - sec) * 2**32) & 0xFFFFFFFF
    return struct.pack("!II", (sec + NTP_DELTA) & 0xFFFFFFFF, frac)


def from_ntp(b):
    sec, frac = struct.unpack("!II", b)
    return sec - NTP_DELTA + frac / 2**32


def build_reply(req, rx_time):
    vn = (req[0] >> 3) & 7
    if vn < 1 or vn > 4:
        vn = 4
    head = struct.pack("!BBBB", (vn << 3) | 4, STRATUM, req[2], 0xEC)  # LI 0, mode 4, precision -20
    root = struct.pack("!II", 0, ROOT_DISPERSION)
    now = time.time()
    ref = to_ntp(now - (now % 64))  # "last set" = within the last minute
    return head + root + REFID + ref + bytes(req[40:48]) + to_ntp(rx_time) + to_ntp(time.time())


def serve(bind, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((bind, port))
    print(f"LAN time server listening on {bind}:{port}/udp, "
          f"clock now {time.strftime('%Y-%m-%d %H:%M:%S %Z')}", flush=True)
    last_log = {}
    while True:
        try:
            req, addr = s.recvfrom(1024)
            rx = time.time()
            if len(req) < 48 or (req[0] & 7) != 3:   # client requests only
                continue
            s.sendto(build_reply(req, rx), addr)
            if rx - last_log.get(addr[0], 0) >= LOG_EVERY_S:
                last_log[addr[0]] = rx
                print(f"served {addr[0]}", flush=True)
        except OSError as e:
            print(f"socket error: {e}", flush=True)
            time.sleep(0.1)


def query(host, port, timeout=3.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    pkt = bytearray(48)
    pkt[0] = (4 << 3) | 3
    t1 = time.time()
    pkt[40:48] = to_ntp(t1)
    s.sendto(pkt, (host, port))
    data, _ = s.recvfrom(512)
    t4 = time.time()
    t2, t3 = from_ntp(data[32:40]), from_ntp(data[40:48])
    offset = ((t2 - t1) + (t3 - t4)) / 2
    delay = (t4 - t1) - (t3 - t2)
    print(f"server {host}:{port}  stratum {data[1]}  refid {data[12:16]!r}")
    print(f"server time (UTC): {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(t3))}")
    print(f"offset vs this machine: {offset * 1000:+.1f} ms   round trip: {delay * 1000:.1f} ms")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=123)
    ap.add_argument("--query", metavar="HOST", help="ask HOST for the time instead of serving")
    a = ap.parse_args()
    try:
        if a.query:
            query(a.query, a.port)
        else:
            serve(a.bind, a.port)
    except socket.timeout:
        sys.exit(f"no answer from {a.query}:{a.port} (server down or firewall blocks UDP {a.port})")
    except PermissionError:
        sys.exit(f"permission denied on port {a.port}: run as root")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
