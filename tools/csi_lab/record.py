"""Record raw CSI from one sensor node to a labelled .csil file.

  python record.py                              # list the nodes on the network
  python record.py --node A1B2C3D4E5F6 --label empty --room kitchen --minutes 5

The PC must be on the same network as the nodes. On the first run Windows asks
whether Python may use the network: allow it on private networks.
"""
from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from datetime import datetime
from pathlib import Path

import csilog

REPORT_PORT = 4211   # node -> Cardputer reports (broadcast); used here to find nodes
CMD_PORT = 4212      # commands to nodes
REFRESH_S = 2.0      # node stops logging 6 s after the last C,LOG


def discover(seconds: float) -> dict[str, tuple[str, str]]:
    """Listens to the nodes' broadcast reports. Returns {mac: (ip, mode)}."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", REPORT_PORT))
    s.settimeout(0.3)
    found: dict[str, tuple[str, str]] = {}
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            data, (ip, _) = s.recvfrom(256)
        except socket.timeout:
            continue
        f = data.decode(errors="replace").strip().split(",")
        if len(f) >= 8 and f[0] == "N":
            try:
                found[csilog.norm_mac(f[1])] = (ip, f[7])
            except ValueError:
                pass
    s.close()
    return found


def send_cmd(sock: socket.socket, ip: str, text: str) -> str | None:
    """Sends a command; returns an error text instead of raising (e.g. Wi-Fi drop)."""
    try:
        sock.sendto((text + "\n").encode(), (ip, CMD_PORT))
        return None
    except OSError as e:
        return str(e)


def local_ip_for(ip: str) -> str:
    """The PC address Windows uses to reach ip (picks the right adapter)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ip, CMD_PORT))
        return s.getsockname()[0]
    except OSError:
        return "?"
    finally:
        s.close()


