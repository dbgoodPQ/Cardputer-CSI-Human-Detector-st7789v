# Version 2 roadmap

Branch: `version2` (from `main` @ `1257b06`, which already contains the `nodes` work).
Status: goal 1 done; goal 2 phases 0-4 done (firmware 2.1.0-dev1, not yet on hardware); next: hardware test + long empty and next-room recordings.

## Goal 1: Cardputer becomes a console only (done)

The Cardputer stops sensing. It manages nodes and shows the motion they report.

- Remove the on-device CSI pipeline from the Cardputer build: `csiCallback`,
  `processCsiPkt`, `serviceCsi`, the 50 Hz gateway ping, the frame-kind lock,
  and Cardputer-side calibration of itself.
- Remove zone 0 "ME" from the zones model and the ZONES view. Zones = paired nodes only.
- Range cone: drop the ME option. The cone shows a selected node or AUTO
  (most active node).
- The IMU guard is no longer needed for sensing (the Cardputer can be carried around freely).
  Decide whether to keep it for anything else or remove it.
- Calibration key `c` becomes "calibrate nodes" (all, or one from the node menu,
  see v1 next step #6).
- Keep: Wi-Fi join, node pairing (ESP-NOW), node UDP receive (4211), commands to
  nodes (4212), NODES menu, both screens.
- Free up the CPU/RAM previously used by CSI. Possibly a smoother UI.

Decisions: legacy builds dropped; the Cardputer keeps joining the home Wi-Fi;
3D view removed; IMU guard removed; build name `cardputer-radar-csi` kept.

## Goal 2: improve motion detection on the nodes

### Comparison with ESPectre (Nov 2026)

[ESPectre](https://github.com/francescopace/espectre) is a mature open-source
ESP32 Wi-Fi motion sensor. Main differences with our v1 node method:

| | Our v1 node | ESPectre |
|---|---|---|
| Traffic | Router ping 50/s | Router ping 100/s by default; fixed TX rate (HT20 MCS0) on classic ESP32 |
| Timing | Every packet as it comes; window = 25 packets; frame-kind lock | Fixed 10 ms slots (one packet each), 1 s windows in real time, ready at 70% filled slots |
| Subcarriers | All 52, divided by their mean | 12 fixed, spread across the band |
| Measure | Mean per-subcarrier variance of the normalised shape | Per packet: std/mean of the 12 (gain-invariant); per 1 s: lag-1 autocorrelation + IQR/mean of a neighbour-averaged version |
| Outliers | None | Hampel filter |
| Decision | metric / reference vs a hand-picked threshold, 10 s hold | Fixed-weight logistic fusion (or small MLP); 4 readings on / 3 off at 250 ms |
| Threshold | Manual calibration (`c`) or 2x a slow floor | Automatic at boot (burst-tolerant), slow downward recovery |
| Validation | Informal | Published dataset, per-chip reports |
| Nodes | ESP-NOW pairing from the Cardputer, UDP reports, central console | Standalone devices: USB provisioning, mDNS, HTTP API + events, MQTT, Matter, OTA |

**Decision: path B.** Keep our firmware, pairing and protocol, and reimplement the
useful ideas ourselves. ESPectre is GPLv3 and this project is MIT, so we never copy
its code or trained weights; methods are rewritten from their description and any
weights are fitted on our own recordings.

### Plan

0. **Docs** (done).
1. **Raw CSI recording** (done, firmware 2.0-dev2): `C,LOG,<mac>,<port>` streams
   every router CSI packet to a PC; `tools/csi_lab/record.py` saves labelled
   recordings and `replay.py` scores detectors on them.
2. **Steadier input** (done, firmware 2.0-dev3): ping 100/s, 10 ms time slots
   on the radio clock (first packet of each slot kept, others logged as
   "slot skipped"), v1 window 50 packets (~0.5 s, as before). Raw log wire
   version 2 records these settings. The 1 s real-time windows and the "ready at
   >= 70% filled slots" rule belong to v2 and come with its port (phase 4).
   Dropped after the first recordings: fixing the node's TX rate (the router
   picks the rate of its replies) and locking one MCS (made no difference).
3. **New detector v2** (done in Python, `detectors.py`): see results below.
   To redo with recordings made at 100 pings/s after phase 2.
4. **v2 on the nodes** (done, firmware 2.1.0-dev1): `include/csi_v2.h` (frame
   gate + detector, plain C++, shared with a PC test build that matches
   `detectors.py` exactly on all 24 recordings: same packet decisions, spread
   within 3e-7, same contact decisions). 50 pings/s by default, `C,RATE` for
   recordings at other rates. Hysteresis 0.12 (stay in contact while smoothness
   > cut - 0.12): raw walking 86.5% -> 94.4%, still 0 empty-room alarms.
   Report `N,...,<mode>,2,<fw>` (detector 2, firmware version); old 8-field
   reports still accepted. Cardputer: 10 s display hold for v2 nodes, `c`/`x`
   removed (no calibration), `,`/`/` = sensitivity (cut-off 0.30-0.60, default
   0.42, `C,SENS`), versions shown (settings menu, NODES > node). Binaries are
   named with `FW_VERSION` (include/fw_version.h).
5. **Later:** signed reports and commands (per-network key sent at pairing; nodes
   must be paired again once).

### Findings from the first recordings (Nov 11, 2026)

Data: 3 rooms (SALON, CUISINE, SOUS-SOL), one node each, 1 x empty / walk / still,
5 min each, 50 pings/s. ~26,000 packets per file, 3 lost datagrams in total.

- **Input:** 78-98 CSI packets/s per node: ~50/s ping replies (HT, MCS 6 or 7,
  mostly STBC) and 25-40/s other legacy frames, which v1 discards (30-47% of
  packets). No gaps over 0.13 s, no queue drops.
- **v1 AUTO is unusable:** empty rooms show contact 100% of the time. AUTO puts
  its reference at ~1.7x the empty-room median, but the v1 metric swings up to
  3.6x in an empty room. Calibrated (`c`), v1 detects walking 84-100% of the time.
- **Measures tested on 1 s windows** (AUC walk vs empty): v1 0.92-1.00; spread
  variance 1.00; lag-1 autocorrelation ("smoothness") 1.00 in all rooms. Smoothness
  is room-independent: empty/still windows stay below 0.5 in >= 98% of cases,
  walking is above 0.5 in 94-100%.

### v2 detector (phase 3)

Per packet: spread = std/mean of 12 subcarrier amplitudes (+-3, 7, 11, 15, 19, 23),
causal Hampel (7, 3 MAD). Every 250 ms over the last 1 s: smoothness (lag-1
autocorrelation of spread) and spread variance vs a self-learned quiet floor.
p = logistic(20.74 x smoothness - 8.64), i.e. motion when smoothness > 0.42
(refitted with 10 ms slots; before slots: 21.49 / -8.97).
Contact after 4 checks in a row, clear after 3. No calibration.

Results (first 15 s of each file not scored):

| | v1 AUTO | v1 calibrated | **v2, unseen room** (leave-one-room-out) |
|---|---|---|---|
| Empty: false contact | 100% | 0-3.5% (in-sample) | **0-0.8%**, 1 alarm per room at most |
| Walking: detected | 99% | 84-100% | **99.1-99.6%** (first contact 2-3 s after start) |
| Still: contact | 100% | 0-4.5% | 0-1.5% |

With the Cardputer's 10 s display hold added: walking 100%, empty 4.5%.

### Second data set (Nov 12, 2026) and decision tuning

Data: 15 recordings, same 3 nodes, recorded 9h10-18h51 (2 x empty per room at
different times, 2 x walk, 1 x still). Still made with firmware 2.0-dev2 at
50 pings/s (the phase 2 firmware was not flashed yet). No "next room" recording yet.

- **v2 as fitted on day 1, on day 2 (unseen day):** empty 0.9% false contact,
  walking 90.3% raw, but 99.1% with the 10 s display hold. Missed stretches while
  walking are short (almost all <= 5 s), except one 26 s gap (SALON walk 14h38).
- **False alarms were the real problem:** with contact after 4 checks (1 s), empty
  rooms had ~11 alarms per hour. Requiring 10 checks in a row (2.5 s):

  | Contact after | Empty-room alarms (43 min, both days) | Walking covered, raw / with 10 s hold | Still, with hold | First contact (median) |
  |---|---|---|---|---|
  | 4 checks (1 s) | 8 (11/h) | 93.3% / 99.4% | 16.3% | 2.5 s |
  | **10 checks (2.5 s)** | **0** | 86.6% / **98.9%** | 6.8% | 4.2 s |

  Chosen: 10 checks. Smoothness cut-off kept at 0.42 (a refit on both days gave
  0.35, better raw walking but more alarms; with the display hold walking coverage
  is the same from 0.30 to 0.45). The energy gate still adds little.
- **SOUS-SOL "still" at 18h40** keeps 8 short contacts (1.5-7 s): possibly real
  fidgeting or someone else nearby; other rooms' still recordings are clean.
- Caveat: 0 alarms in 43 min still allows up to ~4/h (95% bound). Next: a long
  empty recording (e.g. 60 min, house empty or night) and the "next room" test.

Not used yet: the spread-variance term (fitted weight ~0 on this data) and an
energy gate (motion only if spread variance >= 2x the quiet floor). The gate
removed one empty-room alarm but cost up to 8 points of walking detection right
after a boot. Re-check both with more data.

Caveats: one recording per label per room, one evening, 50 pings/s. Next data
set (after phase 2): per room 2 x empty (different times of day), 2 x walk,
1 x still, 1 x other with someone moving in the next room (`--note "next room"`).

Success criteria, measured with `replay.py` per room: walking detected > 95% of
the time, empty-room false contact < 1% of the time and < 1 alarm per hour,
contact within 1 s and clear within 1 s after motion stops (before the display hold).

## Protocol impact
- Node → Cardputer `N,...` report gains probability + state at the end (phase 4).
- Raw logging: `C,LOG,<mac>,<port>` (port 0 = stop), binary `CSIL` datagrams v1 (phase 1, done).
- Later: single-node calibration command; signed reports and commands.
