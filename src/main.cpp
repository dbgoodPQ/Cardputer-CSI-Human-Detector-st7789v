// =============================================================================
// WiFi-CSI Radar — dual-screen console (Cardputer ADV)
//
//   * External 2.8" ILI9341 (top): PPI-style radar scope (sweep + contacts).
//   * Built-in screen (bottom): status pill, PRESENCE/CLEAR, motion graph, keys.
//
//   RADAR_FAKE = 1 (default): synthesizes data, NO sensor needed.
//   RADAR_FAKE = 0          : live frames from the Monster C5 over Serial1/Grove.
//
// Keys:  c = calibrate    , = threshold-    / = threshold+    ` = settings menu
// =============================================================================
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <math.h>
#include "radar_link.h"
#include "ext_panel.h"
#include <Preferences.h>

#ifndef RADAR_FAKE
#define RADAR_FAKE 1
#endif
#ifndef RADAR_RX_PIN
#define RADAR_RX_PIN 1
#endif
#ifndef RADAR_TX_PIN
#define RADAR_TX_PIN 2
#endif

// ── WiFi / UDP receive ─────────────────────────────────────────────────────────
#if defined(RADAR_UDP)
#include <WiFi.h>
#include <WiFiUdp.h>
static WiFiUDP  udpIn;
static bool     wifiReady = false;
static char     wifiIP[16] = "---";
#ifndef HOME_SSID
#define HOME_SSID "your_home_wifi"
#endif
#ifndef HOME_PASS
#define HOME_PASS "your_home_pass"
#endif
#endif

// ── On-device CSI sensing ─────────────────────────────────────────────────────
#if defined(RADAR_CSI)
#include <WiFi.h>
#include "esp_wifi.h"
static bool  wifiReady = false;
static char  wifiIP[16] = "---";

#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "freertos/queue.h"

// Sensing pipeline (reworked in the ST7789V fork):
//  1. The CSI callback keeps only frames transmitted by our own router (BSSID)
//     and queues their L-LTF block (64 subcarriers, present in every frame type).
//  2. A 50 Hz ICMP ping to the gateway guarantees a steady stream of router frames.
//  3. Per frame: amplitude of the 52 data subcarriers divided by their mean. This
//     "shape" cancels the receiver's automatic gain changes.
//  4. Metric = mean over subcarriers of the shape's variance over the last kCsiWin
//     frames: tiny when the room is still, large when someone moves in the link.
//  5. Motion 0..1 = (metric / ref - 1) / kCsiSpan. ref = empty-room calibration
//     (key c) or, before calibration, kAutoMargin x a slowly tracked quiet floor.
struct CsiPkt { int8_t buf[128]; int8_t rssi; };
static QueueHandle_t     gCsiQ     = nullptr;
static uint8_t           gBssid[6] = {0};
static volatile bool     gBssidOk  = false;
static volatile uint32_t gCsiCount = 0;      // router frames accepted
static esp_ping_handle_t gPing     = nullptr;

static const int   kSub        = 52;         // data subcarriers in the L-LTF
static const int   kCsiWin     = 25;         // ~0.5 s at 50 frames/s
static float       gShape[kCsiWin][kSub];
static int         gWinIdx     = 0;
static int         gWinFill    = 0;
static float       gCsiMetric  = 0.0f;       // latest windowed shape variance
static float       gCsiMotion  = 0.0f;       // 0..1, smoothed
static int8_t      gCsiRssi    = -80;        // router RSSI (smoothed)
static float       gRssiEma    = -80.0f;
static float       gFloor      = 0.0f;       // AUTO-mode quiet floor
static uint32_t    gLastPktMs  = 0;
static uint16_t    gPktRate    = 0;          // router frames per second
static const float kCsiThresh  = 0.15f;      // default detection threshold (, and / adjust)
static const float kCsiSpan    = 4.0f;       // motion = 1.0 at (1 + span) x ref
static const float kAutoMargin = 2.0f;       // AUTO: noise ceiling = 2 x quiet floor

// ── Empty-room calibration (key c) ────────────────────────────────────────────
// Records the metric's mean + 3*std in the empty room; motion is then measured
// against that fixed reference. Key x returns to AUTO.
static volatile bool  gCalibrated = false;
static float          gCalRef     = 0.0f;
static const uint32_t kCalLeadMs  = 3000;   // time to step out of the room
static const uint32_t kCalMeasMs  = 10000;  // empty-room measurement
static uint8_t  gCalPhase  = 0;             // 0 idle, 1 lead-in, 2 measuring
static uint32_t gCalT0     = 0;
static uint32_t gCalFailMs = 0;             // shows "CAL FAILED" briefly
static uint32_t gCalOkMs   = 0;             // shows "CALIBRATED" briefly

// ── Device-motion guard (built-in BMI270 IMU) ─────────────────────────────────
// Moving the Cardputer changes the Wi-Fi channel far more than a person does.
// While the IMU sees the device move (and for kSettleMs after), sensing pauses:
// no contacts, the CSI window is flushed, the AUTO floor is frozen. A bump keeps
// the calibration; a real relocation (orientation change or long movement)
// drops it, because the empty-room baseline no longer matches the new spot.
static bool     gImuOk        = false;
static float    gGrav[3]      = {0.0f, 0.0f, 1.0f};  // slow-tracked gravity (g)
static float    gGravStill[3] = {0.0f, 0.0f, 1.0f};  // gravity at the last still spot
static bool     gImuMoving    = false;               // guard active
static uint32_t gImuMoveT0    = 0;                   // first movement sample
static uint32_t gImuMoveMs    = 0;                   // last movement sample
static uint32_t gRelocMs      = 0;                   // shows "MOVED: PRESS C"
static const float    kGyroThr  = 6.0f;    // deg/s
static const float    kAccThr   = 0.05f;   // g, deviation from gravity
static const uint32_t kSettleMs = 2000;    // still time before sensing resumes
static const float    kRelocDeg = 5.0f;    // tilt change => new spot
static const uint32_t kRelocMs  = 1500;    // moving this long => new spot

static void initImu() {
    gImuOk = M5.Imu.isEnabled();
    if (gImuOk) {
        M5.Imu.update();
        const auto& d = M5.Imu.getImuData();
        for (int i = 0; i < 3; i++) gGrav[i] = gGravStill[i] = d.accel.value[i];
    }
    Serial.printf("# IMU motion guard: %s\n", gImuOk ? "on" : "no IMU found, disabled");
}

