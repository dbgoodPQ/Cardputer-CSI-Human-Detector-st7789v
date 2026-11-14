# CLAUDE.md — WiFi-CSI Radar

> Context handoff for continuing this project in Claude Code / VS Code.
> Standalone project — a handheld WiFi-CSI people sensor with a dual-screen UI.

## Goal
**Detect moving people through WiFi** using Channel State Information (CSI),
room by room. Realistic target: **presence + motion** per room.

## v2 architecture (branch `version2`)
- **Sensors = nodes only.** ESP32-WROOM-32/32D boards (`tools/node_esp32`), one
  per room, measure CSI motion on their link to the home router and report it.
- **Console = Cardputer ADV** (ESP32-S3). Joins the home Wi-Fi, pairs and
  manages nodes, receives their reports, drives two screens. It does **no
  sensing**: v2 removed its CSI pipeline, local calibration, the IMU guard,
  the `ME` zone, the 3D raycaster view, the spycam sniffer, `radar_link.h` and
  the inherited fake / UDP-`SQUACHNET` / UART builds.
- Wi-Fi power save is off on the Cardputer (`onWifiUp`, `pairStop`) so the
  nodes' 10 Hz UDP broadcasts aren't delayed to DTIM beacons.
- Plan and open work: `docs/V2_ROADMAP.md`. Firmware version: `include/fw_version.h`
  (`FW_VERSION`, shared by both firmwares; CI/release file names use it; bump it
  for every build the user should flash).
- **Displays (different content per screen, NOT a clone):**
  - Top = external **Waveshare 2" ST7789V** (320x240 @ rotation `EXT_ROTATION`=1),
    `.` toggles two views:
    - **Range cone** (`drawTop()`): apex = viewer at the bottom, distance lines
      every 1 m to 5 m or every 2 m to 10 m (`gRange10`, key r, saved in prefs).
      CSI has no distance, so motion = whole-cone glow + `MOTION xx%`; no dots.
      Key n picks the zone (`gConeSel`: 0..n-1 = node, >= n = AUTO, default AUTO;
      `coneZoneNow()` in zones_core.h returns -1 with no nodes; AUTO sticky +10%
      margin). Zone name top-left, AUTO/MANUAL badge bottom-left. Empty states:
      NO NODES / NO WIFI / OFFLINE / NO SIGNAL / CALIBRATING.
      LD2410 later: dots on the centre line at the measured distance.
    - **ZONES** (`drawZones()`, include/zones_ui.h): one row per node, adaptive
      row height for up to 6.
  - Bottom = built-in 1.14" (240x135): Wi-Fi IP, nodes pill (`nodesTag()`:
    `2/3 ON`, green/orange/red), banner (toast 2 s > NO WIFI > NO NODES >
    `>> ZONE <<` > CLEAR), zone chips, strongest-zone graph (0.5 line = cut-off),
    cut-off + key hints.
- Keys: `,` `/` sensitivity = v2 smoothness cut-off `gSens` (0.30-0.60, step 0.02,
  default 0.42, prefs "sens", sent as `C,SENS` on change + every 5 s), `n`, `r`,
  `.`, `` ` `` menu (PALETTE, SCREEN, EXT PANEL, WIFI NET, NODES; version top-right).
  No `c`/`x`: v2 needs no calibration.
- Display hold: v2 nodes report raw contact; `getZone()` keeps a zone present
  for `kDisplayHoldMs` (10 s) after its last reported motion (`ZoneView.held` =
  present only by hold; cone shows "MOTION / a few seconds ago"). v1 nodes
  (8-field reports) are shown as before.

## Sensor nodes (ESP32-WROOM-32/32D, `tools/node_esp32`)
- One generic firmware; no names/creds built in. Protocol + pairing crypto in
  `include/node_proto.h` (shared by both builds; node build uses `-I../../include`).
- Pairing: node in setup mode hops ch 1-13 sending ESP-NOW HELLO; Cardputer
  (` > NODES > ADD NODE, PS_NONE) answers SEEN → node locks channel;
  Enter sends PAIR = SSID+psk AES-128-GCM (fixed key `NODE_PAIR_KEY`, AAD = node
  MAC + Cardputer MAC); node saves, PAIR_OK, reboots into run mode.
