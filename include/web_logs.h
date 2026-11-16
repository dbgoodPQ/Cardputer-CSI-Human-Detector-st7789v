// Log web page — included ONCE from src/main.cpp, after health_log.h.
//
// Read-only HTTP server on port 80 (LOG SETUP > WEB ACCESS, on by default), for
// any computer on the home Wi-Fi:
//   /                       status of every node + the day files (view / download)
//   /logs.json              the same as JSON
//   /logs/2026-11-15.csv    download a day file (?view=1 shows it in the browser)
// No password: meant for a private home network. Served from the main loop, so a
// big download pauses the screens for a moment.
#ifndef WEB_LOGS_H
#define WEB_LOGS_H

#include <WebServer.h>

static WebServer gWeb(80);
static bool      gWebOn = false;

static void webChunk(const char* fmt, ...) {
    char b[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    gWeb.sendContent(b);
}

static void webNodeFacts(int i, uint32_t now, char* up, size_t un, char* avail, size_t an) {
    const NodeLive& L = gLive[i];
    if (nodeOnline(i, now) && L.hasDiag) fmtUp(L.upS, up, un); else snprintf(up, un, "-");
    const int a = hlogAvail100(i);
    if (a < 0) snprintf(avail, an, "-");
    else       snprintf(avail, an, "%d.%02d%%", a / 100, a % 100);
}

static void webRoot() {
    const uint32_t now = millis();
    DayInfo days[32];
    const int nd = hlogListDays(days, 32);
    char clk[8];
    clockHHMM(clk, sizeof(clk));
    char tb[12];
    int  lvl;
    const char* tag = nodesTag(now, tb, sizeof(tb), lvl);
    const char* pill = lvl == 2 ? "#e03030" : (lvl == 1 ? "#ff8c00" : "#00a83c");

    gWeb.setContentLength(CONTENT_LENGTH_UNKNOWN);
    gWeb.send(200, "text/html; charset=utf-8", "");
    gWeb.sendContent(
        "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>CSI Radar node logs</title><style>"
        "body{margin:0;background:#040008;color:#c8c8d0;font-family:ui-monospace,Menlo,Consolas,monospace}"
        "main{max-width:1040px;margin:0 auto;padding:2rem 1.25rem;display:flex;flex-direction:column;gap:2rem}"
        "h1{margin:0;font-size:1.6rem;color:#e040e0}h2{margin:0;font-size:1.1rem;color:#22c8d8}"
        "a{color:#22c8d8}a:hover{color:#7ee8f2}.dim{color:#9a9aa4}.mut{color:#6e6e78}"
        ".top{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:baseline;gap:.75rem;border-bottom:2px solid #d020d0;padding-bottom:1rem}"
        ".pill{color:#000;font-weight:700;padding:.3rem .8rem;border-radius:8px}"
        ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:1rem}"
        ".card{border:2px solid #3a1040;border-radius:10px;padding:1rem 1.25rem;background:#0a0010;display:flex;flex-direction:column;gap:.35rem}"
        ".card.bad{border-color:#ff8c00}.big{font-size:1.6rem;font-weight:700}"
        ".box{overflow-x:auto;border:1px solid #3a1040;border-radius:10px}"
        "table{width:100%;border-collapse:collapse;min-width:560px}th{background:#12001a;color:#9a9aa4;font-weight:400;text-align:left}"
        "th,td{padding:.65rem 1rem}tr+tr td{border-top:1px solid #2a0a30}.r{text-align:right}"
        ".ok{color:#00dc50}.warn{color:#ff8c00}.bad{color:#ff4848}"
        "</style></head><body><main>");

    webChunk("<div class='top'><div><h1>CSI Radar &middot; node logs</h1>"
             "<div class='dim'>served by the Cardputer at %s &middot; fw %s &middot; %s%s</div></div>"
             "<span class='pill' style='background:%s'>%s</span></div>",
             wifiIP, FW_VERSION, clk, gClockOk ? "" : " (clock not set yet)", pill, tag);

    gWeb.sendContent("<div class='cards'>");
    for (int i = 0; i < gNodeCount; i++) {
        NodeHealth* h = nhFor(i);
        const bool on = nodeOnline(i, now);
        char up[8], avail[12];
        webNodeFacts(i, now, up, sizeof(up), avail, sizeof(avail));
        const int a = hlogAvail100(i);
        const bool bad = !on || (a >= 0 && a < 9990);
        webChunk("<div class='card%s'><div style='display:flex;justify-content:space-between'>"
                 "<span style='color:#fff'>%s</span><span class='%s'>%s</span></div>"
                 "<span class='big %s'>%s</span>"
                 "<span class='dim'>on today &middot; up %s &middot; drops %u &middot; reboots %u</span>"
                 "<span class='mut'>%s</span></div>",
                 bad ? " bad" : "", gNodes[i].name, on ? "ok" : "bad", on ? "ONLINE" : "OFFLINE",
                 bad ? "warn" : "ok", avail, up, h ? (unsigned)h->dropN : 0u, h ? (unsigned)h->bootN : 0u,
                 h && h->lastEv[0] ? h->lastEv : "no event today");
    }
    if (gNodeCount == 0) gWeb.sendContent("<span class='dim'>No sensor nodes paired yet.</span>");
    gWeb.sendContent("</div>");

    webChunk("<div style='display:flex;flex-direction:column;gap:.75rem'>"
             "<h2>Log files <span class='mut' style='font-weight:400;font-size:.9rem'>%s/ on the SD card &middot; one per day &middot; kept %u days</span></h2>",
             kLogDir, (unsigned)gHlog.keepDays);
    if (!gSdOk) {
        gWeb.sendContent("<span class='bad'>No SD card in the Cardputer.</span>");
    } else {
        gWeb.sendContent("<div class='box'><table><thead><tr><th>File</th><th>Events</th>"
                         "<th class='r'>Size</th><th class='r'>Get it</th></tr></thead><tbody>");
        for (int k = 0; k < nd; k++) {
            char sum[32], sz[12];
            int level;
            daySummaryText(days[k], sum, sizeof(sum), level);
            fmtSize(days[k].size, sz, sizeof(sz));
            webChunk("<tr><td style='color:#fff'>%s %s</td><td class='%s'>%s</td><td class='r dim'>%s</td>"
                     "<td class='r' style='white-space:nowrap'><a href='/logs/%s?view=1'>view</a> &nbsp; "
                     "<a href='/logs/%s'>download</a></td></tr>",
                     days[k].name, days[k].ymd == gDayYmd ? "<span class='ok' style='font-size:.8rem'>&#9679; live</span>" : "",
                     level < 0 ? "mut" : (level == 0 ? "ok" : (level == 1 ? "warn" : "bad")), sum, sz,
                     days[k].name, days[k].name);
        }
        if (nd == 0) gWeb.sendContent("<tr><td colspan='4' class='dim'>No log files yet.</td></tr>");
        gWeb.sendContent("</tbody></table></div>");
    }
    webChunk("<span class='mut'>Read-only. From a terminal: curl -O http://%s/logs/&lt;date&gt;.csv &middot; "
             "list as JSON: <a href='/logs.json'>/logs.json</a></span></div>", wifiIP);
    gWeb.sendContent("</main></body></html>");
    gWeb.sendContent("");
}