// Samples the IMU at 50 Hz. Returns true while the device moves or settles.
static bool serviceImu(uint32_t now) {
    if (!gImuOk) return false;
    static uint32_t last = 0;
    if (now - last >= 20) {
        last = now;
        M5.Imu.update();
        const auto& d = M5.Imu.getImuData();
        const float* a = d.accel.value;
        const float* g = d.gyro.value;
        float gyro = sqrtf(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
        float dx = a[0] - gGrav[0], dy = a[1] - gGrav[1], dz = a[2] - gGrav[2];
        float dev = sqrtf(dx*dx + dy*dy + dz*dz);
        bool moving = gyro > kGyroThr || dev > kAccThr;
        float k = moving ? 0.2f : 0.05f;
        for (int i = 0; i < 3; i++) gGrav[i] += (a[i] - gGrav[i]) * k;
        if (moving) {
            if (!gImuMoving) { gImuMoving = true; gImuMoveT0 = now; }
            gImuMoveMs = now;
        }
    }
    if (gImuMoving && now - gImuMoveMs >= kSettleMs) {
        gImuMoving = false;
        float dot = 0.0f, n1 = 0.0f, n2 = 0.0f;
        for (int i = 0; i < 3; i++) {
            dot += gGrav[i] * gGravStill[i];
            n1  += gGrav[i] * gGrav[i];
            n2  += gGravStill[i] * gGravStill[i];
        }
        float c = (n1 > 0.0f && n2 > 0.0f) ? dot / sqrtf(n1 * n2) : 1.0f;
        if (c > 1.0f) c = 1.0f;
        if (c < -1.0f) c = -1.0f;
        float tilt = acosf(c) * 57.29578f;
        bool reloc = tilt > kRelocDeg || (gImuMoveMs - gImuMoveT0) > kRelocMs;
        for (int i = 0; i < 3; i++) gGravStill[i] = gGrav[i];
        Serial.printf("# IMU still again: tilt %.1f deg, moved %lu ms -> %s\n", tilt,
                      (unsigned long)(gImuMoveMs - gImuMoveT0), reloc ? "new spot" : "bump");
        if (reloc) {
            if (gCalibrated) { gCalibrated = false; gRelocMs = now; }
            gFloor = 0.0f;                       // AUTO re-learns the quiet level
        }
        return false;
    }
    return gImuMoving;
}

static void IRAM_ATTR promiscuousRxCb(void*, wifi_promiscuous_pkt_type_t) {}

// WiFi task context: filter + copy only, the maths runs in loop().
static void csiCallback(void*, wifi_csi_info_t* info) {
    if (!info || !info->buf || info->len < 128 || !gBssidOk || !gCsiQ) return;
    if (memcmp(info->mac, gBssid, 6) != 0) return;      // not from our router
    CsiPkt pk;
    memcpy(pk.buf, info->buf, 128);
    pk.rssi = info->rx_ctrl.rssi;
    if (xQueueSend(gCsiQ, &pk, 0) == pdTRUE) gCsiCount++;
}

static void processCsiPkt(const CsiPkt& pk, uint32_t now) {
    // L-LTF order: subcarriers 0..31 then -32..-1. Data: 1..26 and -26..-1 (38..63).
    float amp[kSub];
    float sum = 0.0f;
    int   j   = 0;
    for (int k = 1; k <= 63; k++) {
        if (k > 26 && k < 38) continue;                  // guard band
        float a = (float)pk.buf[2*k], b = (float)pk.buf[2*k + 1];
        amp[j] = sqrtf(a*a + b*b);
        sum += amp[j++];
    }
    float mean = sum / kSub;
    if (mean < 0.5f) return;                             // empty / bogus frame
    float inv  = 1.0f / mean;
    float* slot = gShape[gWinIdx];
    for (int i = 0; i < kSub; i++) slot[i] = amp[i] * inv;
    gWinIdx = (gWinIdx + 1) % kCsiWin;
    if (gWinFill < kCsiWin) gWinFill++;
    gLastPktMs = now;
    gRssiEma  += ((float)pk.rssi - gRssiEma) * 0.1f;
    gCsiRssi   = (int8_t)lroundf(gRssiEma);
    if (gWinFill < kCsiWin) return;

    float acc = 0.0f;
    for (int i = 0; i < kSub; i++) {
        float m = 0.0f, q = 0.0f;
        for (int w = 0; w < kCsiWin; w++) { float v = gShape[w][i]; m += v; q += v*v; }
        m /= kCsiWin;
        float var = q / kCsiWin - m*m;
        if (var > 0.0f) acc += var;
    }
    gCsiMetric = acc / kSub;
}

static void drainCsi(uint32_t now) {
    if (!gCsiQ) return;
    CsiPkt pk;
    int n = 0;
    while (n++ < 32 && xQueueReceive(gCsiQ, &pk, 0) == pdTRUE) processCsiPkt(pk, now);
}

static void stopPing() {
    if (!gPing) return;
    esp_ping_stop(gPing);
    esp_ping_delete_session(gPing);
    gPing = nullptr;
}

// Ping the gateway 50x/s: every reply is a router frame carrying CSI.
static void startPing() {
    stopPing();
    IPAddress gw = WiFi.gatewayIP();
    esp_ping_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.count           = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms     = 20;
    cfg.timeout_ms      = 1000;
    cfg.data_size       = 8;
    cfg.tos             = 0;
    cfg.ttl             = 64;
    IP_ADDR4(&cfg.target_addr, gw[0], gw[1], gw[2], gw[3]);
    cfg.task_stack_size = 3072;
    cfg.task_prio       = 2;
    cfg.interface       = 0;
    esp_ping_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    if (esp_ping_new_session(&cfg, &cbs, &gPing) == ESP_OK) esp_ping_start(gPing);
    else gPing = nullptr;
    Serial.printf("# CSI ping %s: %s\n", gw.toString().c_str(), gPing ? "on" : "FAILED");
}
#endif

static RadarLink       radar;

static M5Canvas        canvas(&M5Cardputer.Display);   // bottom (built-in) buffer
static LGFX_ExtST7789  extPanel;                       // external Waveshare 2" ST7789V
static M5Canvas        topCanvas(&extPanel);           // top render buffer (240x180)
static bool            extReady   = false;
static bool            gRange10   = false;  // range cone: false = 5 m / 1 m lines, true = 10 m / 2 m
#if defined(RADAR_CSI)
static float           gThreshold = 0.15f;   // = kCsiThresh, the original fixed CSI threshold
#else
static float           gThreshold = 0.35f;
#endif
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
    prefs.putUChar("thresh",    (uint8_t)lroundf(gThreshold * 100));
    prefs.putUChar("range10",   gRange10 ? 1 : 0);
    prefs.end();
}

static void loadSettings() {
    Preferences prefs;
    prefs.begin("radar", true);
    gColorIdx  = prefs.getUChar("palette",   0);
    gBright    = prefs.getUChar("bright",   80);
    gExtBright = prefs.getUChar("extbright",80);
    uint8_t thr = prefs.getUChar("thresh", (uint8_t)lroundf(gThreshold * 100));
    gRange10   = prefs.getUChar("range10", 0) != 0;
    prefs.end();
    if (thr >= 5 && thr <= 95) gThreshold = thr / 100.0f;
    if (gColorIdx >= kNumPalettes) gColorIdx = 0;
    if (gBright    < 10 || gBright    > 100) gBright    = 80;
    if (gExtBright < 10 || gExtBright > 100) gExtBright = 80;
    applyPalette(gColorIdx);
}

// --------------------------------------------------------------------------
// Fake sensor (POC without a C5): emits ~15 Hz frames into the parser.
// --------------------------------------------------------------------------
#if RADAR_FAKE
static void serviceFake() {
  static uint32_t seq = 0, last = 0, until = 0;
  static float    phase = 0.0f;
  static bool     person = false;

  uint32_t now = millis();
  if (now - last < 66) return;                  // ~15 Hz
  last = now;

  if (now > until) {
    person = !person;
    until = now + (person ? random(4000, 9000) : random(3000, 7000));
  }
  float base  = person ? 0.25f : 0.02f;
  phase      += 0.20f;
  float wig   = person ? 0.18f * (0.5f + 0.5f * sinf(phase)) : 0.0f;
  float noise = (random(0, 100) / 100.0f) * (person ? 0.12f : 0.04f);
  float mot   = base + wig + noise;
  if (mot > 1.0f) mot = 1.0f;

  int pres = (mot > gThreshold) ? 1 : 0;
  int rssi = -45 - (int)random(0, 25);

  char line[64];
  snprintf(line, sizeof(line), "R,%lu,%d,%.3f,%d,RUN",
           (unsigned long)(++seq), pres, mot, rssi);
  radar.injectLine(line);

  // Keep the fake Ring camera blip alive for UI testing
  camBlips[0].lastSeen = now;
}
#endif

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

// ── WiFi init + UDP service ────────────────────────────────────────────────────
#if defined(RADAR_UDP)
static void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(HOME_SSID, HOME_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    canvas.fillSprite(TFT_BLACK);
    canvas.setTextSize(1);
    canvas.setTextColor(gColB, TFT_BLACK);
    char msg[48];
    snprintf(msg, sizeof(msg), "Joining %s...", HOME_SSID);
    canvas.drawString(msg, (canvas.width() - canvas.textWidth(msg)) / 2, canvas.height() / 2 - 4);
    canvas.pushSprite(0, 0);
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    udpIn.begin(4210);
    wifiReady = true;
    strncpy(wifiIP, WiFi.localIP().toString().c_str(), sizeof(wifiIP) - 1);
    Serial.printf("# WiFi: %s  IP:%s\n", HOME_SSID, wifiIP);
  } else {
    Serial.println("# WiFi failed — rebooting");
    delay(1000);
    ESP.restart();
  }
}

static void serviceUDP() {
  if (!wifiReady) return;
  int sz = udpIn.parsePacket();
  if (sz <= 0) return;
  char buf[RadarLink::kLineMax];
  int  n = udpIn.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';
  while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
  if (n > 0) radar.injectLine(buf);
}
#endif

// ── CSI WiFi init + service ────────────────────────────────────────────────────
#if defined(RADAR_CSI)

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

static void IRAM_ATTR sniffCallback(void*, wifi_promiscuous_pkt_type_t);  // defined later

static void enableCsi() {
    if (!gCsiQ) gCsiQ = xQueueCreate(32, sizeof(CsiPkt));
    gBssidOk = false;
    const uint8_t* bssid = WiFi.BSSID();
    if (bssid) { memcpy(gBssid, bssid, 6); gBssidOk = true; }
    if (gCsiQ) xQueueReset(gCsiQ);
    gWinIdx = 0; gWinFill = 0; gCsiMetric = 0.0f; gCsiMotion = 0.0f;
    gFloor = 0.0f; gLastPktMs = 0; gRssiEma = (float)WiFi.RSSI();
    Serial.printf("# CSI router %02X:%02X:%02X:%02X:%02X:%02X\n",
                  gBssid[0], gBssid[1], gBssid[2], gBssid[3], gBssid[4], gBssid[5]);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promiscuousRxCb);
    wifi_csi_config_t cfg = {};
    cfg.lltf_en = true; cfg.htltf_en = true; cfg.stbc_htltf2_en = true;
    cfg.ltf_merge_en = true; cfg.channel_filter_en = true;
    cfg.manu_scale = false; cfg.shift = 0;
    esp_wifi_set_csi_config(&cfg);
    esp_wifi_set_csi_rx_cb(csiCallback, nullptr);
    esp_wifi_set_csi(true);
    wifi_promiscuous_filter_t pf{};
    pf.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&pf);
    esp_wifi_set_promiscuous_rx_cb(sniffCallback);
    startPing();
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

    // stop CSI + disconnect ───────────────────────────────────────────────────
    if (wifiReady) {
        esp_wifi_set_csi(false);
        esp_wifi_set_promiscuous(false);
        wifiReady = false;
    }
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
            if (WiFi.status() == WL_CONNECTED) {
                strncpy(wifiIP, WiFi.localIP().toString().c_str(), sizeof(wifiIP) - 1);
                wifiReady = true; enableCsi();
            }
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
        strncpy(wifiIP, WiFi.localIP().toString().c_str(), sizeof(wifiIP) - 1);
        wifiReady = true; saveWifiCreds(newSsid, newPass); enableCsi();
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

static void initCsi() {
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
    strncpy(wifiIP, WiFi.localIP().toString().c_str(), sizeof(wifiIP) - 1);
    wifiReady = true;
    Serial.printf("# CSI WiFi: %s  IP:%s\n", savedSsid, wifiIP);
    enableCsi();
    Serial.println("# CSI active");
}

