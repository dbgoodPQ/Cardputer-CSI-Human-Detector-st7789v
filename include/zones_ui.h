// Zones UI — included ONCE from src/main.cpp (RADAR_CSI build only), just before
// setup(). External-screen ZONES view, and the NODES menu (list, one node,
// add node / pairing). Design: claude.ai Design canvas "Sensor Nodes Screens".
#ifndef ZONES_UI_H
#define ZONES_UI_H

#include "zones_core.h"

// ── shared helpers ───────────────────────────────────────────────────────────
static void drawZones();

static void drawExternalView() {
    if (!extReady) return;
    if (gViewMode == 0)      drawTop();
    else if (gViewMode == 1) drawZones();
    else                     drawRaycaster();
}

// Keeps everything running while a blocking menu screen is open.
static void uiBackground(bool sensing) {
    if (sensing) { serviceCsi(); serviceCam(); }
    serviceNodes();
    static uint32_t lastTop = 0;
    uint32_t now = millis();
    if (now - lastTop >= 125) { lastTop = now; drawExternalView(); }
}

// One key press: '\n' enter, '\b' delete, '\t' tab, else the character. 0 = none.
static int uiKey() {
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return 0;
    auto st = M5Cardputer.Keyboard.keysState();
    if (st.enter) return '\n';
    if (st.del)   return '\b';
    if (st.tab)   return '\t';
    if (!st.word.empty()) return (unsigned char)st.word[0];
    return 0;
}

static uint16_t uiCol(uint8_t r, uint8_t g, uint8_t b) { return canvas.color565(r, g, b); }

static void uiFrame(const char* title, const char* footer) {
    const int W = canvas.width(), H = canvas.height();
    const uint16_t cBg = uiCol(4, 0, 8), cBar = uiCol(20, 0, 35);
    canvas.fillSprite(cBg);
    canvas.setTextSize(1);
    canvas.fillRect(0, 0, W, 14, cBar);
    canvas.drawFastHLine(0, 13, W, gColA);
    canvas.setTextColor(gColA, cBar);
    canvas.drawString(title, (W - canvas.textWidth(title)) / 2, 3);
    canvas.fillRect(0, H - 14, W, 14, cBar);
    canvas.drawFastHLine(0, H - 14, W, gColA);
    canvas.setTextColor(uiCol(110, 110, 110), cBar);
    canvas.drawString(footer, (W - canvas.textWidth(footer)) / 2, H - 11);
}

static void uiCenter(const char* t, int y, uint16_t col, int size = 1) {
    canvas.setTextSize(size);
    canvas.setTextColor(col);
    canvas.drawString(t, (canvas.width() - canvas.textWidth(t)) / 2, y);
    canvas.setTextSize(1);
}

static void fmtAge(uint32_t ms, char* out, size_t n) {
    uint32_t s = ms / 1000;
    if (s < 60)        snprintf(out, n, "%lu s", (unsigned long)s);
    else if (s < 3600) snprintf(out, n, "%lu min", (unsigned long)(s / 60));
    else               snprintf(out, n, "%lu h", (unsigned long)(s / 3600));
}

static bool nameCharOk(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-';
}

static void nameEdit(char* name, int c) {
    int l = (int)strlen(name);
    if (c == '\b') { if (l > 0) name[l - 1] = '\0'; return; }
    if (nameCharOk(c) && l < kNodeNameMax) {
        if (c == ' ' && l == 0) return;
        name[l] = (char)toupper(c);
        name[l + 1] = '\0';
    }
}

static void trimName(char* name) {
    int l = (int)strlen(name);
    while (l > 0 && name[l - 1] == ' ') name[--l] = '\0';
}

