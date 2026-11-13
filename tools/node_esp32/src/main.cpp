// CSI sensor node — ESP32-WROOM-32 / WROOM-32D
//
// Setup mode (no Wi-Fi saved): LED blinks slowly; the board waits to be paired
// from the Cardputer (settings > NODES > ADD NODE). The Cardputer sends the
// Wi-Fi details over ESP-NOW, encrypted (see include/node_proto.h).
//
// Run mode: joins the Wi-Fi, pings the router 50x/s and measures motion from
// the router's CSI with the v2 detector (include/csi_v2.h: smoothness of the
// subcarrier spread over the last second, no calibration), then broadcasts its
// result over UDP 10x/s. The Cardputer names the node and shows it as a zone.
// Raw CSI logging (v2): a PC running tools/csi_lab/record.py sends
// C,LOG,<mac>,<port> and the node streams every router CSI packet to it.
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
#include "csi_v2.h"
#include "fw_version.h"

#ifndef NODE_LED_PIN
#define NODE_LED_PIN 2
#endif
#define BOOT_PIN 0

static const char* kFwVer = FW_VERSION;

static uint8_t gMyMac[6];
static char    gMyHex[13];

// Milliseconds from t to now, signed. Timestamps set by commands (millis()) can be
// slightly newer than the loop's `now`; plain unsigned `now - t` then wraps to a
// huge value and fires timeouts at once.
static inline int32_t msSince(uint32_t now, uint32_t t) { return (int32_t)(now - t); }

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
// Run mode: CSI motion sensing
// ══════════════════════════════════════════════════════════════════════════════
struct CsiPkt { int8_t buf[128]; wifi_pkt_rx_ctrl_t rx; uint8_t cat; };
static QueueHandle_t     gCsiQ     = nullptr;
static uint8_t           gBssid[6] = {0};
static volatile bool     gBssidOk  = false;
static volatile uint32_t gCsiCount = 0;
static volatile uint32_t gCsiDrop  = 0;   // queue full (diagnostic, sent in raw logs)
static esp_ping_handle_t gPing     = nullptr;
static bool              gSensing  = false;

// Ping rate: 50/s by default (v2 was tuned at 50); C,RATE switches it at run
// time (not saved) so recordings can be made at other rates.
static const int      kPingHzDefault = 50;
static int            gPingHz   = kPingHzDefault;
static csiv2::FrameGate gGate;
static csiv2::Detector  gV2;
static int8_t      gCsiRssi    = -80;
static float       gRssiEma    = -80.0f;
static uint16_t    gPktRate    = 0;

static WiFiUDP  gUdp;
static bool     gUdpOn     = false;
static uint32_t gBlinkUntil = 0;
static bool     gPresent   = false;
static float    gMotion    = 0.0f;   // v2 probability, shown as motion level

static void csiCallback(void*, wifi_csi_info_t* info) {
    if (!info || !info->buf || info->len < 128 || !gBssidOk || !gCsiQ) return;
    if (memcmp(info->mac, gBssid, 6) != 0) return;
    CsiPkt pk;
    memcpy(pk.buf, info->buf, 128);
    pk.rx   = info->rx_ctrl;
    // frame kind: 11bg / 11n, 20/40 MHz, STBC. Different kinds have different
    // L-LTF shapes, so only one kind is ever compared (see processCsiPkt).
    pk.cat  = (uint8_t)(info->rx_ctrl.sig_mode | (info->rx_ctrl.cwb << 2) | (info->rx_ctrl.stbc << 3));
    if (xQueueSend(gCsiQ, &pk, 0) == pdTRUE) gCsiCount++;
    else gCsiDrop++;
}

// Frame gate (kind lock + 10 ms slots) and v2 detector: see include/csi_v2.h.
static uint8_t processCsiPkt(const CsiPkt& pk, uint32_t now, float* spread) {
    *spread = 0.0f;
    const uint8_t st = gGate.admit(pk.cat, pk.rx.timestamp, now);
    if (gGate.lockChanged) { gGate.lockChanged = false; gV2.clearHistory(); }
    if (st != csiv2::kUsed) return st;
    const uint8_t r = gV2.addPacket(pk.buf, now, spread);
    if (r == csiv2::kUsed) {
        gRssiEma += ((float)pk.rx.rssi - gRssiEma) * 0.1f;
        gCsiRssi  = (int8_t)lroundf(gRssiEma);
    }
    return r;
}

