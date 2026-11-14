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

    def __init__(self, threshold: float = 0.15, cal_ref: float | None = None,
                 win: int = 25, slot_ms: int = 0):
        """win / slot_ms: node settings (firmware 2.0-dev2: 25 / 0; 2.0-dev3: 50 / 10).
        replay.py takes them from each recording."""
        self.threshold = F32(threshold)
        self.cal_ref = None if cal_ref is None else F32(cal_ref)
        self.WIN = int(win)
        self.slot_us = int(slot_ms) * 1000
        self.last_slot = None
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
        if self.slot_us:
            slot = int(rec["rx_us"]) // self.slot_us
            if slot == self.last_slot:
                return 4
            self.last_slot = slot
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


# 12 subcarriers spread over the band, classic ESP32 buffer order
# (+3, +7, ... +23 and -23, -19, ... -3). Avoids the band edges and DC.
V2_SUBCARRIERS = np.array([3, 7, 11, 15, 19, 23, 64 - 23, 64 - 19, 64 - 15, 64 - 11, 64 - 7, 64 - 3])

# Logistic weights for [smoothness, log(spread variance / quiet floor)] + bias.
# Fitted by fit.py on our own recordings (Nov 2026: 3 rooms, 9 x 5 min,
# leave-one-room-out checked); see docs/V2_ROADMAP.md. Never taken from another
# project. The spread-variance weight is 0 for now: it did not help on this data
# (each recording started with no learned floor); see the optional gate instead.
# Refitted with 10 ms slot admission (phase 2).
# p >= 0.5  <=>  smoothness > 8.64 / 20.74 = 0.42
V2_WEIGHTS = (20.74, 0.0, -8.64)


