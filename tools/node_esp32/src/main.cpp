// CSI sensor node — ESP32-WROOM-32 / WROOM-32D
//
// Setup mode (no Wi-Fi saved): LED blinks slowly; the board waits to be paired
// from the Cardputer (settings > NODES > ADD NODE). The Cardputer sends the
// Wi-Fi details over ESP-NOW, encrypted (see include/node_proto.h).
//
// Run mode: joins the Wi-Fi, pings the router 50x/s and measures motion from
// the router's CSI exactly like the Cardputer does, then broadcasts its result
// over UDP 10x/s. The Cardputer names the node and shows it as a zone.
// LED: on while motion is detected; fast blink when the Cardputer asks (BLINK LED).
//
// Reset: hold the BOOT button 5 s (after power-up) -> Wi-Fi forgotten, setup mode.
// A freshly paired node that can't join the Wi-Fi within 60 s goes back to setup mode.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "freertos/queue.h"
#include "node_proto.h"

#ifndef NODE_LED_PIN
#define NODE_LED_PIN 2
#endif
#define BOOT_PIN 0

static const char* kFwVer = "1.0";

static uint8_t gMyMac[6];
static char    gMyHex[13];

// ── LED + BOOT button ─────────────────────────────────────────────────────────
static void ledSet(bool on) { digitalWrite(NODE_LED_PIN, on ? HIGH : LOW); }

static void forgetAndRestart() {
    Preferences p;
    p.begin("csinode", false);
    p.clear();
    p.end();
    Serial.println("# Wi-Fi forgotten, restarting into setup mode");
    for (int i = 0; i < 20; i++) { ledSet(i & 1); delay(60); }
    ESP.restart();
}

static void checkBootButton() {
    static uint32_t t0 = 0;
    if (digitalRead(BOOT_PIN) == LOW) {
        if (!t0) t0 = millis();
        else if (millis() - t0 > 5000) forgetAndRestart();
    } else {
        t0 = 0;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Setup mode: wait for pairing over ESP-NOW
// ══════════════════════════════════════════════════════════════════════════════
static volatile uint32_t gSeenMs     = 0;
static volatile bool     gPairPending = false;
static uint8_t           gPeerMac[6];
static nodeproto::PairMsg gPairBuf;

static void setupRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (nodeproto::hdrOk(data, len, nodeproto::SEEN)) {
        memcpy(gPeerMac, mac, 6);
        gSeenMs = millis();
    } else if (len == (int)sizeof(nodeproto::PairMsg) && nodeproto::hdrOk(data, len, nodeproto::PAIR)
               && !gPairPending) {
        memcpy(gPeerMac, mac, 6);
        memcpy(&gPairBuf, data, len);
        gPairPending = true;
    }
}

static void setChannel(uint8_t ch) {
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
}

static void addPeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;              // current channel
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;          // payload itself is AES-GCM encrypted
    esp_now_add_peer(&p);
}

