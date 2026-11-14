// =============================================================================
// WiFi-CSI Radar v2 — console (Cardputer ADV)
//
// The Cardputer does NOT sense anything itself: paired ESP32 sensor nodes
// (tools/node_esp32) measure Wi-Fi CSI motion in each room and report it over
// UDP. The Cardputer joins the home Wi-Fi, pairs and manages the nodes, and
// shows their motion on two screens:
//   * External Waveshare 2" ST7789V (top): range cone or ZONES list (key .).
//   * Built-in screen (bottom): status, CONTACT/CLEAR banner, zone chips, graph.
//
// Keys:  , / = sensitivity (v2 cut-off -/+, sent to every node)
//        n = cone zone   r = 5 m / 10 m   . = view   ` = settings
// No calibration: v2 nodes need none.
// =============================================================================
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <math.h>
#include "ext_panel.h"
#include <Preferences.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "fw_version.h"

static bool  wifiReady = false;
static char  wifiIP[16] = "---";

static M5Canvas        canvas(&M5Cardputer.Display);   // bottom (built-in) buffer
static LGFX_ExtST7789  extPanel;                       // external Waveshare 2" ST7789V
static M5Canvas        topCanvas(&extPanel);           // top render buffer (240x180)
static bool            extReady   = false;
static bool            gRange10   = false;  // range cone: false = 5 m / 1 m lines, true = 10 m / 2 m
// v2 sensitivity = smoothness cut-off sent to every node (C,SENS). Lower = more
// sensitive. Default = the tuned value (docs/V2_ROADMAP.md).
static const float     kSensDefault = 0.42f, kSensMin = 0.30f, kSensMax = 0.60f, kSensStep = 0.02f;
static float           gSens = kSensDefault;
static const float     EXT_ZOOM   = 320.0f / 240.0f;   // 240x180 * 1.333 = 320x240, fills panel

// ── Color palettes & runtime settings ─────────────────────────────────────────
struct Palette { uint8_t aR,aG,aB, bR,bG,bB; const char* name; };
static const Palette kPalettes[] = {
    { 220,   0, 200,   0, 220, 220, "MAGENTA" },
    {   0, 220,  80,   0, 180, 100,  "GREEN"  },
    { 255, 180,   0, 220, 140,   0,  "AMBER"  },
    { 255,  40,  40, 255, 120,  40,  "RED"    },
    {  40, 120, 255,   0, 200, 255,  "BLUE"   },
};
static const uint8_t kNumPalettes = 5;

static uint8_t  gColorIdx     = 0;
static uint8_t  gBright       = 80;
static uint8_t  gExtBright    = 80;
static uint16_t gColA         = 0;   // primary accent (magenta role)
static uint16_t gColB         = 0;   // secondary accent (cyan role)
static bool     gMenuOpen   = false;
static uint8_t  gMenuCursor = 0;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

static void applyPalette(uint8_t idx) {
    if (idx >= kNumPalettes) idx = 0;
    gColorIdx = idx;
    gColA = rgb565(kPalettes[idx].aR, kPalettes[idx].aG, kPalettes[idx].aB);
    gColB = rgb565(kPalettes[idx].bR, kPalettes[idx].bG, kPalettes[idx].bB);
}

static void applyBrightness() {
    M5Cardputer.Display.setBrightness((uint8_t)((uint32_t)gBright * 255 / 100));
    // EXT panel BL is on GPIO4 (PWM) — set the external backlight directly
    extPanel.setBrightness((uint8_t)((uint32_t)gExtBright * 255 / 100));
}

// ST7789V fork: the Waveshare module's BL pin is wired to GPIO4, so EXT PANEL
// brightness drives the backlight PWM (applyBrightness) instead of scaling pixels.
// Pixel-buffer dimming is kept only as a fallback: build with -DEXT_SOFT_DIM=1
// if BL is wired to 3V3 instead of GPIO4.
#ifndef EXT_SOFT_DIM
#define EXT_SOFT_DIM 0
#endif
static void applyExtBrightness() {
    if (!EXT_SOFT_DIM || gExtBright >= 100) return;
    uint16_t* buf = (uint16_t*)topCanvas.getBuffer();
    if (!buf) return;
    const uint32_t scale = (uint32_t)gExtBright * 256 / 100;
    const int n = topCanvas.width() * topCanvas.height();
    for (int i = 0; i < n; i++) {
        uint16_t px = __builtin_bswap16(buf[i]);
        uint8_t r = (((px >> 11) & 0x1F) * scale) >> 8;
        uint8_t g = (((px >>  5) & 0x3F) * scale) >> 8;
        uint8_t b = (( px        & 0x1F) * scale) >> 8;
        buf[i] = __builtin_bswap16((r << 11) | (g << 5) | b);
    }
}

static void saveSettings() {
    Preferences prefs;
    prefs.begin("radar", false);
    prefs.putUChar("palette",   gColorIdx);
    prefs.putUChar("bright",    gBright);
    prefs.putUChar("extbright", gExtBright);
    prefs.putUChar("sens",      (uint8_t)lroundf(gSens * 100));   // v1 "thresh" no longer used
    prefs.putUChar("range10",   gRange10 ? 1 : 0);
    prefs.end();
}