class V2Detector:
    """v2 motion detector (phase 3), designed to run unchanged on the node.

    Per packet (frames of the locked kind, like v1):
      spread = std / mean of the amplitudes of 12 spread subcarriers
               (gain-invariant: AGC scales all subcarriers alike)
      causal Hampel filter (last 7 values, 3 MAD) removes single spikes.
    Every 250 ms, over the packets of the last 1 s:
      smooth = lag-1 autocorrelation of spread (walking changes the channel
               smoothly; an empty room gives white noise near 0). Room-independent.
      mvs    = variance of spread; compared with a quiet floor the node learns by
               itself (falls fast, rises slowly), as log(mvs / floor).
      p      = logistic(w_s * smooth + w_m * log(mvs/floor) + b)
    Decision: contact after 10 evaluations in a row with p >= 0.5 (2.5 s). Once in
    contact, an evaluation still counts as motion while smoothness is above the
    cut-off minus HYST (0.12: 0.42 -> 0.30); clear after 3 below. (4 in a row gave ~11 false alarms/hour in empty rooms on day 2; 10 gave
    none in 43 min of empty data, walking still covered ~99% with the 10 s display hold.)
    Optional gate: an evaluation only counts as motion if mvs >= gate x floor.
    The floor only rises while p < 0.5, so long motion doesn't teach it "busy".
    """
    name = "v2"
    TICK_MS = 250
    WIN_MS = 1000
    MIN_PKTS = 20           # per 1 s window, else "NO CSI"
    SLOT_US = 10000         # at most one packet per 10 ms of radio clock (phase 2)
    HAMPEL_N = 7
    HAMPEL_K = 3.0
    ON_N = 10
    OFF_N = 3
    HYST = 0.12             # smoothness hysteresis once in contact (day 1+2: walking
                            # raw 86.5% -> 94.4%, still 0 empty-room alarms)
    FLOOR_DOWN = 0.2        # per tick, in log domain
    FLOOR_UP = 0.0025       # per tick: ~100 s time constant

    def __init__(self, threshold: float = 0.5, cal_ref: float | None = None, weights=None,
                 gate: float | None = None, **_node_settings):
        self.threshold = threshold
        self.w = tuple(weights) if weights is not None else V2_WEIGHTS
        # optional energy gate: a tick only counts as motion if mvs >= gate x quiet floor
        self.log_gate = None if gate is None else float(np.log(gate))
        # frame-kind lock (same rule as v1)
        self.cat_cnt = np.zeros(32, dtype=np.int32)
        self.lock_cat = -1
        self.cat_t0 = None
        # per-packet history: (ms, spread)
        self.hist_ms: list[int] = []
        self.hist_v: list[float] = []
        self.raw7: list[float] = []
        self.log_floor = None if cal_ref is None else float(np.log(cal_ref))
        self.last_slot = None
        self.on_cnt = 0
        self.off_cnt = 0
        self.present = False
        self.metric = 0.0
        self.last = {}

    def _kind_ok(self, cat: int, now: int) -> bool:
        if self.cat_t0 is None:
            self.cat_t0 = now
        self.cat_cnt[cat & 31] += 1
        if now - self.cat_t0 >= 1000:
            best = int(np.argmax(self.cat_cnt))
            bc = int(self.cat_cnt[best])
            cur = int(self.cat_cnt[self.lock_cat]) if self.lock_cat >= 0 else 0
            if bc >= 5 and best != self.lock_cat and cur < bc // 2:
                self.lock_cat = best
                self.hist_ms.clear(); self.hist_v.clear(); self.raw7.clear()
            self.cat_cnt[:] = 0
            self.cat_t0 = now
        return self.lock_cat < 0 or cat == self.lock_cat

    def process(self, rec) -> int:
        now = int(rec["ms"])
        if not self._kind_ok(int(rec["cat"]), now):
            return 0
        slot = int(rec["rx_us"]) // self.SLOT_US
        if slot == self.last_slot:
            return 4
        self.last_slot = slot
        buf = rec["buf"].astype(np.float32)
        a = buf[2 * V2_SUBCARRIERS]
        b = buf[2 * V2_SUBCARRIERS + 1]
        amp = np.sqrt(a * a + b * b)
        mean = float(amp.mean())
        if mean < 0.5:
            return 1
        spread = float(amp.std()) / mean
        # causal Hampel: replace the newest value if it is far from the recent median
        self.raw7.append(spread)
        if len(self.raw7) > self.HAMPEL_N:
            self.raw7.pop(0)
        if len(self.raw7) == self.HAMPEL_N:
            med = float(np.median(self.raw7))
            mad = 1.4826 * float(np.median(np.abs(np.array(self.raw7) - med)))
            if mad > 0 and abs(spread - med) > self.HAMPEL_K * mad:
                spread = med
        self.last_spread = spread
        self.hist_ms.append(now)
        self.hist_v.append(spread)
        while self.hist_ms and now - self.hist_ms[0] > 2 * self.WIN_MS:
            self.hist_ms.pop(0); self.hist_v.pop(0)
        return 3

    def features(self, now: int):
        """(smooth, mvs, n) over the last 1 s, or None if too few packets."""
        k = len(self.hist_ms)
        i = k
        while i > 0 and now - self.hist_ms[i - 1] < self.WIN_MS:
            i -= 1
        x = np.array(self.hist_v[i:k])
        if len(x) < self.MIN_PKTS:
            return None
        mvs = float(x.var())
        d = x - x.mean()
        den = float(np.dot(d, d))
        smooth = float(np.dot(d[:-1], d[1:]) / den) if den > 0 else 0.0
        return smooth, mvs, len(x)

    def tick(self, now: int) -> dict:
        f = self.features(now)
        if f is None:
            self.on_cnt = self.off_cnt = 0
            self.present = False
            self.last = {"metric": 0.0, "score": 0.0, "out": 0.0, "present": False, "mode": "NO CSI",
                         "threshold": self.threshold, "smooth": 0.0, "ratio": 0.0}
            return self.last
        smooth, mvs, _ = f
        lx = float(np.log(max(mvs, 1e-12)))
        if self.log_floor is None:
            self.log_floor = lx
        ratio = lx - self.log_floor                    # log(mvs / floor) >= ~0
        ws, wm, b = self.w
        z = ws * smooth + wm * ratio + b
        p = 1.0 / (1.0 + np.exp(-z))
        # quiet floor: falls fast; rises slowly and only while the window looks still
        if lx < self.log_floor:
            self.log_floor += (lx - self.log_floor) * self.FLOOR_DOWN
        elif p < self.threshold:
            self.log_floor += (lx - self.log_floor) * self.FLOOR_UP
        if self.present:   # hysteresis: lower the bar by HYST in smoothness units
            z_stay = z + ws * self.HYST
            p_eff = 1.0 / (1.0 + np.exp(-z_stay))
        else:
            p_eff = p
        moving = p_eff >= self.threshold and (self.log_gate is None or ratio >= self.log_gate)
        if moving:
            self.on_cnt += 1; self.off_cnt = 0
            if self.on_cnt >= self.ON_N:
                self.present = True
        else:
            self.off_cnt += 1; self.on_cnt = 0
            if self.off_cnt >= self.OFF_N:
                self.present = False
        self.metric = mvs
        self.last = {"metric": mvs, "score": float(p), "out": float(p), "present": self.present, "mode": "RUN",
                     "threshold": self.threshold, "smooth": smooth, "ratio": ratio}
        return self.last


DETECTORS = {"v1": V1Detector, "v2": V2Detector}