static double   gCalSum = 0, gCalSq = 0;
static uint32_t gCalN = 0, gCalCsi0 = 0;

static void startCsiCal() {
    if (gCalPhase) { gCalPhase = 0; return; }          // c again = cancel
    if (!wifiReady) { gCalFailMs = millis(); return; } // no WiFi -> no CSI
    gCalPhase = 1; gCalT0 = millis(); gCalFailMs = gCalOkMs = 0;
}

// Runs at 15 Hz from serviceCsi(). Returns true while calibration is in progress.
static bool stepCsiCal(uint32_t now, bool haveCsi) {
    if (gCalPhase == 1) {
        if (now - gCalT0 < kCalLeadMs) return true;
        gCalPhase = 2; gCalT0 = now; gCalN = 0; gCalCsi0 = gCsiCount;
        gCalSum = gCalSq = 0;
        return true;
    }
    if (gCalPhase != 2) return false;
    if (haveCsi) {
        const double v = gCsiMetric;
        gCalSum += v; gCalSq += v * v; gCalN++;
    }
    if (now - gCalT0 < kCalMeasMs) return true;

    gCalPhase = 0;
    const uint32_t frames = gCsiCount - gCalCsi0;
    if (gCalN < 100 || frames < 150) {                 // < 15 router frames/s
        gCalFailMs = now;
        Serial.printf("# CAL failed: samples=%lu frames=%lu\n",
                      (unsigned long)gCalN, (unsigned long)frames);
        return false;
    }
    const double m  = gCalSum / gCalN;
    const double sd = sqrt(fmax(0.0, gCalSq / gCalN - m * m));
    gCalRef     = (float)fmax(m + 3.0 * sd, 1e-7);
    gCalibrated = true;
    gCalOkMs    = now;
    Serial.printf("# CAL done: n=%lu frames=%lu mean=%.6f sd=%.6f ref=%.6f\n",
                  (unsigned long)gCalN, (unsigned long)frames, m, sd, gCalRef);
    return false;
}

static void serviceCsi() {
    static uint32_t seq        = 0;
    static uint32_t last       = 0;
    static uint32_t rateT0     = 0;
    static uint32_t rateCnt0   = 0;
    static int      holdCnt    = 0;
    static float    heldMotion = 0.0f;
    const  int      kHold      = 150;   // 10 s at 15 Hz

    uint32_t now = millis();

    if (wifiReady && WiFi.status() != WL_CONNECTED) {
        // WiFi dropped: stop sensing; radar goes stale -> NO LINK
        wifiReady  = false;
        holdCnt    = 0;
        heldMotion = 0.0f;
        stopPing();
        esp_wifi_set_csi(false);
        esp_wifi_set_promiscuous(false);
        gCsiMotion = 0.0f;
        if (gCalPhase) { gCalPhase = 0; gCalFailMs = now; }  // calibration needs CSI
    } else if (!wifiReady && WiFi.status() == WL_CONNECTED) {
        // Reconnected (auto-reconnect): restart sensing
        strncpy(wifiIP, WiFi.localIP().toString().c_str(), sizeof(wifiIP) - 1);
        wifiReady = true;
        enableCsi();
    }
    if (!wifiReady) return;

    const bool guard = serviceImu(now);  // device moving / settling
    drainCsi(now);                       // every loop: keep the queue empty
    if (guard) gWinFill = 0;             // flush frames taken while moving
    if (now - last < 66) return;         // UI frames at 15 Hz
    last = now;

    if (now - rateT0 >= 1000) {
        gPktRate = (uint16_t)(gCsiCount - rateCnt0);
        rateCnt0 = gCsiCount;
        rateT0   = now;
    }

    const bool pktsOk  = gLastPktMs && (now - gLastPktMs) < 2000;
    const bool haveCsi = pktsOk && !guard && gWinFill >= kCsiWin;   // window warm
    float raw = 0.0f;
    if (haveCsi) {
        // AUTO floor: falls fast, rises very slowly (~90 s), so it settles on the quiet level.
        if (gFloor <= 0.0f)             gFloor = gCsiMetric;
        else if (gCsiMetric < gFloor)   gFloor += (gCsiMetric - gFloor) * 0.2f;
        else                            gFloor += (gCsiMetric - gFloor) * 0.0007f;
        float ref = gCalibrated ? gCalRef : gFloor * kAutoMargin;
        if (ref < 1e-7f) ref = 1e-7f;
        raw = (gCsiMetric / ref - 1.0f) / kCsiSpan;
        raw = raw < 0.0f ? 0.0f : (raw > 1.0f ? 1.0f : raw);
    }
    gCsiMotion += (raw - gCsiMotion) * 0.4f;

    char line[48];
    if (guard) {
        // Device moving: no contacts; a running calibration restarts its lead-in.
        holdCnt = 0; heldMotion = 0.0f; gCsiMotion = 0.0f;
        if (gCalPhase) { gCalPhase = 1; gCalT0 = now; }
        snprintf(line, sizeof(line), "R,%lu,0,0.000,%d,MOVED",
                 (unsigned long)(++seq), (int)gCsiRssi);
        radar.injectLine(line);
        return;
    }
    if (stepCsiCal(now, haveCsi)) {
        // No contacts while learning the empty room.
        holdCnt = 0; heldMotion = 0.0f;
        snprintf(line, sizeof(line), "R,%lu,0,%.3f,%d,CAL",
                 (unsigned long)(++seq), gCsiMotion, (int)gCsiRssi);
        radar.injectLine(line);
        return;
    }
    if (!pktsOk) {
        // Connected but no router frames (router ignores ping / starting up)
        holdCnt = 0; heldMotion = 0.0f;
        snprintf(line, sizeof(line), "R,%lu,0,0.000,%d,NO CSI",
                 (unsigned long)(++seq), (int)gCsiRssi);
        radar.injectLine(line);
        return;
    }

    // Presence coasts ~10 s after the last motion above threshold so a person who
    // stops moving stays on the scope; the blip fades from its peak toward 10%.
    float m = gCsiMotion;
    bool present;
    if (m > gThreshold) {
        holdCnt    = kHold;
        heldMotion = m;
        present    = true;
    } else if (holdCnt > 0) {
        holdCnt--;
        float fade = (float)holdCnt / kHold;
        m       = heldMotion * (0.10f + 0.90f * fade);
        present = true;
    } else {
        present = false;
        m       = 0.0f;
    }

    snprintf(line, sizeof(line), "R,%lu,%d,%.3f,%d,%s",
             (unsigned long)(++seq), (int)present, m, (int)gCsiRssi,
             gCalibrated ? "RUN" : "AUTO");
    radar.injectLine(line);
}
#endif

#if defined(RADAR_CSI)
#include "zones_core.h"      // sensor nodes: storage, UDP reports, pairing, zone helpers
#endif

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
#if defined(RADAR_CSI)
        case 3:
            gMenuOpen = false;
            runWifiPicker();
            break;
        case 4:
            runNodesMenu();
            break;
#endif
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
#if defined(RADAR_CSI)
    const char* const labels[] = { "PALETTE", "SCREEN", "EXT PANEL", "WIFI NET", "NODES" };
    const int numItems = 5, rowH = 20, textOff = 6;
#else
    const char* const labels[] = { "PALETTE", "SCREEN", "EXT PANEL" };
    const int numItems = 3, rowH = 24, textOff = 8;
#endif
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
#if defined(RADAR_CSI)
            case 3: {
                String cur = WiFi.SSID();
                if (cur.length() > 0) snprintf(val, sizeof(val), "%.12s", cur.c_str());
                else strncpy(val, "> SCAN", sizeof(val));
                break;
            }
            case 4: snprintf(val, sizeof(val), "%d >", gNodeCount); break;
#endif
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
#if defined(RADAR_CSI)
    const char* hint = ";/.:nav  ,//:change  enter:open  `:save";
#else
    const char* hint = ";/.:nav  ,//:change  `:save";
#endif
    canvas.drawString(hint, (W - canvas.textWidth(hint)) / 2, H - 11);

    canvas.pushSprite(0, 0);
}

