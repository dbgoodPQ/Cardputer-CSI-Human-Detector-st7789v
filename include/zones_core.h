// Zones core — included ONCE from src/main.cpp, after the Wi-Fi code.
// Holds the paired sensor nodes, their live reports (UDP), the commands sent to
// them, ESP-NOW pairing plumbing and the zone helpers the screens use.
// v2: the Cardputer does no sensing itself, so zone i == node i (no "ME" zone).
#ifndef ZONES_CORE_H
#define ZONES_CORE_H

#include <WiFiUdp.h>
#include <esp_now.h>
#include "node_proto.h"

static const int kMaxNodes = 6;
static const int kNodeNameMax = 10;
static const uint32_t kNodeOfflineMs = 10000;

struct NodeCfg {                 // stored in prefs ("nodes" / "list")
    uint8_t mac[6];
    char    name[kNodeNameMax + 1];
};
struct NodeLive {                // runtime only
    uint32_t lastMs;
    uint32_t lastMotionMs;
    float    motion;
    bool     presence;
    int8_t   rssi;
    uint16_t rate;
    char     mode[8];
};

static NodeCfg  gNodes[kMaxNodes];
static NodeLive gLive[kMaxNodes];
static int      gNodeCount = 0;

static WiFiUDP  gNodeUdp;
static bool     gNodeUdpOn = false;

// strongest zone over time (bottom-screen graph), 15 Hz
static const int kZoneHist = 240;
static float    gZoneHist[kZoneHist];
static int      gZoneHistHead = 0;

static void runNodesMenu();      // zones_ui.h

// ── storage ──────────────────────────────────────────────────────────────────
static void loadNodes() {
    Preferences p;
    p.begin("nodes", true);
    int n = p.getUChar("n", 0);
    if (n > kMaxNodes) n = kMaxNodes;
    size_t got = n ? p.getBytes("list", gNodes, sizeof(NodeCfg) * n) : 0;
    p.end();
    gNodeCount = (got == sizeof(NodeCfg) * (size_t)n) ? n : 0;
    for (int i = 0; i < gNodeCount; i++) gNodes[i].name[kNodeNameMax] = '\0';
    memset(gLive, 0, sizeof(gLive));
}

static void saveNodes() {
    Preferences p;
    p.begin("nodes", false);
    p.putUChar("n", (uint8_t)gNodeCount);
    if (gNodeCount) p.putBytes("list", gNodes, sizeof(NodeCfg) * gNodeCount);
    else            p.remove("list");
    p.end();
}

static int findNode(const uint8_t mac[6]) {
    for (int i = 0; i < gNodeCount; i++) if (memcmp(gNodes[i].mac, mac, 6) == 0) return i;
    return -1;
}

static int findNodeHex(const char* hex) {
    char h[13];
    for (int i = 0; i < gNodeCount; i++) {
        nodeproto::macHex(gNodes[i].mac, h);
        if (strcasecmp(h, hex) == 0) return i;
    }
    return -1;
}

static void removeNode(int idx) {
    if (idx < 0 || idx >= gNodeCount) return;
    for (int i = idx; i < gNodeCount - 1; i++) { gNodes[i] = gNodes[i + 1]; gLive[i] = gLive[i + 1]; }
    gNodeCount--;
    memset(&gLive[gNodeCount], 0, sizeof(NodeLive));
    saveNodes();
}

// Adds a node or renames a known one. Returns its index, -1 if full.
static int upsertNode(const uint8_t mac[6], const char* name) {
    int i = findNode(mac);
    if (i < 0) {
        if (gNodeCount >= kMaxNodes) return -1;
        i = gNodeCount++;
        memcpy(gNodes[i].mac, mac, 6);
        memset(&gLive[i], 0, sizeof(NodeLive));
    }
    strncpy(gNodes[i].name, name, kNodeNameMax);
    gNodes[i].name[kNodeNameMax] = '\0';
    saveNodes();
    return i;
}