static void runSetupMode() {
    Serial.printf("# SETUP MODE: node %s waiting to be paired (Cardputer: ` > NODES > ADD NODE)\n", gMyHex);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    if (esp_now_init() != ESP_OK) { Serial.println("# ESP-NOW init failed"); delay(2000); ESP.restart(); }
    esp_now_register_recv_cb(setupRecv);
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    addPeer(bcast);

    nodeproto::Hello hello = {};
    nodeproto::mkHdr(hello.h, nodeproto::HELLO);
    strncpy(hello.fw, kFwVer, sizeof(hello.fw) - 1);

    uint8_t  ch = 1;
    uint32_t lastHop = 0, lastHello = 0;
    setChannel(ch);
    for (;;) {
        const uint32_t now = millis();
        checkBootButton();
        ledSet((now / 500) & 1);

        const bool locked = gSeenMs && (now - gSeenMs) < 15000;   // a Cardputer answered on this channel
        if (!locked && now - lastHop >= 250) {
            ch = ch % 13 + 1;
            setChannel(ch);
            lastHop = now;
            lastHello = 0;
        }
        if (lastHello == 0 || now - lastHello >= 300) {
            esp_now_send(bcast, (const uint8_t*)&hello, sizeof(hello));
            lastHello = now ? now : 1;
        }

        if (gPairPending) {
            uint8_t aad[12];
            memcpy(aad, gMyMac, 6);
            memcpy(aad + 6, gPeerMac, 6);
            nodeproto::PairPlain pp;
            bool ok = nodeproto::openPair(gPairBuf, aad, pp);
            gPairPending = false;
            if (!ok || pp.ssid[0] == '\0') {
                Serial.println("# pairing message rejected (bad key or corrupted)");
                continue;
            }
            Preferences p;
            p.begin("csinode", false);
            p.putString("ssid", pp.ssid);
            p.putString("pass", pp.pass);
            p.putBool("fresh", true);
            p.end();
            memset(&pp, 0, sizeof(pp));
            Serial.println("# paired, Wi-Fi details saved; restarting");

            addPeer(gPeerMac);
            nodeproto::Hdr okMsg;
            nodeproto::mkHdr(okMsg, nodeproto::PAIR_OK);
            for (int i = 0; i < 5; i++) { esp_now_send(gPeerMac, (const uint8_t*)&okMsg, sizeof(okMsg)); delay(60); }
            delay(200);
            ESP.restart();
        }
        delay(5);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Run mode: CSI motion sensing (same pipeline as the Cardputer, see main.cpp there)
// ══════════════════════════════════════════════════════════════════════════════
struct CsiPkt { int8_t buf[128]; int8_t rssi; };
static QueueHandle_t     gCsiQ     = nullptr;
static uint8_t           gBssid[6] = {0};
static volatile bool     gBssidOk  = false;
static volatile uint32_t gCsiCount = 0;
static esp_ping_handle_t gPing     = nullptr;
static bool              gSensing  = false;

static const int   kSub        = 52;
static const int   kCsiWin     = 25;
static float       gShape[kCsiWin][kSub];
static int         gWinIdx     = 0;
static int         gWinFill    = 0;
static float       gCsiMetric  = 0.0f;
static float       gCsiMotion  = 0.0f;
static int8_t      gCsiRssi    = -80;
static float       gRssiEma    = -80.0f;
static float       gFloor      = 0.0f;
static uint32_t    gLastPktMs  = 0;
static uint16_t    gPktRate    = 0;
static float       gThreshold  = 0.15f;     // set by the Cardputer (C,THR)
static const float kCsiSpan    = 4.0f;
static const float kAutoMargin = 2.0f;

static bool     gCalibrated = false;
static float    gCalRef     = 0.0f;
static const uint32_t kCalLeadMs = 3000;
static const uint32_t kCalMeasMs = 10000;
static uint8_t  gCalPhase = 0;
static uint32_t gCalT0    = 0;
static double   gCalSum = 0, gCalSq = 0;
static uint32_t gCalN = 0, gCalCsi0 = 0;

static WiFiUDP  gUdp;
static bool     gUdpOn     = false;
static uint32_t gBlinkUntil = 0;
static bool     gPresent   = false;

static void csiCallback(void*, wifi_csi_info_t* info) {
    if (!info || !info->buf || info->len < 128 || !gBssidOk || !gCsiQ) return;
    if (memcmp(info->mac, gBssid, 6) != 0) return;
    CsiPkt pk;
    memcpy(pk.buf, info->buf, 128);
    pk.rssi = info->rx_ctrl.rssi;
    if (xQueueSend(gCsiQ, &pk, 0) == pdTRUE) gCsiCount++;
}

static void processCsiPkt(const CsiPkt& pk, uint32_t now) {
    float amp[kSub];
    float sum = 0.0f;
    int   j   = 0;
    for (int k = 1; k <= 63; k++) {
        if (k > 26 && k < 38) continue;
        float a = (float)pk.buf[2*k], b = (float)pk.buf[2*k + 1];
        amp[j] = sqrtf(a*a + b*b);
        sum += amp[j++];
    }
    float mean = sum / kSub;
    if (mean < 0.5f) return;
    float inv = 1.0f / mean;
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

static void stopPing() {
    if (!gPing) return;
    esp_ping_stop(gPing);
    esp_ping_delete_session(gPing);
    gPing = nullptr;
}

static void startPing() {
    stopPing();
    IPAddress gw = WiFi.gatewayIP();
    esp_ping_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.count           = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms     = 20;
    cfg.timeout_ms      = 1000;
    cfg.data_size       = 8;
    cfg.ttl             = 64;
    IP_ADDR4(&cfg.target_addr, gw[0], gw[1], gw[2], gw[3]);
    cfg.task_stack_size = 3072;
    cfg.task_prio       = 2;
    esp_ping_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    if (esp_ping_new_session(&cfg, &cbs, &gPing) == ESP_OK) esp_ping_start(gPing);
    else gPing = nullptr;
    Serial.printf("# ping %s: %s\n", gw.toString().c_str(), gPing ? "on" : "FAILED");
}

static void IRAM_ATTR promiscuousRxCb(void*, wifi_promiscuous_pkt_type_t) {}

static void startSensing() {
    if (!gCsiQ) gCsiQ = xQueueCreate(32, sizeof(CsiPkt));
    const uint8_t* bssid = WiFi.BSSID();
    gBssidOk = false;
    if (bssid) { memcpy(gBssid, bssid, 6); gBssidOk = true; }
    xQueueReset(gCsiQ);
    gWinIdx = 0; gWinFill = 0; gCsiMetric = 0.0f; gCsiMotion = 0.0f;
    gFloor = 0.0f; gLastPktMs = 0; gRssiEma = (float)WiFi.RSSI();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promiscuousRxCb);
    wifi_promiscuous_filter_t pf{};
    pf.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;   // CSI only needs the radio in this mode
    esp_wifi_set_promiscuous_filter(&pf);
    wifi_csi_config_t cfg = {};
    cfg.lltf_en = true; cfg.htltf_en = true; cfg.stbc_htltf2_en = true;
    cfg.ltf_merge_en = true; cfg.channel_filter_en = true;
    cfg.manu_scale = false; cfg.shift = 0;
    esp_wifi_set_csi_config(&cfg);
    esp_wifi_set_csi_rx_cb(csiCallback, nullptr);
    esp_wifi_set_csi(true);
    startPing();
    if (!gUdpOn) gUdpOn = gUdp.begin(nodeproto::kCmdPort);
    gSensing = true;
    Serial.printf("# sensing: router %02X:%02X:%02X:%02X:%02X:%02X  ip %s\n",
                  gBssid[0], gBssid[1], gBssid[2], gBssid[3], gBssid[4], gBssid[5],
                  WiFi.localIP().toString().c_str());
}

static void stopSensing() {
    stopPing();
    esp_wifi_set_csi(false);
    esp_wifi_set_promiscuous(false);
    if (gUdpOn) { gUdp.stop(); gUdpOn = false; }
    gSensing = false;
    gCsiMotion = 0.0f;
    gCalPhase = 0;
}

static void startCal() {
    gCalPhase = 1; gCalT0 = millis();
    Serial.println("# calibration requested");
}

// Returns true while calibrating.
static bool stepCal(uint32_t now, bool haveCsi) {
    if (gCalPhase == 1) {
        if (now - gCalT0 < kCalLeadMs) return true;
        gCalPhase = 2; gCalT0 = now; gCalN = 0; gCalCsi0 = gCsiCount;
        gCalSum = gCalSq = 0;
        return true;
    }
    if (gCalPhase != 2) return false;
    if (haveCsi) { const double v = gCsiMetric; gCalSum += v; gCalSq += v * v; gCalN++; }
    if (now - gCalT0 < kCalMeasMs) return true;
    gCalPhase = 0;
    const uint32_t frames = gCsiCount - gCalCsi0;
    if (gCalN < 100 || frames < 150) {
        Serial.printf("# CAL failed: samples=%lu frames=%lu\n", (unsigned long)gCalN, (unsigned long)frames);
        return false;
    }
    const double m  = gCalSum / gCalN;
    const double sd = sqrt(fmax(0.0, gCalSq / gCalN - m * m));
    gCalRef     = (float)fmax(m + 3.0 * sd, 1e-7);
    gCalibrated = true;
    Serial.printf("# CAL done: ref=%.6f\n", gCalRef);
    return false;
}

static void handleCommand(char* line) {
    // C,CAL | C,CALSTOP | C,UNCAL | C,THR,<f> | C,BLINK,<mac> | C,FORGET,<mac>
    if (strncmp(line, "C,", 2) != 0) return;
    char* cmd = line + 2;
    char* arg = strchr(cmd, ',');
    if (arg) *arg++ = '\0';
    if      (strcmp(cmd, "CAL") == 0)     startCal();
    else if (strcmp(cmd, "CALSTOP") == 0) gCalPhase = 0;
    else if (strcmp(cmd, "UNCAL") == 0)   { gCalPhase = 0; gCalibrated = false; }
    else if (strcmp(cmd, "THR") == 0 && arg) {
        float t = atof(arg);
        if (t >= 0.05f && t <= 0.95f) gThreshold = t;
    }
    else if (strcmp(cmd, "BLINK") == 0 && arg && strcasecmp(arg, gMyHex) == 0) gBlinkUntil = millis() + 5000;
    else if (strcmp(cmd, "FORGET") == 0 && arg && strcasecmp(arg, gMyHex) == 0) { stopSensing(); forgetAndRestart(); }
}

static void serviceUdp() {
    if (!gUdpOn) return;
    for (int i = 0; i < 4; i++) {
        int sz = gUdp.parsePacket();
        if (sz <= 0) return;
        char buf[64];
        int n = gUdp.read(buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = '\0';
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
        handleCommand(buf);
    }
}

// 15 Hz: motion, presence hold, calibration; 10 Hz report.
static void serviceSensing(uint32_t now) {
    static uint32_t last = 0, lastRep = 0, rateT0 = 0, rateCnt0 = 0, seq = 0;
    static int      holdCnt = 0;
    static float    heldMotion = 0.0f, outMotion = 0.0f;
    const  int      kHold = 150;          // 10 s at 15 Hz
    static const char* mode = "AUTO";

    if (gCsiQ) {
        CsiPkt pk;
        int n = 0;
        while (n++ < 32 && xQueueReceive(gCsiQ, &pk, 0) == pdTRUE) processCsiPkt(pk, now);
    }
    if (now - last >= 66) {
        last = now;
        if (now - rateT0 >= 1000) { gPktRate = (uint16_t)(gCsiCount - rateCnt0); rateCnt0 = gCsiCount; rateT0 = now; }
        const bool pktsOk  = gLastPktMs && (now - gLastPktMs) < 2000;
        const bool haveCsi = pktsOk && gWinFill >= kCsiWin;
        float raw = 0.0f;
        if (haveCsi) {
            if (gFloor <= 0.0f)           gFloor = gCsiMetric;
            else if (gCsiMetric < gFloor) gFloor += (gCsiMetric - gFloor) * 0.2f;
            else                          gFloor += (gCsiMetric - gFloor) * 0.0007f;
            float ref = gCalibrated ? gCalRef : gFloor * kAutoMargin;
            if (ref < 1e-7f) ref = 1e-7f;
            raw = (gCsiMetric / ref - 1.0f) / kCsiSpan;
            raw = raw < 0.0f ? 0.0f : (raw > 1.0f ? 1.0f : raw);
        }
        gCsiMotion += (raw - gCsiMotion) * 0.4f;

        if (stepCal(now, haveCsi)) {
            holdCnt = 0; heldMotion = 0.0f; gPresent = false; outMotion = gCsiMotion; mode = "CAL";
        } else if (!pktsOk) {
            holdCnt = 0; heldMotion = 0.0f; gPresent = false; outMotion = 0.0f; mode = "NO CSI";
        } else {
            float m = gCsiMotion;
            if (m > gThreshold)      { holdCnt = kHold; heldMotion = m; gPresent = true; }
            else if (holdCnt > 0)    { holdCnt--; m = heldMotion * (0.10f + 0.90f * holdCnt / kHold); gPresent = true; }
            else                     { gPresent = false; m = 0.0f; }
            outMotion = m;
            mode = gCalibrated ? "RUN" : "AUTO";
        }
    }
    if (now - lastRep >= 100 && gUdpOn) {
        lastRep = now;
        char line[80];
        int n = snprintf(line, sizeof(line), "N,%s,%lu,%d,%.3f,%d,%u,%s\n", gMyHex, (unsigned long)(++seq),
                         gPresent ? 1 : 0, outMotion, (int)gCsiRssi, (unsigned)gPktRate, mode);
        gUdp.beginPacket(IPAddress(255, 255, 255, 255), nodeproto::kReportPort);
        gUdp.write((const uint8_t*)line, n);
        gUdp.endPacket();
    }
}

static void runRunMode(const char* ssid, const char* pass, bool fresh) {
    Serial.printf("# RUN MODE: node %s joining %s\n", gMyHex, ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    if (pass[0]) WiFi.begin(ssid, pass); else WiFi.begin(ssid);
    const uint32_t t0 = millis();
    bool everConnected = false;
    for (;;) {
        const uint32_t now = millis();
        checkBootButton();
        const bool up = WiFi.status() == WL_CONNECTED;
        if (up && !gSensing) {
            if (!everConnected && fresh) {
                Preferences p; p.begin("csinode", false); p.putBool("fresh", false); p.end();
            }
            everConnected = true;
            startSensing();
        } else if (!up && gSensing) {
            Serial.println("# Wi-Fi lost");
            stopSensing();
        }
        if (!everConnected && fresh && now - t0 > 60000) {
            Serial.println("# could not join the Wi-Fi after pairing");
            forgetAndRestart();
        }
        if (gSensing) {
            serviceUdp();
            serviceSensing(now);
        }
        // LED: fast blink on request, else on while motion is detected; slow pulse while offline
        if (gBlinkUntil && (int32_t)(gBlinkUntil - now) > 0) ledSet((now / 100) & 1);
        else if (!up)                                         ledSet(((now / 1000) % 4) == 0);
        else                                                  ledSet(gPresent);
        delay(2);
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(NODE_LED_PIN, OUTPUT);
    pinMode(BOOT_PIN, INPUT_PULLUP);
    ledSet(false);
    WiFi.mode(WIFI_STA);
    WiFi.macAddress(gMyMac);
    nodeproto::macHex(gMyMac, gMyHex);
    Serial.printf("\n# CSI node fw %s  id NODE %02X%02X  mac %s\n", kFwVer, gMyMac[4], gMyMac[5], gMyHex);

    Preferences p;
    p.begin("csinode", true);
    String ssid  = p.getString("ssid", "");
    String pass  = p.getString("pass", "");
    bool   fresh = p.getBool("fresh", false);
    p.end();

    if (ssid.length() == 0) runSetupMode();
    else runRunMode(ssid.c_str(), pass.c_str(), fresh);
}

void loop() {}