static void loadSettings() {
    Preferences prefs;
    prefs.begin("radar", true);
    gColorIdx  = prefs.getUChar("palette",   0);
    gBright    = prefs.getUChar("bright",   80);
    gExtBright = prefs.getUChar("extbright",80);
    uint8_t sens = prefs.getUChar("sens", (uint8_t)lroundf(kSensDefault * 100));
    gRange10   = prefs.getUChar("range10", 0) != 0;
    prefs.end();
    if (sens >= lroundf(kSensMin * 100) && sens <= lroundf(kSensMax * 100)) gSens = sens / 100.0f;
    if (gColorIdx >= kNumPalettes) gColorIdx = 0;
    if (gBright    < 10 || gBright    > 100) gBright    = 80;
    if (gExtBright < 10 || gExtBright > 100) gExtBright = 80;
    applyPalette(gColorIdx);
}

// Small battery icon: 16x7 (14px body + 2px nub).  Level=-1 → grey/unknown.
static void drawBatIcon(M5Canvas& c, int bx, int by) {
  int  level = (int)M5.Power.getBatteryLevel();
  bool chg   = (M5.Power.isCharging() == 1);
  uint16_t col;
  if      (level < 0)    col = c.color565(100, 100, 100);  // unknown
  else if (chg)          col = c.color565(  0, 220, 255);  // charging: cyan
  else if (level > 50)   col = c.color565(  0, 200,  80);  // good: green
  else if (level > 20)   col = c.color565(220, 160,   0);  // low: amber
  else                   col = c.color565(220,  30,  30);  // critical: red
  c.drawRect(bx, by, 14, 7, col);                          // body
  c.fillRect(bx + 14, by + 2, 2, 3, col);                  // nub
  if (level > 0) {
    int fill = (level * 12 + 50) / 100;
    if (fill > 12) fill = 12;
    c.fillRect(bx + 1, by + 1, fill, 5, col);
  }
  if (chg) {                                                // ⚡ cross when charging
    c.drawFastHLine(bx + 4, by + 3, 5, c.color565(255, 255, 255));
    c.drawFastVLine(bx + 6, by + 1, 5, c.color565(255, 255, 255));
  }
  char pct[5];
  if (level < 0) snprintf(pct, sizeof(pct), "--%");
  else           snprintf(pct, sizeof(pct), "%d%%", level);
  c.setTextSize(1);
  c.setTextColor(col, c.color565(20, 0, 35));   // same color as icon, bar background
  c.drawString(pct, bx + 17, by);
}

// ── Wi-Fi (home network; the nodes join the same one) ─────────────────────────
static bool loadWifiCreds(char* ssid, size_t sl, char* pass, size_t pl) {
    Preferences p;
    p.begin("wificreds", true);
    bool ok = p.isKey("ssid");
    if (ok) { p.getString("ssid", ssid, sl); p.getString("pass", pass, pl); }
    p.end();
    return ok && ssid[0] != '\0';
}

static void saveWifiCreds(const char* ssid, const char* pass) {
    Preferences p;
    p.begin("wificreds", false);
    p.putString("ssid", ssid ? ssid : "");
    p.putString("pass", pass ? pass : "");
    p.end();
}

// Called whenever the Wi-Fi comes up. Power save off: the nodes' UDP broadcasts
// (10 Hz) would otherwise be delayed to the router's DTIM beacons.
static void onWifiUp() {
    strncpy(wifiIP, WiFi.localIP().toString().c_str(), sizeof(wifiIP) - 1);
    wifiReady = true;
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
}

