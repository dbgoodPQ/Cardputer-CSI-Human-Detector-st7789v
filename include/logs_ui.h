// Logs UI — included ONCE from src/main.cpp, after zones_ui.h and web_logs.h.
// Settings > LOGS: day files on the built-in screen, LOG SETUP, and the log
// viewer (file on the external screen, keys on the built-in one). Also the
// external HEALTH view (key . cycles cone, ZONES, HEALTH).
// Design: claude.ai Design canvas "Cardputer Node Health Log Screens".
#ifndef LOGS_UI_H
#define LOGS_UI_H

#include "health_log.h"

// ══════════════════════════════════════════════════════════════════════════════
// External screen: HEALTH view
// ══════════════════════════════════════════════════════════════════════════════
static void drawHealth() {
    if (!extReady) return;
    const uint32_t now = millis();
    const int W = topCanvas.width(), H = topCanvas.height();
    const uint16_t cBar    = topCanvas.color565(20, 0, 35);
    const uint16_t cBorder = topCanvas.color565(160, 0, 160);
    const uint16_t cDim    = topCanvas.color565(154, 138, 176);
    const uint16_t cMut    = topCanvas.color565(110, 110, 120);
    const uint16_t cOk     = topCanvas.color565(0, 210, 80);
    const uint16_t cWarn   = topCanvas.color565(255, 140, 0);
    const uint16_t cBad    = topCanvas.color565(255, 72, 72);
    const uint16_t cEmpty  = topCanvas.color565(30, 8, 36);

    topCanvas.fillSprite(TFT_BLACK);
    topCanvas.setTextSize(1);

    // title bar: battery + HEALTH | [ TODAY hh:mm ] | nodes pill
    topCanvas.fillRect(0, 0, W, 14, cBar);
    topCanvas.drawFastHLine(0, 13, W, cBorder);
    drawBatIcon(topCanvas, 2, 3);
    topCanvas.setTextColor(gColB, cBar);
    topCanvas.drawString("HEALTH", 18, 3);
    char clk[8], title[20];
    clockHHMM(clk, sizeof(clk));
    snprintf(title, sizeof(title), "[ TODAY %s ]", clk);
    topCanvas.setTextColor(gColA, cBar);
    topCanvas.drawString(title, (W - topCanvas.textWidth(title)) / 2, 3);
    char tb[12];
    int  lvl;
    const char* tag = nodesTag(now, tb, sizeof(tb), lvl);
    uint16_t pill = lvl == 2 ? TFT_RED : (lvl == 1 ? cWarn : topCanvas.color565(0, 180, 80));
    int tw = topCanvas.textWidth(tag) + 8;
    topCanvas.fillRoundRect(W - tw - 3, 1, tw, 11, 3, pill);
    topCanvas.setTextColor(TFT_WHITE, pill);
    topCanvas.drawString(tag, W - tw + 1, 3);

    // where "now" is on the 24 h bars
    int nowSlot = 96;
    if (gClockOk) {
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        nowSlot = (tm.tm_hour * 60 + tm.tm_min) / 15;
    }

    const int n = gNodeCount;
    const int top = 17, avail = 146;
    const int rowH = n <= 3 ? 48 : avail / n;
    for (int i = 0; i < n; i++) {
        const int y = top + i * rowH, h = rowH - 3;
        const bool on = nodeOnline(i, now);
        NodeHealth* nh = nhFor(i);
        const NodeLive& L = gLive[i];
        const int a = hlogAvail100(i);
        const bool bad = !on || (a >= 0 && a < 9990);
        topCanvas.fillRoundRect(4, y, W - 8, h, 4, topCanvas.color565(7, 0, 12));
        topCanvas.drawRoundRect(4, y, W - 8, h, 4, bad ? cWarn : topCanvas.color565(58, 16, 64));

        // line 1: name + state | online % today
        topCanvas.setTextColor(TFT_WHITE);
        topCanvas.drawString(gNodes[i].name, 10, y + 4);
        topCanvas.setTextColor(on ? cOk : cBad);
        topCanvas.drawString(on ? "ONLINE" : "OFFLINE", 10 + topCanvas.textWidth(gNodes[i].name) + 6, y + 4);
        char pct[12];
        if (a < 0) snprintf(pct, sizeof(pct), "--");
        else       snprintf(pct, sizeof(pct), "%d.%02d%%", a / 100, a % 100);
        topCanvas.setTextColor(a < 0 ? cMut : (bad ? cWarn : cOk));
        topCanvas.drawString(pct, W - 10 - topCanvas.textWidth(pct), y + 4);

        // line 2: uptime, drops and reboots today
        char up[8] = "-", l2[48];
        if (on && L.hasDiag) fmtUp(L.upS, up, sizeof(up));
        const unsigned drops = nh ? nh->dropN : 0, boots = nh ? nh->bootN : 0;
        if (boots && L.hasDiag) snprintf(l2, sizeof(l2), "up %s  drop %u  boot %u %s", up, drops, boots, L.boot);
        else                    snprintf(l2, sizeof(l2), "up %s  drop %u  boot %u", up, drops, boots);
        topCanvas.setTextColor(drops || boots ? cWarn : cDim);
        topCanvas.drawString(l2, 10, y + 15);

        // 24 h bar, 15-min slots: green ok, red was offline, dark = no data / later
        const int by = rowH >= 40 ? y + 26 : y + h - 7, bh = rowH >= 40 ? 5 : 3;
        for (int s = 0; s < 96; s++) {
            const int x0 = 10 + s * 220 / 96, x1 = 10 + (s + 1) * 220 / 96;
            const uint8_t v = nh ? nh->slots[s] : 0;
            uint16_t col = v == 2 ? cBad : (v == 1 ? cOk : cEmpty);
            if (s > nowSlot) col = cEmpty;
            topCanvas.fillRect(x0, by, x1 - x0, bh, col);
        }
        if (nowSlot < 96) topCanvas.drawFastVLine(10 + (nowSlot + 1) * 220 / 96, by - 1, bh + 2, TFT_WHITE);

        // line 4: last event (tall rows only)
        if (rowH >= 44) {
            char l4[48];
            if (nh && nh->lastEv[0]) {
                char t[8] = "";
                if (nh->lastEvT) {
                    time_t et = nh->lastEvT;
                    struct tm tm;
                    localtime_r(&et, &tm);
                    snprintf(t, sizeof(t), "%02d:%02d ", tm.tm_hour, tm.tm_min);
                }
                snprintf(l4, sizeof(l4), "%s%s", t, nh->lastEv);
            } else {
                snprintf(l4, sizeof(l4), "no event today");
            }
            l4[37] = '\0';
            topCanvas.setTextColor(cMut);
            topCanvas.drawString(l4, 10, y + 34);
        }
    }

    if (n == 0) {
        topCanvas.setTextColor(cMut);
        const char* l1 = "No sensor nodes yet";
        const char* l2 = "Add one: ` > NODES > ADD NODE";
        topCanvas.drawString(l1, (W - topCanvas.textWidth(l1)) / 2, 90);
        topCanvas.drawString(l2, (W - topCanvas.textWidth(l2)) / 2, 104);
    }

    // footer: log state | keys
    char foot[32];
    uint16_t fcol = gColB;
    if (!gSdOk)              { snprintf(foot, sizeof(foot), "NO SD: not logging"); fcol = cBad; }
    else if (!gHlog.on)      { snprintf(foot, sizeof(foot), "logging OFF"); fcol = cWarn; }
    else if (!gClockOk)      { snprintf(foot, sizeof(foot), "waiting for clock"); fcol = cWarn; }
    else                     snprintf(foot, sizeof(foot), "log %.10s", gDayName);
    topCanvas.setTextColor(fcol);
    topCanvas.drawString(foot, 4, H - 10);
    const char* hint = "[.] view  [n] cone";
    topCanvas.setTextColor(topCanvas.color565(154, 106, 168));
    topCanvas.drawString(hint, W - topCanvas.textWidth(hint) - 4, H - 10);
    topCanvas.drawRect(0, 0, W, H, cBorder);

    applyExtBrightness();
    topCanvas.pushRotateZoom(160, 120, 0.0f, EXT_ZOOM, EXT_ZOOM);
}

