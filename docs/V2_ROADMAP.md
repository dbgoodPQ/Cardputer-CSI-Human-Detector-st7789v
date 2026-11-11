# Version 2 roadmap

Branch: `version2` (from `main` @ `1257b06`, which already contains the `nodes` work).
Status: planning. Nothing below is implemented yet.

## Goal 1: Cardputer becomes a console only

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

Open questions:
- Keep the legacy builds (`cardputer-radar`, `cardputer-radar-wifi`) or drop them in v2?
- Should the Cardputer still need the home Wi-Fi, or could nodes report over ESP-NOW only?

## Goal 2: challenge and improve motion detection (nodes)

Current method (v1): AGC-normalised amplitude of 52 L-LTF subcarriers,
mean per-subcarrier variance over 25 frames, compared with an empty-room
reference (mean + 3 sd) or an AUTO floor x 2. Constants are untuned estimates.

Weaknesses to challenge:
- A single scalar (mean variance) mixes real motion with noise from AGC steps,
  rate changes and interference. A single burst can trigger it.
- The ref/floor model drifts (doors, furniture, the router changing channel or power).
- No use of phase, and no use of the temporal correlation between subcarriers.
- Thresholds are hand-picked, not measured.

Candidate improvements (to evaluate on logged data before choosing):
1. **Data first:** add a raw logging mode on the node (CSI amplitude per frame over
   Serial/UDP) and record empty / walking / sitting sessions per room. Tune and compare
   offline (Python) before changing firmware.
2. **Better features:**
   - Correlation-based: 1 - correlation between consecutive (or lagged) CSI shapes.
     This is robust to AGC scaling.
   - PCA on the subcarrier matrix: variance of the first principal components after
     removing the first (static) one.
   - Doppler-like band power: energy in 0.3-5 Hz of the amplitude time series
     (human motion band), rejecting slow drift and fast noise.
   - Sanitised phase difference between adjacent subcarriers, if stable on the WROOM.
3. **Outlier handling:** Hampel / median filter per subcarrier, and drop frames with
   abnormal RSSI or AGC jumps.
4. **Adaptive baseline:** a robust running baseline (median/MAD) that only updates when
   no motion is detected. Persist calibration per node/BSSID.
5. **Decision logic:** hysteresis (separate on/off thresholds) plus a short
   "N of M windows" rule instead of a single threshold with a 10 s hold.
6. **Multi-node fusion on the Cardputer:** zone confidence and cross-checks (motion seen
   by one node vs. a global interference event seen by all at once).
7. Later: ESP32-C5 nodes (better CSI PHY) behind the same node protocol.

Success criteria (to be agreed): detection rate when walking, false alarms per hour
in an empty room, and latency to detect and to clear.

## Protocol impact
- Node → Cardputer `N,...` frame may gain fields (feature values, confidence,
  baseline state). Bump a protocol version in `include/node_proto.h`.
- New command for single-node calibration and for raw-log mode.
