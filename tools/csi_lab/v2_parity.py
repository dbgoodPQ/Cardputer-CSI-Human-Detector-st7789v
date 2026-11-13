"""Check that the node's C v2 detector (include/csi_v2.h) matches detectors.V2Detector.

  g++ -O2 -std=c++17 -I../../include v2_host_test.cpp -o v2_host_test
  python v2_parity.py --bin ./v2_host_test "recordings/*.csil"

For each recording it runs the C code (compiled for the PC) and the Python code
on the same packets and the same 250 ms ticks, then compares packet decisions,
per-packet spread values and contact decisions. Recordings made with node
firmware 2.1+ also carry the node's own spread values; replay.py checks those.
"""
from __future__ import annotations

import argparse
import glob
import subprocess
import sys

import numpy as np

import csilog
from detectors import V2Detector


def python_side(rec: csilog.Recording):
    det = V2Detector()
    r = rec.recs
    ms = r["ms"].astype(np.int64)
    t0 = int(ms[0])
    st, sp, ticks = [], [], []
    nxt, i = t0 + det.TICK_MS, 0
    while nxt <= ms[-1] + det.TICK_MS:
        while i < len(r) and ms[i] <= nxt:
            det.last_spread = 0.0
            s = det.process(r[i])
            st.append(s)
            sp.append(det.last_spread if s == 3 else 0.0)
            i += 1
        d = det.tick(nxt)
        ticks.append((nxt - t0, int(d["present"]), int(d["mode"] == "RUN"), d.get("smooth", 0.0)))
        nxt += det.TICK_MS
    return np.array(st), np.array(sp), np.array(ticks)


def c_side(binary: str, path: str):
    out = subprocess.run([binary, path], capture_output=True, text=True, check=True).stdout.split("\n")
    st, sp, ticks = [], [], []
    for line in out:
        f = line.split()
        if not f:
            continue
        if f[0] == "P":
            st.append(int(f[2])); sp.append(float(f[3]))
        elif f[0] == "T":
            ticks.append((int(f[1]), int(f[2]), int(f[3]), float(f[4])))
    return np.array(st), np.array(sp), np.array(ticks)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--bin", default="./v2_host_test", help="compiled v2_host_test")
    args = ap.parse_args()
    files = sorted({f for pat in args.files for f in (glob.glob(pat) or [pat])})
    worst = 0.0
    ok_all = True
    for f in files:
        rec = csilog.load(f)
        pst, psp, pt = python_side(rec)
        cst, csp, ct = c_side(args.bin, f)
        n = min(len(pst), len(cst))
        st_eq = np.mean(pst[:n] == cst[:n]) * 100
        used = (pst[:n] == 3) & (cst[:n] == 3)
        rel = np.abs(psp[:n][used] - csp[:n][used]) / np.maximum(np.abs(psp[:n][used]), 1e-9)
        m = min(len(pt), len(ct))
        pres_eq = np.mean(pt[:m, 1] == ct[:m, 1]) * 100
        sm_err = np.max(np.abs(pt[:m, 3] - ct[:m, 3])) if m else 0.0
        worst = max(worst, float(rel.max()) if len(rel) else 0.0)
        good = st_eq == 100 and pres_eq >= 99.9 and (len(rel) == 0 or rel.max() < 1e-4)
        ok_all &= good
        print(f"{rec.name:40} packets {n:6d} status same {st_eq:6.2f}%  spread max rel err "
              f"{(rel.max() if len(rel) else 0):.1e}  smoothness max diff {sm_err:.1e}  "
              f"contact same {pres_eq:6.2f}%  {'OK' if good else 'CHECK'}")
    print(f"\n{'ALL OK' if ok_all else 'DIFFERENCES FOUND'} (worst spread rel err {worst:.1e})")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