NO_DATA_HINT = """
  No packets yet. Likely causes:
   - the node runs firmware 1.0 (its serial monitor shows the version at boot);
     flash firmware 2.0-dev or newer;
   - Windows Firewall blocks incoming UDP {port}, or the Wi-Fi profile is Public;
   - the PC has several adapters (VPN, Ethernet, WSL): try --bind <PC IP on the nodes' network>.
  With the node's serial monitor open you should see '# raw log: on -> <PC IP>:{port}'.
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--node", help="node MAC (as shown in the Cardputer NODES menu or by this tool)")
    ap.add_argument("--ip", help="node IP (skips discovery)")
    ap.add_argument("--label", choices=csilog.LABELS, help="what happens in the room during the recording")
    ap.add_argument("--room", default="room", help="room name, used in the file name")
    ap.add_argument("--note", default="", help="free text saved in the file")
    ap.add_argument("--minutes", type=float, default=5.0)
    ap.add_argument("--countdown", type=int, default=10, help="seconds to get in place before recording")
    ap.add_argument("--port", type=int, default=4213, help="local UDP port for the stream")
    ap.add_argument("--out", default="recordings", help="output folder")
    ap.add_argument("--bind", default="", help="PC IP address to use (when the PC has several adapters)")
    args = ap.parse_args()

    mac = csilog.norm_mac(args.node) if args.node else None
    ip = args.ip
    if not ip:
        print("Looking for nodes (5 s)...")
        nodes = discover(5.0)
        if not nodes:
            print("No node reports heard. Is the PC on the same network, and is UDP 4211 allowed by the firewall?")
            return 1
        if not mac:
            print("Nodes found:")
            for m, (nip, mode) in sorted(nodes.items()):
                print(f"  {m}  {nip:15}  {mode}")
            print("Run again with --node <MAC> --label <empty|walk|still|other>.")
            return 0
        if mac not in nodes:
            print(f"Node {mac} not heard. Found: {', '.join(sorted(nodes)) or 'none'}")
            return 1
        ip = nodes[mac][0]
        print(f"Node {mac} at {ip} (mode {nodes[mac][1]})")
    if not mac or not args.label:
        print("--node and --label are required to record (or give --ip with --node).")
        return 1

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = out / f"{args.room}_{args.label}_{stamp}_{mac[-4:]}.csil"

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind((args.bind, args.port))
    rx.settimeout(0.2)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if args.bind:
        tx.bind((args.bind, 0))
    print(f"PC address towards the node: {args.bind or local_ip_for(ip)}, stream port {args.port}")

    for left in range(args.countdown, 0, -1):
        print(f"\rRecording '{args.label}' in {args.room} starts in {left:2d} s ", end="", flush=True)
        time.sleep(1)
    print()

    meta = {"label": args.label, "room": args.room, "note": args.note, "node": mac, "node_ip": ip,
            "start": datetime.now().isoformat(timespec="seconds"), "minutes": args.minutes,
            "wire_version": csilog.WIRE_VERSION}
    n_recs = n_dgrams = lost = 0
    last_dseq = None
    first_drops = last_drops = None
    status_cnt = [0, 0, 0, 0]
    t0 = time.monotonic()
    t_end = t0 + args.minutes * 60
    next_cmd = 0.0
    next_print = t0 + 1
    sec_recs = 0
    rssi_last = 0
    send_err = None
    hinted = False
    try:
        with open(path, "wb") as f:
            csilog.write_header(f, meta)
            while True:
                now = time.monotonic()
                if now >= t_end:
                    break
                if now >= next_cmd:
                    err = send_cmd(tx, ip, f"C,LOG,{mac},{args.port}")
                    if err and err != send_err:
                        print(f"\n  network error, retrying: {err}")
                    elif not err and send_err:
                        print("\n  network back")
                    send_err = err
                    next_cmd = now + REFRESH_S
                if not hinted and n_recs == 0 and now - t0 > 8:
                    print(NO_DATA_HINT.format(port=args.port))
                    hinted = True
                if now >= next_print:
                    el = now - t0
                    print(f"\r{el:6.0f}/{args.minutes*60:.0f} s  {sec_recs:3d} pkt/s  rssi {rssi_last:4d}  "
                          f"lost dgrams {lost}  queue drops {(last_drops or 0) - (first_drops or 0)}   ",
                          end="", flush=True)
                    sec_recs = 0
                    next_print += 1
                try:
                    data, _ = rx.recvfrom(4096)
                except socket.timeout:
                    continue
                except OSError:          # e.g. WinError 10054 after an ICMP error; keep going
                    time.sleep(0.2)
                    continue
                p = csilog.parse_datagram(data)
                if not p or p[0] != mac:
                    continue
                _, dseq, drops, recs = p
                if last_dseq is not None and dseq > last_dseq + 1:
                    lost += dseq - last_dseq - 1
                last_dseq = dseq
                if first_drops is None:
                    first_drops = drops
                last_drops = drops
                f.write(recs.tobytes())
                n_dgrams += 1
                n_recs += len(recs)
                sec_recs += len(recs)
                rssi_last = int(recs["rssi"][-1])
                for st in recs["status"]:
                    if st < 4:
                        status_cnt[st] += 1
    except KeyboardInterrupt:
        print("\nStopped early.")
    finally:
        for _ in range(3):
            send_cmd(tx, ip, f"C,LOG,{mac},0")
    dur = time.monotonic() - t0
    summary = dict(meta, seconds=round(dur, 1), records=n_recs, datagrams=n_dgrams, lost_datagrams=lost,
                   queue_drops=(last_drops or 0) - (first_drops or 0),
                   status={csilog.STATUS_NAMES[i]: c for i, c in enumerate(status_cnt)})
    path.with_suffix(".json").write_text(json.dumps(summary, indent=2))
    print(f"\nSaved {n_recs} packets ({n_recs / max(dur, 1):.0f}/s) to {path}")
    if n_recs == 0:
        print("Nothing received: check that the node runs the v2 firmware and that the firewall allows "
              f"UDP {args.port} in.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