// Blocking WiFi picker UI. Returns true if connected on exit, false if cancelled.
static bool runWifiPicker() {
    const int W = canvas.width(), H = canvas.height();
    const uint16_t cBg  = canvas.color565( 4,  0,  8);
    const uint16_t cBar = canvas.color565(20,  0, 35);

    // helpers ─────────────────────────────────────────────────────────────────
    auto hdr = [&](const char* t) {
        canvas.fillRect(0, 0, W, 14, cBar);
        canvas.drawFastHLine(0, 13, W, gColA);
        canvas.setTextSize(1); canvas.setTextColor(gColA, cBar);
        canvas.drawString(t, (W - canvas.textWidth(t)) / 2, 3);
    };
    auto ftr = [&](const char* t) {
        canvas.fillRect(0, H - 13, W, 13, cBar);
        canvas.drawFastHLine(0, H - 13, W, gColA);
        canvas.setTextSize(1); canvas.setTextColor(canvas.color565(70, 70, 70), cBar);
        canvas.drawString(t, (W - canvas.textWidth(t)) / 2, H - 10);
    };
    // wait for a single keypress, return first char in word
    auto waitKey = [&]() -> char {
        for (;;) {
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto w = M5Cardputer.Keyboard.keysState().word;
                if (!w.empty()) return w[0];
            }
            delay(10);
        }
    };

    // disconnect ──────────────────────────────────────────────────────────────
    wifiReady = false;
    WiFi.disconnect();
    delay(300);
    WiFi.mode(WIFI_STA);

    // scan ────────────────────────────────────────────────────────────────────
    int n = 0;
    auto doScan = [&]() {
        canvas.fillSprite(cBg); hdr(">> WIFI SELECT <<");
        canvas.setTextSize(1); canvas.setTextColor(gColB, cBg);
        const char* sm = "Scanning...";
        canvas.drawString(sm, (W - canvas.textWidth(sm)) / 2, H / 2 - 4);
        ftr(""); canvas.pushSprite(0, 0);
        n = WiFi.scanNetworks();
        if (n < 0) n = 0; if (n > 16) n = 16;
    };
    doScan();

    // SSID list ───────────────────────────────────────────────────────────────
    int cursor = 0, scroll = 0, pickedIdx = -1;
    const int rowH = 14, visRows = (H - 29) / rowH;

    while (pickedIdx < 0) {
        canvas.fillSprite(cBg); hdr(">> WIFI SELECT <<");
        if (n == 0) {
            canvas.setTextSize(1); canvas.setTextColor(canvas.color565(80, 80, 80), cBg);
            const char* nm = "No networks found";
            canvas.drawString(nm, (W - canvas.textWidth(nm)) / 2, H / 2 - 4);
            ftr("any key:rescan  `:cancel");
        } else {
            for (int vi = 0; vi < visRows; vi++) {
                int ni = scroll + vi; if (ni >= n) break;
                int y = 16 + vi * rowH; bool sel = (ni == cursor);
                canvas.setTextColor(sel ? gColA : TFT_BLACK, cBg);
                canvas.drawString(">", 3, y + 3);
                char ssid[22]; strncpy(ssid, WiFi.SSID(ni).c_str(), sizeof(ssid) - 1); ssid[21] = '\0';
                canvas.setTextColor(sel ? TFT_WHITE : canvas.color565(80, 80, 80), cBg);
                canvas.drawString(ssid, 12, y + 3);
                bool open = (WiFi.encryptionType(ni) == WIFI_AUTH_OPEN);
                char rhs[10]; snprintf(rhs, sizeof(rhs), "%s%d", open ? "  " : "* ", WiFi.RSSI(ni));
                canvas.setTextColor(sel ? gColB : canvas.color565(50, 50, 50), cBg);
                canvas.drawString(rhs, W - canvas.textWidth(rhs) - 3, y + 3);
            }
            ftr(";/.:nav  /:select  `:cancel");
        }
        canvas.pushSprite(0, 0);

        char c = waitKey();
        if (c == '`') {
            // cancel — restore previous connection if possible
            char cs[64] = {}, cp[64] = {};
            if (loadWifiCreds(cs, sizeof(cs), cp, sizeof(cp))) {
                if (strlen(cp) > 0) WiFi.begin(cs, cp); else WiFi.begin(cs);
                uint32_t t0 = millis();
                while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
            }
            if (WiFi.status() == WL_CONNECTED) onWifiUp();
            return wifiReady;
        }
        if (n == 0) {
            doScan();
        } else {
            if      (c == ';') { if (cursor > 0) cursor--; }
            else if (c == '.') { if (cursor < n - 1) cursor++; }
            else if (c == '/') { pickedIdx = cursor; }
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + visRows) scroll = cursor - visRows + 1;
        }
    }

    // password entry ──────────────────────────────────────────────────────────
    char newSsid[64]; strncpy(newSsid, WiFi.SSID(pickedIdx).c_str(), sizeof(newSsid) - 1); newSsid[63] = '\0';
    bool isOpen = (WiFi.encryptionType(pickedIdx) == WIFI_AUTH_OPEN);
    char newPass[64] = {};

    if (!isOpen) {
        char cs[64] = {}, cp[64] = {};
        if (loadWifiCreds(cs, sizeof(cs), cp, sizeof(cp)) && strcmp(cs, newSsid) == 0)
            strncpy(newPass, cp, sizeof(newPass) - 1);

        for (;;) {
            canvas.fillSprite(cBg);
            char h[30]; snprintf(h, sizeof(h), "PWD: %.18s", newSsid); hdr(h);
            int pl = (int)strlen(newPass);
            char stars[65]; memset(stars, '*', pl); stars[pl] = '\0';
            char field[70]; snprintf(field, sizeof(field), "> %s_", stars);
            canvas.setTextSize(1); canvas.setTextColor(TFT_WHITE, cBg);
            canvas.drawString(field, 8, H / 2 - 6);
            ftr("type pwd  /:confirm  `:cancel"); canvas.pushSprite(0, 0);

            char c = waitKey();
            if (c == '/') break;
            if (c == '`') return false;
            if (c == '\b' || c == 0x7f || c == 8) {
                int l = (int)strlen(newPass); if (l > 0) newPass[l - 1] = '\0';
            } else if (c >= 0x20 && c <= 0x7e) {
                int l = (int)strlen(newPass); if (l < 63) { newPass[l] = c; newPass[l + 1] = '\0'; }
            }
        }
    }

    // connect ─────────────────────────────────────────────────────────────────
    canvas.fillSprite(cBg); hdr(">> CONNECTING <<");
    char cm[48]; snprintf(cm, sizeof(cm), "Joining %s...", newSsid);
    canvas.setTextSize(1); canvas.setTextColor(gColB, cBg);
    canvas.drawString(cm, (W - canvas.textWidth(cm)) / 2, H / 2 - 4); canvas.pushSprite(0, 0);

    if (isOpen || strlen(newPass) == 0) WiFi.begin(newSsid);
    else WiFi.begin(newSsid, newPass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

    canvas.fillSprite(cBg);
    if (WiFi.status() == WL_CONNECTED) {
        onWifiUp(); saveWifiCreds(newSsid, newPass);
        char sm[48]; snprintf(sm, sizeof(sm), "OK: %s", wifiIP);
        canvas.setTextSize(1); canvas.setTextColor(canvas.color565(0, 220, 80), cBg);
        canvas.drawString(sm, (W - canvas.textWidth(sm)) / 2, H / 2 - 4);
    } else {
        canvas.setTextSize(1); canvas.setTextColor(TFT_RED, cBg);
        const char* fm = "Connection failed!";
        canvas.drawString(fm, (W - canvas.textWidth(fm)) / 2, H / 2 - 4);
    }
    canvas.pushSprite(0, 0); delay(1500);
    return wifiReady;
}