// --------------------------------------------------------------------------
// Built-in (bottom) screen: status + controls
// --------------------------------------------------------------------------
static void drawBottom() {
  const auto& s = radar.state();
  const int W = canvas.width();
  const int H = canvas.height();
  const bool linkOk  = !radar.stale(750);
  const bool calib   = (strcmp(s.mode, "CAL") == 0);
#if defined(RADAR_CSI)
  // With sensor nodes: banner, chips and graph cover every zone (ME + nodes).
  const uint32_t zNow     = millis();
  const bool     zoneMode = gNodeCount > 0;
  float          zMax     = 0.0f;
  int            zMaxIdx  = 0;
  const int      zAct     = zoneMode ? activeZone(zNow, &zMax, &zMaxIdx) : -1;
  const bool present = zoneMode ? (zAct >= 0) : (linkOk && s.presence);
#else
  const bool present = linkOk && s.presence;
#endif

  const uint16_t cMag    = gColA;
  const uint16_t cCyan   = gColB;
  const uint16_t cPurple = canvas.color565( 70,   0,  70);
  const uint16_t cBar    = canvas.color565( 20,   0,  35);
  const uint16_t cBorder = canvas.color565(160,   0, 160);
  const uint16_t cCorner = canvas.color565(255, 120, 255);

  canvas.fillSprite(TFT_BLACK);

  // title bar
  canvas.fillRect(0, 0, W, 14, cBar);
  canvas.drawFastHLine(0, 13, W, cBorder);
  canvas.setTextSize(1);
  canvas.setTextColor(cMag, cBar);
#if RADAR_FAKE
  canvas.drawString("[ WiFi-CSI RADAR/FAKE ]", (W - canvas.textWidth("[ WiFi-CSI RADAR/FAKE ]")) / 2, 3);
#elif defined(RADAR_UDP) || defined(RADAR_CSI)
  {
    char udpTitle[32];
    if (wifiReady) snprintf(udpTitle, sizeof(udpTitle), "[WiFi %s]", wifiIP);
    else           snprintf(udpTitle, sizeof(udpTitle), "[ NO-WIFI ]");
    canvas.setTextColor(wifiReady ? cCyan : TFT_RED, cBar);
    canvas.drawString(udpTitle, (W - canvas.textWidth(udpTitle)) / 2, 3);
    canvas.setTextColor(cMag, cBar);  // restore for pill below
  }
#else
  canvas.drawString("[ WiFi-CSI RADAR/UART ]", (W - canvas.textWidth("[ WiFi-CSI RADAR/UART ]")) / 2, 3);
#endif
  drawBatIcon(canvas, 2, 3);
  const char* tag  = !linkOk ? "NO LINK" : (calib ? "CAL" : s.mode);
  const bool  noCsi = linkOk && strcmp(s.mode, "NO CSI") == 0;
  uint16_t    pill = (!linkOk || noCsi) ? TFT_RED
                   : ((calib || strcmp(s.mode, "MOVED") == 0) ? canvas.color565(255, 140, 0)
                                                                : canvas.color565(0, 180, 80));
  int tw = canvas.textWidth(tag) + 8;
  canvas.fillRoundRect(W - tw - 3, 1, tw, 11, 3, pill);
  canvas.setTextColor(TFT_WHITE, pill);
  canvas.drawString(tag, W - tw + 1, 3);

  // presence / clear banner
  uint16_t bg     = present ? canvas.color565(60, 0, 60) : canvas.color565(0, 0, 25);
  uint16_t border = present ? cMag : cPurple;
  canvas.fillRoundRect(4, 17, W - 8, 34, 4, bg);
  canvas.drawRoundRect(4, 17, W - 8, 34, 4, border);
  canvas.setTextSize(2);
  const char* lbl = present ? ">> CONTACT <<" : "~~ CLEAR ~~";
#if defined(RADAR_CSI)
  char zoneLbl[24];
  if (zoneMode && zAct >= 0) {
    snprintf(zoneLbl, sizeof(zoneLbl), ">> %s <<", getZone(zAct, zNow).name);
    lbl = zoneLbl;
  }
  char calLbl[20];
  const uint32_t nowMs = millis();
  if (gImuMoving) {
    lbl = "DEVICE MOVED";
  } else if (gCalPhase == 1) {
    snprintf(calLbl, sizeof(calLbl), "LEAVE ROOM %lu",
             (unsigned long)((kCalLeadMs - (nowMs - gCalT0) + 999) / 1000));
    lbl = calLbl;
  } else if (gCalPhase == 2) {
    uint32_t el = nowMs - gCalT0; if (el > kCalMeasMs) el = kCalMeasMs;
    snprintf(calLbl, sizeof(calLbl), "CALIBRATE %lu", (unsigned long)((kCalMeasMs - el + 999) / 1000));
    lbl = calLbl;
  } else if (gCalFailMs && nowMs - gCalFailMs < 3000) {
    lbl = "CAL FAILED";
  } else if (gCalOkMs && nowMs - gCalOkMs < 2000) {
    lbl = "CALIBRATED";
  } else if (gRelocMs && nowMs - gRelocMs < 4000) {
    lbl = "MOVED:PRESS C";
  }
#endif
  canvas.setTextColor(present ? canvas.color565(255, 100, 255) : cCyan, bg);
  canvas.drawString(lbl, (W - canvas.textWidth(lbl)) / 2, 25);

  // motion graph
  canvas.setTextSize(1);
#if defined(RADAR_CSI)
  if (zoneMode) drawZoneChips(54, zNow);
  const int gx = 4, gy = zoneMode ? 69 : 54, gw = W - 8, gh = zoneMode ? 35 : 50;
#else
  const int gx = 4, gy = 54, gw = W - 8, gh = 50;
#endif
  canvas.drawRect(gx, gy, gw, gh, cBorder);
  int ty = gy + gh - 1 - (int)(gThreshold * (gh - 2));
  canvas.drawFastHLine(gx + 1, ty, gw - 2, canvas.color565(180, 0, 180));

  int cols = gw - 2;
#if defined(RADAR_CSI)
  int n    = zoneMode ? kZoneHist : RadarLink::historySize();
#else
  int n    = RadarLink::historySize();
#endif
  if (cols > n) cols = n;
  int xoff = (gw - 2) - cols;
  for (int i = 0; i < cols; ++i) {
#if defined(RADAR_CSI)
    float v = zoneMode ? zoneHistAt(n - cols + i) : radar.historyAt(n - cols + i);
#else
    float v = radar.historyAt(n - cols + i);
#endif
    int barh = (int)(v * (gh - 2));
    if (barh < 0) barh = 0;
    if (barh > gh - 2) barh = gh - 2;
    if (barh == 0) continue;
    uint16_t col = (v > gThreshold)
      ? canvas.color565(220,   0, 200)
      : canvas.color565(  0, 160, 200);
    canvas.drawFastVLine(gx + 1 + xoff + i, gy + gh - 1 - barh, barh, col);
  }

  // footer
  char foot[48];
#if defined(RADAR_CSI)
  if (zoneMode)
    snprintf(foot, sizeof(foot), "mot %3d%% %-10s thr %2d%%",
             (int)(zMax * 100), getZone(zMaxIdx, zNow).name, (int)(gThreshold * 100));
  else
    snprintf(foot, sizeof(foot), "mot %3d%%  %ddBm  thr %2d%%  %u/s",
             (int)(s.motion * 100), s.rssi, (int)(gThreshold * 100), (unsigned)gPktRate);
#else
  snprintf(foot, sizeof(foot), "mot %3d%%  rssi %ddBm  thr %2d%%",
           (int)(s.motion * 100), s.rssi, (int)(gThreshold * 100));
#endif
  canvas.setTextColor(cCyan, TFT_BLACK);
  canvas.drawString(foot, 4, gy + gh + 4);
  canvas.setTextColor(cPurple, TFT_BLACK);
  canvas.drawString(",.mode [c]cal  [ [ ]thr-  [ ] ]thr+", 4, gy + gh + 15);

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

// --------------------------------------------------------------------------
// External (top) screen: PPI-style radar scope, scaled-to-fit
// --------------------------------------------------------------------------
struct Blip { float ang; float rad; float strength; uint32_t birth; bool active; };
static Blip           blips[12];
static uint32_t       lastSpawn     = 0;
static float          gLastSpawnAng = 0.0f;  // reuse angle on respawn — no bearing, so don't jump
static const uint32_t BLIP_LIFE = 15000;  // ms a contact persists
struct Ripple { float ang; float rad; uint32_t birth; bool active; };
static Ripple         ripples[6];
static float          gBEAR = 0.0f;
static float          gLastBrg  = 0.0f;
static int            gViewMode = 0;   // 0 = range cone, 1 = zones (CSI build) / 3-D raycaster, 2 = 3-D raycaster

// Returns an angle in [0, TAU) that avoids ±30° around 6 o'clock (π/2) and 12 o'clock (3π/2).
// Blips spawned at these angles overlap the central ghost avatar in the raycaster view.
// Maps a uniform random value to three valid arcs: [0,π/3] ∪ [2π/3,4π/3] ∪ [5π/3,2π]
static float pickBlipAngle() {
    const float TAU = 6.2831853f;
    float r = ((float)random(0, 10000) / 10000.0f) * (TAU * 2.0f / 3.0f);
    const float s1 = TAU / 6.0f;  // π/3
    const float s2 = TAU / 3.0f;  // 2π/3
    if      (r < s1)       return r;
    else if (r < s1 + s2)  return r - s1 + TAU / 3.0f;
    else                   return r - s1 - s2 + TAU * 5.0f / 6.0f;
}

// ── Spycam detection ──────────────────────────────────────────────────────────
struct CamBlip {
    uint8_t  mac3[3];   // last 3 MAC bytes — unique enough for tracking
    float    ang;       // assigned once at first detection, never updated
    float    rad;       // RSSI → radius, EMA-smoothed
    int8_t   rssi;
    uint32_t lastSeen;
    bool     active;
    char     vendor[8];
};
static CamBlip        camBlips[8];
static const uint32_t CAM_LIFE = 60000;  // ms before expiry if silent

struct OuiEntry { uint8_t b[3]; const char* name; };
static const OuiEntry kCamOuis[] = {
    {{0x24,0x0A,0xC4},"ESP32"},  {{0x30,0xAE,0xA4},"ESP32"},
    {{0x24,0x6F,0x28},"ESP32"},  {{0xDC,0x54,0x75},"ESP32"},
    {{0xE8,0x9F,0x6D},"ESP32"},  {{0x8C,0xAA,0xB5},"ESP-S3"},
    {{0x34,0x85,0x18},"ESP-S3"}, {{0x2C,0xAA,0x8E},"Wyze"},
    {{0xD0,0x3F,0x27},"Wyze"},   {{0x7C,0x78,0xB2},"Wyze"},
    {{0xFC,0x65,0xDE},"Ring"},   {{0x68,0x37,0xE9},"Ring"},
    {{0x34,0xD2,0x70},"Amazon"}, {{0xF0,0x27,0x2D},"Hikvsn"},
    {{0xC0,0x56,0xE3},"Hikvsn"}, {{0x44,0x19,0xB6},"Hikvsn"},
    {{0x28,0x57,0xBE},"Reolnk"}, {{0x00,0xE0,0x4C},"Rltk"},
    {{0xBC,0xDD,0xC2},"Arlo"},   {{0x4C,0x69,0x05},"Blink"},
};
static const int kNumCamOuis = (int)(sizeof(kCamOuis) / sizeof(kCamOuis[0]));

#if defined(RADAR_CSI)
static volatile uint8_t  gSniffMAC[6];
static volatile int8_t   gSniffRssi    = 0;
static volatile int      gSniffVendor  = 0;
static volatile bool     gSniffPending = false;

static void IRAM_ATTR sniffCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (gSniffPending) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < 16) return;
    const uint8_t* src = pkt->payload + 10;  // Address 2 = source MAC in all common frame types
    for (int i = 0; i < kNumCamOuis; i++) {
        if (src[0]==kCamOuis[i].b[0] && src[1]==kCamOuis[i].b[1] && src[2]==kCamOuis[i].b[2]) {
            gSniffMAC[0]=src[0]; gSniffMAC[1]=src[1]; gSniffMAC[2]=src[2];
            gSniffMAC[3]=src[3]; gSniffMAC[4]=src[4]; gSniffMAC[5]=src[5];
            gSniffRssi   = pkt->rx_ctrl.rssi;
            gSniffVendor = i;
            gSniffPending = true;
            return;
        }
    }
}