// ── raw CSI logging to a PC (tools/csi_lab/record.py) ─────────────────────────
// The PC sends C,LOG,<mac>,<port> (repeated every ~2 s as a keep-alive); the node
// streams every router CSI packet to the sender's IP:<port>, 8 records per
// datagram. Stops after kLogTimeoutMs without a refresh, or on C,LOG,<mac>,0.
// Format (little-endian), version 3 (v2 = same header, metric = v1 metric;
// v1 = header without the last 4 bytes):
//   datagram = LogHdr + count * LogRec
#pragma pack(push, 1)
struct LogHdr {
    char     magic[4];      // "CSIL"
    uint8_t  ver;           // 3
    uint8_t  count;         // records in this datagram
    uint8_t  mac[6];        // node station MAC
    uint32_t dseq;          // datagram sequence
    uint32_t drops;         // CSI packets lost to a full queue since boot
    uint8_t  ping_hz;       // gateway pings per second
    uint8_t  v1_win;        // v1 window, packets (0 = no v1 on this firmware)
    uint8_t  slot_ms;       // slot admission (0 = off)
    uint8_t  flags;         // bit 0: metric = v2 spread after Hampel
};
static_assert(sizeof(LogHdr) == 24, "LogHdr layout");
struct LogRec {
    uint32_t rx_us;         // rx_ctrl.timestamp (radio clock, us)
    uint32_t ms;            // millis() when processed (detector "now")
    float    metric;        // v2: spread of this packet after Hampel (0 if not used)
    int8_t   rssi;
    int8_t   noise;
    uint8_t  rate;
    uint8_t  sig_mode;
    uint8_t  mcs;
    uint8_t  cwb;
    uint8_t  stbc;
    uint8_t  channel;
    uint8_t  status;        // 0 kind skipped, 1 weak, 3 used, 4 second packet in a slot
    uint8_t  sgi;
    uint8_t  lock_cat;      // locked frame kind (255 = none yet)
    uint8_t  cat;           // this packet's frame kind
    int8_t   buf[128];      // raw CSI (classic ESP32 order: 0..63 = SC 0..31, -32..-1)
};
#pragma pack(pop)
static_assert(sizeof(LogRec) == 152, "LogRec layout");

static const int      kLogPerDgram  = 8;
static const uint32_t kLogTimeoutMs = 6000;
static IPAddress gLogIp;
static uint16_t  gLogPort    = 0;      // 0 = off
static uint32_t  gLogLastCmd = 0;
static uint32_t  gLogDseq    = 0;
static uint32_t  gLogFirstMs = 0;
static uint8_t   gLogN       = 0;
static uint8_t   gLogBuf[sizeof(LogHdr) + kLogPerDgram * sizeof(LogRec)];

static void logFlush() {
    if (!gLogN || !gLogPort || !gUdpOn) { gLogN = 0; return; }
    LogHdr h;
    memcpy(h.magic, "CSIL", 4);
    h.ver = 3; h.count = gLogN;
    h.ping_hz = (uint8_t)gPingHz; h.v1_win = 0; h.slot_ms = csiv2::FrameGate::kSlotUs / 1000; h.flags = 1;
    memcpy(h.mac, gMyMac, 6);
    h.dseq = ++gLogDseq; h.drops = gCsiDrop;
    memcpy(gLogBuf, &h, sizeof(h));
    gUdp.beginPacket(gLogIp, gLogPort);
    gUdp.write(gLogBuf, sizeof(LogHdr) + gLogN * sizeof(LogRec));
    gUdp.endPacket();
    gLogN = 0;
}

static void logPacket(const CsiPkt& pk, uint8_t status, float spread, uint32_t now) {
    if (!gLogPort) return;
    LogRec r;
    r.rx_us = pk.rx.timestamp; r.ms = now; r.metric = spread;
    r.rssi = pk.rx.rssi; r.noise = pk.rx.noise_floor; r.rate = pk.rx.rate;
    r.sig_mode = pk.rx.sig_mode; r.mcs = pk.rx.mcs; r.cwb = pk.rx.cwb; r.stbc = pk.rx.stbc;
    r.channel = pk.rx.channel; r.status = status; r.sgi = pk.rx.sgi;
    r.lock_cat = gGate.lockCat < 0 ? 255 : (uint8_t)gGate.lockCat; r.cat = pk.cat;
    memcpy(r.buf, pk.buf, 128);
    if (gLogN == 0) gLogFirstMs = now;
    memcpy(gLogBuf + sizeof(LogHdr) + gLogN * sizeof(LogRec), &r, sizeof(r));
    if (++gLogN >= kLogPerDgram) logFlush();
}

static void serviceLog(uint32_t now) {
    if (!gLogPort) return;
    if (msSince(now, gLogLastCmd) > (int32_t)kLogTimeoutMs) {
        Serial.println("# raw log: PC stopped refreshing, off");
        gLogN = 0; gLogPort = 0;
        return;
    }
    if (gLogN && msSince(now, gLogFirstMs) >= 100) logFlush();
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
    cfg.interval_ms     = 1000 / gPingHz;
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
    Serial.printf("# ping %s %d/s: %s\n", gw.toString().c_str(), gPingHz, gPing ? "on" : "FAILED");
}

__attribute__((unused)) static void IRAM_ATTR promiscuousRxCb(void*, wifi_promiscuous_pkt_type_t) {}

