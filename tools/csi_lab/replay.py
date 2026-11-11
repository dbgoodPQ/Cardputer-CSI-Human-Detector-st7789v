"""Replay recordings through a detector and score it.

  python replay.py recordings/*.csil
  python replay.py recordings/*.csil --cal recordings/kitchen_empty_*.csil --plot
  python replay.py recordings/kitchen_walk_*.csil --threshold 0.25

Scores (after --skip seconds of warm-up):
  empty -> false contact % of time, alarms (CLEAR -> CONTACT switches) per hour
  walk  -> detection % of time
  still -> contact % of time (v1 is not expected to see still people beyond its hold)
"""
from __future__ import annotations

import argparse
import glob
import sys
from collections import Counter
from pathlib import Path

import numpy as np

import csilog
from detectors import DETECTORS


def run(rec: csilog.Recording, det) -> dict:
    """Feeds records in order and ticks on the detector's cadence. Returns arrays."""
    r = rec.recs
    if len(r) == 0:
        return {}
    ms = r["ms"].astype(np.int64)
    # millis() wraps after 49 days; unwrap just in case
    ms = ms + np.concatenate([[0], np.cumsum(np.diff(ms) < -(1 << 31))]) * (1 << 32)
    t0 = int(ms[0])
    tick_ms = det.TICK_MS
    py_metric = np.full(len(r), np.nan)
    ticks = []
    next_tick = t0 + tick_ms
    i = 0
    end = int(ms[-1])
    while next_tick <= end + tick_ms:
        while i < len(r) and ms[i] <= next_tick:
            st = det.process(r[i])
            if st == 3:
                py_metric[i] = det.metric
            i += 1
        d = det.tick(next_tick)
        d["t"] = (next_tick - t0) / 1000.0
        ticks.append(d)
        next_tick += tick_ms
    out = {k: np.array([d[k] for d in ticks]) for k in ticks[0]}
    out["py_metric"] = py_metric
    out["pkt_t"] = (ms - t0) / 1000.0
    return out


def input_stats(rec: csilog.Recording) -> dict:
    r = rec.recs
    t = (r["ms"].astype(np.int64) - int(r["ms"][0])) / 1000.0
    dur = max(t[-1], 1e-3)
    gaps = np.diff(t)
    kinds = Counter(zip(r["sig_mode"].tolist(), r["mcs"].tolist(), r["cwb"].tolist(), r["stbc"].tolist()))
    status = Counter(int(s) for s in r["status"])
    return {
        "seconds": dur,
        "pps": len(r) / dur,
        "gaps_100ms": int(np.sum(gaps > 0.1)),
        "max_gap_s": float(gaps.max()) if len(gaps) else 0.0,
        "rssi_mean": float(np.mean(r["rssi"])),
        "kinds": kinds.most_common(4),
        "status": {csilog.STATUS_NAMES[k]: v for k, v in sorted(status.items())},
    }


def parity(rec: csilog.Recording, res: dict, skip_s: float) -> str:
    """Compares the Python v1 metric with the one the node logged."""
    node = rec.recs["metric"].astype(np.float64)
    py = res["py_metric"]
    ok = (rec.recs["status"] == 3) & ~np.isnan(py) & (res["pkt_t"] >= skip_s) & (node > 0)
    if ok.sum() < 10:
        return "parity: not enough samples"
    rel = np.abs(py[ok] - node[ok]) / node[ok]
    return (f"parity vs node: median rel err {np.median(rel):.1e}, "
            f"{100 * np.mean(rel < 1e-3):.1f}% within 0.1%")


def calibrate(files: list[str], skip_s: float) -> float:
    """v1 empty-room reference (mean + 3 sd of the metric), from empty recordings."""
    vals = []
    for f in files:
        rec = csilog.load(f)
        det = DETECTORS["v1"]()
        res = run(rec, det)
        m = res["py_metric"][(res["pkt_t"] >= skip_s) & ~np.isnan(res["py_metric"])]
        vals.append(m)
    v = np.concatenate(vals) if vals else np.array([])
    if len(v) < 50:
        raise SystemExit("calibration: not enough metric samples in the --cal files")
    return float(v.mean() + 3 * v.std())


def score(label: str, res: dict, skip_s: float) -> dict:
    keep = res["t"] >= skip_s
    p = res["present"][keep].astype(bool)
    hours = max(keep.sum() * 0.066 / 3600.0, 1e-9)
    alarms = int(np.sum(p[1:] & ~p[:-1])) + int(p[0]) if len(p) else 0
    return {"contact_pct": 100.0 * p.mean() if len(p) else 0.0, "alarms": alarms, "alarms_h": alarms / hours}