- Run (fw 2.1, v2 detector, `include/csi_v2.h` shared with a PC test build):
  router-BSSID frames only, no promiscuous mode, gateway ping 50/s (`C,RATE,<mac>,<hz>`
  10-100, runtime only), `FrameGate` = frame-kind lock + 10 ms slots on
  rx_ctrl.timestamp; `Detector`: spread = std/mean of 12 subcarrier amplitudes
  (+-3,7,11,15,19,23), causal Hampel(7, 3 MAD); every 250 ms over the last 1 s:
  smoothness = lag-1 autocorrelation; motion if smoothness >= cut (0.4166; in
  contact: cut - 0.12); contact after 10 checks, clear after 3; NO CSI if < 20
  packets. prob = logistic(20.74 (smooth - cut)) = motion level. No hold, no
  calibration. Broadcasts `N,<mac>,<seq>,<pres>,<motion>,<rssi>,<rate>,<RUN|NO CSI>,2,<fw>`
  on UDP 4211 at 10 Hz. Cardputer → nodes UDP 4212: `C,SENS,<cut>|BLINK,<mac>|FORGET,<mac>`;
  from a PC: `C,LOG,<mac>,<port>`, `C,RATE,<mac>,<hz>`. v1 commands ignored.
  (v1 method up to fw 2.0-dev3: 52-subcarrier shape variance, calibration, 10 s hold.)
- Raw CSI logging (fw 2.0-dev2+; 2.0-dev had a timeout bug): `C,LOG,<mac>,<port>` from a PC (keep-alive every
  2 s, node stops after 6 s; port 0 = stop) → node streams every router CSI packet
  to sender IP:port as `CSIL` datagrams: wire v3 (fw 2.1) = 24 B header (ping_hz,
  v1_win 0, slot_ms, flags bit0 = metric is v2 spread) + up to 8 x 152 B records
  (radio ts, millis, metric, rx_ctrl fields, status 0/1/3/4, frame kind, 128 B CSI);
  v2 (2.0-dev3) metric = v1; v1 (2.0-dev2) = 20 B header.
  Normal sensing continues. Tools: `tools/csi_lab/` (record.py, replay.py with
  an exact float32 Python port of v1 in detectors.py; recordings gitignored).
- Cardputer side: `include/zones_core.h` (storage prefs "nodes", UDP, zones,
  ESP-NOW plumbing, bottom chips) and `include/zones_ui.h` (ZONES view, NODES
  menu: list / node RENAME-BLINK-REMOVE / add-node flow). Zone i == node i. Names live on the
  Cardputer only (max 6 nodes, 10 chars). Node offline after 10 s silence.
- Node: BOOT 5 s = forget creds; fresh pairing + no Wi-Fi in 60 s = forget;
  LED (GPIO2) slow blink = setup, on = motion, fast = BLINK.
- Design mockups: claude.ai Design canvas "Sensor Nodes Screens".
- CI builds `csi-node-esp32.bin` (merged: 0x1000 bootloader, 0x8000 partitions,
  0xe000 boot_app0, 0x10000 app; flash at 0x0).

## Build / run
- `pio run -e cardputer-radar-csi -t upload` — Cardputer console (name kept from
  v1; only build). CI output `cardputer-csi-st7789v.bin`, flash at 0x0.
- `cd tools/node_esp32 && pio run -t upload` — sensor node. CI output `csi-node-esp32.bin`.
- CI (`build.yml`) builds both on pushes to `main` and `version2`, PRs, manual runs.
- Releases (`release.yml`, on `v*` tags; the user creates tags, this environment
  can't push them): full images `*.bin` at 0x0 (erase NVS → nodes must re-pair)
  and app-only `*-app.bin` at 0x10000 (keep Wi-Fi/pairing/settings). GitHub tends to
  mark `-dev` tags as pre-release, so `releases/latest/download` may not resolve.

## Hard constraints (don't regress these)
- Node timeouts: compare times with `msSince(now, t)` (signed). Commands set
  timestamps with millis() after the loop's `now` was taken; unsigned `now - t`
  wraps and fires at once (bug that killed raw logging in fw 2.0-dev).
- Build sets `ARDUINO_USB_CDC_ON_BOOT=1` → **`Serial` is USB-C**. Any wired
  sensor (e.g. LD2410 on Grove) MUST use a HardwareSerial (`Serial1`), never `Serial`.
- **No PSRAM** (StampS3). Don't allocate a full 320x240 sprite. Pattern: render
  at 240x180 and scale-to-fit (`pushRotateZoom`). Two 240x180x16bpp canvases
  (~84 KB each) is the budget.
- External panel = `include/ext_panel.h`: Waveshare 2" ST7789V (IPS, `invert=true`,
  no offsets), rotation `EXT_ROTATION` (1), CS5/RST3/DC6/MOSI14/SCK40, BL=GPIO4 (PWM,
  `Light_PWM` ch 3), SPI2_HOST **shared with SD** (`bus_shared=true`),
  `rgb_order=false`, 27 MHz.