// ── commands to nodes (UDP broadcast) ────────────────────────────────────────
static void sendNodeCmd(const char* fmt, ...) {
    if (!gNodeUdpOn || gNodeCount == 0) return;
    char line[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    line[n++] = '\n';
    for (int k = 0; k < 2; k++) {        // broadcast is unacknowledged: send twice
        gNodeUdp.beginPacket(IPAddress(255, 255, 255, 255), nodeproto::kCmdPort);
        gNodeUdp.write((const uint8_t*)line, n);
        gNodeUdp.endPacket();
    }
}

static void sendNodeThreshold() { sendNodeCmd("C,THR,%.2f", gThreshold); }

static void sendNodeCmdMac(const char* cmd, int idx) {
    if (idx < 0 || idx >= gNodeCount) return;
    char h[13];
    nodeproto::macHex(gNodes[idx].mac, h);
    sendNodeCmd("C,%s,%s", cmd, h);
}

// ── reports from nodes ───────────────────────────────────────────────────────
static void parseNodeReport(char* line, uint32_t now) {
    // N,<mac>,<seq>,<presence>,<motion>,<rssi>,<rate>,<mode>
    if (strncmp(line, "N,", 2) != 0) return;
    char* f[8];
    int nf = 0;
    char* save = nullptr;
    for (char* t = strtok_r(line, ",", &save); t && nf < 8; t = strtok_r(nullptr, ",", &save)) f[nf++] = t;
    if (nf < 8) return;
    int i = findNodeHex(f[1]);
    if (i < 0) return;                    // not paired with this Cardputer
    NodeLive& L = gLive[i];
    L.lastMs   = now;
    L.presence = atoi(f[3]) != 0;
    float m = atof(f[4]);
    L.motion   = m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
    L.rssi     = (int8_t)atoi(f[5]);
    L.rate     = (uint16_t)atoi(f[6]);
    strncpy(L.mode, f[7], sizeof(L.mode) - 1);
    L.mode[sizeof(L.mode) - 1] = '\0';
    if (L.presence) L.lastMotionMs = now;
}

static bool nodeOnline(int i, uint32_t now) {
    return i >= 0 && i < gNodeCount && gLive[i].lastMs && (now - gLive[i].lastMs) < kNodeOfflineMs;
}

// ── zones (one per node) ──────────────────────────────────────────────
struct ZoneView {
    const char* name;
    bool        online;
    bool        present;
    float       motion;
    int8_t      rssi;
    uint16_t    rate;
    uint32_t    lastMotionMs;
    const char* mode;
};

static int zoneCount() { return gNodeCount; }

static ZoneView getZone(int z, uint32_t now) {
    ZoneView v{};
    const int i = z;
    v.name = gNodes[i].name;
    v.online = nodeOnline(i, now);
    v.present = v.online && gLive[i].presence;
    v.motion = v.online ? gLive[i].motion : 0.0f;
    v.rssi = gLive[i].rssi;
    v.rate = gLive[i].rate;
    v.lastMotionMs = gLive[i].lastMotionMs;
    v.mode = gLive[i].lastMs ? gLive[i].mode : "WAIT";
    return v;
}

// Strongest zone with presence (-1 = none). *maxMotion = strongest motion of any online zone.
static int activeZone(uint32_t now, float* maxMotion = nullptr, int* maxZone = nullptr) {
    int best = -1, bestAny = 0;
    float bm = -1.0f, am = -1.0f;
    for (int z = 0; z < zoneCount(); z++) {
        ZoneView v = getZone(z, now);
        if (!v.online) continue;
        if (v.motion > am) { am = v.motion; bestAny = z; }
        if (v.present && v.motion > bm) { bm = v.motion; best = z; }
    }
    if (maxMotion) *maxMotion = am < 0.0f ? 0.0f : am;
    if (maxZone)   *maxZone = bestAny;
    return best;
}

static bool anyZonePresent(uint32_t now) { return activeZone(now) >= 0; }

static int onlineNodeCount(uint32_t now) {
    int n = 0;
    for (int i = 0; i < gNodeCount; i++) if (nodeOnline(i, now)) n++;
    return n;
}

// True while any online node reports calibration mode.
static bool anyNodeCalibrating(uint32_t now) {
    for (int i = 0; i < gNodeCount; i++)
        if (nodeOnline(i, now) && strcmp(gLive[i].mode, "CAL") == 0) return true;
    return false;
}

// Status pill for the title bars: "2/3 ON". level 0 = all online (green),
// 1 = some offline or calibrating (orange), 2 = none online / no Wi-Fi / no nodes (red).
static const char* nodesTag(uint32_t now, char* buf, size_t n, int& level) {
    if (!wifiReady)      { level = 2; return "NO WIFI"; }
    if (gNodeCount == 0) { level = 2; return "NO NODES"; }
    const int on = onlineNodeCount(now);
    snprintf(buf, n, "%d/%d ON", on, gNodeCount);
    level = on == 0 ? 2 : ((on < gNodeCount || anyNodeCalibrating(now)) ? 1 : 0);
    return buf;
}

static float zoneHistAt(int idx) { return gZoneHist[(gZoneHistHead + idx) % kZoneHist]; }

// Called every loop (and from the nodes screens).
static void serviceNodes() {
    const uint32_t now = millis();
    const bool up = wifiReady && WiFi.status() == WL_CONNECTED;
    if (up && !gNodeUdpOn) {
        gNodeUdpOn = gNodeUdp.begin(nodeproto::kReportPort);
        if (gNodeUdpOn) sendNodeThreshold();
    } else if (!up && gNodeUdpOn) {
        gNodeUdp.stop();
        gNodeUdpOn = false;
    }
    if (gNodeUdpOn) {
        for (int k = 0; k < 12; k++) {
            int sz = gNodeUdp.parsePacket();
            if (sz <= 0) break;
            char buf[96];
            int n = gNodeUdp.read(buf, sizeof(buf) - 1);
            if (n <= 0) continue;
            buf[n] = '\0';
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
            parseNodeReport(buf, now);
        }
        static uint32_t lastThr = 0;
        if (now - lastThr >= 5000) { lastThr = now; sendNodeThreshold(); }   // nodes that rebooted
    }
    static uint32_t lastHist = 0;
    if (now - lastHist >= 66) {
        lastHist = now;
        float m = 0.0f;
        activeZone(now, &m);
        gZoneHist[gZoneHistHead] = m;
        gZoneHistHead = (gZoneHistHead + 1) % kZoneHist;
    }
}

// ── ESP-NOW pairing plumbing ─────────────────────────────────────────────────
struct PairEvt { uint8_t type; uint8_t mac[6]; };
static PairEvt      gPairEvts[8];
static volatile int gPairEvtW = 0, gPairEvtR = 0;
static portMUX_TYPE gPairMux = portMUX_INITIALIZER_UNLOCKED;
static bool         gEspNowOn = false;

static void pairRecv(const uint8_t* mac, const uint8_t* data, int len) {
    uint8_t type = 0;
    if      (nodeproto::hdrOk(data, len, nodeproto::HELLO))   type = nodeproto::HELLO;
    else if (nodeproto::hdrOk(data, len, nodeproto::PAIR_OK)) type = nodeproto::PAIR_OK;
    else return;
    portENTER_CRITICAL(&gPairMux);
    int nw = (gPairEvtW + 1) % 8;
    if (nw != gPairEvtR) {
        gPairEvts[gPairEvtW].type = type;
        memcpy(gPairEvts[gPairEvtW].mac, mac, 6);
        gPairEvtW = nw;
    }
    portEXIT_CRITICAL(&gPairMux);
}

static bool popPairEvt(PairEvt& e) {
    bool ok = false;
    portENTER_CRITICAL(&gPairMux);
    if (gPairEvtR != gPairEvtW) { e = gPairEvts[gPairEvtR]; gPairEvtR = (gPairEvtR + 1) % 8; ok = true; }
    portEXIT_CRITICAL(&gPairMux);
    return ok;
}

static void pairAddPeer(const uint8_t mac[6]) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;
    esp_now_add_peer(&p);
}

