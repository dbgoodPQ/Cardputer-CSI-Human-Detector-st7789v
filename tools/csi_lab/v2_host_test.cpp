// Host build of the node's v2 detector (include/csi_v2.h) for parity tests.
// Reads a .csil recording and replays it exactly like replay.py's run():
// packets in order, a tick every 250 ms of node time (first tick 250 ms after
// the first packet), packets with ms <= tick processed before the tick.
//
//   g++ -O2 -std=c++17 -I../../include v2_host_test.cpp -o v2_host_test
//   ./v2_host_test recording.csil > out.txt
//
// Output lines:  P <index> <status> <spread>   (per packet)
//                T <ms since start> <present> <ready> <smooth>   (per tick)
// v2_parity.py runs it and compares with detectors.V2Detector.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "csi_v2.h"

#pragma pack(push, 1)
struct LogRec {
    uint32_t rx_us, ms; float metric;
    int8_t rssi, noise; uint8_t rate, sig_mode, mcs, cwb, stbc, channel, status, sgi, lock_cat, cat;
    int8_t buf[128];
};
#pragma pack(pop)
static_assert(sizeof(LogRec) == 152, "LogRec");

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.csil\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    int nl = 0, c;
    while (nl < 2 && (c = fgetc(f)) != EOF) if (c == '\n') nl++;   // magic + JSON lines
    std::vector<LogRec> r;
    LogRec x;
    while (fread(&x, sizeof(x), 1, f) == 1) r.push_back(x);
    fclose(f);
    if (r.empty()) return 0;

    csiv2::FrameGate gate; csiv2::Detector det;
    const uint32_t t0 = r[0].ms;
    gate.reset(t0); det.reset();
    uint32_t tick = t0 + csiv2::kTickMs;
    const uint32_t end = r.back().ms;
    size_t i = 0;
    while ((int32_t)(tick - end) <= (int32_t)csiv2::kTickMs) {
        while (i < r.size() && (int32_t)(r[i].ms - tick) <= 0) {
            float spread = 0.0f;
            uint8_t st = gate.admit(r[i].cat, r[i].rx_us, r[i].ms);
            if (gate.lockChanged) { gate.lockChanged = false; det.clearHistory(); }
            if (st == csiv2::kUsed) st = det.addPacket(r[i].buf, r[i].ms, &spread);
            printf("P %zu %u %.9g\n", i, st, spread);
            i++;
        }
        det.tick(tick);
        printf("T %u %d %d %.9g\n", tick - t0, det.present ? 1 : 0, det.ready ? 1 : 0, det.smooth);
        tick += csiv2::kTickMs;
    }
    return 0;
}
