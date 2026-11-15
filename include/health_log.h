// Node health log — included ONCE from src/main.cpp, after zones_core.h.
//
// The Cardputer watches every paired node (its 10 Hz reports and the health line
// a node sends every 10 s) and writes what happens to the microSD card, one CSV
// file per day:  /CSIRADAR/logs/2026-11-15.csv
//
//   time,node,id,event,detail
//   18:01:37,BEDROOM,C3D4,OFFLINE,no report for 10 s
//   18:02:00,KITCHEN,A1B2,HEALTH,3h12m -58dB 49/s 141k
//
// Events: ONLINE, OFFLINE, BOOT (node restarted, with the reason), DROP (node lost
// the Wi-Fi), SYS (the Cardputer itself) and a HEALTH snapshot of every online node
// every N minutes (uptime, signal, router frames/s, free memory).
//
// The Cardputer has no battery-backed clock: the time comes from the internet
// (NTP) over the home Wi-Fi. Lines made before the clock is set wait in memory.
// Per-day counters (online %, offline / boot / drop counts, 15-min bars) live in
// /CSIRADAR/state.bin so a Cardputer restart keeps today's numbers; past days get
// one line each in /CSIRADAR/summary.csv (LOGS list and web page use it).
//
// The SD card shares the SPI bus with the external screen (ext_panel.h,
// bus_shared). Writes are batched (every 10 s or 3.5 KB) from the main loop, so
// they never interleave with a screen push.
#ifndef HEALTH_LOG_H
#define HEALTH_LOG_H

#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <esp_system.h>

// microSD slot of the Cardputer (ADV): SCK and MOSI are shared with the EXT header.
static const int kSdSck = 40, kSdMiso = 39, kSdMosi = 14, kSdCs = 12;

static const char* const kLogRoot   = "/CSIRADAR";
static const char* const kLogDir    = "/CSIRADAR/logs";
static const char* const kStatePath = "/CSIRADAR/state.bin";
static const char* const kSumPath   = "/CSIRADAR/summary.csv";
static const char* const kCsvHeader = "time,node,id,event,detail\n";