static void startSensing() {
    if (!gCsiQ) gCsiQ = xQueueCreate(32, sizeof(CsiPkt));
    const uint8_t* bssid = WiFi.BSSID();
    gBssidOk = false;
    if (bssid) { memcpy(gBssid, bssid, 6); gBssidOk = true; }
    xQueueReset(gCsiQ);
    const double cut = gV2.cut > 0 ? gV2.cut : csiv2::kCutDefault;   // keep SENS across reconnects
    gV2.reset(); gV2.cut = cut;
    gGate.reset(millis());
    gPresent = false; gMotion = 0.0f; gRssiEma = (float)WiFi.RSSI();
    esp_wifi_set_ps(WIFI_PS_NONE);
    // No promiscuous mode: only router frames addressed to this node (its ping
    // replies), not the ones sent to other devices. Same as the Cardputer.
    esp_wifi_set_promiscuous(false);
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
    gLogN = 0; gLogPort = 0;
    stopPing();
    esp_wifi_set_csi(false);
    esp_wifi_set_promiscuous(false);
    if (gUdpOn) { gUdp.stop(); gUdpOn = false; }
    gSensing = false;
    gMotion = 0.0f;
    gPresent = false;
}

static void handleCommand(char* line, const IPAddress& from) {
    // C,SENS,<cut> (v2 smoothness cut-off, from the Cardputer) | C,BLINK,<mac> | C,FORGET,<mac>
    // C,LOG,<mac>,<port> (port 0 = stop) | C,RATE,<mac>,<hz> (pings/s, not saved)  -- from a PC
    // v1 commands (CAL, CALSTOP, UNCAL, THR) are ignored: v2 needs no calibration.
    if (strncmp(line, "C,", 2) != 0) return;
    char* cmd = line + 2;
    char* arg = strchr(cmd, ',');
    if (arg) *arg++ = '\0';
    if (strcmp(cmd, "SENS") == 0 && arg) {
        const float c = atof(arg);
        if (c >= 0.20f && c <= 0.80f) gV2.cut = c;
    }
    else if (strcmp(cmd, "RATE") == 0 && arg) {
        char* hzStr = strchr(arg, ',');
        if (!hzStr) return;
        *hzStr++ = '\0';
        if (strcasecmp(arg, gMyHex) != 0) return;
        const int hz = atoi(hzStr);
        if (hz >= 10 && hz <= 100 && hz != gPingHz) {
            gPingHz = hz;
            if (gSensing) startPing();
        }
    }
    else if (strcmp(cmd, "BLINK") == 0 && arg && strcasecmp(arg, gMyHex) == 0) gBlinkUntil = millis() + 5000;
    else if (strcmp(cmd, "FORGET") == 0 && arg && strcasecmp(arg, gMyHex) == 0) { stopSensing(); forgetAndRestart(); }
    else if (strcmp(cmd, "LOG") == 0 && arg) {
        char* portStr = strchr(arg, ',');
        if (!portStr) return;
        *portStr++ = '\0';
        if (strcasecmp(arg, gMyHex) != 0) return;
        const long port = atol(portStr);
        if (port <= 0 || port > 65535) {
            if (gLogPort) Serial.println("# raw log: off");
            logFlush(); gLogPort = 0;
            return;
        }
        if (!gLogPort || gLogIp != from || gLogPort != (uint16_t)port)
            Serial.printf("# raw log: on -> %s:%ld\n", from.toString().c_str(), port);
        gLogIp = from; gLogPort = (uint16_t)port; gLogLastCmd = millis();
    }
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
        handleCommand(buf, gUdp.remoteIP());
    }
}

// v2 tick every 250 ms; report 10 Hz. No hold here: the Cardputer holds the display.
static void serviceSensing(uint32_t now) {
    static uint32_t lastTick = 0, lastRep = 0, rateT0 = 0, rateCnt0 = 0, seq = 0;

    if (gCsiQ) {
        CsiPkt pk;
        int n = 0;
        while (n++ < 32 && xQueueReceive(gCsiQ, &pk, 0) == pdTRUE) {
            float spread;
            const uint8_t st = processCsiPkt(pk, now, &spread);
            logPacket(pk, st, spread, now);
        }
    }
    serviceLog(now);
    if (msSince(now, rateT0) >= 1000) { gPktRate = (uint16_t)(gCsiCount - rateCnt0); rateCnt0 = gCsiCount; rateT0 = now; }
    if (msSince(now, lastTick) >= (int32_t)csiv2::kTickMs) {
        lastTick = now;
        gV2.tick(now);
        gPresent = gV2.present;
        gMotion  = (float)gV2.prob;
    }
    if (msSince(now, lastRep) >= 100 && gUdpOn) {
        lastRep = now;
        // N,<mac>,<seq>,<pres>,<motion>,<rssi>,<rate>,<mode>,<detector>,<fw>
        char line[96];
        int n = snprintf(line, sizeof(line), "N,%s,%lu,%d,%.3f,%d,%u,%s,2,%s\n", gMyHex, (unsigned long)(++seq),
                         gPresent ? 1 : 0, gMotion, (int)gCsiRssi, (unsigned)gPktRate,
                         gV2.ready ? "RUN" : "NO CSI", kFwVer);
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