- Display libs: **M5Unified + M5Cardputer (M5GFX/LovyanGFX)**, Arduino framework,
  platform espressif32@6.12.0, board m5stack-stamps3.

## Files
- `src/main.cpp` — console: Wi-Fi (picker, creds in prefs `wificreds`,
  `onWifiUp`, `serviceWifi`), settings menu, `drawBottom()`, `drawTop()` (range
  cone), `serviceKeys()`. Bottom @30 fps, top @8 fps.
- `include/ext_panel.h` — external ST7789V panel class (`LGFX_ExtST7789`).
- `include/node_proto.h` — node protocol + pairing crypto (shared with the node build).
- `include/csi_v2.h` — v2 frame gate + detector (node firmware and PC parity test).
- `include/fw_version.h` — `FW_VERSION` for both firmwares and the binary names.
- `include/zones_core.h` — nodes storage, UDP reports/commands, zones, pairing
  plumbing, cone selection, chips, `nodesTag()`.
- `include/zones_ui.h` — ZONES view, NODES menu, add-node flow.
- `tools/node_esp32/` — node firmware (PlatformIO, esp32dev).
- `tools/csi_lab/` — PC tools (Python 3.10+, numpy, matplotlib): record.py
  (raw CSI from a node), replay.py (score v1/v2, --hold/--chain/--gate, plots),
  fit.py (v2 weights, leave-one-room-out), detectors.py (V1 exact port, V2),
  v2_host_test.cpp + v2_parity.py (compile csi_v2.h on the PC, compare with V2).
- `docs/V2_ROADMAP.md` — v2 goals and the motion-detection rework plan.

## Status (v2)
✅ Cardputer is console only (this branch). 🧪 fw 2.1.0-dev1 (v2 on nodes,
display hold, sensitivity keys, versioned binaries) not yet tested on hardware.
✅ External Waveshare 2" ST7789V working (pinout confirmed by user).
⚠️ Libraries pinned: `M5GFX@0.2.36`, `M5Unified@0.2.29`, `M5Cardputer@1.1.1`.
M5GFX 0.3.0's board detection probes G5/G6 (= external CS/DC) and misdetects
the ADV (rotated internal screen, dead external screen). Don't unpin without
re-testing on hardware.
✅ Sensor nodes: user paired 3 nodes successfully (v1). User has 3 WROOM-32/32D
boards, one per room.
✅ v2 constants tuned on 24 recordings (docs/V2_ROADMAP.md); 50 pings/s only.

## Next steps (priority order)
1. **Rework node motion detection** (docs/V2_ROADMAP.md, goal 2, path B: our
   own reimplementation of ESPectre-style ideas, never copy its GPL code/weights).
   Phases 0, 1, 3 done: v2 detector in Python (`tools/csi_lab/detectors.py`
   V2Detector: 12-subcarrier spread std/mean, causal Hampel, every 250 ms over 1 s
   lag-1 autocorrelation "smoothness" > 0.42, contact after 10 checks (2.5 s),
   clear after 3, no calibration). Day 1 (9 recordings) + day 2 (15, still
   50 pings/s, fw 2.0-dev2): 0 empty-room alarms in 43 min, walking 98.9% with
   the Cardputer's 10 s hold; v1 AUTO 27-100% false. Findings in docs/V2_ROADMAP.md.
   Phase 4 done in code (fw 2.1.0-dev1). Next: user flashes the app-only files
   (release tag v2.1.0-dev1), checks versions, records a 60 min empty (3 nodes in
   parallel, --port 4213/4214/4215), a walk per node (v2 parity on hardware) and
   "next room" per room; optionally --rate 100.
2. **Signed reports and commands** (roadmap phase 5).
3. **Presence event log** on the bottom screen with optional SD persistence.
4. **HLK-LD2410 mmWave** (24 GHz, UART, 5 V) on the Grove port (G1/G2 = Serial1)
   for still people + distance; dots in the range cone (design agreed).
5. **ESP32-C5 nodes** (MonsterC5) later: different `wifi_csi_acquire_config_t`.

## Hardware inventory (chip cheat-sheet)
- C5 (best CSI): MonsterC5, AWOK Dual Touch (2x each), Marauder v8.
- C3: BLEShark Nanos (many; mesh-capable).
- S3: Cardputer ADV (console, COM18); S3 DevKit (sensor, COM44).
- Classic ESP32: Cheap Yellow Displays, HaleHound (CYD-based).
- Not ESP32: Cardputer Zero (Raspberry Pi CM Zero), Hackberry Pi, HackRF/PortaPack.