// ── settings (prefs "hlog") ──────────────────────────────────────────────────
struct TzOpt { const char* name; const char* posix; };
static const TzOpt kTzOpts[] = {
    { "New York",    "EST5EDT,M3.2.0,M11.1.0" },
    { "Chicago",     "CST6CDT,M3.2.0,M11.1.0" },
    { "Denver",      "MST7MDT,M3.2.0,M11.1.0" },
    { "Phoenix",     "MST7" },
    { "Los Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "UTC",         "UTC0" },
    { "London",      "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Paris",       "CET-1CEST,M3.5.0,M10.5.0/3" },
};
static const int     kNumTz       = sizeof(kTzOpts) / sizeof(kTzOpts[0]);
static const uint8_t kSnapOpts[]  = { 1, 5, 10, 15, 30 };
static const uint8_t kKeepOpts[]  = { 1, 3, 5, 7, 14, 30 };
static const int     kNumSnap     = sizeof(kSnapOpts);
static const int     kNumKeep     = sizeof(kKeepOpts);

struct HlogCfg { bool on; uint8_t snapMin; uint8_t keepDays; uint8_t tz; bool web; };
static HlogCfg gHlog = { true, 1, 5, 0, true };

static void hlogLoadCfg() {
    Preferences p;
    p.begin("hlog", true);
    gHlog.on       = p.getBool("on", true);
    gHlog.snapMin  = p.getUChar("snap", 1);
    gHlog.keepDays = p.getUChar("keep", 5);
    gHlog.tz       = p.getUChar("tz", 0);
    gHlog.web      = p.getBool("web", true);
    p.end();
    bool ok = false;
    for (int i = 0; i < kNumSnap; i++) ok |= kSnapOpts[i] == gHlog.snapMin;
    if (!ok) gHlog.snapMin = 1;
    ok = false;
    for (int i = 0; i < kNumKeep; i++) ok |= kKeepOpts[i] == gHlog.keepDays;
    if (!ok) gHlog.keepDays = 5;
    if (gHlog.tz >= kNumTz) gHlog.tz = 0;
}

static void hlogSaveCfg() {
    Preferences p;
    p.begin("hlog", false);
    p.putBool("on", gHlog.on);
    p.putUChar("snap", gHlog.snapMin);
    p.putUChar("keep", gHlog.keepDays);
    p.putUChar("tz", gHlog.tz);
    p.putBool("web", gHlog.web);
    p.end();
}

// ── runtime state ────────────────────────────────────────────────────────────
static bool     gSdOk       = false;
static uint32_t gSdSizeMB   = 0;
static uint32_t gSdRetryMs  = 0;
static bool     gClockOk    = false;
static bool     gNtpStarted = false;
static int32_t  gDayYmd     = 0;           // day of the open log file, yyyymmdd
static char     gDayName[16] = "";         // "2026-11-15.csv"
static uint32_t gLogLost    = 0;           // lines dropped (SD missing and buffer full)

static char     gLogBuf[4096];             // lines waiting to be written
static size_t   gLogLen     = 0;
static uint32_t gLogFlushMs = 0;

struct PendLine { uint32_t ms; char text[80]; };   // made before the clock was set
static PendLine gPend[24];
static int      gPendN = 0;

// Per node, keyed by MAC (nodes can be removed / re-ordered in the NODES menu).
struct NodeHealth {
    bool     used;
    uint8_t  mac[6];
    // event detection (runtime)
    bool     seen, online, haveDiag;
    uint32_t lastUpS;
    uint16_t lastDrops;
    // last health line (fw 2.1.0-dev2+)
    bool     hasH;
    uint32_t heap, minHeap;
    uint16_t pingHz;
    uint8_t  disc;
    // today (saved in state.bin)
    uint32_t obsS, onS;                    // seconds watched / seconds online
    uint16_t offN, bootN, dropN;
    uint8_t  slots[96];                    // 15-min slots: 0 none, 1 ok, 2 was offline
    uint32_t lastEvT;                      // time_t of the last event (0 = none)
    char     lastEv[32];                   // "BOOT NET: link was dead"
};
static NodeHealth gNH[kMaxNodes];

static NodeHealth* nhFor(int i) {
    if (i < 0 || i >= gNodeCount) return nullptr;
    for (int k = 0; k < kMaxNodes; k++)
        if (gNH[k].used && memcmp(gNH[k].mac, gNodes[i].mac, 6) == 0) return &gNH[k];
    int freeK = -1;
    for (int k = 0; k < kMaxNodes && freeK < 0; k++) if (!gNH[k].used) freeK = k;
    if (freeK < 0) {                       // reuse a slot whose node is no longer paired
        for (int k = 0; k < kMaxNodes && freeK < 0; k++) if (findNode(gNH[k].mac) < 0) freeK = k;
    }
    if (freeK < 0) return nullptr;
    memset(&gNH[freeK], 0, sizeof(NodeHealth));
    gNH[freeK].used = true;
    memcpy(gNH[freeK].mac, gNodes[i].mac, 6);
    return &gNH[freeK];
}

// ── helpers ──────────────────────────────────────────────────────────────────
static int32_t ymdOf(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

static void ymdName(int32_t ymd, char* out, size_t n) {
    snprintf(out, n, "%04ld-%02ld-%02ld.csv", (long)(ymd / 10000), (long)(ymd / 100 % 100), (long)(ymd % 100));
}

// "2026-11-15.csv" -> 20261115, 0 if not a day file.
static int32_t ymdFromName(const char* nm) {
    if (strlen(nm) != 14 || strcmp(nm + 10, ".csv") != 0 || nm[4] != '-' || nm[7] != '-') return 0;
    for (int i : { 0, 1, 2, 3, 5, 6, 8, 9 }) if (nm[i] < '0' || nm[i] > '9') return 0;
    return atol(nm) * 10000 + atol(nm + 5) * 100 + atol(nm + 8);
}

// Uptime as at most 5 chars: 45s, 12m, 3h12m, 9d02h.
static void fmtUp(uint32_t s, char* out, size_t n) {
    if (s < 60)          snprintf(out, n, "%lus", (unsigned long)s);
    else if (s < 3600)   snprintf(out, n, "%lum", (unsigned long)(s / 60));
    else if (s < 86400)  snprintf(out, n, "%luh%02lum", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
    else                 snprintf(out, n, "%lud%02luh", (unsigned long)(s / 86400), (unsigned long)(s / 3600 % 24));
}

static void fmtDur(uint32_t ms, char* out, size_t n) {
    const uint32_t s = ms / 1000;
    if (s < 120)        snprintf(out, n, "%lu s", (unsigned long)s);
    else if (s < 7200)  snprintf(out, n, "%lu min", (unsigned long)(s / 60));
    else                snprintf(out, n, "%luh%02lum", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

static const char* bootWhy(const char* code) {
    if (!strcmp(code, "PWR"))   return "power on";
    if (!strcmp(code, "EXT"))   return "reset button";
    if (!strcmp(code, "SW"))    return "restarted";
    if (!strcmp(code, "NET"))   return "link was dead";
    if (!strcmp(code, "HEAP"))  return "low memory";
    if (!strcmp(code, "WDT"))   return "it froze";
    if (!strcmp(code, "PANIC")) return "crashed";
    if (!strcmp(code, "BOD"))   return "weak power";
    return "unknown";
}

static const char* resetCode(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "PWR";
        case ESP_RST_EXT:      return "EXT";
        case ESP_RST_SW:       return "SW";
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_BROWNOUT: return "BOD";
        default:               return "?";
    }
}

static void nodeIdHex(int i, char* out) { snprintf(out, 5, "%02X%02X", gNodes[i].mac[4], gNodes[i].mac[5]); }

static void clockHHMM(char* out, size_t n) {
    if (!gClockOk) { snprintf(out, n, "--:--"); return; }
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// ── SD card ──────────────────────────────────────────────────────────────────
static bool hlogMount() {
    SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
    if (!SD.begin(kSdCs, SPI, 20000000)) { SD.end(); gSdOk = false; return false; }
    if (SD.cardType() == CARD_NONE)      { SD.end(); gSdOk = false; return false; }
    if (!SD.exists(kLogRoot)) SD.mkdir(kLogRoot);
    if (!SD.exists(kLogDir))  SD.mkdir(kLogDir);
    gSdSizeMB = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
    gSdOk = true;
    Serial.printf("# SD card: %lu MB\n", (unsigned long)gSdSizeMB);
    return true;
}

static void hlogUnmount() {
    SD.end();
    gSdOk = false;
    gSdRetryMs = millis();
    Serial.println("# SD card lost");
}

// Writes the buffered lines to today's file.
static void hlogFlush() {
    gLogFlushMs = millis();
    if (gLogLen == 0 || !gSdOk || !gDayName[0]) return;
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", kLogDir, gDayName);
    File f = SD.open(path, FILE_APPEND);
    if (!f) { hlogUnmount(); return; }
    bool ok = true;
    if (f.size() == 0) ok = f.print(kCsvHeader) > 0;
    if (ok) ok = f.write((const uint8_t*)gLogBuf, gLogLen) == gLogLen;
    f.close();
    if (!ok) { hlogUnmount(); return; }
    gLogLen = 0;
}

static void hlogAppendRaw(const char* line) {
    const size_t l = strlen(line);
    if (gLogLen + l >= sizeof(gLogBuf) - 1) hlogFlush();
    if (gLogLen + l >= sizeof(gLogBuf) - 1) { gLogLost++; return; }   // no SD: keep what fits
    memcpy(gLogBuf + gLogLen, line, l);
    gLogLen += l;
    if (gLogLen > 3500) hlogFlush();
}

// ── day state (state.bin) and summary.csv ───────────────────────────────────
struct HlogStateNode {
    uint8_t  mac[6];
    uint16_t offN, bootN, dropN;
    uint32_t obsS, onS;
    uint8_t  slots[96];
    uint32_t lastEvT;
    char     lastEv[32];
};
struct HlogState { uint32_t magic; int32_t ymd; HlogStateNode n[kMaxNodes]; };
static const uint32_t kStateMagic = 0x484C4F47;   // "HLOG"

static void hlogSaveState() {
    if (!gSdOk || !gDayYmd) return;
    static HlogState st;
    memset(&st, 0, sizeof(st));
    st.magic = kStateMagic;
    st.ymd   = gDayYmd;
    for (int k = 0; k < kMaxNodes; k++) {
        if (!gNH[k].used) continue;
        HlogStateNode& s = st.n[k];
        memcpy(s.mac, gNH[k].mac, 6);
        s.offN = gNH[k].offN; s.bootN = gNH[k].bootN; s.dropN = gNH[k].dropN;
        s.obsS = gNH[k].obsS; s.onS = gNH[k].onS;
        memcpy(s.slots, gNH[k].slots, sizeof(s.slots));
        s.lastEvT = gNH[k].lastEvT;
        memcpy(s.lastEv, gNH[k].lastEv, sizeof(s.lastEv));
    }
    File f = SD.open(kStatePath, FILE_WRITE);
    if (!f) { hlogUnmount(); return; }
    f.write((const uint8_t*)&st, sizeof(st));
    f.close();
}

static void hlogAppendSummary(int32_t ymd, int off, int boots, int drops) {
    if (!gSdOk || !ymd) return;
    File f = SD.open(kSumPath, FILE_APPEND);
    if (!f) return;
    char d[16];
    ymdName(ymd, d, sizeof(d));
    d[10] = '\0';
    f.printf("%s,%d,%d,%d\n", d, off, boots, drops);
    f.close();
}

static void hlogDayTotals(int& off, int& boots, int& drops) {
    off = boots = drops = 0;
    for (int k = 0; k < kMaxNodes; k++)
        if (gNH[k].used) { off += gNH[k].offN; boots += gNH[k].bootN; drops += gNH[k].dropN; }
}

static void hlogResetDay() {
    for (int k = 0; k < kMaxNodes; k++) {
        gNH[k].obsS = gNH[k].onS = 0;
        gNH[k].offN = gNH[k].bootN = gNH[k].dropN = 0;
        memset(gNH[k].slots, 0, sizeof(gNH[k].slots));
    }
}

// At clock set: restore today's counters, or close the day state.bin belongs to.
static void hlogLoadState(int32_t today) {
    if (!gSdOk) return;
    File f = SD.open(kStatePath, FILE_READ);
    if (!f) return;
    static HlogState st;
    const bool ok = f.read((uint8_t*)&st, sizeof(st)) == sizeof(st) && st.magic == kStateMagic;
    f.close();
    if (!ok) return;
    if (st.ymd != today) {             // the Cardputer was off over midnight
        int off = 0, boots = 0, drops = 0;
        for (int k = 0; k < kMaxNodes; k++) { off += st.n[k].offN; boots += st.n[k].bootN; drops += st.n[k].dropN; }
        hlogAppendSummary(st.ymd, off, boots, drops);
        for (int k = 0; k < kMaxNodes; k++) {     // keep "last event" across days
            const int i = findNode(st.n[k].mac);
            NodeHealth* h = i >= 0 ? nhFor(i) : nullptr;
            if (h) { h->lastEvT = st.n[k].lastEvT; memcpy(h->lastEv, st.n[k].lastEv, sizeof(h->lastEv)); }
        }
        return;
    }
    for (int k = 0; k < kMaxNodes; k++) {
        const int i = findNode(st.n[k].mac);
        NodeHealth* h = i >= 0 ? nhFor(i) : nullptr;
        if (!h) continue;
        h->offN = st.n[k].offN; h->bootN = st.n[k].bootN; h->dropN = st.n[k].dropN;
        h->obsS = st.n[k].obsS; h->onS = st.n[k].onS;
        memcpy(h->slots, st.n[k].slots, sizeof(h->slots));
        h->lastEvT = st.n[k].lastEvT;
        memcpy(h->lastEv, st.n[k].lastEv, sizeof(h->lastEv));
    }
}

// Deletes day files older than the KEEP setting (today counts as one day).
static void hlogCleanup() {
    if (!gSdOk || !gClockOk) return;
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_mday -= (int)gHlog.keepDays - 1;
    tm.tm_hour = 12;
    const int32_t cutoff = ymdOf(mktime(&tm));
    File dir = SD.open(kLogDir);
    if (!dir) return;
    char doomed[8][16];
    int nd = 0;
    for (File e = dir.openNextFile(); e && nd < 8; e = dir.openNextFile()) {
        const char* nm = e.name();
        const char* base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
        const int32_t ymd = ymdFromName(base);
        if (ymd && ymd < cutoff) { strncpy(doomed[nd], base, 15); doomed[nd][15] = '\0'; nd++; }
        e.close();
    }
    dir.close();
    for (int k = 0; k < nd; k++) {
        char path[48];
        snprintf(path, sizeof(path), "%s/%s", kLogDir, doomed[k]);
        SD.remove(path);
        Serial.printf("# log deleted (keep %u days): %s\n", (unsigned)gHlog.keepDays, doomed[k]);
    }
}

// New day: close the old one (summary line), reset the counters, clean up.
static void hlogRollTo(int32_t ymd) {
    hlogFlush();
    if (gDayYmd && gDayYmd != ymd) {
        int off, boots, drops;
        hlogDayTotals(off, boots, drops);
        hlogAppendSummary(gDayYmd, off, boots, drops);
        hlogResetDay();
    }
    gDayYmd = ymd;
    ymdName(ymd, gDayName, sizeof(gDayName));
    hlogCleanup();
    hlogSaveState();
}

// ── writing lines ────────────────────────────────────────────────────────────
// text = "node,id,event,detail" (no time). t = wall time of the line.
static void hlogWriteAt(time_t t, const char* text) {
    const int32_t ymd = ymdOf(t);
    if (ymd != gDayYmd) hlogRollTo(ymd);
    struct tm tm;
    localtime_r(&t, &tm);
    char line[112];
    snprintf(line, sizeof(line), "%02d:%02d:%02d,%s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, text);
    hlogAppendRaw(line);
}

static void hlogLine(const char* node, const char* id, const char* ev, const char* detail) {
    char text[80];
    snprintf(text, sizeof(text), "%s,%s,%s,%s", node, id, ev, detail);
    for (char* p = text + strlen(node) + strlen(id) + strlen(ev) + 3; *p; p++) if (*p == ',') *p = ';';
    Serial.printf("# log %s\n", text);
    if (!gHlog.on) return;
    if (gClockOk) { hlogWriteAt(time(nullptr), text); return; }
    if (gPendN == (int)(sizeof(gPend) / sizeof(gPend[0]))) {   // full: drop the oldest
        memmove(&gPend[0], &gPend[1], sizeof(gPend) - sizeof(gPend[0]));
        gPendN--;
        gLogLost++;
    }
    gPend[gPendN].ms = millis();
    strncpy(gPend[gPendN].text, text, sizeof(gPend[0].text) - 1);
    gPend[gPendN].text[sizeof(gPend[0].text) - 1] = '\0';
    gPendN++;
}

static void hlogSys(const char* fmt, ...) {
    char d[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof(d), fmt, ap);
    va_end(ap);
    hlogLine("CARDPUTER", "-", "SYS", d);
}

static void hlogNode(int i, const char* ev, const char* fmt, ...) {
    char d[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof(d), fmt, ap);
    va_end(ap);
    char id[5];
    nodeIdHex(i, id);
    NodeHealth* h = nhFor(i);
    if (h && strcmp(ev, "HEALTH") != 0) {
        h->lastEvT = gClockOk ? (uint32_t)time(nullptr) : 0;
        snprintf(h->lastEv, sizeof(h->lastEv), "%s %s", ev, d);
    }
    hlogLine(gNodes[i].name, id, ev, d);
}

// ── hooks from zones_core.h ──────────────────────────────────────────────────
// Every N report, after gLive[i] was updated. prevMs = the node's previous report.
static void hlogOnReport(int i, uint32_t prevMs, uint32_t now) {
    NodeHealth* h = nhFor(i);
    if (!h) return;
    const NodeLive& L = gLive[i];
    const uint32_t gap = prevMs ? now - prevMs : 0;
    char dur[16];
    if (!h->seen) {
        h->seen = true;
        h->online = true;
        hlogNode(i, "ONLINE", "first report fw %s", L.fw[0] ? L.fw : "<2.1");
    } else if (!h->online) {
        h->online = true;
        fmtDur(gap, dur, sizeof(dur));
        if (prevMs) hlogNode(i, "ONLINE", "back after %s", dur);
        else        hlogNode(i, "ONLINE", "first report fw %s", L.fw[0] ? L.fw : "<2.1");
    }
    if (!L.hasDiag) return;                            // fw < 2.5: no uptime / drops / boot
    const bool rebooted = h->haveDiag && L.upS + 5 < h->lastUpS;
    if (rebooted) {
        h->bootN++;
        hlogNode(i, "BOOT", "%s: %s", L.boot, bootWhy(L.boot));
    } else if (h->haveDiag && L.drops > h->lastDrops) {
        const uint16_t d = L.drops - h->lastDrops;
        h->dropN += d;
        if (gap >= 2000) { fmtDur(gap, dur, sizeof(dur)); hlogNode(i, "DROP", "Wi-Fi drop %u back %s", (unsigned)L.drops, dur); }
        else              hlogNode(i, "DROP", "Wi-Fi drop %u", (unsigned)L.drops);
    }
    h->haveDiag  = true;
    h->lastUpS   = L.upS;
    h->lastDrops = L.drops;
}

// "H,<mac>,<uptime_s>,<reset_reason>,<prev_cause>,<boots>,<wdt>,<link>,<rejoin>,<heap>,<min_heap>,<ping_hz>,<disc>"
static void hlogOnHealthLine(char* line, uint32_t now) {
    (void)now;
    char* f[13];
    int nf = 0;
    char* save = nullptr;
    for (char* t = strtok_r(line, ",", &save); t && nf < 13; t = strtok_r(nullptr, ",", &save)) f[nf++] = t;
    if (nf < 13) return;
    const int i = findNodeHex(f[1]);
    NodeHealth* h = nhFor(i);
    if (!h) return;
    h->hasH    = true;
    h->heap    = strtoul(f[9], nullptr, 10);
    h->minHeap = strtoul(f[10], nullptr, 10);
    h->pingHz  = (uint16_t)atoi(f[11]);
    h->disc    = (uint8_t)atoi(f[12]);
}

// ── clock ────────────────────────────────────────────────────────────────────
static void hlogApplyTz() {
    setenv("TZ", kTzOpts[gHlog.tz].posix, 1);
    tzset();
}

static void hlogClockReady() {
    gClockOk = true;
    hlogApplyTz();
    const time_t nowT = time(nullptr);
    hlogLoadState(ymdOf(nowT));
    // lines made before the clock was set, at their real time
    const uint32_t nowMs = millis();
    for (int k = 0; k < gPendN; k++) {
        const time_t t = nowT - (time_t)((nowMs - gPend[k].ms) / 1000);
        hlogWriteAt(t, gPend[k].text);
    }
    gPendN = 0;
    hlogSys("clock set (NTP)");
    if (gDayYmd == 0) hlogRollTo(ymdOf(nowT));
    hlogFlush();
}

// ── per-second work, called from serviceNodes() ─────────────────────────────
static uint32_t gWifiUpMs = 0;

static void hlogService(uint32_t now) {
    static uint32_t lastTick = 0, lastState = 0, lastSnapKey = 0;
    static bool     wasWifi = false, everWifi = false;

    if (!gSdOk && now - gSdRetryMs >= 30000) {         // card inserted later / came back
        gSdRetryMs = now;
        if (hlogMount()) { hlogSys("SD card ready %lu MB", (unsigned long)gSdSizeMB); }
    }
    if (wifiReady && !gNtpStarted) {
        configTzTime(kTzOpts[gHlog.tz].posix, "pool.ntp.org", "time.google.com", "time.nist.gov");
        gNtpStarted = true;
    }
    if (!gClockOk && gNtpStarted && time(nullptr) > 1700000000) hlogClockReady();

    if (now - lastTick < 1000) return;
    lastTick = now;

    if (wifiReady != wasWifi) {
        if (wifiReady) { gWifiUpMs = now; if (everWifi) hlogSys("Wi-Fi back %s", wifiIP); everWifi = true; }
        else           hlogSys("Wi-Fi lost");
        wasWifi = wifiReady;
    }

    // node states: offline transitions, online seconds, 15-min bars
    const bool watching = wifiReady && now - gWifiUpMs >= 15000;
    int slot = -1;
    if (gClockOk) {
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        slot = (tm.tm_hour * 60 + tm.tm_min) / 15;
        if (ymdOf(t) != gDayYmd) hlogRollTo(ymdOf(t));
    }
    if (watching) {
        for (int i = 0; i < gNodeCount; i++) {
            NodeHealth* h = nhFor(i);
            if (!h) continue;
            const bool on = nodeOnline(i, now);
            if (!h->seen && now - gWifiUpMs >= 30000) {     // paired, never heard since boot
                h->seen = true; h->online = false; h->offN++;
                hlogNode(i, "OFFLINE", "no report since boot");
            } else if (h->seen && h->online && !on) {
                h->online = false; h->offN++;
                hlogNode(i, "OFFLINE", "no report for %lu s", (unsigned long)(kNodeOfflineMs / 1000));
            }
            if (gClockOk && h->seen) {
                h->obsS++;
                if (on) h->onS++;
                if (slot >= 0) { const uint8_t v = on ? 1 : 2; if (h->slots[slot] < v) h->slots[slot] = v; }
            }
        }
    }

    // HEALTH snapshot of every online node, on the clock (every N minutes)
    if (gClockOk && gHlog.on) {
        const uint32_t key = (uint32_t)(time(nullptr) / 60) / gHlog.snapMin;
        if (lastSnapKey == 0) lastSnapKey = key;
        if (key != lastSnapKey) {
            lastSnapKey = key;
            for (int i = 0; i < gNodeCount; i++) {
                if (!nodeOnline(i, now)) continue;
                const NodeLive& L = gLive[i];
                NodeHealth* h = nhFor(i);
                char up[8] = "?";
                if (L.hasDiag) fmtUp(L.upS, up, sizeof(up));
                if (h && h->hasH) hlogNode(i, "HEALTH", "%s %ddB %u/s %luk", up, L.rssi, (unsigned)L.rate, (unsigned long)(h->heap / 1024));
                else              hlogNode(i, "HEALTH", "%s %ddB %u/s", up, L.rssi, (unsigned)L.rate);
            }
        }
    }

    if (gLogLen && now - gLogFlushMs >= 10000) hlogFlush();
    if (now - lastState >= 60000) { lastState = now; hlogSaveState(); }
}

static void hlogBegin() {
    hlogLoadCfg();
    hlogApplyTz();
    hlogMount();
    gSdRetryMs = millis();
    hlogSys("boot fw %s %s", FW_VERSION, resetCode(esp_reset_reason()));
    if (!gSdOk) Serial.println("# no SD card: node log waits in memory");
}

// Settings changes from LOG SETUP.
static void hlogSetTz(uint8_t tz) {
    gHlog.tz = tz;
    hlogApplyTz();
    if (gClockOk) hlogSys("time zone %s", kTzOpts[tz].name);
}

// ── day list for the LOGS menu and the web page ─────────────────────────────
struct DayInfo { char name[16]; uint32_t size; int32_t ymd; bool hasSum; int off, boots, drops; };

// Fills out[] newest first. Returns the count.
static int hlogListDays(DayInfo* out, int maxN) {
    if (!gSdOk) return 0;
    hlogFlush();
    int n = 0;
    File dir = SD.open(kLogDir);
    if (!dir) return 0;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        const char* nm = e.name();
        const char* base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
        const int32_t ymd = ymdFromName(base);
        if (ymd && !e.isDirectory()) {
            DayInfo d = {};
            strncpy(d.name, base, 15);
            d.size = e.size();
            d.ymd  = ymd;
            int pos = n < maxN ? n : maxN - 1;                       // insertion sort, newest first
            if (n >= maxN && ymd <= out[maxN - 1].ymd) { e.close(); continue; }
            while (pos > 0 && out[pos - 1].ymd < ymd) { out[pos] = out[pos - 1]; pos--; }
            out[pos] = d;
            if (n < maxN) n++;
        }
        e.close();
    }
    dir.close();
    File s = SD.open(kSumPath, FILE_READ);
    if (s) {
        char ln[48];
        while (s.available()) {
            const int l = s.readBytesUntil('\n', ln, sizeof(ln) - 1);
            ln[l] = '\0';
            if (l < 16) continue;
            ln[10] = '\0';
            char nm[16];
            snprintf(nm, sizeof(nm), "%s.csv", ln);
            int off = 0, boots = 0, drops = 0;
            sscanf(ln + 11, "%d,%d,%d", &off, &boots, &drops);
            for (int k = 0; k < n; k++)
                if (!strcmp(out[k].name, nm)) { out[k].hasSum = true; out[k].off = off; out[k].boots = boots; out[k].drops = drops; }
        }
        s.close();
    }
    for (int k = 0; k < n; k++) {
        if (out[k].ymd == gDayYmd) { out[k].hasSum = true; hlogDayTotals(out[k].off, out[k].boots, out[k].drops); }
    }
    return n;
}

// "all ok", "2 off 1 boot", "-" ; level 0 ok, 1 some, 2 bad
static void daySummaryText(const DayInfo& d, char* out, size_t n, int& level) {
    if (!d.hasSum) { snprintf(out, n, "-"); level = -1; return; }
    if (!d.off && !d.boots && !d.drops) { snprintf(out, n, "all ok"); level = 0; return; }
    out[0] = '\0';
    int parts = 0;
    auto add = [&](int v, const char* one, const char* many) {
        if (!v || parts >= 2) return;
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%d %s", parts ? " " : "", v, v == 1 ? one : many);
        parts++;
    };
    add(d.off, "off", "off");
    add(d.boots, "boot", "boots");
    add(d.drops, "drop", "drops");
    level = d.off >= 5 || d.boots >= 3 ? 2 : 1;
}

static void fmtSize(uint32_t b, char* out, size_t n) {
    if (b < 1024)              snprintf(out, n, "%luB", (unsigned long)b);
    else if (b < 1024UL * 1024) snprintf(out, n, "%luK", (unsigned long)((b + 512) / 1024));
    else                       snprintf(out, n, "%.1fM", b / 1048576.0f);
}

// Today's online share of node i, in hundredths of a percent; -1 = not measured yet.
static int hlogAvail100(int i) {
    NodeHealth* h = nhFor(i);
    if (!h || h->obsS == 0) return -1;
    return (int)((uint64_t)h->onS * 10000ULL / h->obsS);
}

#endif  // HEALTH_LOG_H