// Status word + colour for a zone row / list entry.
static const char* zoneStatus(const ZoneView& v, char* buf, size_t n, uint16_t& col) {
    if (!v.online)                       { col = uiCol(255, 80, 80);  return "OFFLINE"; }
    if (strcmp(v.mode, "NO CSI") == 0)   { col = uiCol(255, 80, 80);  return "NO SIGNAL"; }
    if (strcmp(v.mode, "CAL") == 0)      { col = uiCol(255, 140, 0);  return "CAL"; }
    if (strcmp(v.mode, "MOVED") == 0)    { col = uiCol(255, 140, 0);  return "MOVED"; }
    if (v.present) {
        snprintf(buf, n, "MOTION %d%%", (int)lroundf(v.motion * 100));
        col = uiCol(255, 120, 255);
        return buf;
    }
    col = gColB;
    return "CLEAR";
}

// ══════════════════════════════════════════════════════════════════════════════
// External screen: ZONES view
// ══════════════════════════════════════════════════════════════════════════════
static void drawZones() {
    if (!extReady) return;
    const auto& s = radar.state();
    const uint32_t now = millis();
    const int W = topCanvas.width(), H = topCanvas.height();
    const bool linkOk = !radar.stale(750);
    const int  act = activeZone(now);

    const uint16_t cBar    = topCanvas.color565(20, 0, 35);
    const uint16_t cBorder = topCanvas.color565(160, 0, 160);
    const uint16_t cSub    = topCanvas.color565(154, 138, 176);

    topCanvas.fillSprite(TFT_BLACK);
    topCanvas.setTextSize(1);

    // title bar
    topCanvas.fillRect(0, 0, W, 14, cBar);
    topCanvas.drawFastHLine(0, 13, W, cBorder);
    drawBatIcon(topCanvas, 2, 3);
    topCanvas.setTextColor(gColB, cBar);
    topCanvas.drawString("ZONES", 18, 3);
    const char* title = act >= 0 ? "[ CONTACT ]" : "[ CLEAR ]";
    topCanvas.setTextColor(gColA, cBar);
    topCanvas.drawString(title, (W - topCanvas.textWidth(title)) / 2, 3);
    const char* tag = !linkOk ? "NO LINK" : s.mode;
    uint16_t pill = (!linkOk || strcmp(s.mode, "NO CSI") == 0) ? TFT_RED
                  : ((strcmp(s.mode, "CAL") == 0 || strcmp(s.mode, "MOVED") == 0)
                     ? topCanvas.color565(255, 140, 0) : topCanvas.color565(0, 180, 80));
    int tw = topCanvas.textWidth(tag) + 8;
    topCanvas.fillRoundRect(W - tw - 3, 1, tw, 11, 3, pill);
    topCanvas.setTextColor(TFT_WHITE, pill);
    topCanvas.drawString(tag, W - tw + 1, 3);

    // rows
    const int n = zoneCount();
    const int top = 17, avail = 146;
    const int rowH = n <= 4 ? 37 : avail / n;
    const bool tall = rowH >= 30;
    const int  thrX = 10 + (int)(gThreshold * 220);
    for (int z = 0; z < n; z++) {
        ZoneView v = getZone(z, now);
        const int y = top + z * rowH, h = rowH - 2;
        uint16_t bg, border, nameCol;
        if (!v.online)      { bg = topCanvas.color565(12, 12, 12); border = topCanvas.color565(58, 58, 58); nameCol = topCanvas.color565(154, 154, 154); }
        else if (v.present) { bg = topCanvas.color565(60, 0, 60);  border = gColA;                         nameCol = topCanvas.color565(255, 120, 255); }
        else                { bg = topCanvas.color565(0, 0, 25);   border = topCanvas.color565(70, 0, 70); nameCol = gColB; }
        topCanvas.fillRoundRect(4, y, W - 8, h, 4, bg);
        topCanvas.drawRoundRect(4, y, W - 8, h, 4, border);

        char vb[16];
        uint16_t vcol;
        const char* val = zoneStatus(v, vb, sizeof(vb), vcol);
        topCanvas.setTextColor(nameCol);
        topCanvas.drawString(v.name, 10, y + (tall ? 5 : 3));

        if (tall) {
            char sub[32], age[12];
            if (z == 0)                         snprintf(sub, sizeof(sub), "this Cardputer %ddBm %u/s", v.rssi, (unsigned)v.rate);
            else if (!v.online && gLive[z-1].lastMs) { fmtAge(now - gLive[z-1].lastMs, age, sizeof(age)); snprintf(sub, sizeof(sub), "no report for %s", age); }
            else if (!v.online)                 snprintf(sub, sizeof(sub), "never reported");
            else if (!v.present && v.lastMotionMs) { fmtAge(now - v.lastMotionMs, age, sizeof(age)); snprintf(sub, sizeof(sub), "last motion %s ago", age); }
            else                                snprintf(sub, sizeof(sub), "%ddBm  %u/s", v.rssi, (unsigned)v.rate);
            topCanvas.setTextColor(v.online ? cSub : topCanvas.color565(138, 138, 138));
            topCanvas.drawString(sub, 10, y + 16);
            if (v.online && v.present) {
                char pct[8];
                snprintf(pct, sizeof(pct), "%d%%", (int)lroundf(v.motion * 100));
                topCanvas.setTextSize(2);
                topCanvas.setTextColor(vcol);
                int pw = topCanvas.textWidth(pct);
                topCanvas.drawString(pct, W - 10 - pw, y + 7);
                topCanvas.setTextSize(1);
                topCanvas.drawString("MOTION", W - 10 - pw - 4 - topCanvas.textWidth("MOTION"), y + 11);
            } else {
                topCanvas.setTextColor(vcol);
                topCanvas.drawString(val, W - 10 - topCanvas.textWidth(val), y + 11);
            }
        } else {
            topCanvas.setTextColor(vcol);
            topCanvas.drawString(val, W - 10 - topCanvas.textWidth(val), y + 3);
        }

        // motion bar + threshold tick
        const int by = y + h - 6;
        topCanvas.fillRect(10, by, 220, 3, v.online ? topCanvas.color565(42, 0, 51) : topCanvas.color565(30, 30, 30));
        if (v.online) {
            int bw = (int)(v.motion * 220);
            if (bw > 0) topCanvas.fillRect(10, by, bw, 3, v.present ? gColA : topCanvas.color565(0, 160, 200));
            topCanvas.drawFastVLine(thrX, by - 2, 7, v.present ? TFT_WHITE : topCanvas.color565(180, 0, 180));
        }
    }

    if (gNodeCount == 0) {
        topCanvas.setTextColor(topCanvas.color565(138, 138, 138));
        const char* l1 = "No sensor nodes yet";
        const char* l2 = "Add one: ` > NODES > ADD NODE";
        topCanvas.drawString(l1, (W - topCanvas.textWidth(l1)) / 2, 90);
        topCanvas.drawString(l2, (W - topCanvas.textWidth(l2)) / 2, 104);
    }

    // footer
    char thr[16];
    snprintf(thr, sizeof(thr), "thr %d%%", (int)lroundf(gThreshold * 100));
    topCanvas.setTextColor(gColB);
    topCanvas.drawString(thr, 4, H - 10);
    const char* hint = "[.] view  [c] cal all";
    topCanvas.setTextColor(topCanvas.color565(154, 106, 168));
    topCanvas.drawString(hint, W - topCanvas.textWidth(hint) - 4, H - 10);
    topCanvas.drawRect(0, 0, W, H, cBorder);

    applyExtBrightness();
    topCanvas.pushRotateZoom(160, 120, 0.0f, EXT_ZOOM, EXT_ZOOM);
}