static void serviceCam() {
    if (!gSniffPending) return;
    gSniffPending = false;
    uint8_t mac[6]; memcpy(mac, (void*)gSniffMAC, 6);
    const int8_t   rssi      = gSniffRssi;
    const int      vendorIdx = gSniffVendor;
    const uint32_t now       = millis();
    float t = ((float)rssi + 45.0f) / (-33.0f);
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    const float targetRad = 74.0f * (0.30f + t * 0.60f);
    for (int i = 0; i < 8; i++) {
        if (!camBlips[i].active) continue;
        if (camBlips[i].mac3[0]==mac[3] && camBlips[i].mac3[1]==mac[4] && camBlips[i].mac3[2]==mac[5]) {
            camBlips[i].rssi     = rssi;
            camBlips[i].lastSeen = now;
            camBlips[i].rad     += (targetRad - camBlips[i].rad) * 0.05f;
            return;
        }
    }
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (!camBlips[i].active || now - camBlips[i].lastSeen > CAM_LIFE) { slot = i; break; }
    }
    if (slot < 0) return;
    camBlips[slot].mac3[0] = mac[3]; camBlips[slot].mac3[1] = mac[4]; camBlips[slot].mac3[2] = mac[5];
    camBlips[slot].ang      = pickBlipAngle();
    camBlips[slot].rad      = targetRad;
    camBlips[slot].rssi     = rssi;
    camBlips[slot].lastSeen = now;
    camBlips[slot].active   = true;
    strncpy(camBlips[slot].vendor, kCamOuis[vendorIdx].name, 7);
    camBlips[slot].vendor[7] = '\0';
}
#endif  // RADAR_CSI

// Matrix rain — 7 columns in each side strip; scope disc + bars cover the middle
static const uint8_t RAIN_X[14] = { 3,9,15,21,27,33,39, 201,207,213,219,225,231,237 };
struct RainDrop { int16_t y; uint8_t speed; uint8_t tick; };
static RainDrop rain[14];
static bool     rainReady = false;

// 3-D mode sky animation state (initialized once)
struct Star3D { uint8_t  x; uint8_t  y; uint8_t phase; };
struct Bird3D { int16_t  x; int16_t  y; uint8_t speed; bool dir; };
struct ShotSt { int16_t  x; int16_t  y; uint32_t birth; bool active; };
static Star3D gStars3D[12];
static Bird3D gBirds3D[3];
static ShotSt gShot3D;
static bool   g3DReady = false;

__attribute__((unused)) static void drawRain(uint32_t now) {
  static const char GL[]  = "0123456789ABCDEF!:;@#%";
  static const int  GLN   = 22;
  static const int  TRAIL = 7;
  if (!rainReady) {
    for (int i = 0; i < 14; i++) {
      rain[i].y     = (int16_t)random(-60, 160);
      rain[i].speed = 2 + (uint8_t)random(0, 3);
      rain[i].tick  = (uint8_t)random(0, 4);
    }
    rainReady = true;
  }
  topCanvas.setTextSize(1);
  for (int i = 0; i < 14; i++) {
    if (++rain[i].tick >= rain[i].speed) {
      rain[i].tick = 0;
      rain[i].y   += 8;
      if (rain[i].y > 180 + TRAIL * 8) {
        rain[i].y     = -(int16_t)random(0, 60);
        rain[i].speed = 2 + (uint8_t)random(0, 3);
      }
    }
    for (int j = TRAIL - 1; j >= 0; j--) {
      int16_t ry = rain[i].y - j * 8;
      if (ry < 0 || ry >= 180) continue;
      char buf[2] = { GL[((uint32_t)(i * 13 + j * 7) + now / 350) % GLN], 0 };
      bool isCyan = (i % 2) == 0;   // alternate cyan/magenta columns
      if (j == 0) {
        topCanvas.setTextColor(isCyan ? topCanvas.color565(120, 255, 255)   // bright cyan head
                                      : topCanvas.color565(220, 200, 255),  // lavender head
                               TFT_BLACK);
      } else {
        uint8_t b = (uint8_t)(210 * (TRAIL - j) / TRAIL);
        topCanvas.setTextColor(isCyan ? topCanvas.color565(0, b / 2, b)   // cyan fade
                                      : topCanvas.color565(b / 2, 0, b),  // magenta fade
                               TFT_BLACK);
      }
      topCanvas.drawString(buf, RAIN_X[i], ry);
    }
  }
}

// =============================================================================
// Vaporwave 3-D view: sunset sky + full-floor radar + ghost avatar + sky FX.
// Third-person: WE stand in the center; contacts appear all 360° around us.
// Keys: , or . to toggle modes.
// =============================================================================

// Cute ghost avatar — we are the sensor origin.  cy = body centre.
// Drawn LAST in drawRaycaster so nothing overdraw it.
static void drawGhost(int cx, int cy, uint32_t now) {
  // pulsing cyan glow: breathes over ~1.6 s
  float pulse = 0.55f + 0.45f * sinf((float)(now % 1600) / 1600.0f * 6.2831853f);

  // ── body ─────────────────────────────────────────────────────────────────
  const uint16_t cBody = topCanvas.color565(215, 215, 252);
  const uint16_t cEye  = topCanvas.color565( 18,  18,  58);
  const uint16_t cBlsh = topCanvas.color565(255, 145, 175);

  topCanvas.fillEllipse(cx, cy, 12, 15, cBody);
  topCanvas.fillRect(cx - 12, cy + 8, 25, 7, cBody);

  // 5 wavy tail bumps
  topCanvas.fillCircle(cx - 10, cy + 15, 5, cBody);
  topCanvas.fillCircle(cx -  5, cy + 17, 5, cBody);
  topCanvas.fillCircle(cx,      cy + 18, 5, cBody);
  topCanvas.fillCircle(cx +  5, cy + 17, 5, cBody);
  topCanvas.fillCircle(cx + 10, cy + 15, 5, cBody);

  // ── face ─────────────────────────────────────────────────────────────────
  topCanvas.fillEllipse(cx - 5, cy - 2, 3, 5, cEye);
  topCanvas.fillEllipse(cx + 5, cy - 2, 3, 5, cEye);
  // eye-shine
  topCanvas.fillRect(cx - 7, cy - 5, 2, 2, topCanvas.color565(210, 210, 255));
  topCanvas.fillRect(cx + 3, cy - 5, 2, 2, topCanvas.color565(210, 210, 255));
  // smile arc
  topCanvas.drawPixel(cx - 3, cy + 5, cEye);
  topCanvas.drawPixel(cx - 2, cy + 6, cEye);
  topCanvas.drawPixel(cx + 2, cy + 6, cEye);
  topCanvas.drawPixel(cx + 3, cy + 5, cEye);
  // blush spots
  for (int dx = 7; dx <= 9; dx++) {
    topCanvas.drawPixel(cx - dx, cy + 2, cBlsh);
    topCanvas.drawPixel(cx + dx, cy + 2, cBlsh);
  }

  // ── thin outline so the body pops off the dark floor ─────────────────────
  topCanvas.drawEllipse(cx, cy, 12, 15, topCanvas.color565(160, 160, 240));
}

// Amber camera icon: rectangular body + viewfinder notch + dark lens + glint.
static void drawCamIcon(int cx, int cy) {
    const uint16_t amber = topCanvas.color565(255, 190, 0);
    const uint16_t dark  = topCanvas.color565(10, 10, 10);
    topCanvas.fillRoundRect(cx - 6, cy - 3, 12, 7, 1, amber);
    topCanvas.fillRect(cx - 2, cy - 5, 4, 2, amber);
    topCanvas.drawCircle(cx, cy, 3, dark);
    topCanvas.fillCircle(cx, cy, 2, dark);
    topCanvas.drawPixel(cx + 1, cy - 1, topCanvas.color565(210, 210, 210));
}