static void initWifi() {
    WiFi.mode(WIFI_STA);
    char savedSsid[64] = {}, savedPass[64] = {};
    bool hasSaved = loadWifiCreds(savedSsid, sizeof(savedSsid), savedPass, sizeof(savedPass));

    if (!hasSaved) {
        if (!runWifiPicker()) { delay(1000); ESP.restart(); }
        return;
    }

    if (strlen(savedPass) > 0) WiFi.begin(savedSsid, savedPass);
    else WiFi.begin(savedSsid);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
        canvas.fillSprite(TFT_BLACK);
        canvas.setTextSize(1);
        canvas.setTextColor(gColB, TFT_BLACK);
        char msg[48]; snprintf(msg, sizeof(msg), "Joining %s...", savedSsid);
        canvas.drawString(msg, (canvas.width() - canvas.textWidth(msg)) / 2, canvas.height() / 2 - 4);
        canvas.pushSprite(0, 0);
        delay(200);
    }
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(); delay(500);
        if (!runWifiPicker()) { delay(1000); ESP.restart(); }
        return;
    }
    onWifiUp();
    Serial.printf("# WiFi: %s  IP:%s\n", savedSsid, wifiIP);
}


// Tracks Wi-Fi drops / auto-reconnects.
static void serviceWifi() {
    if (wifiReady && WiFi.status() != WL_CONNECTED) {
        wifiReady = false;
        Serial.println("# WiFi lost");
    } else if (!wifiReady && WiFi.status() == WL_CONNECTED) {
        onWifiUp();
        Serial.printf("# WiFi back  IP:%s\n", wifiIP);
    }
}

#include "zones_core.h"      // sensor nodes: storage, UDP reports, pairing, zone helpers

// --------------------------------------------------------------------------
// In-app settings menu (ESC to open/close)
// --------------------------------------------------------------------------
static void menuAdjust(int dir) {
    switch (gMenuCursor) {
        case 0:
            gColorIdx = (gColorIdx + kNumPalettes + (uint8_t)(dir > 0 ? 1 : kNumPalettes - 1)) % kNumPalettes;
            applyPalette(gColorIdx);
            break;
        case 1:
            gBright = (uint8_t)constrain((int)gBright + (dir > 0 ? 10 : -10), 10, 100);
            M5Cardputer.Display.setBrightness((uint8_t)((uint32_t)gBright * 255 / 100));
            break;
        case 2:
            gExtBright = (uint8_t)constrain((int)gExtBright + (dir > 0 ? 10 : -10), 10, 100);
            extPanel.setBrightness((uint8_t)((uint32_t)gExtBright * 255 / 100));
            break;
        case 3:
            gMenuOpen = false;
            runWifiPicker();
            break;
        case 4:
            runNodesMenu();
            break;
        default: break;
    }
}

