# CLAUDE.md — WiFi-CSI Radar

> Context handoff for continuing this project in Claude Code / VS Code.
> Standalone project — a handheld WiFi-CSI people sensor with a dual-screen UI.

## Goal
**Detect moving people through WiFi** using Channel State Information (CSI),
room by room. Realistic target: **presence + motion** per room.

## v2 architecture (branch `main`)
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
    - **Range cone** (`drawTop()`): apex = viewer at the bottom; since fw 2.5 a
      motion gauge: five 20% bands (labels 20%..100%), filled from the apex up
      to the motion level, band colour by height (`coneBandCol()`: red <= 30%,
      yellow 31-70%, green > 70%), alpha 0.62 + bright edge at the level; display
      hold = fill at `ZoneView.heldMotion` (last motion with presence), alpha 0.28;
      CLEAR = empty cone. Text has a black shadow. `MOTION xx%`; no dots. (Key r /
      `gRange10` 5 m/10 m removed in 2.5; old prefs key "range10" is just unused.)
      Key n picks the zone (`gConeSel`: 0..n-1 = node, >= n = AUTO, default AUTO;
      `coneZoneNow()` in zones_core.h returns -1 with no nodes; AUTO sticky +10%
      margin). Zone name top-left, AUTO/MANUAL badge bottom-left. Empty states:
      NO NODES / NO WIFI / OFFLINE / NO SIGNAL / CALIBRATING.
      LD2410 later: dots on the centre line at the measured distance.
    - **ZONES** (`drawZones()`, include/zones_ui.h): one row per node, adaptive
      row height for up to 6.
    - **HEALTH** (`drawHealth()`, include/logs_ui.h, fw 2.6): per node online % today
      (`hlogAvail100`), uptime, drops/boots today, 24 h bar of 15-min slots
      (green ok, red was offline), last event. `gViewMode` 0 cone, 1 zones, 2 health.
  - Bottom = built-in 1.14" (240x135): Wi-Fi IP, nodes pill (`nodesTag()`:
    `2/3 ON`, green/orange/red), banner (toast 2 s > NO WIFI > NO NODES >
    `>> ZONE <<` > CLEAR), zone chips, strongest-zone graph (0.5 line = cut-off),
    cut-off + key hints.
