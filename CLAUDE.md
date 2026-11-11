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
- Plan and open work: `docs/V2_ROADMAP.md` (next: improve the nodes' motion method).
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
    NODES: CAL > `>> ZONE <<` > CLEAR), zone chips, strongest-zone graph,
    threshold + key hints.
- Keys: `c` sends `C,CAL` (or `C,CALSTOP` if any node reports CAL), `x` sends
  `C,UNCAL`, `,` `/` threshold (sent to nodes + prefs), `n`, `r`, `.`, `` ` `` menu
  (PALETTE, SCREEN, EXT PANEL, WIFI NET, NODES).

## Sensor nodes (ESP32-WROOM-32/32D, `tools/node_esp32`)
- One generic firmware; no names/creds built in. Protocol + pairing crypto in
  `include/node_proto.h` (shared by both builds; node build uses `-I../../include`).
- Pairing: node in setup mode hops ch 1-13 sending ESP-NOW HELLO; Cardputer
  (` > NODES > ADD NODE, PS_NONE) answers SEEN → node locks channel;
  Enter sends PAIR = SSID+psk AES-128-GCM (fixed key `NODE_PAIR_KEY`, AAD = node
  MAC + Cardputer MAC); node saves, PAIR_OK, reboots into run mode.
- Run: node CSI pipeline (v1 method, to be reworked in v2): router-BSSID frames
  only, no promiscuous mode, 50 Hz gateway ping, frame-kind lock, 52 L-LTF data
  subcarriers / mean (AGC cancel), shape variance over 25 frames vs empty-room
  ref (mean+3sd) or AUTO floor x2, 10 s hold. Modes AUTO | CAL | RUN | NO CSI; broadcasts `N,<mac>,<seq>,<pres>,<motion>,
  <rssi>,<rate>,<mode>` on UDP 4211 at 10 Hz. Cardputer → nodes UDP 4212:
  `C,CAL|CALSTOP|UNCAL|THR,<f>|BLINK,<mac>|FORGET,<mac>` (THR resent every 5 s).
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

## Hard constraints (don't regress these)
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
- `include/zones_core.h` — nodes storage, UDP reports/commands, zones, pairing
  plumbing, cone selection, chips, `nodesTag()`.
- `include/zones_ui.h` — ZONES view, NODES menu, add-node flow.
- `tools/node_esp32/` — node firmware (PlatformIO, esp32dev).
- `docs/V2_ROADMAP.md` — v2 goals and the motion-detection rework plan.

## Status (v2)
✅ Cardputer is console only (this branch). Not yet tested on hardware.
✅ External Waveshare 2" ST7789V working (pinout confirmed by user).
⚠️ Libraries pinned: `M5GFX@0.2.36`, `M5Unified@0.2.29`, `M5Cardputer@1.1.1`.
M5GFX 0.3.0's board detection probes G5/G6 (= external CS/DC) and misdetects
the ADV (rotated internal screen, dead external screen). Don't unpin without
re-testing on hardware.
✅ Sensor nodes: user paired 3 nodes successfully (v1). User has 3 WROOM-32/32D
boards, one per room.
⚠️ Node tuning constants are first estimates: `kCsiSpan`=4, `kAutoMargin`=2,
`kCsiWin`=25, hold 10 s, default threshold 0.15.

## Next steps (priority order)
1. **Rework node motion detection** (docs/V2_ROADMAP.md, goal 2): raw CSI
   logging mode first, compare methods offline on real sessions, then change
   the node firmware.
2. **Per-node calibration** from the node menu; persist calibration on the node.
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