static void webJson() {
    const uint32_t now = millis();
    DayInfo days[32];
    const int nd = hlogListDays(days, 32);
    char clk[8];
    clockHHMM(clk, sizeof(clk));
    gWeb.setContentLength(CONTENT_LENGTH_UNKNOWN);
    gWeb.send(200, "application/json", "");
    webChunk("{\"device\":\"CSI Radar Cardputer\",\"fw\":\"%s\",\"ip\":\"%s\",\"time\":\"%s\",\"clock_set\":%s,"
             "\"sd_card\":%s,\"keep_days\":%u,\"nodes\":[",
             FW_VERSION, wifiIP, clk, gClockOk ? "true" : "false", gSdOk ? "true" : "false", (unsigned)gHlog.keepDays);
    for (int i = 0; i < gNodeCount; i++) {
        NodeHealth* h = nhFor(i);
        char id[5];
        nodeIdHex(i, id);
        const NodeLive& L = gLive[i];
        webChunk("%s{\"name\":\"%s\",\"id\":\"%s\",\"online\":%s,\"fw\":\"%s\",\"uptime_s\":%lu,"
                 "\"online_pct_today\":%d,\"offline_today\":%u,\"boots_today\":%u,\"drops_today\":%u,\"last_event\":\"%s\"}",
                 i ? "," : "", gNodes[i].name, id, nodeOnline(i, now) ? "true" : "false", L.fw,
                 (unsigned long)(L.hasDiag ? L.upS : 0), hlogAvail100(i),
                 h ? (unsigned)h->offN : 0u, h ? (unsigned)h->bootN : 0u, h ? (unsigned)h->dropN : 0u,
                 h ? h->lastEv : "");
    }
    gWeb.sendContent("],\"files\":[");
    for (int k = 0; k < nd; k++) {
        webChunk("%s{\"name\":\"%s\",\"size\":%lu,\"live\":%s,\"offline\":%d,\"boots\":%d,\"drops\":%d,\"url\":\"/logs/%s\"}",
                 k ? "," : "", days[k].name, (unsigned long)days[k].size, days[k].ymd == gDayYmd ? "true" : "false",
                 days[k].hasSum ? days[k].off : -1, days[k].hasSum ? days[k].boots : -1,
                 days[k].hasSum ? days[k].drops : -1, days[k].name);
    }
    gWeb.sendContent("]}");
    gWeb.sendContent("");
}

static void webFile() {
    const String uri = gWeb.uri();
    if (!uri.startsWith("/logs/")) { gWeb.send(404, "text/plain", "not found\n"); return; }
    const String name = uri.substring(6);
    if (!ymdFromName(name.c_str())) { gWeb.send(404, "text/plain", "not found\n"); return; }
    if (!gSdOk) { gWeb.send(503, "text/plain", "no SD card\n"); return; }
    if (ymdFromName(name.c_str()) == gDayYmd) hlogFlush();      // today: write what is waiting first
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", kLogDir, name.c_str());
    File f = SD.open(path, FILE_READ);
    if (!f) { gWeb.send(404, "text/plain", "not found\n"); return; }
    if (gWeb.arg("view") == "1") {
        gWeb.streamFile(f, "text/plain; charset=utf-8");
    } else {
        gWeb.sendHeader("Content-Disposition", String("attachment; filename=\"") + name + "\"");
        gWeb.streamFile(f, "text/csv");
    }
    f.close();
}

static void webService() {
    static bool routes = false;
    const bool want = gHlog.web && wifiReady;
    if (want && !gWebOn) {
        if (!routes) {
            gWeb.on("/", HTTP_GET, webRoot);
            gWeb.on("/logs.json", HTTP_GET, webJson);
            gWeb.onNotFound(webFile);
            routes = true;
        }
        gWeb.begin();
        gWebOn = true;
        Serial.printf("# web: http://%s/\n", wifiIP);
    } else if (!want && gWebOn) {
        gWeb.stop();
        gWebOn = false;
    }
    if (gWebOn) gWeb.handleClient();
}

#endif  // WEB_LOGS_H