// ══════════════════════════════════════════════════════════════════════════════
// Log viewer
// ══════════════════════════════════════════════════════════════════════════════
// Index of one day file: offset of every 16th line (ALL) and of every event
// line (EVENTS, everything but HEALTH). Grows as today's file grows.
struct LogIdx {
    char      path[48];
    uint32_t  lines, endOff;
    uint32_t* sparse; uint32_t sparseN, sparseCap;
    uint32_t* ev;     uint32_t evN, evCap;
    bool      oom;
};

static bool idxPush(uint32_t*& arr, uint32_t& n, uint32_t& cap, uint32_t v) {
    if (n == cap) {
        uint32_t nc = cap ? cap * 2 : 256;
        uint32_t* p = (uint32_t*)realloc(arr, nc * sizeof(uint32_t));
        if (!p) return false;
        arr = p; cap = nc;
    }
    arr[n++] = v;
    return true;
}

// Read buffers, allocated only while the viewer is open (no PSRAM on the StampS3).
static const size_t kViewBufLen = 4096;      // 16 + 13 lines of up to ~110 bytes
static char*        gViewBuf    = nullptr;

static void idxFree(LogIdx& x) {
    free(x.sparse); free(x.ev);
    x.sparse = x.ev = nullptr;
    x.sparseN = x.sparseCap = x.evN = x.evCap = 0;
}

