#!/usr/bin/env python3
"""Node uptime / health logger (fw 2.1.0-dev2+).

Listens on UDP 4211 (the nodes' report broadcast, same port the Cardputer
uses) and measures how much of the time each node is reachable. Also decodes
the nodes' health lines (H,...) so reboots, watchdog resets, link rejoins and
heap trends are visible. Standard library only.

    python health.py                       # all nodes, summary every 60 s
    python health.py --csv health.csv      # also save every H line
    python health.py --nodes 3             # count missing nodes as down from the start

Availability:
  strict      time not inside a report gap longer than 1 s (nodes report 10x/s)
  cardputer   time the Cardputer would show the node ON (gap < 10 s)
Target: 99.99 % = at most 8.6 s down per day, 60 s per week.
Ctrl+C prints the final summary.
"""
import argparse
import csv
import socket
import sys
import time

PORT = 4211
STRICT_GAP_S = 1.0
OFFLINE_GAP_S = 10.0     # = kNodeOfflineMs on the Cardputer

RESET_REASONS = {0: "unknown", 1: "power-on", 2: "ext-pin", 3: "software", 4: "PANIC",
                 5: "INT-WDT", 6: "TASK-WDT", 7: "WDT", 8: "deep-sleep", 9: "BROWNOUT", 10: "sdio"}
SELF_CAUSES = {0: "-", 1: "link-check", 2: "offline-5min", 3: "low-heap", 4: "forget"}
DISC_REASONS = {0: "-", 1: "unspecified", 2: "auth-expire", 3: "auth-leave", 4: "assoc-expire",
                6: "not-authed", 7: "not-assoced", 8: "assoc-leave", 15: "4way-timeout",
                200: "beacon-timeout", 201: "no-AP-found", 202: "auth-fail", 203: "assoc-fail",
                204: "handshake-timeout", 205: "connection-fail"}

H_FIELDS = ["mac", "uptime_s", "reset_reason", "prev_cause", "boots", "wdt", "link", "rejoin",
            "free_heap", "min_heap", "ping_hz", "disc_reason"]


def fmt_dur(s):
    s = int(s)
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    return (f"{d}d " if d else "") + f"{h:02d}:{m:02d}:{s:02d}"


