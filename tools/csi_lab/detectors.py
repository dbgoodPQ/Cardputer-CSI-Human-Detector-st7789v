"""Detectors for offline replay.

V1Detector is a line-by-line port of the v1 node firmware
(tools/node_esp32/src/main.cpp: csiKindOk, processCsiPkt, serviceSensing).
It uses float32 and the same summation order so its metric can be checked
against the metric each node logs with every packet.

A detector consumes records in order (process) and is asked for a decision on a
fixed tick (tick). New detectors (phase 3) implement the same two methods.
"""
from __future__ import annotations

import numpy as np

F32 = np.float32

# v1 data subcarriers in the classic ESP32 buffer order (index k -> I,Q at 2k, 2k+1):
# 1..26 are +1..+26, 38..63 are -26..-1.
V1_SUBCARRIERS = np.array([k for k in range(1, 64) if not (26 < k < 38)])
assert len(V1_SUBCARRIERS) == 52


def seq_sum(x: np.ndarray, axis: int = 0) -> np.ndarray:
    """Sequential float32 sum (like a C for-loop), not numpy's pairwise sum."""
    return np.cumsum(x, axis=axis, dtype=F32).take(-1, axis=axis)


class V1Detector:
    name = "v1"
    TICK_MS = 66          # serviceSensing cadence (15 Hz)
    WIN = 25              # kCsiWin
    SPAN = F32(4.0)       # kCsiSpan
    AUTO_MARGIN = F32(2.0)
    HOLD = 150            # 10 s at 15 Hz

    def __init__(self, threshold: float = 0.15, cal_ref: float | None = None):
        self.threshold = F32(threshold)
        self.cal_ref = None if cal_ref is None else F32(cal_ref)
        # processCsiPkt state
        self.shape = np.zeros((self.WIN, 52), dtype=F32)
        self.win_idx = 0
        self.win_fill = 0
        self.metric = F32(0)
        self.last_pkt_ms = 0
        # csiKindOk state
        self.cat_cnt = np.zeros(32, dtype=np.int32)
        self.lock_cat = -1
        self.cat_t0 = None
        # serviceSensing state
        self.floor = F32(0)
        self.motion = F32(0)
        self.hold_cnt = 0
        self.held = F32(0)
        self.present = False

    # -- per packet ---------------------------------------------------------
    def _kind_ok(self, cat: int, now: int) -> bool:
        if self.cat_t0 is None:
            self.cat_t0 = now
        self.cat_cnt[cat & 31] += 1
        if now - self.cat_t0 >= 1000:
            best = int(np.argmax(self.cat_cnt))      # first max, like the C loop
            bc = int(self.cat_cnt[best])
            cur = int(self.cat_cnt[self.lock_cat]) if self.lock_cat >= 0 else 0
            if bc >= 5 and best != self.lock_cat and cur < bc // 2:
                self.lock_cat = best
                self.win_fill = 0
                self.win_idx = 0
            self.cat_cnt[:] = 0
            self.cat_t0 = now
        return self.lock_cat < 0 or cat == self.lock_cat

    def process(self, rec) -> int:
        """Feeds one record; returns the same status code as the node."""
        now = int(rec["ms"])
        if not self._kind_ok(int(rec["cat"]), now):
            return 0
        buf = rec["buf"].astype(F32)
        a = buf[2 * V1_SUBCARRIERS]
        b = buf[2 * V1_SUBCARRIERS + 1]
        amp = np.sqrt(a * a + b * b, dtype=F32)
        mean = seq_sum(amp) / F32(52)
        if mean < F32(0.5):
            return 1
        inv = F32(1) / mean
        self.shape[self.win_idx] = amp * inv
        self.win_idx = (self.win_idx + 1) % self.WIN
        if self.win_fill < self.WIN:
            self.win_fill += 1
        self.last_pkt_ms = now
        if self.win_fill < self.WIN:
            return 2
        m = seq_sum(self.shape) / F32(self.WIN)
        q = seq_sum(self.shape * self.shape) / F32(self.WIN)
        var = q - m * m
        self.metric = seq_sum(np.where(var > 0, var, F32(0)).astype(F32)) / F32(52)
        return 3

    # -- per tick -----------------------------------------------------------
    def tick(self, now: int) -> dict:
        pkts_ok = self.last_pkt_ms and (now - self.last_pkt_ms) < 2000
        have = pkts_ok and self.win_fill >= self.WIN
        raw = F32(0)
        if have:
            m = self.metric
            if self.floor <= 0:
                self.floor = m
            elif m < self.floor:
                self.floor += (m - self.floor) * F32(0.2)
            else:
                self.floor += (m - self.floor) * F32(0.0007)
            ref = self.cal_ref if self.cal_ref is not None else self.floor * self.AUTO_MARGIN
            ref = max(ref, F32(1e-7))
            raw = F32(min(max((m / ref - F32(1)) / self.SPAN, 0.0), 1.0))
        self.motion += (raw - self.motion) * F32(0.4)
        if not pkts_ok:
            self.hold_cnt, self.held, self.present, out, mode = 0, F32(0), False, 0.0, "NO CSI"
        else:
            out = self.motion
            if self.motion > self.threshold:
                self.hold_cnt, self.held, self.present = self.HOLD, self.motion, True
            elif self.hold_cnt > 0:
                self.hold_cnt -= 1
                out = self.held * F32(0.10 + 0.90 * self.hold_cnt / self.HOLD)
                self.present = True
            else:
                self.present, out = False, 0.0
            mode = "RUN" if self.cal_ref is not None else "AUTO"
        return {"metric": float(self.metric), "score": float(self.motion), "out": float(out),
                "present": self.present, "mode": mode, "threshold": float(self.threshold)}


DETECTORS = {"v1": V1Detector}
