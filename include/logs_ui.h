// Logs UI — included ONCE from src/main.cpp, after zones_ui.h and web_logs.h.
// Settings > LOGS: day files on the built-in screen (information only: date,
// event summary, size; the files are read on a computer, see web_logs.h) and
// LOG SETUP. Also the external HEALTH view (key . cycles cone, ZONES, HEALTH).
// No on-device log viewer: indexing a day file costs RAM the StampS3 lacks.
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
    const int W = tg.width(), H = tg.height();
    const uint16_t cBar    = tg.color565(20, 0, 35);
    const uint16_t cBorder = tg.color565(160, 0, 160);
    const uint16_t cDim    = tg.color565(154, 138, 176);
    const uint16_t cMut    = tg.color565(110, 110, 120);
    const uint16_t cOk     = tg.color565(0, 210, 80);
    const uint16_t cWarn   = tg.color565(255, 140, 0);
    const uint16_t cBad    = tg.color565(255, 72, 72);
    const uint16_t cEmpty  = tg.color565(30, 8, 36);

    topFrameStart();                    // clears to black
    tg.setTextSize(1);

    // title bar: battery + HEALTH | [ TODAY hh:mm ] | nodes pill
    tg.fillRect(0, 0, W, 14, cBar);
    tg.drawFastHLine(0, 13, W, cBorder);
    drawBatIcon(tg, 2, 3);
    tg.setTextColor(gColB, cBar);
    tg.drawString("HEALTH", 18, 3);
    char clk[8], title[20];
    clockHHMM(clk, sizeof(clk));
    snprintf(title, sizeof(title), "[ TODAY %s ]", clk);
    tg.setTextColor(gColA, cBar);
    tg.drawString(title, (W - tg.textWidth(title)) / 2, 3);
    char tb[12];
    int  lvl;
    const char* tag = nodesTag(now, tb, sizeof(tb), lvl);
    uint16_t pill = lvl == 2 ? TFT_RED : (lvl == 1 ? cWarn : tg.color565(0, 180, 80));
    int tw = tg.textWidth(tag) + 8;
    tg.fillRoundRect(W - tw - 3, 1, tw, 11, 3, pill);
    tg.setTextColor(TFT_WHITE, pill);
    tg.drawString(tag, W - tw + 1, 3);

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
        tg.fillRoundRect(4, y, W - 8, h, 4, tg.color565(7, 0, 12));
        tg.drawRoundRect(4, y, W - 8, h, 4, bad ? cWarn : tg.color565(58, 16, 64));

        // line 1: name + state | online % today
        tg.setTextColor(TFT_WHITE);
        tg.drawString(gNodes[i].name, 10, y + 4);
        tg.setTextColor(on ? cOk : cBad);
        tg.drawString(on ? "ONLINE" : "OFFLINE", 10 + tg.textWidth(gNodes[i].name) + 6, y + 4);
        char pct[12];
        if (a < 0) snprintf(pct, sizeof(pct), "--");
        else       snprintf(pct, sizeof(pct), "%d.%02d%%", a / 100, a % 100);
        tg.setTextColor(a < 0 ? cMut : (bad ? cWarn : cOk));
        tg.drawString(pct, W - 10 - tg.textWidth(pct), y + 4);

        // line 2: uptime, drops and reboots today
        char up[8] = "-", l2[48];
        if (on && L.hasDiag) fmtUp(L.upS, up, sizeof(up));
        const unsigned drops = nh ? nh->dropN : 0, boots = nh ? nh->bootN : 0;
        if (boots && L.hasDiag) snprintf(l2, sizeof(l2), "up %s  drop %u  boot %u %s", up, drops, boots, L.boot);
        else                    snprintf(l2, sizeof(l2), "up %s  drop %u  boot %u", up, drops, boots);
        tg.setTextColor(drops || boots ? cWarn : cDim);
        tg.drawString(l2, 10, y + 15);

        // 24 h bar, 15-min slots: green ok, red was offline, dark = no data / later
        const int by = rowH >= 40 ? y + 26 : y + h - 7, bh = rowH >= 40 ? 5 : 3;
        for (int s = 0; s < 96; s++) {
            const int x0 = 10 + s * 220 / 96, x1 = 10 + (s + 1) * 220 / 96;
            const uint8_t v = nh ? nh->slots[s] : 0;
            uint16_t col = v == 2 ? cBad : (v == 1 ? cOk : cEmpty);
            if (s > nowSlot) col = cEmpty;
            tg.fillRect(x0, by, x1 - x0, bh, col);
        }
        if (nowSlot < 96) tg.drawFastVLine(10 + (nowSlot + 1) * 220 / 96, by - 1, bh + 2, TFT_WHITE);

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
            tg.setTextColor(cMut);
            tg.drawString(l4, 10, y + 34);
        }
    }

    if (n == 0) {
        tg.setTextColor(cMut);
        const char* l1 = "No sensor nodes yet";
        const char* l2 = "Add one: ` > NODES > ADD NODE";
        tg.drawString(l1, (W - tg.textWidth(l1)) / 2, 90);
        tg.drawString(l2, (W - tg.textWidth(l2)) / 2, 104);
    }

    // footer: log state | keys
    char foot[32];
    uint16_t fcol = gColB;
    if (!gSdOk)              { snprintf(foot, sizeof(foot), "NO SD: not logging"); fcol = cBad; }
    else if (!gHlog.on)      { snprintf(foot, sizeof(foot), "logging OFF"); fcol = cWarn; }
    else if (!gClockOk)      { snprintf(foot, sizeof(foot), "clock NOT set: no log"); fcol = cWarn; }
    else                     snprintf(foot, sizeof(foot), "log %.10s", gDayName);
    tg.setTextColor(fcol);
    tg.drawString(foot, 4, H - 10);
    const char* hint = "[.] view  [n] cone";
    tg.setTextColor(tg.color565(154, 106, 168));
    tg.drawString(hint, W - tg.textWidth(hint) - 4, H - 10);
    tg.drawRect(0, 0, W, H, cBorder);

    topFramePresent();
}