// Reads the part of the file not indexed yet. Only whole lines are indexed.
static void idxScan(LogIdx& x) {
    File f = SD.open(x.path, FILE_READ);
    if (!f) return;
    const uint32_t size = f.size();
    if (size <= x.endOff) { f.close(); return; }
    f.seek(x.endOff);
    if (!gViewBuf) { f.close(); return; }
    uint8_t* buf = (uint8_t*)gViewBuf;
    const size_t bufLen = 512;
    char head[40];
    int  hl = 0;
    uint32_t off = x.endOff, lineStart = x.endOff;
    while (off < size) {
        const int n = f.read(buf, size - off < bufLen ? size - off : bufLen);
        if (n <= 0) break;
        for (int k = 0; k < n; k++) {
            const char c = (char)buf[k];
            if (c == '\n') {
                head[hl] = '\0';
                const bool header = lineStart == 0 && strncmp(head, "time,", 5) == 0;
                if (!header && hl > 0) {
                    bool ok = true;
                    if (x.lines % 16 == 0) ok = idxPush(x.sparse, x.sparseN, x.sparseCap, lineStart);
                    // 4th field (time,node,id,EVENT,...) : everything but HEALTH is an event
                    const char* p = head;
                    for (int cm = 0; cm < 3 && p; cm++) { p = strchr(p, ','); if (p) p++; }
                    if (ok && p && strncmp(p, "HEALTH,", 7) != 0) ok = idxPush(x.ev, x.evN, x.evCap, x.lines);
                    if (!ok) { x.oom = true; f.close(); return; }
                    x.lines++;
                }
                lineStart = off + k + 1;
                x.endOff  = lineStart;
                hl = 0;
            } else if (hl < (int)sizeof(head) - 1 && c != '\r') {
                head[hl++] = c;
            }
        }
        off += n;
    }
    f.close();
}

struct ViewLine { char t[6]; char node[11]; char ev[8]; char d[40]; };

static void splitLine(char* s, ViewLine& v) {
    memset(&v, 0, sizeof(v));
    char* f[5] = { nullptr };
    int nf = 0;
    f[nf++] = s;
    for (char* p = s; *p && nf < 5; p++) if (*p == ',') { *p = '\0'; f[nf++] = p + 1; }
    if (f[0]) { strncpy(v.t, f[0], 5); }
    if (nf > 1) strncpy(v.node, f[1], sizeof(v.node) - 1);
    if (nf > 3) strncpy(v.ev, f[3], sizeof(v.ev) - 1);
    if (nf > 4) strncpy(v.d, f[4], sizeof(v.d) - 1);
}