// ══════════════════════════════════════════════════════════════════════════════
// NODES menu
// ══════════════════════════════════════════════════════════════════════════════
// Name entry for RENAME. Returns false if cancelled.
static bool runNameInput(const char* title, char* name) {
    char buf[kNodeNameMax + 1];
    strncpy(buf, name, kNodeNameMax);
    buf[kNodeNameMax] = '\0';
    uint32_t lastDraw = 0, msgUntil = 0;
    const char* msg = nullptr;
    for (;;) {
        uiBackground(true);
        int c = uiKey();
        if (c == '`') return false;
        if (c == '\n') {
            trimName(buf);
            if (buf[0]) { strcpy(name, buf); return true; }
            msg = "Type a name first"; msgUntil = millis() + 2000;
        } else if (c) {
            nameEdit(buf, c);
        }
        uint32_t now = millis();
        if (now - lastDraw < 50) { delay(5); continue; }
        lastDraw = now;
        uiFrame(title, "enter:save  del:erase  `:cancel");
        const int W = canvas.width();
        canvas.setTextColor(uiCol(138, 138, 138));
        canvas.drawString("Name:", 8, 40);
        canvas.fillRoundRect(8, 50, W - 16, 22, 3, TFT_BLACK);
        canvas.drawRoundRect(8, 50, W - 16, 22, 3, gColA);
        canvas.setTextSize(2);
        canvas.setTextColor(TFT_WHITE);
        canvas.drawString(buf, 14, 54);
        int cx = 14 + canvas.textWidth(buf) + 1;
        canvas.setTextSize(1);
        if ((now / 400) & 1) canvas.fillRect(cx, 54, 2, 15, gColA);
        canvas.setTextColor(uiCol(138, 138, 138));
        canvas.drawString("max 10 letters, A-Z 0-9", 8, 80);
        if (msg && now < msgUntil) uiCenter(msg, 96, uiCol(255, 140, 0));
        canvas.pushSprite(0, 0);
    }
}