static bool pairStart() {
    gPairEvtR = gPairEvtW = 0;
    gEspNowOn = esp_now_init() == ESP_OK;
    if (gEspNowOn) esp_now_register_recv_cb(pairRecv);
    return gEspNowOn;
}

static void pairStop() {
    if (gEspNowOn) { esp_now_unregister_recv_cb(); esp_now_deinit(); gEspNowOn = false; }
    esp_wifi_set_ps(WIFI_PS_NONE);   // console default (see onWifiUp)
}

static void pairSendSeen(const uint8_t mac[6]) {
    pairAddPeer(mac);
    nodeproto::Hdr h;
    nodeproto::mkHdr(h, nodeproto::SEEN);
    esp_now_send(mac, (const uint8_t*)&h, sizeof(h));
}

static bool pairSendCreds(const uint8_t mac[6]) {
    nodeproto::PairPlain pp = {};
    strncpy(pp.ssid, WiFi.SSID().c_str(), sizeof(pp.ssid) - 1);
    strncpy(pp.pass, WiFi.psk().c_str(), sizeof(pp.pass) - 1);
    uint8_t aad[12];
    memcpy(aad, mac, 6);
    WiFi.macAddress(aad + 6);
    nodeproto::PairMsg msg;
    esp_fill_random(msg.nonce, sizeof(msg.nonce));
    bool ok = nodeproto::sealPair(pp, aad, msg);
    memset(&pp, 0, sizeof(pp));
    if (!ok) return false;
    pairAddPeer(mac);
    return esp_now_send(mac, (const uint8_t*)&msg, sizeof(msg)) == ESP_OK;
}