static void drawRaycaster() {
  if (!extReady) return;
  const auto& s      = radar.state();
  const bool  linkOk  = !radar.stale(750);
  const bool  present = linkOk && s.presence;
  const uint32_t now  = millis();
  const int W = topCanvas.width(), H = topCanvas.height();
  const float TAU = 6.2831853f;

  // ── SPAWN / MERGE BLIPS ───────────────────────────────────────────────────
  if (present) {
    float rssiT = ((float)s.rssi + 45.0f) / (-33.0f);
    if (rssiT < 0.0f) rssiT = 0.0f; if (rssiT > 1.0f) rssiT = 1.0f;
    float targetRad  = 74.0f * (0.25f + rssiT * 0.65f);
    if (targetRad < 74.0f * 0.30f) targetRad = 74.0f * 0.30f;
    const float mergeThresh = 74.0f * 0.20f;  // ~15 px ≈ a few feet of RSSI change

    // Update all active blips; find the one closest to the current RSSI ring.
    int   closestIdx  = -1;
    float closestDist = 74.0f;
    for (int i = 0; i < 12; ++i) {
      if (!blips[i].active || blips[i].birth + BLIP_LIFE <= now) continue;
      blips[i].strength += (s.motion - blips[i].strength) * 0.12f;
      float d = fabsf(blips[i].rad - targetRad);
      if (d < closestDist) { closestDist = d; closestIdx = i; }
    }

    if (closestIdx >= 0 && closestDist <= mergeThresh) {
      // Same contact — refresh life and gently nudge toward current radius.
      blips[closestIdx].birth  = now;
      blips[closestIdx].rad   += (targetRad - blips[closestIdx].rad) * 0.05f;
    } else if (now - lastSpawn > 800) {
      // New or distinct position — spawn a separate dot.
      int slot = -1;
      for (int i = 0; i < 12; ++i) { if (!blips[i].active) { slot = i; break; } }
      if (slot < 0) {
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < 12; ++i) if (blips[i].birth < oldest) { oldest = blips[i].birth; slot = i; }
      }
      if (slot >= 0) {
        gLastSpawnAng = pickBlipAngle();
        blips[slot].ang      = gLastSpawnAng;
        blips[slot].rad      = targetRad;
        blips[slot].strength = s.motion;
        blips[slot].birth    = now;
        blips[slot].active   = true;
        lastSpawn            = now;
        gLastBrg  = fmodf((gLastSpawnAng + TAU / 4.0f) * (360.0f / TAU) + 360.0f, 360.0f);
        for (int r = 0; r < 6; ++r) {
          if (!ripples[r].active) { ripples[r] = { gLastSpawnAng, targetRad, now, true }; break; }
        }
      }
    }
  }

  const uint16_t cMag    = gColA;
  const uint16_t cCyan   = gColB;
  const uint16_t cBar    = topCanvas.color565( 20,   0,  35);
  const uint16_t cBorder = topCanvas.color565(160,   0, 160);
  const uint16_t cCorner = topCanvas.color565(255, 120, 255);

  const int barH    = 14;
  const int sceneY  = barH;         // y=14
  const int sceneBot = H - barH;    // y=166
  const int horizY  = sceneY + 55;  // y=69: sky/floor split
  const int vanishX = W / 2;        // x=120

  // ── ONE-TIME SKY INIT ─────────────────────────────────────────────────────
  if (!g3DReady) {
    for (int i = 0; i < 12; i++) {
      gStars3D[i].x     = (uint8_t)random(5, 235);
      gStars3D[i].y     = (uint8_t)(sceneY + 2 + random(0, horizY - sceneY - 6));
      gStars3D[i].phase = (uint8_t)random(0, 256);
    }
    for (int i = 0; i < 3; i++) {
      gBirds3D[i].x     = (int16_t)random(0, 240);
      gBirds3D[i].y     = (int16_t)(sceneY + 6 + random(0, horizY - sceneY - 16));
      gBirds3D[i].speed = (uint8_t)(1 + random(0, 3));
      gBirds3D[i].dir   = (random(0, 2) == 0);
    }
    gShot3D.active = false;
    g3DReady = true;
  }

  topCanvas.fillSprite(TFT_BLACK);

  // ── SKY GRADIENT (dark purple → magenta → orange) ────────────────────────
  for (int y = sceneY; y < horizY; y++) {
    float t = (float)(y - sceneY) / (horizY - sceneY);
    uint8_t sr, sg, sb;
    if (t < 0.5f) {
      float bl = t * 2.0f;
      sr = (uint8_t)(15 + bl * 150); sg = 0; sb = (uint8_t)(55 - bl * 20);
    } else {
      float bl = (t - 0.5f) * 2.0f;
      sr = (uint8_t)(165 + bl * 90); sg = (uint8_t)(bl * 75); sb = (uint8_t)(35 - bl * 15);
    }
    topCanvas.drawFastHLine(0, y, W, topCanvas.color565(sr, sg, sb));
  }

  // ── TWINKLING STARS ───────────────────────────────────────────────────────
  for (int i = 0; i < 12; i++) {
    uint32_t tw = (now / 10 + (uint32_t)gStars3D[i].phase * 22) % 300;
    if (tw > 220) continue;
    uint8_t bri = (tw < 100) ? 255 : (uint8_t)(255 - (tw - 100) * 3);
    topCanvas.drawPixel(gStars3D[i].x, gStars3D[i].y,
                        topCanvas.color565(bri, bri, (uint8_t)(bri * 0.88f)));
    if (tw < 55) {  // sparkle cross at peak brightness
      uint16_t dim = topCanvas.color565(bri / 3, bri / 3, bri / 4);
      if (gStars3D[i].x > 0)   topCanvas.drawPixel(gStars3D[i].x - 1, gStars3D[i].y, dim);
      if (gStars3D[i].x < W-1) topCanvas.drawPixel(gStars3D[i].x + 1, gStars3D[i].y, dim);
    }
  }

  // ── SHOOTING STAR ─────────────────────────────────────────────────────────
  if (!gShot3D.active && random(0, 420) == 0) {
    gShot3D.x      = (int16_t)random(140, 230);
    gShot3D.y      = (int16_t)(sceneY + 2 + random(0, 10));
    gShot3D.birth  = now;
    gShot3D.active = true;
  }
  if (gShot3D.active) {
    uint32_t age = now - gShot3D.birth;
    if (age > 550) {
      gShot3D.active = false;
    } else {
      int sx = gShot3D.x - (int)(age * 0.18f);
      int sy = gShot3D.y + (int)(age * 0.09f);
      if (sy >= horizY - 2 || sx < 0) {
        gShot3D.active = false;
      } else {
        float fade = 1.0f - (float)age / 550.0f;
        uint8_t bri = (uint8_t)(255 * fade);
        int trail = (int)(age * 0.12f); if (trail > 18) trail = 18;
        int tx = sx + trail, ty = sy - trail / 2;
        if (ty >= sceneY && tx < W)
          topCanvas.drawLine(sx, sy, tx, ty,
                             topCanvas.color565(bri, (uint8_t)(bri * 0.8f), bri));
        topCanvas.drawPixel(sx, sy, topCanvas.color565(255, 255, 255));
      }
    }
  }

  // ── RETROWAVE SUN ────────────────────────────────────────────────────────
  const int sunCx = vanishX, sunCy = 50, sunR = 35;
  topCanvas.fillCircle(sunCx, sunCy, sunR,      topCanvas.color565(255,  50,  80));
  topCanvas.fillCircle(sunCx, sunCy, sunR -  6, topCanvas.color565(255, 100,  40));
  topCanvas.fillCircle(sunCx, sunCy, sunR - 13, topCanvas.color565(255, 165,  20));
  topCanvas.fillCircle(sunCx, sunCy, sunR - 20, topCanvas.color565(255, 215,  70));
  topCanvas.fillCircle(sunCx, sunCy, sunR - 27, topCanvas.color565(255, 240, 150));
  for (int stripe = 1; stripe <= 9; stripe++) {
    int sy2 = sunCy + stripe * 4;
    if (sy2 >= horizY) break;
    int delta = sy2 - sunCy;
    if (delta <= 0 || delta >= sunR) continue;
    int sw = (int)sqrtf((float)(sunR * sunR - delta * delta));
    float t = (float)(sy2 - sceneY) / (horizY - sceneY);
    uint8_t r2 = (uint8_t)(15 + t * 220);
    uint8_t g2 = (uint8_t)(t > 0.5f ? (t - 0.5f) * 2.0f * 75.0f : 0.0f);
    uint8_t b2 = (uint8_t)(55 - t * 35);
    topCanvas.fillRect(sunCx - sw, sy2, sw * 2, 2, topCanvas.color565(r2, g2, b2));
  }

  // ── BIRDS (silhouettes over the sky, even over the sun) ───────────────────
  for (int i = 0; i < 3; i++) {
    if (gBirds3D[i].dir) {
      gBirds3D[i].x += gBirds3D[i].speed;
      if (gBirds3D[i].x > 252) {
        gBirds3D[i].x = -14;
        gBirds3D[i].y = (int16_t)(sceneY + 6 + random(0, horizY - sceneY - 16));
      }
    } else {
      gBirds3D[i].x -= gBirds3D[i].speed;
      if (gBirds3D[i].x < -14) {
        gBirds3D[i].x = 252;
        gBirds3D[i].y = (int16_t)(sceneY + 6 + random(0, horizY - sceneY - 16));
      }
    }
    int bx = (int)gBirds3D[i].x, by = (int)gBirds3D[i].y;
    if (bx < -8 || bx > W + 8 || by < sceneY || by >= horizY) continue;
    uint16_t birdCol = topCanvas.color565(25, 10, 45);
    topCanvas.drawLine(bx - 5, by - 2, bx,     by,     birdCol);
    topCanvas.drawLine(bx,     by,     bx + 5, by - 2, birdCol);
  }

  // ── FLOOR BASE + HORIZON GLOW ─────────────────────────────────────────────
  topCanvas.fillRect(0, horizY, W, sceneBot - horizY, topCanvas.color565(5, 0, 15));
  topCanvas.drawFastHLine(0, horizY,     W, topCanvas.color565(255, 80, 120));
  topCanvas.drawFastHLine(0, horizY - 1, W, topCanvas.color565(120, 20,  60));

  // dim vaporwave perspective texture behind the radar
  for (int i = 1; i <= 7; i++) {
    float t = (float)i / 7.0f;
    int   y = horizY + (int)(t * t * (sceneBot - horizY));
    uint8_t b = (uint8_t)(15 + 32 * t);
    topCanvas.drawFastHLine(0, y, W, topCanvas.color565(b, 0, (uint8_t)(b * 0.7f)));
  }
  for (int i = 0; i <= 8; i++) {
    float t    = (float)i / 8.0f;
    int   xBot = (int)(t * W);
    uint8_t mc = (uint8_t)(16 * (0.3f + 0.7f * (1.0f - fabsf(t - 0.5f) * 2.0f)));
    topCanvas.drawLine(xBot, sceneBot, vanishX, horizY, topCanvas.color565(0, mc/2, mc));
  }

  // ── RADAR FLOOR: full-size ellipse covering the whole dark plane ──────────
  // rH/rV sized so the outer ring nearly touches the floor edges on all sides.
  const int rCx = W / 2, rCy = 122;
  const int rH  = 108,   rV  = 40;

  // bearing spokes
  for (int b = 0; b < 12; b++) {
    float a  = b * TAU / 12.0f;
    int   x2 = rCx + (int)(sinf(a) * rH);
    int   y2 = rCy - (int)(cosf(a) * rV);
    topCanvas.drawLine(rCx, rCy, x2, y2, topCanvas.color565(42, 0, 58));
  }
  // range rings
  topCanvas.drawEllipse(rCx, rCy, rH / 3,     rV / 3,     topCanvas.color565( 60,  0,  82));
  topCanvas.drawEllipse(rCx, rCy, rH * 2 / 3, rV * 2 / 3, topCanvas.color565( 88,  0, 115));
  topCanvas.drawEllipse(rCx, rCy, rH,          rV,         topCanvas.color565(132,  0, 165));

  // cardinal labels (placed just inside the outer ring)
  topCanvas.setTextSize(1);
  topCanvas.setTextColor(topCanvas.color565(120, 0, 148), topCanvas.color565(5, 0, 15));
  topCanvas.drawString("N", rCx - 2,         rCy - rV + 3);
  topCanvas.drawString("S", rCx - 2,         rCy + rV - 10);
  topCanvas.drawString("E", rCx + rH - 10,   rCy - 4);
  topCanvas.drawString("W", rCx - rH + 2,    rCy - 4);

  // ── CONTACTS: glitchy stick figures ──────────────────────────────────────
  // Perspective-scaled humanoid silhouette. High motion strength scatters
  // corrupt pixels around the figure to simulate a bad signal lock.
  for (int i = 0; i < 12; i++) {
    if (!blips[i].active) continue;
    uint32_t age = now - blips[i].birth;
    if (age > BLIP_LIFE) { blips[i].active = false; continue; }

    float brgN  = blips[i].ang + TAU / 4.0f;
    float distN = blips[i].rad / 74.0f;

    int csx = rCx + (int)(sinf(brgN) * distN * rH);
    int csy = rCy - (int)(cosf(brgN) * distN * rV);
    if (csy < horizY + 4 || csy > sceneBot - 2) continue;
    if (csx < 6           || csx > W - 6)        continue;

    float perspS = 0.28f + 0.72f * (float)(csy - (rCy - rV)) / (float)(2 * rV);
    if (perspS < 0.1f) perspS = 0.1f;
    if (perspS > 1.0f) perspS = 1.0f;

    float fade = 1.0f - (float)age / BLIP_LIFE;
    float str  = blips[i].strength;

    // figure geometry (all in pixels, perspective-scaled)
    int figH   = (int)(8 + perspS * 18);       // total figure height
    int headR  = figH / 5; if (headR < 1) headR = 1;
    int headCy = csy - figH + headR;            // head centre
    int shouldY = headCy + headR + 2;           // shoulder level
    int hipY   = csy - figH / 4;               // hip level
    int armW   = figH / 4; if (armW < 2) armW = 2;
    int legW   = figH / 5; if (legW < 1) legW = 1;

    // clip entire figure to scene bounds
    if (headCy - headR < horizY + 2) continue;

    // colour: cyan when calm, shifts toward magenta as motion rises
    uint8_t fR = (uint8_t)(220 * str  * fade);
    uint8_t fG = (uint8_t)(220 * (1.0f - str * 0.7f) * fade);
    uint8_t fB = (uint8_t)(255 * fade);
    uint16_t figCol = topCanvas.color565(fR, fG, fB);
    uint16_t dimCol = topCanvas.color565(fR / 3, fG / 3, fB / 3);

    // head
    topCanvas.fillCircle(csx, headCy, headR, figCol);
    // body
    topCanvas.drawLine(csx, shouldY, csx, hipY, figCol);
    // arms (angled down-and-out from shoulder)
    topCanvas.drawLine(csx, shouldY, csx - armW, shouldY + armW, figCol);
    topCanvas.drawLine(csx, shouldY, csx + armW, shouldY + armW, figCol);
    // legs (spread from hip to floor)
    topCanvas.drawLine(csx, hipY, csx - legW, csy, figCol);
    topCanvas.drawLine(csx, hipY, csx + legW, csy, figCol);

    // glitch scatter: corrupt pixels around the figure, count driven by strength
    int nGlitch = (int)(str * 9.0f * fade);
    uint32_t rng = (uint32_t)(i * 1337 + now / 70);
    for (int g = 0; g < nGlitch; g++) {
      rng = rng * 1664525u + 1013904223u;
      int gx = csx - armW - 2 + (int)((rng >> 16) % (uint32_t)(armW * 2 + 5));
      rng = rng * 1664525u + 1013904223u;
      int gy = headCy - 1 + (int)((rng >> 16) % (uint32_t)(figH + 3));
      if (gx >= 0 && gx < W && gy > horizY && gy < sceneBot)
        topCanvas.drawPixel(gx, gy, (rng & 0x8000) ? figCol : dimCol);
    }

    // floor dot — contact's ground position
    topCanvas.drawPixel(csx, csy, figCol);
    if (csx > 0)   topCanvas.drawPixel(csx - 1, csy, dimCol);
    if (csx < W-1) topCanvas.drawPixel(csx + 1, csy, dimCol);
  }

  // ── camera blip icons in 3-D view ────────────────────────────────────────
  for (int i = 0; i < 8; i++) {
    if (!camBlips[i].active) continue;
    if (now - camBlips[i].lastSeen > CAM_LIFE) { camBlips[i].active = false; continue; }
    float brgN  = camBlips[i].ang + TAU / 4.0f;
    float distN = camBlips[i].rad / 74.0f;
    int icsx = rCx + (int)(sinf(brgN) * distN * rH);
    int icsy = rCy - (int)(cosf(brgN) * distN * rV);
    if (icsy < horizY + 4 || icsy > sceneBot - 2) continue;
    if (icsx < 8           || icsx > W - 8)        continue;
    drawCamIcon(icsx, icsy - 7);  // slightly above floor = wall-mounted
  }

  // ── TITLE BAR ────────────────────────────────────────────────────────────
  topCanvas.fillRect(0, 0, W, barH, cBar);
  topCanvas.drawFastHLine(0, barH - 1, W, cBorder);
  topCanvas.setTextSize(1);
  const char* title = "[ WiFi-CSI  3D ]";
  topCanvas.setTextColor(cMag, cBar);
  topCanvas.drawString(title, (W - topCanvas.textWidth(title)) / 2, 3);
  drawBatIcon(topCanvas, 2, 3);
