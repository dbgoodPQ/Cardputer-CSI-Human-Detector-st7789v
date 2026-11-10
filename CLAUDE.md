# CLAUDE.md — WiFi-CSI Radar

> Context handoff for continuing this project in Claude Code / VS Code.
> Standalone project — a handheld WiFi-CSI people sensor with a dual-screen UI.

## Goal
A **handheld device that senses people through WiFi** using Channel State
Information (CSI). Realistic target: on-device **presence + motion**.

## Architecture
- **Sensor:** ESP32-**S3** DevKit (prototype), runs `tools/sensor_s3/sensor_s3.ino`.
  Hosts a WiFi AP (`SQUACHNET`), captures CSI from the Cardputer's own WiFi
  frames, computes amplitude variance as a motion proxy, emits `R,...` frames
  over UDP broadcast at ~15 Hz. **Future goal:** migrate to ESP32-C5 (MonsterC5)
  which has a better CSI PHY.
- **Console:** **Cardputer ADV** (ESP32-**S3**) connects to `SQUACHNET`, receives
  UDP frames on port 4210, runs the UI, drives two screens.
- **Displays (different content per screen, NOT a clone):**
  - Top = external **Waveshare 2" ST7789V** (320x240 @ rotation `EXT_ROTATION`=1): PPI **radar scope**
    (rotating sweep + phosphor trail; motion>threshold paints stable contact
    blips — radius from RSSI, size/heat from motion).
  - Bottom = built-in 1.14" (240x135): WiFi IP, status pill, PRESENCE/CLEAR
    banner, scrolling motion graph, threshold + key hints.
- **No router needed (UART/UDP builds only):** S3 is the AP; Cardputer connects
  directly to it. The on-device CSI build uses the home router instead.

## On-device CSI pipeline (`cardputer-radar-csi`, ST7789V fork)
- `csiCallback` keeps only frames whose `info->mac` == router BSSID, queues the
  first 128 bytes (L-LTF). `startPing()` pings the gateway at 50 Hz (esp_ping).
- `processCsiPkt`: 52 data subcarriers (1..26, 38..63), divided by their mean
  (AGC cancel); `gCsiMetric` = mean per-subcarrier variance over `kCsiWin`=25 frames.
- `serviceCsi` (15 Hz): motion = (metric/ref - 1)/`kCsiSpan`; ref = `gCalRef`
  (empty-room mean+3sd, key c) or `kAutoMargin` x slow floor (AUTO). Modes:
  AUTO | CAL | RUN | MOVED | NO CSI. Threshold `gThreshold` (, /), saved in prefs.
- IMU guard (`serviceImu`, BMI270 via M5.Imu on In_I2C, 50 Hz): gyro > 6 dps or
  |accel - gravity| > 0.05 g => MOVED, CSI window flushed, floor frozen, until
  2 s still. Tilt > 5 deg or > 1.5 s of movement = relocation => calibration
  dropped (user must press c; no auto-recal, the user is usually in the room).

## Wire protocol (UDP port 4210, newline-terminated ASCII, ~15 Hz)
- S3 → Cardputer: `R,<seq>,<presence 0|1>,<motion 0.000-1.000>,<rssi>,<mode>`
  - e.g. `R,1042,1,0.37,-54,RUN`  (mode = CAL | RUN | IDLE)
- Cardputer → S3: `CAL` | `CALSTOP` | `THR <float>` | `RATE <hz>` | `PING`→`PONG`

## Build / run
- `pio run -e cardputer-radar-csi -t upload` — **on-device CSI** (main build
  of this fork; what CI builds).
- `pio run -e cardputer-radar -t upload` — **fake-data mode** (`RADAR_FAKE=1`),
  no sensor needed.
- `pio run -e cardputer-radar-wifi -t upload` — live WiFi UDP mode; S3 must be
  running and `SQUACHNET` must be up before the Cardputer boots.
- `cd tools/sensor_s3 && pio run -t upload` — flash the S3 sensor (COM44).
- `tools/fake_c5/fake_c5.ino` — spare-S3 UART emitter for wired link testing.

