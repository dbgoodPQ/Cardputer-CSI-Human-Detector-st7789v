# Cardputer Human Detector — ST7789V edition

A fork of [**skizzophrenic/Cardputer-CSI-Human-Detector**](https://github.com/skizzophrenic/Cardputer-CSI-Human-Detector)
that swaps the external 2.8" ILI9341 screen for a **Waveshare 2inch LCD module (ST7789V, 240x320, IPS)**.

**All credit for the project goes to the original author** (TalkingSasquach / skizzophrenic): the
Wi-Fi CSI sensing concept, the radar scope, the raycaster view, the UI and everything else. This fork
changes the external display driver and reworks the on-device CSI signal processing (see below).
For the project's background, read the
[original README](https://github.com/skizzophrenic/Cardputer-CSI-Human-Detector#readme).
Licensed MIT, same as the original.

---

## Changes from the original

| Area | Original | This fork |
| --- | --- | --- |
| External screen | 2.8" ILI9341 (240x320) | Waveshare 2inch LCD, ST7789V (240x320, IPS) |
| Panel driver (`include/ext_panel.h`) | `Panel_ILI9341_Pork` with a custom init table, class `LGFX_ExtILI9341` | LovyanGFX `Panel_ST7789`, class `LGFX_ExtST7789`; colour inversion on (IPS), no offsets |
| Rotation | `setRotation(7)` | `setRotation(EXT_ROTATION)`, default `1` (landscape 320x240, same as the built-in screen). Use `3` to turn the picture 180° |
| Backlight | BL wired to 5V; EXT PANEL brightness dimmed by scaling every pixel of the frame | BL on **G4** with PWM; EXT PANEL brightness sets the real backlight. Pixel dimming kept as an option (`-DEXT_SOFT_DIM=1`) if BL is wired to 3V3 instead |
| Pins | defined inline | same pins, now `EXT_TFT_*` macros (overridable with `-D` flags) |
| Libraries | `M5Unified@^0.2.11` (now resolves to M5GFX 0.3.0) | pinned `M5GFX@0.2.36`, `M5Unified@0.2.29`, `M5Cardputer@1.1.1`: M5GFX 0.3.0's board detection probes G5/G6 (external CS/DC) and misdetects the Cardputer ADV |
| CSI sources | CSI from every frame on the channel (router, phones, neighbours' devices), so the measurement mostly reflected which device transmitted | only frames sent by the router the Cardputer is connected to (matched by BSSID) |
| CSI traffic | whatever traffic happened to be on the air | the Cardputer pings the gateway 50x per second, so the router sends a steady stream of frames to measure |
| CSI processing | mean raw amplitude + mean phase over all bytes; affected by the receiver's automatic gain changes | L-LTF data subcarriers only (52, present in every frame type), each frame divided by its mean amplitude to cancel gain changes; motion = per-subcarrier variance over a 0.5 s window |
| Motion scale | self-adjusting min/max: noise got stretched toward 100% in an empty room, and long motion faded into the baseline | measured against a reference: the empty-room calibration (`RUN`), or before calibration a slowly tracked quiet floor (`AUTO`) |
| Device-motion guard | moving or bumping the Cardputer showed up as false contacts | the built-in IMU (BMI270) detects when the Cardputer itself moves: sensing pauses (`MOVED`, banner `DEVICE MOVED`) until it has been still for 2 s. A bump keeps the calibration; a real relocation (tilt change over 5° or more than 1.5 s of movement) drops it and asks you to press `c` again (`MOVED:PRESS C`) |
| Status | | `NO CSI` (red) when no router frames arrive; router frames per second shown on the bottom screen footer; sensing restarts on its own after a Wi-Fi reconnect |
| Calibration (`c`) | sent `CAL` to an external sensor board that the on-device CSI build doesn't have, so nothing happened | on-device empty-room calibration: 3 s to leave the room, 10 s measurement (countdown on the bottom screen), then motion is measured against that baseline. `x` returns to `AUTO`. Not saved across reboots: recalibrate after moving the device |
| Threshold (`,` `/`) | changed the on-screen value only; detection used a fixed 0.15 | sets the real detection threshold (default 0.15, as before), saved across reboots |
| CI | release workflow on `v*` tags | adds **Build firmware** workflow: builds on every push / PR / manual run and uploads the `.bin` as an artifact. Release asset renamed `cardputer-csi-st7789v-<tag>.bin` |
| Docs | | this README; `CLAUDE.md` updated for the new panel |

Unchanged: UI, rendering (240x180 canvas zoomed to 320x240), SPI bus (SPI2_HOST shared
with the SD card, 27 MHz), keyboard controls, settings, partitions, build environment.

---

## Wiring (Cardputer ADV header → Waveshare 2inch module)

| Waveshare pin | Cardputer ADV | Notes |
| --- | --- | --- |
| DIN (MOSI) | G14 | shared with the SD card |
| CLK (SCK) | G40 | shared with the SD card |
| CS | G5 | |
| DC | G6 | |
| RST | G3 | |
| BL | G4 | backlight, software-controlled |
| VCC | 3V3 | set the module's voltage selector to 3.3V |
| GND | GND | |

---

## Flash

**Pre-built binary:** open the **Actions** tab → latest **Build firmware** run → download the
`cardputer-csi-st7789v` artifact (or take the `.bin` from a [release](../../releases)). Flash
`cardputer-csi-st7789v.bin` at address `0x0` with [ESP Web Flasher](https://esptool.spacehuhn.com/).

**From source** ([PlatformIO](https://platformio.org/)):

```sh
pio run -e cardputer-radar-csi -t upload
```

On first boot it scans for networks and asks for the password, as in the original.

---

## Keyboard controls

| Key | Action |
|-----|--------|
| `c` | Calibrate: leave the room within 3 s, keep the device still for 10 s. `c` again cancels |
| `x` | Drop the calibration, back to `AUTO` |
| `,` | Decrease detection threshold (5% steps) |
| `/` | Increase detection threshold (5% steps) |
| `` ` `` | Open settings menu (EXT PANEL = external backlight) |

---

## What it can and can't do

The Cardputer measures how the Wi-Fi signal from your router changes as it travels through the room.
It detects **motion**, best when someone moves between the Cardputer and the router, within a few
metres. It can't locate people: a contact's angle on the scope is random and its distance follows the
router's signal strength, so the scope position is illustrative. Someone sitting perfectly still stays
on the scope for about 10 s after their last movement, then disappears. Through-wall sensing is weak.

## Usage tips

- Put the Cardputer down where it will stay; moving it changes the signal far more than a person does.
  The router should be in or near the room, with the area you want to watch between the two.
- The footer shows router frames per second (`.../s`); around 50 is ideal. Below ~15 detection is
  unreliable and calibration fails.
- The status pill shows `AUTO` (uncalibrated), `CAL` (calibrating), `RUN` (calibrated),
  `MOVED` (the Cardputer itself is moving; sensing paused) or
  `NO CSI` (no router frames: the router may be ignoring pings, or it is still starting up).
- Moving the Cardputer to a new spot drops the calibration on purpose: recalibrate there.
  It doesn't recalibrate by itself, because you're usually still in the room right after moving it.
- If you pick it up during calibration, the countdown restarts once it is still again.
- Calibrate (`c`) once the device is in place. `CAL FAILED` means too few router frames or Wi-Fi dropped.
- After calibrating, an empty room should sit near 0% motion. If you get false contacts, raise the
  threshold with `/`; if you're missed, lower it with `,`.

---

## Troubleshooting

- **Picture upside down:** build with `-DEXT_ROTATION=3` (add it to `build_flags` in `platformio.ini`).
- **Colours inverted (black looks white):** set `cfg.invert = false` in `include/ext_panel.h`.
- **Red and blue swapped:** set `cfg.rgb_order = true` in `include/ext_panel.h`.
- **`NO CSI` all the time:** the router doesn't answer ping (some block ICMP from clients, or guest
  networks isolate clients). Try another network or enable ping replies on the router.
- **Screen stays dark:** check BL on G4; or wire BL to 3V3 and build with `-DEXT_SOFT_DIM=1`.

ST7789V settings follow the working Cardputer + Waveshare 2inch setup in
[dbgoodPQ/LauncherDual](https://github.com/dbgoodPQ/LauncherDual).