// ── which zone the range cone shows (key n): 0..n-1 = node, >= n = AUTO ──────
static int gConeSel  = kMaxNodes;  // start in AUTO
static int gAutoZone = 0;          // zone AUTO currently follows (sticky, avoids flicker)

static bool coneAuto() { return gNodeCount > 0 && gConeSel >= gNodeCount; }

static void coneNext() {
    if (gNodeCount == 0) return;
    gConeSel = coneAuto() ? 0 : gConeSel + 1;   // nodes..., AUTO, first node
}

// Zone to draw now. AUTO follows the strongest zone with motion and keeps it
// until another one is clearly stronger (+10%) or it goes clear; when every
// zone is clear it stays on the last one shown.
static int coneZoneNow(uint32_t now) {
    if (gNodeCount == 0) return -1;
    if (!coneAuto()) return gConeSel;
    if (gAutoZone >= zoneCount()) gAutoZone = 0;
    ZoneView cur = getZone(gAutoZone, now);
    const int act = activeZone(now);
    if (act >= 0 && act != gAutoZone) {
        ZoneView best = getZone(act, now);
        if (!cur.present || best.motion > cur.motion + 0.10f) gAutoZone = act;
    }
    return gAutoZone;
}

// ── bottom screen: one chip per zone (magenta = motion, outline = clear, grey = offline)
static void drawZoneChips(int y, uint32_t now) {
    const int n = zoneCount();
    if (n == 0) return;
    const int W = canvas.width();
    const int gap = 2;
    const int cw = (W - 8 - (n - 1) * gap) / n;
    canvas.setTextSize(1);
    for (int z = 0; z < n; z++) {
        ZoneView v = getZone(z, now);
        int x = 4 + z * (cw + gap);
        uint16_t txt;
        if (!v.online) {
            canvas.fillRoundRect(x, y, cw, 12, 3, canvas.color565(42, 42, 42));
            txt = canvas.color565(160, 160, 160);
        } else if (v.present) {
            canvas.fillRoundRect(x, y, cw, 12, 3, gColA);
            txt = TFT_WHITE;
        } else {
            canvas.drawRoundRect(x, y, cw, 12, 3, gColB);
            txt = gColB;
        }
        char nm[kNodeNameMax + 1];
        int maxc = (cw - 2) / 6;
        if (maxc > kNodeNameMax) maxc = kNodeNameMax;
        if (maxc < 1) maxc = 1;
        strncpy(nm, v.name, maxc);
        nm[maxc] = '\0';
        canvas.setTextColor(txt);
        canvas.drawString(nm, x + (cw - canvas.textWidth(nm)) / 2, y + 2);
    }
}

#endif  // ZONES_CORE_H