// Small confirm / info screen. Returns true on enter.
static bool runConfirm(const char* title, const char* l1, const char* l2, const char* footer) {
    for (;;) {
        uiBackground(true);
        int c = uiKey();
        if (c == '\n') return true;
        if (c == '`')  return false;
        uiFrame(title, footer);
        uiCenter(l1, 46, TFT_WHITE);
        if (l2) uiCenter(l2, 62, uiCol(138, 138, 138));
        canvas.pushSprite(0, 0);
        delay(30);
    }
}

static void runNodeEdit(int idx) {
    int cursor = 0;
    uint32_t lastDraw = 0, msgUntil = 0;
    char msg[40] = "";
    for (;;) {
        if (idx < 0 || idx >= gNodeCount) return;
        uiBackground(true);
        int c = uiKey();
        if (c == '`') return;
        if (c == ';') cursor = (cursor + 2) % 3;
        if (c == '.') cursor = (cursor + 1) % 3;
        if (c == '\n' || c == '/') {
            const uint32_t now = millis();
            if (cursor == 0) {
                char nm[kNodeNameMax + 1];
                strcpy(nm, gNodes[idx].name);
                char t[24]; snprintf(t, sizeof(t), ">> RENAME <<");
                if (runNameInput(t, nm)) { strcpy(gNodes[idx].name, nm); saveNodes(); }
            } else if (cursor == 1) {
                if (nodeOnline(idx, now)) { sendNodeCmdMac("BLINK", idx); snprintf(msg, sizeof(msg), "LED blinks for 5 s"); }
                else snprintf(msg, sizeof(msg), "Offline: can't reach it");
                msgUntil = now + 2500;
            } else {
                char q[32]; snprintf(q, sizeof(q), "Remove %s?", gNodes[idx].name);
                const char* l2 = nodeOnline(idx, now) ? "The board goes back to setup mode"
                                                      : "Offline: hold its BOOT 5 s to reset";
                if (runConfirm(">> REMOVE <<", q, l2, "enter:remove  `:keep")) {
                    sendNodeCmdMac("FORGET", idx);
                    removeNode(idx);
                    return;
                }
            }
        }
        const uint32_t now = millis();
        if (now - lastDraw < 100) { delay(5); continue; }
        lastDraw = now;

        char title[24];
        snprintf(title, sizeof(title), ">> %s <<", gNodes[idx].name);
        uiFrame(title, ";/.:nav  enter:select  `:back");
        ZoneView v = getZone(idx + 1, now);
        const uint16_t cLbl = uiCol(138, 138, 138);
        char line[40];
        canvas.setTextColor(cLbl); canvas.drawString("ID", 8, 18);
        snprintf(line, sizeof(line), "NODE %02X%02X", gNodes[idx].mac[4], gNodes[idx].mac[5]);
        canvas.setTextColor(gColB); canvas.drawString(line, 62, 18);
        canvas.setTextColor(cLbl); canvas.drawString("SIGNAL", 8, 29);
        if (v.online) snprintf(line, sizeof(line), "%ddBm  %u frames/s", v.rssi, (unsigned)v.rate);
        else          snprintf(line, sizeof(line), "-");
        canvas.setTextColor(gColB); canvas.drawString(line, 62, 29);
        canvas.setTextColor(cLbl); canvas.drawString("STATUS", 8, 40);
        const char* st;
        if (!v.online)                         st = gLive[idx].lastMs ? "offline" : "waiting for report";
        else if (strcmp(v.mode, "RUN") == 0)   st = "online, calibrated";
        else if (strcmp(v.mode, "CAL") == 0)   st = "online, calibrating";
        else if (strcmp(v.mode, "NO CSI") == 0) st = "online, no router frames";
        else                                   st = "online, not calibrated";
        canvas.setTextColor(v.online ? gColB : uiCol(255, 80, 80)); canvas.drawString(st, 62, 40);
        canvas.drawFastHLine(4, 52, canvas.width() - 8, uiCol(70, 0, 70));

        const char* opts[3] = { "RENAME", "BLINK LED", "REMOVE" };
        for (int i = 0; i < 3; i++) {
            int y = 56 + i * 19;
            bool sel = i == cursor;
            canvas.setTextColor(sel ? gColA : uiCol(4, 0, 8));
            canvas.drawString(">", 4, y + 5);
            uint16_t col = i == 2 ? uiCol(255, 80, 80) : (sel ? TFT_WHITE : cLbl);
            canvas.setTextColor(col);
            canvas.drawString(opts[i], 16, y + 5);
            if (i == 1) { canvas.setTextColor(cLbl); canvas.drawString("find the board", canvas.width() - 8 - canvas.textWidth("find the board"), y + 5); }
            if (i < 2) canvas.drawFastHLine(8, y + 18, canvas.width() - 16, uiCol(35, 0, 50));
        }
        if (msg[0] && now < msgUntil) {
            canvas.fillRect(0, 104, canvas.width(), 13, uiCol(4, 0, 8));
            uiCenter(msg, 106, uiCol(255, 140, 0));
        }
        canvas.pushSprite(0, 0);
    }
}