// Reads `count` lines starting at line `first` (ALL) into out[]. Returns how many.
static int idxReadAll(const LogIdx& x, uint32_t first, ViewLine* out, int count) {
    if (first >= x.lines || !x.sparseN) return 0;
    const uint32_t blk = first / 16;
    if (blk >= x.sparseN) return 0;
    if (!gViewBuf) return 0;
    File f = SD.open(x.path, FILE_READ);
    if (!f) return 0;
    f.seek(x.sparse[blk]);
    char* buf = gViewBuf;
    const int n = f.read((uint8_t*)buf, kViewBufLen - 1);
    f.close();
    if (n <= 0) return 0;
    buf[n] = '\0';
    int skip = first % 16, got = 0;
    char* p = buf;
    while (*p && got < count) {
        char* e = strchr(p, '\n');
        if (!e) break;                      // partial line at the end of the chunk
        *e = '\0';
        if (e > p && e[-1] == '\r') e[-1] = '\0';
        if (skip > 0) skip--;
        else splitLine(p, out[got++]);
        p = e + 1;
    }
    return got;
}

// Line number k of the file -> one ViewLine (EVENTS mode reads them one by one).
static bool idxReadOne(const LogIdx& x, uint32_t lineNo, ViewLine& out) {
    ViewLine tmp[1];
    if (idxReadAll(x, lineNo, tmp, 1) != 1) return false;
    out = tmp[0];
    return true;
}

static uint16_t evColor(const char* ev, const char*& shortName) {
    if (!strcmp(ev, "OFFLINE")) { shortName = "OFF";  return topCanvas.color565(255, 72, 72); }
    if (!strcmp(ev, "ONLINE"))  { shortName = "ON";   return topCanvas.color565(0, 210, 80); }
    if (!strcmp(ev, "BOOT"))    { shortName = "BOOT"; return topCanvas.color565(255, 140, 0); }
    if (!strcmp(ev, "DROP"))    { shortName = "DROP"; return topCanvas.color565(255, 140, 0); }
    if (!strcmp(ev, "SYS"))     { shortName = "SYS";  return gColA; }
    if (!strcmp(ev, "HEALTH"))  { shortName = "HLTH"; return topCanvas.color565(90, 90, 102); }
    shortName = ev;
    return topCanvas.color565(200, 200, 208);
}

static const int kViewRows = 13;

