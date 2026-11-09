# Cardputer Human Detector — ST7789V edition

A fork of [**skizzophrenic/Cardputer-CSI-Human-Detector**](https://github.com/skizzophrenic/Cardputer-CSI-Human-Detector)
that swaps the external 2.8" ILI9341 screen for a **Waveshare 2inch LCD module (ST7789V, 240x320, IPS)**.

**All credit for the project goes to the original author** (TalkingSasquach / skizzophrenic): the
Wi-Fi CSI sensing, the radar scope, the raycaster view, the UI and everything else. This fork only
changes the external display driver. For what the device does and how it works, read the
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
| CI | release workflow on `v*` tags | adds **Build firmware** workflow: builds on every push / PR / manual run and uploads the `.bin` as an artifact. Release asset renamed `cardputer-csi-st7789v-<tag>.bin` |
| Docs | | this README; `CLAUDE.md` updated for the new panel |

Unchanged: CSI sensing, UI, rendering (240x180 canvas zoomed to 320x240), SPI bus (SPI2_HOST shared
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

## Keyboard controls (unchanged)

| Key | Action |
|-----|--------|
| `c` | Trigger calibration (empty-room baseline) |
| `,` | Decrease detection threshold |
| `/` | Increase detection threshold |
| `` ` `` | Open settings menu (EXT PANEL = external backlight) |

---

## Troubleshooting

- **Picture upside down:** build with `-DEXT_ROTATION=3` (add it to `build_flags` in `platformio.ini`).
- **Colours inverted (black looks white):** set `cfg.invert = false` in `include/ext_panel.h`.
- **Red and blue swapped:** set `cfg.rgb_order = true` in `include/ext_panel.h`.
- **Screen stays dark:** check BL on G4; or wire BL to 3V3 and build with `-DEXT_SOFT_DIM=1`.

ST7789V settings follow the working Cardputer + Waveshare 2inch setup in
[dbgoodPQ/LauncherDual](https://github.com/dbgoodPQ/LauncherDual).