static void runAddNode() {
    const int W = canvas.width();
    if (!wifiReady || WiFi.status() != WL_CONNECTED) {
        runConfirm(">> ADD NODE <<", "Connect the Wi-Fi first", "settings > WIFI NET", "`:back");
        return;
    }
    if (!pairStart()) {
        pairStop();
        runConfirm(">> ADD NODE <<", "ESP-NOW failed to start", nullptr, "`:back");
        return;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    struct Found { uint8_t mac[6]; uint32_t lastMs; };
    Found found[4];
    int   nFound = 0, sel = 0;
    char  name[kNodeNameMax + 1] = "";
    bool  nameTouched = false;
    enum { SEARCH, SENDING, JOINING, DONE, NOREPORT, FAILED } st = SEARCH;
    uint32_t tState = millis(), lastSend = 0, lastDraw = 0, msgUntil = 0;
    const char* msg = nullptr;
    uint8_t target[6] = {0};
    int targetIdx = -1;
    char ssid[33];
    strncpy(ssid, WiFi.SSID().c_str(), sizeof(ssid) - 1);
    ssid[32] = '\0';

    auto prefillName = [&]() {
        if (nameTouched || nFound == 0) return;
        int k = findNode(found[sel].mac);
        if (k >= 0) strcpy(name, gNodes[k].name); else name[0] = '\0';
    };

    for (;;) {
        const uint32_t now = millis();
        const bool espNow = (st == SEARCH || st == SENDING || st == FAILED);

        if (espNow) {
            PairEvt e;
            while (popPairEvt(e)) {
                if (e.type == nodeproto::HELLO) {
                    int k = -1;
                    for (int i = 0; i < nFound; i++) if (memcmp(found[i].mac, e.mac, 6) == 0) k = i;
                    if (k < 0 && nFound < 4) { k = nFound++; memcpy(found[k].mac, e.mac, 6); if (nFound == 1) { sel = 0; prefillName(); } }
                    if (k >= 0) found[k].lastMs = now;
                    if (st == SEARCH || memcmp(e.mac, target, 6) == 0) pairSendSeen(e.mac);
                } else if (e.type == nodeproto::PAIR_OK && st == SENDING && memcmp(e.mac, target, 6) == 0) {
                    targetIdx = upsertNode(target, name);
                    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                    pairStop();
                    st = JOINING; tState = now;
                    Serial.printf("# node %s paired\n", name);
                }
            }
            if (st == SEARCH) {   // forget boards that stopped calling
                for (int i = 0; i < nFound; ) {
                    if (now - found[i].lastMs > 5000) {
                        for (int j = i; j < nFound - 1; j++) found[j] = found[j + 1];
                        nFound--;
                        if (sel >= nFound) sel = 0;
                    } else i++;
                }
            }
        }
        if (st == SENDING) {
            if (lastSend == 0 || now - lastSend >= 700) { pairSendCreds(target); lastSend = now; }
            if (now - tState > 6000) { st = FAILED; tState = now; }
        }
        if (st == JOINING) {
            uiBackground(true);
            if (targetIdx >= 0 && gLive[targetIdx].lastMs > tState) { st = DONE; tState = now; }
            else if (now - tState > 45000) { st = NOREPORT; tState = now; }
        } else if (st == DONE || st == NOREPORT) {
            uiBackground(true);
        } else {
            uiBackground(false);
        }

        // keys
        int c = uiKey();
        if (c) {
            if (st == SEARCH) {
                if (c == '`') { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); pairStop(); return; }
                if (c == '\t' && nFound > 1) { sel = (sel + 1) % nFound; prefillName(); }
                else if (c == '\n') {
                    trimName(name);
                    if (nFound == 0) { msg = "No board found yet"; msgUntil = now + 2000; }
                    else if (!name[0]) { msg = "Type a name first"; msgUntil = now + 2000; }
                    else if (findNode(found[sel].mac) < 0 && gNodeCount >= kMaxNodes) { msg = "6 nodes max"; msgUntil = now + 2000; }
                    else { memcpy(target, found[sel].mac, 6); st = SENDING; tState = now; lastSend = 0; }
                } else { nameEdit(name, c); nameTouched = true; }
            } else if (st == SENDING) {
                if (c == '`') st = SEARCH;
            } else if (st == FAILED) {
                if (c == '\n') st = SEARCH;
                else if (c == '`') { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); pairStop(); return; }
            } else if (st == JOINING) {
                if (c == '`') return;                       // node is saved; it shows up when it reports
            } else {                                        // DONE / NOREPORT
                if (c == '`') return;
                if (c == '\n') {
                    if (gNodeCount >= kMaxNodes) return;
                    if (!pairStart()) { pairStop(); return; }
                    esp_wifi_set_ps(WIFI_PS_NONE);
                    nFound = 0; sel = 0; name[0] = '\0'; nameTouched = false; targetIdx = -1;
                    st = SEARCH; tState = now;
                }
            }
        }

        if (now - lastDraw < 60) { delay(3); continue; }
        lastDraw = now;
        char line[48];
        const uint16_t cLbl = uiCol(138, 138, 138), cSub = uiCol(154, 138, 176);
        switch (st) {
        case SEARCH:
            if (nFound == 0) {
                uiFrame(">> ADD NODE <<", "`:cancel");
                uiCenter("Searching for boards...", 34, gColB);
                uiCenter("A new board: LED blinks slowly", 56, cSub);
                uiCenter("A used board: hold BOOT 5 s", 70, cSub);
                uiCenter("Keep it within a few metres", 92, cLbl);
            } else {
                uiFrame(">> ADD NODE <<", "enter:pair  tab:next board  `:cancel");
                canvas.setTextColor(cLbl); canvas.drawString("Found:", 8, 20);
                snprintf(line, sizeof(line), "NODE %02X%02X", found[sel].mac[4], found[sel].mac[5]);
                canvas.setTextColor(gColB); canvas.drawString(line, 50, 20);
                snprintf(line, sizeof(line), "%d waiting", nFound);
                canvas.setTextColor(cLbl); canvas.drawString(line, W - 8 - canvas.textWidth(line), 20);
                int k = findNode(found[sel].mac);
                if (k >= 0) snprintf(line, sizeof(line), "Known board: %s", gNodes[k].name);
                else        snprintf(line, sizeof(line), "Its LED blinks, if the board has one");
                canvas.setTextColor(cSub); canvas.drawString(line, 8, 32);
                canvas.setTextColor(cLbl); canvas.drawString("Name:", 8, 48);
                canvas.fillRoundRect(8, 58, W - 16, 22, 3, TFT_BLACK);
                canvas.drawRoundRect(8, 58, W - 16, 22, 3, gColA);
                canvas.setTextSize(2); canvas.setTextColor(TFT_WHITE);
                canvas.drawString(name, 14, 62);
                int cx = 14 + canvas.textWidth(name) + 1;
                canvas.setTextSize(1);
                if ((now / 400) & 1) canvas.fillRect(cx, 62, 2, 15, gColA);
                canvas.setTextColor(cLbl); canvas.drawString("Joins:", 8, 88);
                canvas.setTextColor(gColB); canvas.drawString(ssid, 50, 88);
                if (msg && now < msgUntil) uiCenter(msg, 104, uiCol(255, 140, 0));
                else { canvas.setTextColor(cLbl); canvas.drawString("max 10 letters, A-Z 0-9", 8, 104); }
            }
            break;
        case SENDING:
            uiFrame(">> ADD NODE <<", "`:back");
            uiCenter("PAIRING", 36, gColA, 2);
            snprintf(line, sizeof(line), "Sending Wi-Fi details to %s", name);
            uiCenter(line, 64, gColB);
            uiCenter("encrypted, over the air", 78, cSub);
            break;
        case FAILED:
            uiFrame(">> ADD NODE <<", "enter:retry  `:cancel");
            uiCenter("NO ANSWER", 36, uiCol(255, 80, 80), 2);
            uiCenter("The board did not confirm", 64, cSub);
            uiCenter("Keep it close and try again", 78, cSub);
            break;
        case JOINING:
            uiFrame(">> ADD NODE <<", "`:done (it appears when ready)");
            uiCenter(name, 30, uiCol(255, 100, 255), 2);
            snprintf(line, sizeof(line), "joining %s...", ssid);
            uiCenter(line, 56, gColB);
            uiCenter("waiting for its first report", 72, cSub);
            break;
        case DONE: {
            uiFrame(">> ADD NODE <<", "enter:add another  `:done");
            uiCenter(name, 22, uiCol(255, 100, 255), 2);
            snprintf(line, sizeof(line), "joined %s", ssid);
            uiCenter(line, 44, gColB);
            canvas.fillRoundRect(W / 2 - 46, 56, 92, 14, 3, uiCol(0, 180, 80));
            uiCenter("ONLINE", 59, TFT_WHITE);
            if (targetIdx >= 0) {
                snprintf(line, sizeof(line), "%ddBm  %u frames/s", gLive[targetIdx].rssi, (unsigned)gLive[targetIdx].rate);
                uiCenter(line, 78, gColB);
            }
            uiCenter("Place it, then press c to calibrate", 96, cSub);
            break;
        }
        case NOREPORT:
            uiFrame(">> ADD NODE <<", "enter:add another  `:done");
            uiCenter(name, 22, uiCol(255, 100, 255), 2);
            uiCenter("Saved, but no report yet", 46, uiCol(255, 140, 0));
            uiCenter("Check it is in Wi-Fi range.", 64, cSub);
            uiCenter("If it can't join in 60 s it", 78, cSub);
            uiCenter("goes back to setup mode.", 90, cSub);
            break;
        }
        canvas.pushSprite(0, 0);
    }
}