- Keys: `,` `/` sensitivity = v2 smoothness cut-off `gSens` (0.30-0.60, step 0.02,
  default 0.42, prefs "sens", sent as `C,SENS` on change + every 5 s), `n`,
  `.`, `` ` `` menu (PALETTE, SCREEN, EXT PANEL, SCREEN OFF, WIFI NET, NODES, LOGS; version
  top-right; 7 rows of 15 px, `kMenuItems`, `kMenuNodes`/`kMenuLogs` = rows opened with enter).
- Screen off (fw 2.6.0-dev6): SCREEN OFF = NEVER/1/5/10/30 min (prefs "radar"/"scroff", default 10).
  No key press for that long -> both backlights 0 and no drawing (`gScreensOff`; logging, nodes,
  web keep running). Any key wakes both and is swallowed (`screensWakeKey()` in `uiKey()` and
  `serviceKeys()`). Wake re-sends SLPOUT/INVON/MADCTL/DISPON and re-inits the backlight PWM; never
  call LGFX `init()` again (its `_bus->init()` restarts SPI2 and breaks the SD card).
- External backlight check (issue #6: screen black with backlight off until reboot, cause
  unknown): `checkExtBacklight()` every 5 s verifies GPIO4 is routed to LEDC channel
  `EXT_BL_PWM_CH` (3) and the duty equals `gExtDuty`; else `reinitBacklight()` + SYS log line
  "ext backlight lost (pin|level|pin+level), restored" (max every 10 min). Set the external
  brightness only through `setExtBacklight()` so `gExtDuty` stays right.
  No `c`/`x`/`r`: v2 needs no calibration, cone scale is fixed in %.
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
  calibration. Broadcasts `N,<mac>,<seq>,<pres>,<motion>,<rssi>,<rate>,<RUN|NO CSI>,2,<fw>,<uptime s>,<drops>,<boot>`
  on UDP 4211 at 10 Hz (last 3 fields since fw 2.5; Cardputer `NodeLive.hasDiag`, shown in
  NODES > node; drops = Wi-Fi lost + link-check rejoins since boot (`gDrops`); boot = `bootReasonStr()`: PWR EXT SW NET HEAP WDT PANIC BOD ?, NET/HEAP from `gRtc.cause`). Cardputer → nodes UDP 4212: `C,SENS,<cut>|BLINK,<mac>|FORGET,<mac>`;
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
  LED (GPIO2) slow blink = setup, on = motion, fast = BLINK, 60 ms heartbeat
  flick every 5 s while running (no flick = frozen).
- Node self-recovery (from `node-reliability`, fw 2.1.0-dev2, kept in 2.5; goal 99.99% online; user saw nodes go
  offline for good with the LED steady on, back after reset): task watchdog 30 s
  on the setup/run loops (`wdtStart`/`wdtFeed`); link check on ping replies
  (`on_ping_success`, armed after the first reply): 15 s no reply or no report
  sent -> WiFi rejoin, no recovery 60 s later -> restart; backup reconnect every
  30 s while down (Arduino auto-reconnect stays on); no healthy link 5 min ->
  restart; free heap < 20 KB for 10 s -> restart. Counters in RTC_NOINIT `gRtc`
  (boots, wdt, link, rejoin, cause). Health line every 10 s on UDP 4211:
  `H,<mac>,<uptime_s>,<reset_reason>,<prev_cause>,<boots>,<wdt>,<link>,<rejoin>,<heap>,<min_heap>,<ping_hz>,<disc>,<prev_stage>,<rom_reset>,<chip_rev>`
  (keep < 92 B, worst case 80). `tools/csi_lab/health.py` logs it and measures availability
  (ignores the last 3 fields).
- Issue #5 (fw 2.6.0-dev7): RTC watchdog 60 s (`rtc_wdt_*`, action RESET_SYSTEM so RTC memory
  survives) on top of the 30 s task watchdog, both started first thing in `setup()` (`wdtStart()`,
  idempotent). `stage(STG_*)` writes the current step into `gRtc.stage` (enum + `stageName()` in
  node_proto.h); at boot it becomes `gPrevStage` (STG_POWER_UP = blank RTC memory). Cardputer
  `hlogOnHealthLine()` logs `INFO chip rev R boots B wdt W` once per node per Cardputer start and
  `RESET <reason> in <stage> rom<N> <name> rev<R>` after a crash-type boot (PANIC/WDTs/BOD).
  The problem node is the only WROOM-32 (others WROOM-32D): older silicon (rev 0/1) has errata,
  e.g. a spurious watchdog reset at power-up on rev 0.
- Problem node facts (serial, 2026-11-16): NODE A1CC, mac 7C9EBDEDA1CC, ESP32-D0WDQ6 **rev 1.0**,
  flash JEDEC manufacturer `d8` device `4016` (4 MB, not a common maker), CP210x USB on COM6.
  Every power-up: ROM `flash read err, 1000` on the first read, then the ROM's RTC watchdog
  (`RTCWDT_RTC_RESET`, rom 16) resets it and the second boot works -> this is the "BOOT WDT after
  each replug", a board/flash power-up problem, not firmware. Suspect: the same flash not answering
  at run time = the ~30 min freeze. If RESET lines confirm odd resets, replace the board.
- Flashing: app-only `firmware.bin` goes at **0x10000** only. Writing it at 0x0 (user did once)
  overwrites the bootloader (ROM loops on `flash read err, 1000`) and NVS (pairing lost); fix =
  `pio run -t erase` + `pio run -t upload --upload-port COMx`, then pair again.
- WiFiUDP (Arduino-ESP32 2.0.17) keeps unread bytes of a datagram and then
  `parsePacket()` returns 0 forever: always `flush()` after `read()`. The
  Cardputer (`serviceNodes()`) and nodes (`serviceUdp()`) both flush since fw
  2.1.0-dev2. Still keep node broadcasts under 96 B (Cardputer reads 95 B).
- Design mockups: claude.ai Design canvas "Sensor Nodes Screens".
- CI builds `csi-node-esp32.bin` (merged: 0x1000 bootloader, 0x8000 partitions,
  0xe000 boot_app0, 0x10000 app; flash at 0x0).

## Node health log (Cardputer, fw 2.6, `include/health_log.h`)
- microSD on the shared SPI2 bus: SCK40 MISO39 MOSI14 CS12, `SPI.begin` + `SD.begin(12, SPI, 20 MHz)`
  after `extPanel.init()` (`hlogBegin()` in setup). `ext_panel.h` has `spi_3wire = false` for this
  (3-wire leaves MOSI half-duplex) and `pin_miso = 39` (see Hard constraints). Not yet confirmed on hardware. No card: retry every 30 s.
- Files: `/CSIRADAR/logs/YYYY-MM-DD.csv` (header `time,node,id,event,detail`, local time,
  commas in detail -> `;`), `/CSIRADAR/state.bin` (today's per-node counters + 96 15-min slots,
  keyed by MAC, saved every 60 s; restored at clock set if same day), `/CSIRADAR/summary.csv`
  (`date,offline,boots,drops` per closed day; LOGS list + web page).
- Events from hooks in zones_core.h: `hlogOnReport()` after each N report (ONLINE first/back,
  BOOT when uptime goes down, DROP when drops go up, gap = outage), `hlogOnHealthLine()` for H lines
  (heap, ping/s), `hlogService()` every second from `serviceNodes()` (OFFLINE after 10 s silence,
  not while the Cardputer's own Wi-Fi is down or < 15 s after it came back; online seconds; HEALTH
  snapshot every `snapMin` on the clock; flush every 10 s or 3.5 KB; state every 60 s; day roll).
- Clock: no RTC. User's home network has NO internet. Since fw 2.6.0-dev5 (issue #7): LOG SETUP >
  TIME SERVER = IPv4 of an NTP server on the LAN (prefs "hlog"/"ntp", "" = AUTO internet pools;
  the user runs `timeserver/` on an Ubuntu Server). `hlogNtpStart()` = `configTzTime` + sync
  callback (`gNtpSynced` flag, logged in `hlogService`: "clock set (time server <ip>)", later
  "clock corrected +N s" if >= 2 s) + `sntp_set_sync_interval` 1 h (SDK default is 3 h); restarted
  on Wi-Fi back and on a server change. `hlogNtpStatus()` = ok / asking / no answer. Fallback:
  the clock is set by hand in LOG SETUP > CLOCK (`runClockSet`, `hlogSetClock` ->
  settimeofday, SYS "clock set (by hand)"). System time survives software restarts (RTC timer;
  "clock set (kept over restart)"), not power-off. Last known time in prefs "hlog"/"last" every
  10 min pre-fills the CLOCK screen. Bottom screen shows "set clock: ..." after 30 s without a
  clock; settings LOGS row says NO CLOCK. Lines before the clock is set wait in `gPend[24]` and
  get their real time at clock set. Time zones = POSIX strings in `kTzOpts`.
- Settings prefs "hlog": last (epoch), on, snap (1/5/10/15/30 min), keep (1/3/5/7/14/30 days, default 5,
  today counts), tz (default New York), web, ntp. LOG SETUP rows (15 px): CLOCK, TIME SERVER, LOGGING,
  SNAPSHOT, KEEP, TIME ZONE, WEB ACCESS. Keys in LOG SETUP: `,` `/` change.
- LOGS menu (`runLogsMenu`, logs_ui.h): day list only (date, summary, size; `;` `.` scroll,
  `s` LOG SETUP). No on-device viewer: removed in 2.6.0-dev2 at the user's request (RAM:
  indexing a day file needs ~15-20 KB, no PSRAM). Files are read via the web page.
- Web (`include/web_logs.h`, WebServer port 80, `handleClient()` from `serviceNodes()`): `/`,
  `/logs.json`, `/logs/<day>.csv` (`?view=1` inline). Name checked with `ymdFromName()`.

## Build / run
- `pio run -e cardputer-radar-csi -t upload` — Cardputer console (name kept from
  v1; only build). CI output `cardputer-csi-st7789v.bin`, flash at 0x0.
- `cd tools/node_esp32 && pio run -t upload` — sensor node. CI output `csi-node-esp32.bin`.
- CI (`build.yml`) builds both on pushes to `main`, PRs, manual runs.
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
- SD card on the panel's bus: M5GFX `spi::init` does `SPI.end(); SPI.begin(sclk, miso, mosi)`
  for SPI2_HOST, and Arduino `SPI.begin()` is a no-op once begun, so the panel config must
  carry the SD MISO (`pin_miso = EXT_SD_MISO` = 39). With -1 the card never answers
  (fw 2.6.0-dev1/dev2 showed NO SD with a working card).
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
- `include/health_log.h` — node health log: SD, clock, events, day files, state, summary.
- `include/logs_ui.h` — LOGS menu (day list), LOG SETUP, HEALTH view.
- `include/web_logs.h` — log web page / downloads (port 80).
- `tools/node_esp32/` — node firmware (PlatformIO, esp32dev).
- `tools/csi_lab/` — PC tools (Python 3.10+, numpy, matplotlib): record.py
  (raw CSI from a node), replay.py (score v1/v2, --hold/--chain/--gate, plots),
  fit.py (v2 weights, leave-one-room-out), detectors.py (V1 exact port, V2),
  health.py (node uptime/health logger, stdlib only),
  v2_host_test.cpp + v2_parity.py (compile csi_v2.h on the PC, compare with V2).
- `docs/V2_ROADMAP.md` — v2 goals and the motion-detection rework plan.
- `timeserver/` — offline LAN NTP server package for a USB key (Windows: PowerShell server as a
  startup task, replaces W32Time; Linux: chrony config or bundled Python server). For next step 3.

## Status (v2)
🧪 fw 2.6.0-dev9 (main): del key erases while typing the Wi-Fi password (`waitKey()` returns `'\b'` on `st.del`).
🧪 fw 2.6.0-dev8 (main): Wi-Fi password shown in clear text while typing (tail shown, `<` prefix, past 35 chars).
🧪 fw 2.6.0-dev7 (main, merged from branch `fix-issues-5-6-7` by PR #8, branch deleted): nodes RTC
watchdog + reset step, Cardputer logs RESET/INFO (issue #5); dev6 = external backlight check +
SCREEN OFF (#6); dev5 = TIME SERVER (#7).
🧪 fw 2.6.0-dev4 (main, merged from branch `sd-logs` by PR #4; dev4 = manual CLOCK for the no-internet network, dev3 = SD MISO fix, dev2 showed NO SD, Cardputer only): node health log on SD, LOGS list,
HEALTH view, web download (2.6.0-dev1 also had an on-device log viewer, removed). Not yet tested on hardware (SD on the shared bus is the
main risk). Design canvas "Cardputer Node Health Log Screens".
🧪 fw 2.5.0-dev2 (main, merged from branches `v2.5` + `node-reliability` on 2026-11-15): node
self-recovery from node-reliability + Cardputer cone in %, node diagnostics, no `r` key from v2.5, WiFiUDP
flush fixes. Not yet tested on hardware. README previews in docs/screens re-rendered for 2.5
(PIL, DejaVu Sans Mono, from the drawing code; not device photos).
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
🧪 fw 2.1.0-dev2: node self-recovery (watchdog, link check, backup reconnect,
heap guard, health line, LED heartbeat) and the WiFiUDP flush fix on nodes and
Cardputer; not yet
tested on hardware. Acceptance: router reboot / router off 30 min -> every node
back within 60 s; forced hang -> watchdog reboot within ~40 s; 7-day soak of
3 nodes >= 99.99% (health.py), router downtime excluded. Router: Cisco/Linksys E3000.

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
2. **Node uptime soak**: nodes on 2.5.0-dev2, Cardputer on 2.6.0-dev4 with an SD card and the clock set;
   7 days, read online % in the HEALTH view (day files via the web page; health.py
   still works), then the router reboot / router off tests.
3. **GitHub issues (fixes on main since PR #8):** #7 clock from a LAN time server (done in
   fw 2.6.0-dev5, not yet tested on hardware), #6 external screen black + backlight off until
   reboot, bottom screen fine (fw 2.6.0-dev6: backlight check + screen-off timer; check the day
   log for "ext backlight lost"; if it goes dark with no such line, suspect wiring/power), #5
   one node (WROOM-32, red power LED + purple GPIO2 LED) goes offline
   after ~30 min with the heartbeat stopped; log shows BOOT + WDT only after a power cycle
   (fw 2.6.0-dev7: RTC watchdog + reset step; read the RESET / INFO lines in the day log).
4. **Signed reports and commands** (roadmap phase 5).
5. **Presence event log** on the bottom screen with optional SD persistence.
6. **HLK-LD2410 mmWave** (24 GHz, UART, 5 V) on the Grove port (G1/G2 = Serial1)
   for still people + distance; dots in the range cone (design agreed).
7. **ESP32-C5 nodes** (MonsterC5) later: different `wifi_csi_acquire_config_t`.

## Hardware inventory (chip cheat-sheet)
- C5 (best CSI): MonsterC5, AWOK Dual Touch (2x each), Marauder v8.
- C3: BLEShark Nanos (many; mesh-capable).
- S3: Cardputer ADV (console, COM18); S3 DevKit (sensor, COM44).
- Classic ESP32: Cheap Yellow Displays, HaleHound (CYD-based).
- Not ESP32: Cardputer Zero (Raspberry Pi CM Zero), Hackberry Pi, HackRF/PortaPack.