static void drawViewer(const LogIdx& x, const char* dayName, bool live, bool events, uint32_t top) {
    const int W = topCanvas.width(), H = topCanvas.height();
    const uint16_t cBar    = topCanvas.color565(10, 0, 16);
    const uint16_t cBorder = topCanvas.color565(160, 0, 160);
    const uint16_t cMut    = topCanvas.color565(110, 110, 120);
    const uint16_t cTxt    = topCanvas.color565(200, 200, 208);
    const uint16_t cDimTxt = topCanvas.color565(110, 110, 120);
    const uint32_t total   = events ? x.evN : x.lines;

    topCanvas.fillSprite(TFT_BLACK);
    topCanvas.setTextSize(1);
    topCanvas.fillRect(0, 0, W, 14, cBar);
    topCanvas.drawFastHLine(0, 13, W, cBorder);
    char t[24];
    snprintf(t, sizeof(t), "LOG %.10s", dayName);
    topCanvas.setTextColor(gColB, cBar);
    topCanvas.drawString(t, 4, 3);
    const char* mode = events ? "[ EVENTS ]" : "[ ALL ]";
    topCanvas.setTextColor(gColA, cBar);
    topCanvas.drawString(mode, (W - topCanvas.textWidth(mode)) / 2 + 14, 3);
    if (live) {
        topCanvas.setTextColor(topCanvas.color565(0, 210, 80), cBar);
        topCanvas.drawString("LIVE", W - 4 - topCanvas.textWidth("LIVE"), 3);
    }
    topCanvas.setTextColor(cMut);
    topCanvas.drawString("TIME", 2, 16);
    topCanvas.drawString("NODE", 35, 16);
    topCanvas.drawString("EVNT", 80, 16);
    topCanvas.drawString("DETAIL", 107, 16);
    topCanvas.drawFastHLine(0, 25, W, topCanvas.color565(30, 8, 36));

    ViewLine rows[kViewRows];
    int got = 0;
    if (events) {
        for (int k = 0; k < kViewRows && top + k < x.evN; k++)
            if (idxReadOne(x, x.ev[top + k], rows[got])) got++;
    } else {
        got = idxReadAll(x, top, rows, kViewRows);
    }
    for (int k = 0; k < got; k++) {
        const int y = 28 + k * 10;
        const ViewLine& v = rows[k];
        const char* sn = "";
        const uint16_t ec = evColor(v.ev, sn);
        const bool health = !strcmp(v.ev, "HEALTH");
        char node[8];
        strncpy(node, v.node, 7); node[7] = '\0';
        char d[22];
        strncpy(d, v.d, 21); d[21] = '\0';
        topCanvas.setTextColor(cMut);    topCanvas.drawString(v.t, 2, y);
        topCanvas.setTextColor(cTxt);    topCanvas.drawString(node, 35, y);
        topCanvas.setTextColor(ec);      topCanvas.drawString(sn, 80, y);
        topCanvas.setTextColor(health ? cDimTxt : cTxt);
        topCanvas.drawString(d, 107, y);
    }
    if (total == 0) {
        const char* m = events ? "no events in this file" : "this file is empty";
        topCanvas.setTextColor(cMut);
        topCanvas.drawString(m, (W - topCanvas.textWidth(m)) / 2, 80);
    }

    // scroll bar
    const int sy = 27, sh = kViewRows * 10;
    topCanvas.fillRect(W - 4, sy, 2, sh, topCanvas.color565(30, 8, 36));
    if (total > (uint32_t)kViewRows) {
        int th = sh * kViewRows / total; if (th < 6) th = 6;
        const int ty = sy + (int)((uint64_t)(sh - th) * top / (total - kViewRows));
        topCanvas.fillRect(W - 4, ty, 2, th, gColA);
    } else if (total) {
        topCanvas.fillRect(W - 4, sy, 2, sh, gColA);
    }

    // footer
    topCanvas.drawFastHLine(0, 158, W, topCanvas.color565(58, 16, 64));
    char pos[40];
    const uint32_t last = top + (uint32_t)got;
    if (total) snprintf(pos, sizeof(pos), "%s %lu-%lu / %lu", events ? "events" : "lines",
                        (unsigned long)(top + 1), (unsigned long)last, (unsigned long)total);
    else       snprintf(pos, sizeof(pos), "0 lines");
    topCanvas.setTextColor(gColB);
    topCanvas.drawString(pos, 4, 161);
    if (x.oom) { topCanvas.setTextColor(topCanvas.color565(255, 140, 0)); topCanvas.drawString("file too big", W - 4 - topCanvas.textWidth("file too big"), 161); }
    topCanvas.setTextColor(cMut);
    topCanvas.drawString("[;.]line [,/]page [e]filter [`]quit", 4, 171);
    topCanvas.drawRect(0, 0, W, H, cBorder);

    applyExtBrightness();
    topCanvas.pushRotateZoom(160, 120, 0.0f, EXT_ZOOM, EXT_ZOOM);
}

static void drawViewerBottom(const char* dayName, bool live, bool events, uint32_t top, uint32_t total) {
    const int W = canvas.width();
    uiFrame(">> VIEWING LOG <<", "keys work here, file is up top");
    char d[12];
    snprintf(d, sizeof(d), "%.10s", dayName);
    canvas.setTextSize(2);
    canvas.setTextColor(gColB);
    canvas.drawString(d, 8, 19);
    canvas.setTextSize(1);
    if (live) { canvas.setTextColor(uiCol(0, 210, 80)); canvas.drawString("LIVE", W - 8 - canvas.textWidth("LIVE"), 23); }
    char l[40];
    snprintf(l, sizeof(l), "%s %lu / %lu", events ? "event" : "line",
             (unsigned long)(total ? top + 1 : 0), (unsigned long)total);
    canvas.setTextColor(uiCol(168, 168, 176));
    canvas.drawString(l, 8, 38);
    snprintf(l, sizeof(l), "filter: %s", events ? "EVENTS" : "ALL");
    canvas.drawString(l, W - 8 - canvas.textWidth(l), 38);

    const char* keys[6][2] = { { "; .", "line up/dn" }, { ", /", "page up/dn" }, { "e", "events only" },
                               { "t", "top" }, { "b", "bottom" }, { "`", "quit view" } };
    const int gx = 6, gy = 52, cw = (W - 12 - 8) / 3, ch = 30;
    for (int k = 0; k < 6; k++) {
        const int x = gx + (k % 3) * (cw + 4), y = gy + (k / 3) * (ch + 4);
        canvas.drawRoundRect(x, y, cw, ch, 3, k == 5 ? gColA : uiCol(58, 16, 64));
        canvas.setTextColor(k == 5 ? gColA : TFT_WHITE);
        canvas.drawString(keys[k][0], x + (cw - canvas.textWidth(keys[k][0])) / 2, y + 5);
        canvas.setTextColor(uiCol(154, 154, 164));
        canvas.drawString(keys[k][1], x + (cw - canvas.textWidth(keys[k][1])) / 2, y + 18);
    }
    canvas.pushSprite(0, 0);
}