class Node:
    def __init__(self, mac, t0):
        self.mac = mac
        self.t0 = t0            # start of measurement for this node
        self.last = None        # last N report (time.monotonic)
        self.strict_down = 0.0
        self.offline_down = 0.0
        self.offline_events = 0
        self.reports = 0
        self.fw = "?"
        self.mode = "?"
        self.h = None           # last H dict
        self.reboots_seen = 0
        self.offline_logged = False

    def gap(self, now):
        return now - (self.last if self.last is not None else self.t0)

    def on_report(self, now):
        g = self.gap(now)
        if g > STRICT_GAP_S:
            self.strict_down += g
        if g > OFFLINE_GAP_S:
            self.offline_down += g
            self.offline_events += 1
            log(f"{self.mac} back ONLINE after {g:.1f} s")
        self.offline_logged = False
        self.last = now
        self.reports += 1

    def avail(self, now):
        total = max(now - self.t0, 1e-9)
        g = self.gap(now)
        strict = self.strict_down + (g if g > STRICT_GAP_S else 0)
        offl = self.offline_down + (g if g > OFFLINE_GAP_S else 0)
        return 100.0 * (1 - strict / total), 100.0 * (1 - offl / total), strict, total


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S ") + msg, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--csv", help="append every H line to this CSV file")
    ap.add_argument("--every", type=int, default=60, help="summary interval, s (default 60)")
    ap.add_argument("--nodes", type=int, default=0,
                    help="expected node count: warn while fewer have been seen")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", args.port))
    sock.settimeout(0.5)

    csv_f = csv_w = None
    if args.csv:
        csv_f = open(args.csv, "a", newline="")
        csv_w = csv.writer(csv_f)
        if csv_f.tell() == 0:
            csv_w.writerow(["time"] + H_FIELDS)

    nodes = {}
    start = time.monotonic()
    last_sum = start
    log(f"listening on UDP {args.port} (Ctrl+C to stop)")

    def summary(now):
        if args.nodes and len(nodes) < args.nodes:
            log(f"only {len(nodes)}/{args.nodes} nodes seen so far")
        for mac in sorted(nodes):
            n = nodes[mac]
            strict, offl, down, total = n.avail(now)
            h = n.h or {}
            print(f"  {mac} fw {n.fw:<11} {n.mode:<6} over {fmt_dur(total)}: strict {strict:8.4f} %  "
                  f"cardputer {offl:8.4f} %  down {down:7.1f} s  offline x{n.offline_events}  "
                  f"| up {fmt_dur(h.get('uptime_s', 0))} boots {h.get('boots', '?')} wdt {h.get('wdt', '?')} "
                  f"link {h.get('link', '?')} rejoin {h.get('rejoin', '?')} heap {h.get('free_heap', '?')}"
                  f"/{h.get('min_heap', '?')} ping {h.get('ping_hz', '?')}/s", flush=True)

    try:
        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                data = None
            now = time.monotonic()
            if data:
                line = data.decode("ascii", "replace").strip()
                f = line.split(",")
                if f[0] == "N" and len(f) >= 8:
                    mac = f[1].upper()
                    n = nodes.get(mac)
                    if n is None:
                        n = nodes[mac] = Node(mac, now)
                        log(f"new node {mac} from {addr[0]}")
                    n.on_report(now)
                    n.mode = f[7]
                    if len(f) >= 10:
                        n.fw = f[9]
                elif f[0] == "H" and len(f) >= len(H_FIELDS) + 1:
                    try:
                        vals = [f[1].upper()] + [int(x) for x in f[2:2 + len(H_FIELDS) - 1]]
                    except ValueError:
                        vals = None
                    if vals:
                        h = dict(zip(H_FIELDS, vals))
                        mac = h["mac"]
                        n = nodes.get(mac)
                        if n is None:
                            n = nodes[mac] = Node(mac, now)
                        prev = n.h
                        if prev is None or h["uptime_s"] < prev["uptime_s"] or h["boots"] != prev["boots"]:
                            log(f"{mac} booted: reset {RESET_REASONS.get(h['reset_reason'], h['reset_reason'])}, "
                                f"self-restart cause {SELF_CAUSES.get(h['prev_cause'], h['prev_cause'])}, "
                                f"uptime {fmt_dur(h['uptime_s'])}, boots {h['boots']}, wdt {h['wdt']}")
                            if prev is not None:
                                n.reboots_seen += 1
                        elif prev:
                            if h["link"] > prev["link"]:
                                log(f"{mac} link check rejoined Wi-Fi (router stopped answering) x{h['link']}")
                            if h["rejoin"] > prev["rejoin"]:
                                log(f"{mac} backup reconnect x{h['rejoin']} "
                                    f"(last disconnect: {DISC_REASONS.get(h['disc_reason'], h['disc_reason'])})")
                        n.h = h
                        if csv_w:
                            csv_w.writerow([time.strftime("%Y-%m-%d %H:%M:%S")] + vals)
                            csv_f.flush()
            for n in nodes.values():
                g = n.gap(now)
                if g > OFFLINE_GAP_S and n.last is not None and not n.offline_logged:
                    n.offline_logged = True
                    log(f"{n.mac} OFFLINE (no report for {OFFLINE_GAP_S:.0f} s)")
            if now - last_sum >= args.every:
                last_sum = now
                summary(now)
    except KeyboardInterrupt:
        print()
        log("final summary")
        summary(time.monotonic())
    finally:
        if csv_f:
            csv_f.close()


if __name__ == "__main__":
    sys.exit(main())