def plot(rec: csilog.Recording, res: dict, det_name: str, out: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(3, 1, figsize=(12, 7), sharex=True)
    ax[0].semilogy(res["t"], np.maximum(res["metric"], 1e-9), lw=0.8)
    ax[0].set_ylabel("metric")
    ax[1].plot(res["t"], res["score"], lw=0.8, label="score")
    ax[1].axhline(res["threshold"][0], color="r", ls="--", lw=0.8, label="threshold")
    ax[1].fill_between(res["t"], 0, 1, where=res["present"].astype(bool), color="m", alpha=0.15,
                       step="post", label="contact")
    ax[1].set_ylim(0, 1)
    ax[1].set_ylabel("motion")
    ax[1].legend(loc="upper right", fontsize=8)
    t = res["pkt_t"]
    sec = np.floor(t).astype(int)
    pps = np.bincount(sec)[:-1] if sec[-1] > 0 else np.bincount(sec)   # last second is partial
    ax[2].plot(np.arange(len(pps)), pps, lw=0.8, label="packets/s")
    ax2 = ax[2].twinx()
    ax2.plot(t, rec.recs["rssi"], color="g", lw=0.4, alpha=0.6)
    ax2.set_ylabel("rssi", color="g")
    ax[2].set_ylabel("pkt/s")
    ax[2].set_xlabel("seconds")
    fig.suptitle(f"{rec.name}  [{rec.label}]  detector {det_name}")
    fig.tight_layout()
    fig.savefig(out, dpi=110)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help=".csil recordings (wildcards ok)")
    ap.add_argument("--detector", default="v1", choices=sorted(DETECTORS))
    ap.add_argument("--threshold", type=float, default=0.15)
    ap.add_argument("--cal", nargs="*", default=[], help="empty-room recordings for a calibrated (RUN) reference")
    ap.add_argument("--skip", type=float, default=5.0, help="warm-up seconds ignored in scores")
    ap.add_argument("--plot", action="store_true", help="save a PNG next to each recording")
    args = ap.parse_args()

    files = sorted({f for pat in args.files for f in (glob.glob(pat) or [pat])})
    cal_files = sorted({f for pat in args.cal for f in (glob.glob(pat) or [pat])})
    cal_ref = calibrate(cal_files, args.skip) if cal_files else None
    if cal_ref is not None:
        print(f"calibrated reference from {len(cal_files)} file(s): {cal_ref:.6g}")

    rows = []
    for f in files:
        rec = csilog.load(f)
        if len(rec.recs) < 50:
            print(f"{rec.name}: too short, skipped")
            continue
        det = DETECTORS[args.detector](threshold=args.threshold, cal_ref=cal_ref)
        res = run(rec, det)
        st = input_stats(rec)
        sc = score(rec.label, res, args.skip)
        rows.append((rec, sc))
        print(f"\n{rec.name}  [{rec.label}]  {st['seconds']:.0f} s, {st['pps']:.0f} pkt/s, "
              f"rssi {st['rssi_mean']:.0f}, gaps>100ms {st['gaps_100ms']} (max {st['max_gap_s']:.2f} s)")
        print(f"  status {st['status']}")
        print("  frame kinds (sig_mode, mcs, cwb, stbc): "
              + ", ".join(f"{k}x{n}" for k, n in st["kinds"]))
        if args.detector == "v1":
            print("  " + parity(rec, res, args.skip))
        print(f"  contact {sc['contact_pct']:.1f}% of time, {sc['alarms']} alarms ({sc['alarms_h']:.0f}/h)")
        if args.plot:
            png = rec.path.with_name(f"{rec.name}.{args.detector}.png")
            plot(rec, res, args.detector, png)
            print(f"  plot: {png}")

    if rows:
        print(f"\n== {args.detector}, threshold {args.threshold}, "
              f"{'RUN (calibrated)' if cal_ref is not None else 'AUTO'} ==")
        for label, what in (("empty", "false contact"), ("walk", "detection"), ("still", "contact")):
            sel = [sc for rec, sc in rows if rec.label == label]
            if not sel:
                continue
            pct = np.mean([s["contact_pct"] for s in sel])
            extra = f", {sum(s['alarms'] for s in sel)} alarms" if label == "empty" else ""
            print(f"  {label:6} {len(sel)} file(s): {what} {pct:.1f}% of time{extra}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