static void drawMenu() {
    const int W = canvas.width(), H = canvas.height();
    const uint16_t cBg  = canvas.color565(  4,  0,  8);
    const uint16_t cBar = canvas.color565( 20,  0, 35);
    const uint16_t cDim = canvas.color565( 70, 70, 70);
    const uint16_t cSep = canvas.color565( 35,  0, 50);

    canvas.fillSprite(cBg);

    // title bar
    canvas.fillRect(0, 0, W, 14, cBar);
    canvas.drawFastHLine(0, 13, W, gColA);
    canvas.setTextSize(1);
    canvas.setTextColor(gColA, cBar);
    const char* hdr = ">> SETTINGS <<";
    canvas.drawString(hdr, (W - canvas.textWidth(hdr)) / 2, 3);

    // menu rows
    const char* const labels[] = { "PALETTE", "SCREEN", "EXT PANEL", "WIFI NET", "NODES" };
    const int numItems = 5, rowH = 20, textOff = 6;
    for (int i = 0; i < numItems; i++) {
        int  y   = 16 + i * rowH;
        bool sel = (i == (int)gMenuCursor);

        canvas.setTextColor(sel ? gColA : TFT_BLACK, cBg);
        canvas.drawString(">", 4, y + textOff);

        canvas.setTextColor(sel ? TFT_WHITE : cDim, cBg);
        canvas.drawString(labels[i], 16, y + textOff);

        char val[20];
        switch (i) {
            case 0: snprintf(val, sizeof(val), "%s", kPalettes[gColorIdx].name); break;
            case 1: snprintf(val, sizeof(val), "%d%%", (int)gBright); break;
            case 2: snprintf(val, sizeof(val), "%d%%", (int)gExtBright); break;
            case 3: {
                String cur = WiFi.SSID();
                if (cur.length() > 0) snprintf(val, sizeof(val), "%.12s", cur.c_str());
                else strncpy(val, "> SCAN", sizeof(val));
                break;
            }
            case 4: snprintf(val, sizeof(val), "%d >", gNodeCount); break;
            default: val[0] = '\0'; break;
        }
        canvas.setTextColor(sel ? gColB : cDim, cBg);
        canvas.drawString(val, W - canvas.textWidth(val) - 8, y + textOff);

        if (i < numItems - 1) canvas.drawFastHLine(8, y + rowH - 2, W - 16, cSep);
    }

    // footer
    canvas.fillRect(0, H - 14, W, 14, cBar);
    canvas.drawFastHLine(0, H - 14, W, gColA);
    canvas.setTextColor(cDim, cBar);
    const char* hint = ";/.:nav ,//:change enter:open `:save";
    canvas.drawString(hint, 4, H - 11);
    const char* ver = "v" FW_VERSION;
    canvas.setTextColor(gColB, cBar);
    canvas.drawString(ver, W - canvas.textWidth(ver) - 4, 3);

    canvas.pushSprite(0, 0);
}

// --------------------------------------------------------------------------
// Short messages on the bottom banner (key feedback)
// --------------------------------------------------------------------------
static const char* gToast   = nullptr;
static uint32_t    gToastMs = 0;
static void toast(const char* msg) { gToast = msg; gToastMs = millis(); }