static void runLogViewer(const char* dayName) {
    static LogIdx x;
    memset(&x, 0, sizeof(x));
    snprintf(x.path, sizeof(x.path), "%s/%s", kLogDir, dayName);
    const bool live = ymdFromName(dayName) == gDayYmd;
    if (live) hlogFlush();
    gViewBuf = (char*)malloc(kViewBufLen);
    if (!gViewBuf) { runConfirm(">> VIEWING LOG <<", "Not enough memory", nullptr, "`:back"); return; }

    // indexing a full day takes a moment: say so
    uiFrame(">> VIEWING LOG <<", "");
    uiCenter("Reading the file...", 60, gColB);
    canvas.pushSprite(0, 0);
    idxScan(x);

    bool events = false, follow = live;
    auto total = [&]() -> uint32_t { return events ? x.evN : x.lines; };
    auto maxTop = [&]() -> uint32_t { return total() > (uint32_t)kViewRows ? total() - kViewRows : 0; };
    uint32_t top = live ? maxTop() : 0;

    gExtOwned = true;
    bool dirty = true;
    uint32_t lastScan = millis(), lastBot = 0;
    for (;;) {
        uiBackground();                              // nodes, log, web (external screen is ours)
        const uint32_t now = millis();
        int c = uiKey();
        if (c) {
            const uint32_t mt = maxTop();
            if      (c == '`')             break;
            else if (c == ';')             { if (top > 0) top--; }
            else if (c == '.')             { if (top < mt) top++; }
            else if (c == ',')             top = top > (uint32_t)kViewRows ? top - kViewRows : 0;
            else if (c == '/')             top = top + kViewRows < mt ? top + kViewRows : mt;
            else if (c == 't' || c == 'T') top = 0;
            else if (c == 'b' || c == 'B') top = mt;
            else if (c == 'e' || c == 'E') { events = !events; top = live ? maxTop() : 0; }
            follow = live && top >= maxTop();
            dirty = true;
        }
        if (live && now - lastScan >= 2000) {        // today: pick up new lines
            lastScan = now;
            const uint32_t before = total();
            hlogFlush();
            idxScan(x);
            if (total() != before) { if (follow) top = maxTop(); dirty = true; }
        }
        if (dirty) {
            if (top > maxTop()) top = maxTop();
            drawViewer(x, dayName, live, events, top);
            dirty = false;
            lastBot = 0;
        }
        if (now - lastBot >= 250) { lastBot = now; drawViewerBottom(dayName, live, events, top, total()); }
        delay(5);
    }
    gExtOwned = false;
    idxFree(x);
    free(gViewBuf);
    gViewBuf = nullptr;
}

// ══════════════════════════════════════════════════════════════════════════════
// LOG SETUP
// ══════════════════════════════════════════════════════════════════════════════
static void logSetupAdjust(int row, int dir) {
    auto stepIn = [&](const uint8_t* opts, int n, uint8_t cur) -> uint8_t {
        int k = 0;
        for (int i = 0; i < n; i++) if (opts[i] == cur) k = i;
        k = (k + n + dir) % n;
        return opts[k];
    };
    switch (row) {
        case 0:
            if (gHlog.on) hlogSys("logging off");
            gHlog.on = !gHlog.on;
            if (gHlog.on) hlogSys("logging on");
            hlogFlush();
            break;
        case 1: gHlog.snapMin  = stepIn(kSnapOpts, kNumSnap, gHlog.snapMin); break;
        case 2: gHlog.keepDays = stepIn(kKeepOpts, kNumKeep, gHlog.keepDays); break;
        case 3: hlogSetTz((uint8_t)((gHlog.tz + kNumTz + dir) % kNumTz)); break;
        case 4: gHlog.web = !gHlog.web; break;
    }
}