// ══════════════════════════════════════════════════════════════════════════════
// LOG SETUP > CLOCK: set the date and time by hand (network without internet)
// ══════════════════════════════════════════════════════════════════════════════
static bool validDate(int y, int m, int d, int hh, int mm) {
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (y < 2024 || y > 2099 || m < 1 || m > 12 || hh > 23 || mm > 59 || d < 1) return false;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d <= dim[m - 1] + (m == 2 && leap ? 1 : 0);
}

// Digits YYYYMMDDHHMM edited in place; cursor starts on the hour (after a
// power-off the date is usually right, the time is not).
static void runClockSet() {
    time_t base = gClockOk ? time(nullptr) : hlogLastKnown();
    if (base < 1700000000) base = 1767268800;          // 2026-01-01 12:00 UTC
    struct tm tm;
    localtime_r(&base, &tm);
    char d[13];
    snprintf(d, sizeof(d), "%04d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    int cur = 8;
    uint32_t lastDraw = 0, msgUntil = 0;
    const char* msg = nullptr;
    for (;;) {
        uiBackground();
        const int c = uiKey();
        if (c == '`') return;
        if (c == ',' || c == '\b') { if (cur > 0) cur--; }
        else if (c == '/') { if (cur < 11) cur++; }
        else if (c >= '0' && c <= '9') { d[cur] = (char)c; if (cur < 11) cur++; }
        else if (c == '\n') {
            auto num = [&](int at, int len) { int v = 0; for (int k = 0; k < len; k++) v = v * 10 + (d[at + k] - '0'); return v; };
            const int y = num(0, 4), mo = num(4, 2), dy = num(6, 2), hh = num(8, 2), mi = num(10, 2);
            if (validDate(y, mo, dy, hh, mi) && hlogSetClock(y, mo, dy, hh, mi)) return;
            msg = "Not a valid date / time"; msgUntil = millis() + 2500;
        }
        const uint32_t now = millis();
        if (now - lastDraw < 60) { delay(5); continue; }
        lastDraw = now;

        uiFrame(">> SET CLOCK <<", ",/:move 0-9:type enter:save `:back");
        const int W = canvas.width();
        const uint16_t cDim = uiCol(138, 138, 138);
        char l1[40];
        snprintf(l1, sizeof(l1), "Date and time, 24 h, %s", kTzOpts[gHlog.tz].name);
        canvas.setTextColor(cDim);
        canvas.drawString(l1, 8, 20);
        // "YYYY-MM-DD HH:MM" at text size 2 (12 px per char)
        static const int8_t pos[12] = { 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15 };
        char shown[17];
        snprintf(shown, sizeof(shown), "%.4s-%.2s-%.2s %.2s:%.2s", d, d + 4, d + 6, d + 8, d + 10);
        const int fx = (W - 16 * 12) / 2, fy = 40;
        canvas.fillRoundRect(fx - 6, fy - 5, 16 * 12 + 12, 26, 3, TFT_BLACK);
        canvas.drawRoundRect(fx - 6, fy - 5, 16 * 12 + 12, 26, 3, gColA);
        canvas.setTextSize(2);
        for (int k = 0; k < 16; k++) {
            const bool isCur = pos[cur] == k;
            if (isCur && (now / 400) % 2 == 0) canvas.fillRect(fx + k * 12 - 1, fy - 2, 12, 20, gColA);
            canvas.setTextColor(isCur ? TFT_WHITE : (shown[k] == '-' || shown[k] == ':' ? cDim : TFT_WHITE));
            char ch[2] = { shown[k], 0 };
            canvas.drawString(ch, fx + k * 12, fy);
        }
        canvas.setTextSize(1);
        canvas.setTextColor(cDim);
        canvas.drawString(gClockOk ? "Clock is set: correct it if needed" : "Clock NOT set: logging waits", 8, 74);
        canvas.drawString("Kept over restarts, not power-off", 8, 86);
        if (msg && now < msgUntil) uiCenter(msg, 103, uiCol(255, 140, 0));
        else { canvas.setTextColor(uiCol(80, 80, 88)); canvas.drawString("Automatic: LOG SETUP > TIME SERVER", 8, 103); }
        canvas.pushSprite(0, 0);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// LOG SETUP > TIME SERVER: IP of an NTP server on the LAN (empty = AUTO, internet)
// ══════════════════════════════════════════════════════════════════════════════
static void runTimeServerSet() {
    char buf[16];
    strncpy(buf, gHlog.ntp, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    uint32_t lastDraw = 0, msgUntil = 0;
    const char* msg = nullptr;
    for (;;) {
        uiBackground();
        const int c = uiKey();
        if (c == '`') return;
        int l = (int)strlen(buf);
        if (c == '\b') { if (l > 0) buf[l - 1] = '\0'; }
        else if (((c >= '0' && c <= '9') || c == '.') && l < 15) { buf[l] = (char)c; buf[l + 1] = '\0'; }
        else if (c == '\n') {
            if (!buf[0] || ipv4Ok(buf)) { hlogSetNtp(buf); hlogSaveCfg(); return; }
            msg = "Not an IP address (e.g. 192.168.1.10)"; msgUntil = millis() + 2500;
        }
        const uint32_t now = millis();
        if (now - lastDraw < 50) { delay(5); continue; }
        lastDraw = now;

        uiFrame(">> TIME SERVER <<", "0-9 . :type  enter:save  `:back");
        const int W = canvas.width();
        const uint16_t cDim = uiCol(138, 138, 138);
        canvas.setTextColor(cDim);
        canvas.drawString("IP of the time server on your network", 8, 20);
        canvas.fillRoundRect(8, 32, W - 16, 22, 3, TFT_BLACK);
        canvas.drawRoundRect(8, 32, W - 16, 22, 3, gColA);
        canvas.setTextSize(2);
        canvas.setTextColor(buf[0] ? TFT_WHITE : uiCol(80, 80, 88));
        canvas.drawString(buf[0] ? buf : "AUTO", 14, 36);
        const int cx = 14 + (buf[0] ? canvas.textWidth(buf) : 0) + 1;
        canvas.setTextSize(1);
        if ((now / 400) & 1) canvas.fillRect(cx, 36, 2, 15, gColA);
        canvas.setTextColor(cDim);
        canvas.drawString("Empty = AUTO (internet time servers)", 8, 62);
        canvas.drawString("Asked at start, then every hour", 8, 74);
        char st[24], l3[48];
        int lvl;
        hlogNtpStatus(st, sizeof(st), lvl);
        snprintf(l3, sizeof(l3), "Now: %s %s", gHlog.ntp[0] ? gHlog.ntp : "AUTO", st);
        if (msg && now < msgUntil) uiCenter(msg, 96, uiCol(255, 140, 0));
        else {
            canvas.setTextColor(lvl == 0 ? uiCol(0, 210, 80) : (lvl == 2 ? uiCol(255, 140, 0) : cDim));
            canvas.drawString(l3, 8, 96);
        }
        canvas.pushSprite(0, 0);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// LOG SETUP
// ══════════════════════════════════════════════════════════════════════════════
static const int kSetupRows = 7;   // CLOCK, TIME SERVER, LOGGING, SNAPSHOT, KEEP, TIME ZONE, WEB ACCESS

static void logSetupAdjust(int row, int dir) {
    auto stepIn = [&](const uint8_t* opts, int n, uint8_t cur) -> uint8_t {
        int k = 0;
        for (int i = 0; i < n; i++) if (opts[i] == cur) k = i;
        k = (k + n + dir) % n;
        return opts[k];
    };
    switch (row) {
        case 0: runClockSet(); break;
        case 1: runTimeServerSet(); break;
        case 2:
            if (gHlog.on) hlogSys("logging off");
            gHlog.on = !gHlog.on;
            if (gHlog.on) hlogSys("logging on");
            hlogFlush();
            break;
        case 3: gHlog.snapMin  = stepIn(kSnapOpts, kNumSnap, gHlog.snapMin); break;
        case 4: gHlog.keepDays = stepIn(kKeepOpts, kNumKeep, gHlog.keepDays); break;
        case 5: hlogSetTz((uint8_t)((gHlog.tz + kNumTz + dir) % kNumTz)); break;
        case 6: gHlog.web = !gHlog.web; break;
    }
}

static void runLogSetup() {
    int cursor = gClockOk ? 2 : 0;           // clock not set: start on CLOCK, else LOGGING
    uint32_t lastDraw = 0;
    const uint8_t keepBefore = gHlog.keepDays;
    for (;;) {
        uiBackground();
        int c = uiKey();
        if (c == '`') break;
        if (c == ';') cursor = (cursor + kSetupRows - 1) % kSetupRows;
        if (c == '.') cursor = (cursor + 1) % kSetupRows;
        if (c == ',') { logSetupAdjust(cursor, -1); lastDraw = 0; }
        if (c == '/' || c == '\n') { logSetupAdjust(cursor, +1); lastDraw = 0; }
        const uint32_t now = millis();
        if (now - lastDraw < 100) { delay(5); continue; }
        lastDraw = now;

        uiFrame(">> LOG SETUP <<", ";/.:nav  ,//:change  `:save");
        const int W = canvas.width();
        const uint16_t cDim = uiCol(110, 110, 110);
        const char* labels[kSetupRows] = { "CLOCK", "TIME SERVER", "LOGGING", "SNAPSHOT", "KEEP", "TIME ZONE", "WEB ACCESS" };
        for (int i = 0; i < kSetupRows; i++) {
            const int y = 15 + i * 15;
            const bool sel = i == cursor;
            canvas.setTextColor(sel ? gColA : uiCol(4, 0, 8));
            canvas.drawString(">", 4, y + 4);
            canvas.setTextColor(sel ? TFT_WHITE : cDim);
            canvas.drawString(labels[i], 16, y + 4);
            char v[32];
            uint16_t vc = sel ? gColB : cDim;
            switch (i) {
                case 0:
                    if (gClockOk) {
                        time_t t = time(nullptr);
                        struct tm tm;
                        localtime_r(&t, &tm);
                        snprintf(v, sizeof(v), "%04d-%02d-%02d %02d:%02d >", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
                    } else {
                        snprintf(v, sizeof(v), "NOT SET >");
                        vc = uiCol(255, 140, 0);
                    }
                    break;
                case 1: {
                    char st[16];
                    int lvl;
                    hlogNtpStatus(st, sizeof(st), lvl);
                    static const char* const kWord[3] = { "ok", "wait", "FAIL" };   // row is short
                    snprintf(v, sizeof(v), "%s %s >", gHlog.ntp[0] ? gHlog.ntp : "AUTO", kWord[lvl]);
                    if (lvl == 0) vc = uiCol(0, 210, 80);
                    else if (lvl == 2) vc = uiCol(255, 140, 0);
                    break;
                }
                case 2: snprintf(v, sizeof(v), gHlog.on ? "ON" : "OFF");
                        vc = gHlog.on ? uiCol(0, 210, 80) : uiCol(255, 72, 72); break;
                case 3: snprintf(v, sizeof(v), "every %u min", (unsigned)gHlog.snapMin); break;
                case 4: snprintf(v, sizeof(v), gHlog.keepDays == 1 ? "%u day" : "%u days", (unsigned)gHlog.keepDays); break;
                case 5: snprintf(v, sizeof(v), "%s", kTzOpts[gHlog.tz].name); break;
                case 6: if (gHlog.web) snprintf(v, sizeof(v), "ON  %s", wifiReady ? wifiIP : "no Wi-Fi");
                        else           snprintf(v, sizeof(v), "OFF");
                        if (!gHlog.web) vc = sel ? uiCol(255, 140, 0) : cDim;
                        break;
            }
            canvas.setTextColor(vc);
            canvas.drawString(v, W - 8 - canvas.textWidth(v), y + 5);
            if (i < kSetupRows - 1) canvas.drawFastHLine(8, y + 14, W - 16, uiCol(35, 0, 50));
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
    int scroll = 0;
    uint32_t lastDraw = 0, lastList = millis();
    const int rowH = 16, visRows = 5;
    for (;;) {
        uiBackground();
        const uint32_t now = millis();
        int c = uiKey();
        if (c == '`') return;
        if (c == ';' && scroll > 0) { scroll--; lastDraw = 0; }
        if (c == '.' && scroll + visRows < n) { scroll++; lastDraw = 0; }
        if (c == 's' || c == 'S') {
            runLogSetup();
            n = hlogListDays(days, 32); lastList = millis(); lastDraw = 0;
            continue;
        }
        if (now - lastList >= 10000) { lastList = now; n = hlogListDays(days, 32); }   // sizes grow
        if (scroll > (n > visRows ? n - visRows : 0)) scroll = n > visRows ? n - visRows : 0;
        if (now - lastDraw < 150) { delay(5); continue; }
        lastDraw = now;

        uiFrame(">> LOGS <<", ";/.:scroll  s:setup  `:back");
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
            uiCenter(gClockOk ? "No log files yet" : "Clock not set", 60, gClockOk ? cDim : uiCol(255, 140, 0));
            if (!gClockOk) uiCenter("press s, then CLOCK", 74, cMut);
        }
        for (int vi = 0; vi < visRows; vi++) {
            const int i = scroll + vi;
            if (i >= n) break;
            const DayInfo& d = days[i];
            const int y = 28 + vi * rowH;
            char date[12];
            snprintf(date, sizeof(date), "%.10s", d.name);
            canvas.setTextColor(uiCol(200, 200, 208));
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