#if defined(RADAR_CSI)
  const char* mStr = wifiReady ? "WiFi OK" : "No WiFi";
  topCanvas.setTextColor(wifiReady ? topCanvas.color565(0, 210, 80) : TFT_RED, cBar);
#else
  const char* mStr = linkOk ? s.mode : "NO LINK";
  topCanvas.setTextColor(linkOk ? cCyan : TFT_RED, cBar);
#endif
  topCanvas.drawString(mStr, W - topCanvas.textWidth(mStr) - 4, 3);

  // ── STATUS BAR ───────────────────────────────────────────────────────────
  bool blink = (now % 600) < 300;
  topCanvas.fillRect(0, H - barH, W, barH, cBar);
  topCanvas.drawFastHLine(0, H - barH, W, cBorder);
  const char* ctlbl = present ? ">>CONTACT<<" : " scanning.. ";
  uint16_t ctCol = present
    ? (blink ? topCanvas.color565(255, 60, 255) : topCanvas.color565(110, 0, 100))
    : topCanvas.color565(55, 0, 70);
  topCanvas.setTextColor(ctCol, cBar);
  topCanvas.drawString(ctlbl, 14, H - 11);
  char midStr[14];
  snprintf(midStr, sizeof(midStr), "T:%d%%", (int)(gThreshold * 100));
  topCanvas.setTextColor(present ? topCanvas.color565(255, 140, 60) : topCanvas.color565(100, 0, 110), cBar);
  topCanvas.drawString(midStr, (W - topCanvas.textWidth(midStr)) / 2, H - 11);
  char stats[24];
  snprintf(stats, sizeof(stats), "M:%d%% %ddBm", (int)(s.motion * 100), s.rssi);
  topCanvas.setTextColor(cCyan, cBar);
  topCanvas.drawString(stats, W - topCanvas.textWidth(stats) - 4, H - 11);

  // ── FRAME + CORNERS ──────────────────────────────────────────────────────
  topCanvas.drawFastHLine(0, 0, W, cBorder);     topCanvas.drawFastHLine(0, H-1, W, cBorder);
  topCanvas.drawFastVLine(0, 0, H, cBorder);     topCanvas.drawFastVLine(W-1, 0, H, cBorder);
  topCanvas.fillRect(0, 0, 2, 2, cCorner);       topCanvas.fillRect(W-2, 0, 2, 2, cCorner);
  topCanvas.fillRect(0, H-2, 2, 2, cCorner);     topCanvas.fillRect(W-2, H-2, 2, 2, cCorner);

  // ── GHOST — very last draw call, guaranteed on top of everything ──────────
  drawGhost(rCx, rCy - 5, now);

  applyExtBrightness();
  topCanvas.pushRotateZoom(160, 120, 0.0f, EXT_ZOOM, EXT_ZOOM);
}