static void runNodesMenu() {
    int cursor = 0, scroll = 0;
    uint32_t lastDraw = 0;
    const int rowH = 20, visRows = 5;
    for (;;) {
        uiBackground(true);
        const int items = gNodeCount + (gNodeCount < kMaxNodes ? 1 : 0);
        if (cursor >= items) cursor = items - 1;
        if (cursor < 0) cursor = 0;
        int c = uiKey();
        if (c == '`') return;
        if (c == ';') cursor = (cursor + items - 1) % items;
        if (c == '.') cursor = (cursor + 1) % items;
        if (c == '\n' || c == '/') {
            if (cursor < gNodeCount) runNodeEdit(cursor);
            else runAddNode();
            lastDraw = 0;
            continue;
        }
        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + visRows) scroll = cursor - visRows + 1;

        const uint32_t now = millis();
        if (now - lastDraw < 100) { delay(5); continue; }
        lastDraw = now;
        uiFrame(">> NODES <<", ";/.:nav  enter:open  `:back");
        const int W = canvas.width();
        const uint16_t cLbl = uiCol(138, 138, 138);
        for (int vi = 0; vi < visRows; vi++) {
            int i = scroll + vi;
            if (i >= items) break;
            int y = 16 + vi * rowH;
            bool sel = i == cursor;
            canvas.setTextColor(sel ? gColA : uiCol(4, 0, 8));
            canvas.drawString(">", 4, y + 6);
            if (i < gNodeCount) {
                ZoneView v = getZone(i + 1, now);
                if (!v.online)      canvas.fillCircle(19, y + 9, 3, uiCol(90, 90, 90));
                else if (v.present) canvas.fillCircle(19, y + 9, 3, gColA);
                else                canvas.drawCircle(19, y + 9, 3, gColB);
                canvas.setTextColor(sel ? TFT_WHITE : cLbl);
                canvas.drawString(v.name, 27, y + 6);
                char vb[16];
                uint16_t vcol;
                const char* val = zoneStatus(v, vb, sizeof(vb), vcol);
                if (!v.online && !gLive[i].lastMs) { val = "WAITING"; vcol = cLbl; }
                canvas.setTextColor(vcol);
                canvas.drawString(val, W - 8 - canvas.textWidth(val), y + 6);
            } else {
                canvas.setTextColor(gColB);
                canvas.drawString("+ ADD NODE", 16, y + 6);
                char cnt[12];
                snprintf(cnt, sizeof(cnt), "%d of %d", gNodeCount, kMaxNodes);
                canvas.setTextColor(cLbl);
                canvas.drawString(cnt, W - 8 - canvas.textWidth(cnt), y + 6);
            }
            if (vi < visRows - 1 && i < items - 1) canvas.drawFastHLine(8, y + rowH - 2, W - 16, uiCol(35, 0, 50));
        }
        if (gNodeCount == 0) {
            canvas.setTextColor(uiCol(154, 138, 176));
            canvas.drawString("Flash a board with the node", 16, 50);
            canvas.drawString("firmware, power it, then", 16, 62);
            canvas.drawString("choose + ADD NODE", 16, 74);
        }
        canvas.pushSprite(0, 0);
    }
}

#endif  // ZONES_UI_H