// --------------------------------------------------------------------------
// Built-in (bottom) screen: status + controls
// --------------------------------------------------------------------------
static void drawBottom() {
  const int W = canvas.width();
  const int H = canvas.height();
  const uint32_t now = millis();
  float zMax    = 0.0f;
  int   zMaxIdx = 0;
  const int  zAct    = activeZone(now, &zMax, &zMaxIdx);
  const bool present = zAct >= 0;

  const uint16_t cMag    = gColA;
  const uint16_t cCyan   = gColB;
  const uint16_t cPurple = canvas.color565( 70,   0,  70);
  const uint16_t cBar    = canvas.color565( 20,   0,  35);
  const uint16_t cBorder = canvas.color565(160,   0, 160);
  const uint16_t cCorner = canvas.color565(255, 120, 255);

  canvas.fillSprite(TFT_BLACK);

  // title bar: battery | Wi-Fi IP | nodes pill
  canvas.fillRect(0, 0, W, 14, cBar);
  canvas.drawFastHLine(0, 13, W, cBorder);
  canvas.setTextSize(1);
  {
    char ttl[32];
    if (wifiReady) snprintf(ttl, sizeof(ttl), "[WiFi %s]", wifiIP);
    else           snprintf(ttl, sizeof(ttl), "[ NO-WIFI ]");
    canvas.setTextColor(wifiReady ? cCyan : TFT_RED, cBar);
    canvas.drawString(ttl, (W - canvas.textWidth(ttl)) / 2, 3);
  }
  drawBatIcon(canvas, 2, 3);
  char tb[12];
  int  lvl;
  const char* tag = nodesTag(now, tb, sizeof(tb), lvl);
  uint16_t pill = lvl == 2 ? TFT_RED
                : (lvl == 1 ? canvas.color565(255, 140, 0) : canvas.color565(0, 180, 80));
  int tw = canvas.textWidth(tag) + 8;
  canvas.fillRoundRect(W - tw - 3, 1, tw, 11, 3, pill);
  canvas.setTextColor(TFT_WHITE, pill);
  canvas.drawString(tag, W - tw + 1, 3);

  // contact / clear banner
  uint16_t bg     = present ? canvas.color565(60, 0, 60) : canvas.color565(0, 0, 25);
  uint16_t border = present ? cMag : cPurple;
  canvas.fillRoundRect(4, 17, W - 8, 34, 4, bg);
  canvas.drawRoundRect(4, 17, W - 8, 34, 4, border);
  canvas.setTextSize(2);
  const char* lbl = "~~ CLEAR ~~";
  char zoneLbl[24];
  if (gToast && now - gToastMs < 2000)  lbl = gToast;
  else if (!wifiReady)                  lbl = "NO WIFI";
  else if (gNodeCount == 0)             lbl = "NO NODES";
  else if (present) {
    snprintf(zoneLbl, sizeof(zoneLbl), ">> %s <<", getZone(zAct, now).name);
    lbl = zoneLbl;
  }
  canvas.setTextColor(present ? canvas.color565(255, 100, 255) : cCyan, bg);
  canvas.drawString(lbl, (W - canvas.textWidth(lbl)) / 2, 25);

  // zone chips + motion graph (strongest zone over time)
  canvas.setTextSize(1);
  const int gx = 4, gw = W - 8;
  int gy = 54, gh = 50;
  if (gNodeCount > 0) { drawZoneChips(54, now); gy = 69; gh = 35; }
  canvas.drawRect(gx, gy, gw, gh, cBorder);
  int ty = gy + gh - 1 - (int)(0.5f * (gh - 2));     // v2 motion level is 0.5 at the cut-off
  canvas.drawFastHLine(gx + 1, ty, gw - 2, canvas.color565(180, 0, 180));

  int cols = gw - 2;
  const int n = kZoneHist;
  if (cols > n) cols = n;
  int xoff = (gw - 2) - cols;
  for (int i = 0; i < cols; ++i) {
    float v = zoneHistAt(n - cols + i);
    int barh = (int)(v * (gh - 2));
    if (barh < 0) barh = 0;
    if (barh > gh - 2) barh = gh - 2;
    if (barh == 0) continue;
    uint16_t col = (v > 0.5f)
      ? canvas.color565(220,   0, 200)
      : canvas.color565(  0, 160, 200);
    canvas.drawFastVLine(gx + 1 + xoff + i, gy + gh - 1 - barh, barh, col);
  }
  if (gNodeCount == 0) {
    const char* hint = "settings ` > NODES > ADD NODE";
    canvas.setTextColor(canvas.color565(138, 138, 138));
    canvas.drawString(hint, (W - canvas.textWidth(hint)) / 2, gy + gh / 2 - 4);
  }

  // footer
  char foot[48];
  if (gNodeCount > 0)
    snprintf(foot, sizeof(foot), "mot %3d%% %-10s cut %.2f",
             (int)(zMax * 100), getZone(zMaxIdx, now).name, gSens);
  else
    snprintf(foot, sizeof(foot), "mot  --%%             cut %.2f", gSens);
  canvas.setTextColor(cCyan, TFT_BLACK);
  canvas.drawString(foot, 4, gy + gh + 4);
  canvas.setTextColor(cPurple, TFT_BLACK);
  canvas.drawString(".:view  ,/:sensitivity  `:menu", 4, gy + gh + 15);

  // outer frame + corner dots
  canvas.drawFastHLine(0,   0,   W, cBorder);
  canvas.drawFastHLine(0,   H-1, W, cBorder);
  canvas.drawFastVLine(0,   0,   H, cBorder);
  canvas.drawFastVLine(W-1, 0,   H, cBorder);
  canvas.fillRect(0,   0,   2, 2, cCorner);
  canvas.fillRect(W-2, 0,   2, 2, cCorner);
  canvas.fillRect(0,   H-2, 2, 2, cCorner);
  canvas.fillRect(W-2, H-2, 2, 2, cCorner);

  canvas.pushSprite(0, 0);
}


static int gViewMode = 0;   // external screen: 0 = range cone, 1 = zones