## Hard constraints (don't regress these)
- Build sets `ARDUINO_USB_CDC_ON_BOOT=1` → **`Serial` is USB-C**. Any sensor
  link MUST use a HardwareSerial (`Serial1`), never `Serial`.
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
- `include/radar_link.h` — protocol parser + state + 240-sample motion history +
  command senders. No display deps. Has `injectLine()` for fake frames.
- `src/main.cpp` — UI: `drawBottom()` (status), `drawTop()` (radar scope),
  `drawRaycaster()` (vaporwave 3D mode), `serviceKeys()`, fake generator.
  Bottom @30 fps, top @~8 fps.
- `include/ext_panel.h` — external ST7789V panel class (`LGFX_ExtST7789`).
- `tools/sensor_s3/` — S3 CSI sensor firmware (AP mode, UDP broadcast).
- `tools/fake_c5/fake_c5.ino` — spare-S3 UART protocol emitter.

## Status
✅ **On-device CSI build (`cardputer-radar-csi`) is the main target of this fork.**
Cardputer ADV joins the home Wi-Fi (picker on first boot, creds in prefs
`wificreds`), senses motion from router frames only, drives both screens.
User-tested: detection clearly better after the rework (router-only CSI,
50 Hz gateway ping, AGC-normalised L-LTF shape variance, calibration).
✅ External Waveshare 2" ST7789V working (pinout confirmed by user).
✅ CI: `.github/workflows/build.yml` builds on push/PR/manual and uploads
`cardputer-csi-st7789v.bin` (merged, flash at 0x0); `release.yml` on `v*` tags.
⚠️ Libraries pinned: `M5GFX@0.2.36`, `M5Unified@0.2.29`, `M5Cardputer@1.1.1`.
M5GFX 0.3.0's board detection probes G5/G6 (= external CS/DC) and misdetects
the ADV (rotated internal screen, dead external screen). Don't unpin without
re-testing on hardware.
✅ IMU device-motion guard (pauses sensing while the Cardputer moves).
⚠️ Tuning constants are first estimates, not yet tuned on real data:
`kCsiSpan`=4, `kAutoMargin`=2, `kCsiWin`=25, hold `kHold`=150 (10 s),
default threshold 0.15, IMU thresholds 6 dps / 0.05 g / 2 s settle.
ℹ️ Blip angle is random and radius follows router RSSI: scope position is
illustrative only. Spycam icons = OUI match on sniffed MACs (many false
positives: ESP32, Amazon, Realtek are generic).
ℹ️ The UART/UDP builds (`cardputer-radar`, `cardputer-radar-wifi`) and
`tools/sensor_s3` are inherited from the original and untouched by this fork.

## Next steps (priority order)
1. **Tune on real data:** log `gCsiMetric`, ref and motion over Serial while
   walking in/out, then set `kCsiSpan` / `kAutoMargin` / default threshold.
2. **Persist calibration** per BSSID in prefs (currently lost on reboot).
3. **Bearing / distance:** not possible with one antenna; keep scope honest or
   explore a 2nd receiver (C5) for RSSI/phase deltas.
4. **Spycam icons:** show vendor name next to icon, add a setting to hide them.
5. **TODO (user request, later): add mmWave radar HLK-LD2410** (24 GHz, UART,
   5 V) on the Grove port (G1/G2 = Serial1). Detects still people (breathing
   micro-motion) + distance gates; fuse with CSI (contact if either is
   confident, distance from the radar). Grove can do UART *or* I2C, not both.
6. **Presence event log** on the bottom screen with optional SD persistence.
7. **Port sensing to ESP32-C5** (MonsterC5) later: different
   `wifi_csi_acquire_config_t` (no `lltf_en`, `sig_mode`, ...).

## Hardware inventory (chip cheat-sheet)
- C5 (best CSI): MonsterC5, AWOK Dual Touch (2x each), Marauder v8.
- C3: BLEShark Nanos (many; mesh-capable).
- S3: Cardputer ADV (console, COM18); S3 DevKit (sensor, COM44).
- Classic ESP32: Cheap Yellow Displays, HaleHound (CYD-based).
- Not ESP32: Cardputer Zero (Raspberry Pi CM Zero), Hackberry Pi, HackRF/PortaPack.
