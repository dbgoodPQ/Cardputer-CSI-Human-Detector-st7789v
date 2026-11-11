# Version 2 roadmap

Branch: `version2` (from `main` @ `1257b06`, which already contains the `nodes` work).
Status: goal 1 done (not yet hardware-tested); goal 2 phase 1 (raw recording) done.

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

0. **Docs** (this section).
1. **Raw CSI recording** (done in firmware 2.0-dev): `C,LOG,<mac>,<port>` streams
   every router CSI packet to a PC; `tools/csi_lab/record.py` saves labelled
   recordings and `replay.py` scores detectors on them (with an exact Python copy
   of v1). Collect per room: 2 x empty, 2 x walk, 1 x still, 5 min each.
2. **Steadier input:** ping 100/s, fixed TX rate HT20 MCS0, 10 ms time slots with
   one packet each, 1 s windows in real time, ready at >= 70% filled slots. Keep the
   frame-kind lock as a safety net.
3. **New measure** (Python first, node only if it beats v1 on the recordings):
   12 spread subcarriers, per-packet std/mean and a neighbour-averaged version,
   Hampel (window 7), per 1 s window lag-1 autocorrelation and IQR/mean, combined
   into a probability by a 3-parameter logistic model fitted on our data.
4. **Decision and protocol:** evaluate every 250 ms, 4 on / 3 off; automatic
   threshold at boot (burst-tolerant) with slow downward recovery, `c` as manual
   recalibration; node report gains probability + state (old 8-field reports still
   accepted); the 10 s "someone was here" hold moves to the Cardputer display.
5. **Later:** signed reports and commands (per-network key sent at pairing; nodes
   must be paired again once).

Success criteria, measured with `replay.py` per room: walking detected > 95% of
the time, empty-room false contact < 1% of the time and < 1 alarm per hour,
contact within 1 s and clear within 1 s after motion stops (before the display hold).

## Protocol impact
- Node → Cardputer `N,...` report gains probability + state at the end (phase 4).
- Raw logging: `C,LOG,<mac>,<port>` (port 0 = stop), binary `CSIL` datagrams v1 (phase 1, done).
- Later: single-node calibration command; signed reports and commands.