// --------------------------------------------------------------------------
// External (top) screen: range cone
// --------------------------------------------------------------------------
// The cone shows one node's zone (or AUTO). The viewer sits at the apex; lines mark distance every 1 m
// (5 m range) or every 2 m (10 m range, key r). Wi-Fi CSI knows THAT something
// moves and how strongly, not where, so motion lights up the whole cone instead
// of drawing a dot at an invented distance. Distance dots arrive with the
// LD2410 mmWave radar (see CLAUDE.md, next steps).
static uint16_t blend565(uint16_t bg, uint16_t fg, float a) {
  if (a <= 0.0f) return bg;
  if (a >= 1.0f) return fg;
  int br = (bg >> 11) & 31, bgc = (bg >> 5) & 63, bb = bg & 31;
  int fr = (fg >> 11) & 31, fgc = (fg >> 5) & 63, fb = fg & 31;
  int r = br + (int)((fr - br) * a), g = bgc + (int)((fgc - bgc) * a), b = bb + (int)((fb - bb) * a);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void drawTop() {
  if (!extReady) return;
  const uint32_t now     = millis();
  // The cone shows one node's zone, or AUTO (key n). No nodes = empty cone.
  const bool     noNodes = gNodeCount == 0;
  const int      coneZ   = coneZoneNow(now);
  const ZoneView zv      = noNodes ? ZoneView{"NO NODES", false, false, false, 0.0f, 0, 0, 0, "-"}
                                   : getZone(coneZ, now);
  const bool     linkOk  = zv.online;
  const bool     present = zv.present;
  const float    zMotion = zv.motion;
  const int      zRssi   = zv.rssi;
  const char*    zMode   = zv.mode;

  const int W = topCanvas.width();   // 240 (x1.333 on the panel)
  const int H = topCanvas.height();  // 180
  const int apexX = 120, apexY = 166, topY = 22, halfTop = 112;
  const int coneH = apexY - topY;    // 144 px = full range

  const uint16_t cMag    = gColA;
  const uint16_t cCyan   = gColB;
  const uint16_t cBar    = topCanvas.color565( 20,   0,  35);
  const uint16_t cBorder = topCanvas.color565(160,   0, 160);
  const uint16_t coneBg  = topCanvas.color565( 12,   0,  24);
  const uint16_t cLine   = present ? topCanvas.color565(180, 0, 180) : topCanvas.color565(90, 0, 90);

  topCanvas.fillSprite(TFT_BLACK);

  // --- cone, row by row: background + motion glow (strong near the device) ---
  const float glow = present ? zMotion : 0.0f;
  for (int y = topY; y <= apexY; ++y) {
    int hw = halfTop * (apexY - y) / coneH;
    float depth = (float)(apexY - y) / coneH;           // 0 at the device, 1 at full range
    float a = glow * (0.55f - 0.43f * depth);
    topCanvas.drawFastHLine(apexX - hw, y, 2 * hw + 1, blend565(coneBg, cMag, a));
  }
  topCanvas.drawLine(apexX, apexY, apexX - halfTop, topY, cBorder);
  topCanvas.drawLine(apexX, apexY, apexX + halfTop, topY, cBorder);

  // --- distance lines + labels ---
  const int step = gRange10 ? 2 : 1;
  topCanvas.setTextSize(1);
  for (int k = 1; k <= 5; ++k) {
    int y  = apexY - k * coneH / 5;
    int hw = halfTop * (apexY - y) / coneH;
    topCanvas.drawFastHLine(apexX - hw, y, 2 * hw + 1, cLine);
    char lb[6]; snprintf(lb, sizeof(lb), "%dm", k * step);
    topCanvas.setTextColor(cCyan);
    topCanvas.drawString(lb, apexX + hw - topCanvas.textWidth(lb) - 1, y - 9);
  }

  // --- state text inside the cone ---
  char big[16] = "";
  const char* small = nullptr;
  uint16_t bigCol = cCyan;
  if (noNodes) {
    strcpy(big, "NO NODES"); bigCol = topCanvas.color565(255, 140, 0); small = "settings ` > NODES > ADD NODE";
  } else if (!wifiReady) {
    strcpy(big, "NO WIFI"); bigCol = TFT_RED; small = "settings ` > WIFI NET";
  } else if (!linkOk) {
    strcpy(big, "OFFLINE"); bigCol = TFT_RED; small = "no report from this node";
  } else if (strcmp(zMode, "NO CSI") == 0) {
    strcpy(big, "NO SIGNAL"); bigCol = TFT_RED; small = "node gets no router frames";
  } else if (strcmp(zMode, "CAL") == 0) {                     // v1 node only
    strcpy(big, "CALIBRATING"); bigCol = topCanvas.color565(255, 140, 0); small = "leave the room";
  } else if (present && zv.held) {
    strcpy(big, "MOTION"); bigCol = topCanvas.color565(255, 120, 255); small = "a few seconds ago";
  } else if (present) {
    snprintf(big, sizeof(big), "MOTION %d%%", (int)lroundf(zMotion * 100));
    bigCol = topCanvas.color565(255, 120, 255); small = "distance unknown (Wi-Fi only)";
  } else {
    strcpy(big, "CLEAR"); bigCol = topCanvas.color565(0, 120, 150);
  }
  topCanvas.setTextSize(2);
  topCanvas.setTextColor(bigCol);
  topCanvas.drawString(big, (W - topCanvas.textWidth(big)) / 2, 58);
  if (small) {
    topCanvas.setTextSize(1);
    topCanvas.setTextColor(topCanvas.color565(200, 160, 216));
    topCanvas.drawString(small, (W - topCanvas.textWidth(small)) / 2, 82);
  }

  // --- the viewer at the apex ---
  topCanvas.fillRoundRect(apexX - 8, apexY - 3, 16, 6, 1, cCyan);

  // --- cone mode badge (only with nodes): AUTO follows motion, MANUAL = picked with n ---
  if (gNodeCount > 0) {
    const bool  autoMode = coneAuto();
    const char* badge    = autoMode ? "AUTO" : "MANUAL";
    const int   bw       = topCanvas.textWidth(badge) + 10;
    topCanvas.setTextSize(1);
    if (autoMode) {
      topCanvas.fillRoundRect(4, 136, bw, 12, 3, cCyan);
      topCanvas.setTextColor(TFT_BLACK, cCyan);
    } else {
      topCanvas.fillRoundRect(4, 136, bw, 12, 3, TFT_BLACK);
      topCanvas.drawRoundRect(4, 136, bw, 12, 3, cCyan);
      topCanvas.setTextColor(cCyan, TFT_BLACK);
    }
    topCanvas.drawString(badge, 9, 138);
    topCanvas.setTextColor(topCanvas.color565(154, 106, 168));
    topCanvas.drawString(autoMode ? "follows motion" : "n: next zone", 4, 152);
  }

  // --- title bar: battery + range | contact state | mode pill ---
  topCanvas.setTextSize(1);
  topCanvas.fillRect(0, 0, W, 14, cBar);
  topCanvas.drawFastHLine(0, 13, W, cBorder);
  drawBatIcon(topCanvas, 2, 3);
  topCanvas.setTextColor(cCyan, cBar);
  topCanvas.drawString(zv.name, 18, 3);              // which zone (range is on the lines)
  const char* title = present ? "[ CONTACT ]" : "[ CLEAR ]";
  bool blink = (now % 600) < 300;
  topCanvas.setTextColor(present ? (blink ? topCanvas.color565(255, 60, 255) : cMag) : cMag, cBar);
  topCanvas.drawString(title, (W - topCanvas.textWidth(title)) / 2, 3);
  const char* tag = noNodes ? "NO NODES" : (!linkOk ? "OFFLINE" : zMode);
  uint16_t pill = (!linkOk || strcmp(zMode, "NO CSI") == 0) ? TFT_RED
                : (strcmp(zMode, "CAL") == 0
                   ? topCanvas.color565(255, 140, 0) : topCanvas.color565(0, 180, 80));
  int tw = topCanvas.textWidth(tag) + 8;
  topCanvas.fillRoundRect(W - tw - 3, 1, tw, 11, 3, pill);
  topCanvas.setTextColor(TFT_WHITE, pill);
  topCanvas.drawString(tag, W - tw + 1, 3);

  // --- footer corners ---
  char stats[24];
  snprintf(stats, sizeof(stats), "M:%d%% %ddBm", (int)lroundf(zMotion * 100), zRssi);
  topCanvas.setTextColor(cCyan);
  topCanvas.drawString(stats, 4, H - 10);
  const char* hint = gNodeCount == 0 ? "[r] 5m/10m" : "[n] zone [r] 5m/10m";
  topCanvas.setTextColor(topCanvas.color565(154, 106, 168));
  topCanvas.drawString(hint, W - topCanvas.textWidth(hint) - 4, H - 10);

  // --- outer frame ---
  topCanvas.drawRect(0, 0, W, H, cBorder);

  // scale-to-fit onto the 320x240 TFT (240x180 * EXT_ZOOM = 320x240, no letterbox)
  applyExtBrightness();
  topCanvas.pushRotateZoom(160, 120, 0.0f, EXT_ZOOM, EXT_ZOOM);
}

// --------------------------------------------------------------------------
#include "zones_ui.h"        // ZONES view + NODES menu

// , / : move the v2 cut-off (lower = more sensitive) and send it to every node.
static void changeSens(float d) {
  gSens += d;
  if (gSens < kSensMin) gSens = kSensMin;
  if (gSens > kSensMax) gSens = kSensMax;
  gSens = roundf(gSens * 100.0f) / 100.0f;
  sendNodeSens();
  saveSettings();
  static char msg[20];
  snprintf(msg, sizeof(msg), "CUT %.2f", gSens);
  toast(msg);
}

static void serviceKeys() {
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
  auto st = M5Cardputer.Keyboard.keysState();
  if (gMenuOpen && st.enter && gMenuCursor == 4) { runNodesMenu(); return; }
  for (char c : st.word) {
    if (gMenuOpen) {
      if      (c == ';') gMenuCursor = (gMenuCursor + 4) % 5;
      else if (c == '.') gMenuCursor = (gMenuCursor + 1) % 5;
      else if (c == ',') menuAdjust(-1);
      else if (c == '/') menuAdjust(+1);
      else if (c == '`') {
        saveSettings();
        gMenuOpen = false;
      }
    } else {
      if      (c == '`')             { gMenuOpen = true; gMenuCursor = 0; }
      else if (c == '.')             gViewMode = (gViewMode + 1) % 2;   // cone, zones
      else if (c == 'r' || c == 'R') { gRange10 = !gRange10; saveSettings(); }
      else if (c == 'n' || c == 'N') {                // cone: next zone (nodes, AUTO)
        if (gViewMode != 0) gViewMode = 0;            // first press just shows the cone
        else coneNext();
      }
      else if (c == ',') changeSens(-kSensStep);   // more sensitive
      else if (c == '/') changeSens(+kSensStep);   // less sensitive
    }
  }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  canvas.setColorDepth(16);
  canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());

  extPanel.init();
  extPanel.setRotation(EXT_ROTATION);
  extPanel.fillScreen(TFT_BLACK);
  topCanvas.setColorDepth(16);
  extReady = (topCanvas.createSprite(240, 180) != nullptr);  // 240x180 * EXT_ZOOM = 320x240, fills panel

  loadSettings();
  applyBrightness();

  loadNodes();
  initWifi();
}

void loop() {
  M5Cardputer.update();
  serviceKeys();
  serviceWifi();
  serviceNodes();

  const uint32_t now = millis();
  static uint32_t lastBot = 0, lastTop = 0;
  if (now - lastBot >= 33) { lastBot = now; if (gMenuOpen) drawMenu(); else drawBottom(); }
  if (extReady && now - lastTop >= 125) { lastTop = now; drawExternalView(); }
}

