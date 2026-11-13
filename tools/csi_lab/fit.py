"""Fit the v2 weights on recordings and check them on rooms they never saw.

  python fit.py "recordings/*.csil"

The room is taken from each recording's metadata (record.py --room). For each
room, the weights are fitted on the OTHER rooms only, then the full v2 detector
(full decision rule) is replayed on that room: leave-one-room-out. That is
the honest estimate for a new room without any calibration. Last, it fits on all
rooms and prints the weights to put in detectors.py (V2_WEIGHTS).

Uses only `empty` (no motion) and `walk` (motion) recordings for fitting;
`still` and `other` are scored but not used to fit.
"""
from __future__ import annotations

import argparse
import glob
import sys

import numpy as np

import csilog
from detectors import V2Detector
from replay import run, score

FEATURES = ("smooth", "ratio")

SKIP_S = 15.0          # first seconds of each recording: people still leaving / settling


def tick_features(rec: csilog.Recording) -> np.ndarray:
    """[smooth, log ratio] per tick, after SKIP_S. Independent of the weights."""
    det = V2Detector()
    feats = []
    r = rec.recs
    ms = r["ms"].astype(np.int64)
    t0 = int(ms[0])
    nxt = t0 + det.TICK_MS
    i = 0
    while nxt <= ms[-1]:
        while i < len(r) and ms[i] <= nxt:
            det.process(r[i])
            i += 1
        d = det.tick(nxt)
        if (nxt - t0) / 1000.0 >= SKIP_S and d["mode"] == "RUN":
            feats.append((d["smooth"], d["ratio"]))
        nxt += det.TICK_MS
    return np.array(feats)


def fit_logistic(X: np.ndarray, y: np.ndarray, l2: float = 1e-2, iters: int = 50) -> np.ndarray:
    """Plain IRLS logistic regression with a small L2 penalty (bias not penalised)."""
    Xb = np.hstack([X, np.ones((len(X), 1))])
    w = np.zeros(Xb.shape[1])
    R = np.eye(Xb.shape[1]) * l2
    R[-1, -1] = 0
    # balance classes
    sw = np.where(y == 1, 0.5 / max(y.mean(), 1e-9), 0.5 / max(1 - y.mean(), 1e-9))
    for _ in range(iters):
        p = 1 / (1 + np.exp(-Xb @ w))
        g = Xb.T @ (sw * (p - y)) + R @ w
        H = (Xb * (sw * p * (1 - p))[:, None]).T @ Xb + R
        step = np.linalg.solve(H, g)
        w -= step
        if np.abs(step).max() < 1e-8:
            break
    return w


def replay_room(recs: list[csilog.Recording], w) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = {}
    for rec in recs:
        det = V2Detector(weights=w)
        res = run(rec, det)
        out.setdefault(rec.label, []).append(score(rec.label, res, SKIP_S, det.TICK_MS / 1000.0))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--with-ratio", action="store_true",
                    help="also fit the spread-variance term (off by default, see detectors.py)")
    args = ap.parse_args()
    cols = [0, 1] if args.with_ratio else [0]

    def full_w(w):
        """(smooth, ratio, bias) from the fitted vector."""
        return (w[0], w[1], w[2]) if args.with_ratio else (w[0], 0.0, w[1])
    files = sorted({f for pat in args.files for f in (glob.glob(pat) or [pat])})
    recs = [csilog.load(f) for f in files]
    rooms = sorted({r.meta.get("room", "room") for r in recs})
    print(f"{len(recs)} recordings, rooms: {', '.join(rooms)}")

    feats = {}
    for r in recs:
        if r.label in ("empty", "walk"):
            feats[r.path] = tick_features(r)

    def data(rs):
        X = [feats[r.path][:, cols] for r in rs if r.path in feats and len(feats[r.path])]
        y = [np.full(len(feats[r.path]), 1.0 if r.label == "walk" else 0.0) for r in rs
             if r.path in feats and len(feats[r.path])]
        return np.vstack(X), np.concatenate(y)

    if len(rooms) >= 2:
        print("\nLeave-one-room-out (weights fitted without the room, then a replay of the room with the full decision rule):")
        print(f"  {'room':10} {'weights (smooth, ratio, bias)':32} {'empty false':>12} {'alarms':>7} "
              f"{'walk det.':>10} {'still':>7}")
        for room in rooms:
            train = [r for r in recs if r.meta.get("room") != room]
            test = [r for r in recs if r.meta.get("room") == room]
            X, y = data(train)
            if len(set(y)) < 2:
                print(f"  {room:10} not enough empty+walk data in the other rooms")
                continue
            w = full_w(fit_logistic(X, y))
            s = replay_room(test, w)
            fmt = lambda lab, k: (f"{np.mean([x[k] for x in s[lab]]):.1f}%" if lab in s else "-")
            al = sum(x["alarms"] for x in s.get("empty", []))
            print(f"  {room:10} ({w[0]:6.2f}, {w[1]:6.2f}, {w[2]:6.2f})       "
                  f"{fmt('empty', 'contact_pct'):>12} {al:>7} {fmt('walk', 'contact_pct'):>10} "
                  f"{fmt('still', 'contact_pct'):>7}")

    X, y = data(recs)
    w = full_w(fit_logistic(X, y))
    cut = f"  (smoothness cut-off {-w[2] / w[0]:.3f})" if not args.with_ratio else ""
    print(f"\nAll rooms: V2_WEIGHTS = ({w[0]:.2f}, {w[1]:.2f}, {w[2]:.2f}){cut}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