static void runLogSetup() {
    int cursor = 0;
    uint32_t lastDraw = 0;
    const uint8_t keepBefore = gHlog.keepDays;
    for (;;) {
        uiBackground();
        int c = uiKey();
        if (c == '`') break;
        if (c == ';') cursor = (cursor + 4) % 5;
        if (c == '.') cursor = (cursor + 1) % 5;
        if (c == ',') { logSetupAdjust(cursor, -1); lastDraw = 0; }
        if (c == '/' || c == '\n') { logSetupAdjust(cursor, +1); lastDraw = 0; }
        const uint32_t now = millis();
        if (now - lastDraw < 100) { delay(5); continue; }
        lastDraw = now;

        uiFrame(">> LOG SETUP <<", ";/.:nav  ,//:change  `:save");
        const int W = canvas.width();
        const uint16_t cDim = uiCol(110, 110, 110);
        const char* labels[5] = { "LOGGING", "SNAPSHOT", "KEEP", "TIME ZONE", "WEB ACCESS" };
        for (int i = 0; i < 5; i++) {
            const int y = 16 + i * 21;
            const bool sel = i == cursor;
            canvas.setTextColor(sel ? gColA : uiCol(4, 0, 8));
            canvas.drawString(">", 4, y + 6);
            canvas.setTextColor(sel ? TFT_WHITE : cDim);
            canvas.drawString(labels[i], 16, y + 6);
            char v[32];
            uint16_t vc = sel ? gColB : cDim;
            switch (i) {
                case 0: snprintf(v, sizeof(v), gHlog.on ? "ON" : "OFF");
                        vc = gHlog.on ? uiCol(0, 210, 80) : uiCol(255, 72, 72); break;
                case 1: snprintf(v, sizeof(v), "every %u min", (unsigned)gHlog.snapMin); break;
                case 2: snprintf(v, sizeof(v), gHlog.keepDays == 1 ? "%u day" : "%u days", (unsigned)gHlog.keepDays); break;
                case 3: snprintf(v, sizeof(v), "%s", kTzOpts[gHlog.tz].name); break;
                case 4: if (gHlog.web) snprintf(v, sizeof(v), "ON  %s", wifiReady ? wifiIP : "no Wi-Fi");
                        else           snprintf(v, sizeof(v), "OFF");
                        if (!gHlog.web) vc = sel ? uiCol(255, 140, 0) : cDim;
                        break;
            }
            canvas.setTextColor(vc);
            canvas.drawString(v, W - 8 - canvas.textWidth(v), y + 6);
            if (i < 4) canvas.drawFastHLine(8, y + 19, W - 16, uiCol(35, 0, 50));
        }
        canvas.pushSprite(0, 0);
    }
    hlogSaveCfg();
    if (gHlog.keepDays < keepBefore) hlogCleanup();
}