// Range cone (ST7789V fork; replaces the PPI radar circle).
// The Cardputer sits at the apex at the bottom; lines mark distance every 1 m
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
  const auto& s = radar.state();
  const bool     linkOk  = !radar.stale(750);
  const bool     present = linkOk && s.presence;
  const uint32_t now     = millis();

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
  const float glow = present ? s.motion : 0.0f;
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
  if (!linkOk) {
    strcpy(big, "NO LINK"); bigCol = TFT_RED;
  } else if (strcmp(s.mode, "NO CSI") == 0) {
    strcpy(big, "NO SIGNAL"); bigCol = TFT_RED; small = "no router frames";
  } else if (strcmp(s.mode, "MOVED") == 0) {
    strcpy(big, "MOVED"); bigCol = topCanvas.color565(255, 140, 0); small = "hold the device still";
  } else if (strcmp(s.mode, "CAL") == 0) {
    strcpy(big, "CALIBRATING"); bigCol = topCanvas.color565(255, 140, 0); small = "leave the room";
  } else if (present) {
    snprintf(big, sizeof(big), "MOTION %d%%", (int)lroundf(s.motion * 100));
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

  // --- the Cardputer at the apex ---
  topCanvas.fillRoundRect(apexX - 8, apexY - 3, 16, 6, 1, cCyan);

  // --- title bar: battery + range | contact state | mode pill ---
  topCanvas.setTextSize(1);
  topCanvas.fillRect(0, 0, W, 14, cBar);
  topCanvas.drawFastHLine(0, 13, W, cBorder);
  drawBatIcon(topCanvas, 2, 3);
  topCanvas.setTextColor(cCyan, cBar);
  topCanvas.drawString(gRange10 ? "RANGE 10m" : "RANGE 5m", 18, 3);
  const char* title = present ? "[ CONTACT ]" : "[ CLEAR ]";
  bool blink = (now % 600) < 300;
  topCanvas.setTextColor(present ? (blink ? topCanvas.color565(255, 60, 255) : cMag) : cMag, cBar);
  topCanvas.drawString(title, (W - topCanvas.textWidth(title)) / 2, 3);
  const char* tag = !linkOk ? "NO LINK" : s.mode;
  uint16_t pill = (!linkOk || strcmp(s.mode, "NO CSI") == 0) ? TFT_RED
                : ((strcmp(s.mode, "CAL") == 0 || strcmp(s.mode, "MOVED") == 0)
                   ? topCanvas.color565(255, 140, 0) : topCanvas.color565(0, 180, 80));
  int tw = topCanvas.textWidth(tag) + 8;
  topCanvas.fillRoundRect(W - tw - 3, 1, tw, 11, 3, pill);
  topCanvas.setTextColor(TFT_WHITE, pill);
  topCanvas.drawString(tag, W - tw + 1, 3);

  // --- footer corners ---
  char stats[24];
  snprintf(stats, sizeof(stats), "M:%d%% %ddBm", (int)lroundf(s.motion * 100), s.rssi);
  topCanvas.setTextColor(cCyan);
  topCanvas.drawString(stats, 4, H - 10);
  const char* hint = "[r] 5m/10m";
  topCanvas.setTextColor(topCanvas.color565(154, 106, 168));
  topCanvas.drawString(hint, W - topCanvas.textWidth(hint) - 4, H - 10);

  // --- outer frame ---
  topCanvas.drawRect(0, 0, W, H, cBorder);

  // scale-to-fit onto the 320x240 TFT (240x180 * EXT_ZOOM = 320x240, no letterbox)
  applyExtBrightness();
  topCanvas.pushRotateZoom(160, 120, 0.0f, EXT_ZOOM, EXT_ZOOM);
}

// --------------------------------------------------------------------------
#if defined(RADAR_CSI)
#include "zones_ui.h"        // ZONES view + NODES menu
#endif

static void serviceKeys() {
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
  auto st = M5Cardputer.Keyboard.keysState();
#if defined(RADAR_CSI)
  if (gMenuOpen && st.enter && gMenuCursor == 4) { runNodesMenu(); return; }
#endif
  for (char c : st.word) {
    if (gMenuOpen) {
#if defined(RADAR_CSI)
      if      (c == ';') gMenuCursor = (gMenuCursor + 4) % 5;
      else if (c == '.') gMenuCursor = (gMenuCursor + 1) % 5;
#else
      if      (c == ';') gMenuCursor = (gMenuCursor + 2) % 3;
      else if (c == '.') gMenuCursor = (gMenuCursor + 1) % 3;
#endif
      else if (c == ',') menuAdjust(-1);
      else if (c == '/') menuAdjust(+1);
      else if (c == '`') {
        saveSettings();
        gMenuOpen = false;
      }
    } else {
      if      (c == '`')             { gMenuOpen = true; gMenuCursor = 0; }
#if defined(RADAR_CSI)
      else if (c == '.')             gViewMode = (gViewMode + 1) % 3;   // cone, zones, 3D
#else
      else if (c == '.')             gViewMode = (gViewMode + 1) % 2;
#endif
      else if (c == 'r' || c == 'R') { gRange10 = !gRange10; saveSettings(); }
#if defined(RADAR_CSI)
      else if (c == 'c' || c == 'C') {
        const bool cancel = gCalPhase != 0;
        startCsiCal();
        sendNodeCmd(cancel ? "C,CALSTOP" : "C,CAL");                         // all zones together
      }
      else if (c == 'x' || c == 'X') { gCalPhase = 0; gCalibrated = false; sendNodeCmd("C,UNCAL"); }
#else
      else if (c == 'c' || c == 'C') radar.calibrate();
#endif
      else if (c == ',') {
        gThreshold -= 0.05f; if (gThreshold < 0.05f) gThreshold = 0.05f;
#if !RADAR_FAKE
        radar.setThreshold(gThreshold);
#endif
#if defined(RADAR_CSI)
        sendNodeThreshold();
#endif
        saveSettings();
      } else if (c == '/') {
        gThreshold += 0.05f; if (gThreshold > 0.95f) gThreshold = 0.95f;
#if !RADAR_FAKE
        radar.setThreshold(gThreshold);
#endif
#if defined(RADAR_CSI)
        sendNodeThreshold();
#endif
        saveSettings();
      }
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
  extPanel.setRotation(EXT_ROTATION);   // ST7789V landscape (was 7 on the ILI9341)
  extPanel.fillScreen(TFT_BLACK);
  topCanvas.setColorDepth(16);
  extReady = (topCanvas.createSprite(240, 180) != nullptr);  // 240x180 * EXT_ZOOM = 320x240, fills panel

  randomSeed(micros());
  loadSettings();
  applyBrightness();

#if RADAR_FAKE
  camBlips[0].mac3[0] = 0x65; camBlips[0].mac3[1] = 0xDE; camBlips[0].mac3[2] = 0xAA;
  camBlips[0].ang     = pickBlipAngle();
  camBlips[0].rad     = 74.0f * 0.55f;
  camBlips[0].rssi    = -65;
  camBlips[0].lastSeen = millis();
  camBlips[0].active  = true;
  strncpy(camBlips[0].vendor, "Ring", 7);
#endif

#if defined(RADAR_CSI)
  initImu();
  loadNodes();
  initCsi();
#elif defined(RADAR_UDP)
  initWiFi();
#elif !RADAR_FAKE
  radar.begin(Serial1, RADAR_RX_PIN, RADAR_TX_PIN, 115200);
  radar.setRate(15);
#endif
}

void loop() {
  M5Cardputer.update();
  serviceKeys();

#if RADAR_FAKE
  serviceFake();
#elif defined(RADAR_CSI)
  serviceCsi();
  serviceCam();
  serviceNodes();
#elif defined(RADAR_UDP)
  serviceUDP();
#else
  radar.poll();
#endif

  uint32_t now = millis();


  static uint32_t lastBot = 0, lastTop = 0;
  if (now - lastBot >= 33) { lastBot = now; if (gMenuOpen) drawMenu(); else drawBottom(); }
  if (extReady && now - lastTop >= 125) {
    lastTop = now;
#if defined(RADAR_CSI)
    drawExternalView();
#else
    if (gViewMode == 0) drawTop(); else drawRaycaster();
#endif
  }
}