// ══════════════════════════════════════════════════════════════════════════════
// LOGS: day files
// ══════════════════════════════════════════════════════════════════════════════
static void runLogsMenu() {
    static DayInfo days[32];
    int n = hlogListDays(days, 32);
    int cursor = 0, scroll = 0;
    uint32_t lastDraw = 0, lastList = millis();
    const int rowH = 16, visRows = 5;
    for (;;) {
        uiBackground();
        const uint32_t now = millis();
        int c = uiKey();
        if (c == '`') return;
        if (n && c == ';') cursor = (cursor + n - 1) % n;
        if (n && c == '.') cursor = (cursor + 1) % n;
        if (n && (c == '\n' || c == '/')) {
            runLogViewer(days[cursor].name);
            n = hlogListDays(days, 32); lastList = millis(); lastDraw = 0;
            continue;
        }
        if (c == 's' || c == 'S') {
            runLogSetup();
            n = hlogListDays(days, 32); lastList = millis(); lastDraw = 0;
            continue;
        }
        if (now - lastList >= 10000) { lastList = now; n = hlogListDays(days, 32); }   // sizes grow
        if (cursor >= n) cursor = n ? n - 1 : 0;
        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + visRows) scroll = cursor - visRows + 1;
        if (now - lastDraw < 150) { delay(5); continue; }
        lastDraw = now;

        uiFrame(">> LOGS <<", ";/.:nav enter:view s:setup `:back");
        const int W = canvas.width();
        const uint16_t cBar = uiCol(20, 0, 35), cDim = uiCol(110, 110, 110), cMut = uiCol(80, 80, 88);
        // title bar corners: card | clock
        char sd[12], clk[8];
        if (gSdOk) { if (gSdSizeMB >= 1024) snprintf(sd, sizeof(sd), "SD %luG", (unsigned long)((gSdSizeMB + 512) / 1024)); else snprintf(sd, sizeof(sd), "SD %luM", (unsigned long)gSdSizeMB); }
        else       snprintf(sd, sizeof(sd), "NO SD");
        clockHHMM(clk, sizeof(clk));
        canvas.setTextColor(gSdOk ? cDim : uiCol(255, 72, 72), cBar);
        canvas.drawString(sd, 4, 3);
        canvas.setTextColor(cDim, cBar);
        canvas.drawString(clk, W - 4 - canvas.textWidth(clk), 3);

        if (!gSdOk) {
            uiCenter("NO SD CARD", 44, uiCol(255, 72, 72), 2);
            uiCenter("Insert a microSD card (FAT32)", 70, uiCol(154, 138, 176));
            uiCenter("Logs wait in memory meanwhile", 84, cDim);
            canvas.pushSprite(0, 0);
            continue;
        }
        canvas.setTextColor(cMut);
        canvas.drawString("DATE", 16, 17);
        canvas.drawString("EVENTS  SIZE", W - 8 - canvas.textWidth("EVENTS  SIZE"), 17);
        canvas.drawFastHLine(8, 26, W - 16, uiCol(35, 0, 50));
        if (n == 0) {
            uiCenter(gClockOk ? "No log files yet" : "Waiting for the clock (NTP)", 60, cDim);
            if (!gClockOk) uiCenter("needs the internet over Wi-Fi", 74, cMut);
        }
        for (int vi = 0; vi < visRows; vi++) {
            const int i = scroll + vi;
            if (i >= n) break;
            const DayInfo& d = days[i];
            const int y = 28 + vi * rowH;
            const bool sel = i == cursor;
            canvas.setTextColor(sel ? gColA : uiCol(4, 0, 8));
            canvas.drawString(">", 4, y + 4);
            char date[12];
            snprintf(date, sizeof(date), "%.10s", d.name);
            canvas.setTextColor(sel ? TFT_WHITE : uiCol(168, 168, 176));
            canvas.drawString(date, 16, y + 4);
            // LIVE for today, else the weekday
            char tag[6] = "";
            uint16_t tagCol = cMut;
            if (d.ymd == gDayYmd) { strcpy(tag, "LIVE"); tagCol = uiCol(0, 210, 80); }
            else {
                struct tm tm = {};
                tm.tm_year = d.ymd / 10000 - 1900; tm.tm_mon = d.ymd / 100 % 100 - 1; tm.tm_mday = d.ymd % 100; tm.tm_hour = 12;
                mktime(&tm);
                static const char* wd[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
                strcpy(tag, wd[tm.tm_wday % 7]);
            }
            canvas.setTextColor(tagCol);
            canvas.drawString(tag, 16 + canvas.textWidth(date) + 6, y + 4);
            char sum[24], sz[10];
            int level;
            daySummaryText(d, sum, sizeof(sum), level);
            fmtSize(d.size, sz, sizeof(sz));
            const int szX = W - 8 - canvas.textWidth(sz);
            canvas.setTextColor(cMut);
            canvas.drawString(sz, szX, y + 4);
            canvas.setTextColor(level < 0 ? cMut : (level == 0 ? uiCol(0, 210, 80) : (level == 1 ? uiCol(255, 140, 0) : uiCol(255, 72, 72))));
            canvas.drawString(sum, szX - 6 - canvas.textWidth(sum), y + 4);
            if (vi < visRows - 1 && i < n - 1) canvas.drawFastHLine(8, y + rowH - 1, W - 16, uiCol(35, 0, 50));
        }
        canvas.pushSprite(0, 0);
    }
}

#endif  // LOGS_UI_H
