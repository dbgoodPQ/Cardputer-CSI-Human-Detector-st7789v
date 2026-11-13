// v2 CSI motion detector — shared by the sensor node firmware (tools/node_esp32)
// and the host parity test (tools/csi_lab/v2_host_test.cpp). Plain C++, no
// Arduino dependencies.
//
// Same algorithm as V2Detector in tools/csi_lab/detectors.py (keep them in step;
// replay.py checks the node's per-packet value against Python on recordings):
//
//   FrameGate (per router CSI packet):
//     frame-kind lock: each second the most common kind (sig_mode|cwb<<2|stbc<<3)
//     becomes the only one used; then at most one packet per 10 ms of radio clock.
//   Detector::addPacket:
//     spread = std / mean of the amplitudes of 12 spread subcarriers
//     (+-3, 7, 11, 15, 19, 23; gain-invariant), causal Hampel (7 values, 3 MAD).
//   Detector::tick (every 250 ms), over the packets of the last 1 s:
//     smoothness = lag-1 autocorrelation of spread (walking = smooth changes,
//     empty room = noise). Motion check: smoothness > cut (0.42 by default);
//     once in contact: > cut - 0.12. Contact after 10 checks in a row (2.5 s),
//     clear after 3. "No CSI" with fewer than 20 packets in the window.
//   prob = logistic(kSlope * (smoothness - cut)), shown as motion level.
//
// Tuned on 24 user recordings (3 rooms, 2 days, 50 pings/s); see docs/V2_ROADMAP.md.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace csiv2 {

// ── frame gate: kind lock + 10 ms slots ───────────────────────────────────────
enum : uint8_t { kKindSkipped = 0, kWeak = 1, kUsed = 3, kSlotSkipped = 4 };

struct FrameGate {
    uint16_t catCnt[32];
    int      lockCat;
    uint32_t catT0;
    uint32_t lastSlot;
    bool     lockChanged;     // set when the lock moves; the caller clears history

    static const uint32_t kSlotUs = 10000;

    void reset(uint32_t now) {
        memset(catCnt, 0, sizeof(catCnt));
        lockCat = -1; catT0 = now; lastSlot = 0xFFFFFFFFu; lockChanged = false;
    }
    // Returns kUsed if the packet may go to the detector, else the skip reason.
    uint8_t admit(uint8_t cat, uint32_t rxUs, uint32_t now) {
        catCnt[cat & 31]++;
        if ((int32_t)(now - catT0) >= 1000) {
            int best = -1; uint16_t bc = 0;
            for (int i = 0; i < 32; i++) if (catCnt[i] > bc) { bc = catCnt[i]; best = i; }
            const uint16_t cur = lockCat >= 0 ? catCnt[lockCat] : 0;
            if (best >= 0 && bc >= 5 && best != lockCat && cur < bc / 2) {
                lockCat = best; lockChanged = true;
            }
            memset(catCnt, 0, sizeof(catCnt));
            catT0 = now;
        }
        if (lockCat >= 0 && cat != lockCat) return kKindSkipped;
        const uint32_t slot = rxUs / kSlotUs;
        if (slot == lastSlot) return kSlotSkipped;
        lastSlot = slot;
        return kUsed;
    }
};

// ── detector ──────────────────────────────────────────────────────────────────
static const int      kSub      = 12;
static const uint8_t  kSubIdx[kSub] = {3, 7, 11, 15, 19, 23, 41, 45, 49, 53, 57, 61};
static const int      kHampelN  = 7;
static const double   kHampelK  = 3.0;
static const uint32_t kWinMs    = 1000;
static const uint32_t kTickMs   = 250;
static const int      kMinPkts  = 20;
static const int      kOnN      = 10;
static const int      kOffN     = 3;
static const double   kSlope    = 20.74;
static const double   kCutDefault = 8.64 / 20.74;   // 0.4166
static const double   kHyst     = 0.12;
static const int      kHist     = 256;               // > 2 s at 100 packets/s

struct Detector {
    uint32_t hMs[kHist];
    float    hV[kHist];
    int      hHead, hCount;                 // ring: newest at hHead - 1
    double   raw[kHampelN];
    int      rawN, rawIdx;
    double   cut;
    int      onCnt, offCnt;
    bool     present, ready;
    double   smooth, mvs, prob;
    int      nWin;

    void clearHistory() { hHead = hCount = 0; rawN = rawIdx = 0; }
    void reset() {
        clearHistory();
        cut = kCutDefault;
        onCnt = offCnt = 0; present = ready = false;
        smooth = mvs = prob = 0.0; nWin = 0;
    }

    static double median7(const double* v, int n) {
        double s[kHampelN];
        memcpy(s, v, n * sizeof(double));
        for (int i = 1; i < n; i++) {          // insertion sort, n = 7
            double x = s[i]; int j = i - 1;
            while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
            s[j + 1] = x;
        }
        return s[n / 2];
    }

    // One admitted packet (128-byte CSI buffer, classic ESP32 order). Returns
    // kUsed or kWeak; *spreadOut = value stored (after Hampel), 0 if weak.
    uint8_t addPacket(const int8_t* buf, uint32_t ms, float* spreadOut) {
        float amp[kSub];
        double sum = 0.0;
        for (int i = 0; i < kSub; i++) {
            const float a = (float)buf[2 * kSubIdx[i]], b = (float)buf[2 * kSubIdx[i] + 1];
            amp[i] = sqrtf(a * a + b * b);
            sum += amp[i];
        }
        const double mean = sum / kSub;
        if (mean < 0.5) { if (spreadOut) *spreadOut = 0.0f; return kWeak; }
        double sq = 0.0;
        for (int i = 0; i < kSub; i++) { const double d = amp[i] - mean; sq += d * d; }
        double spread = sqrt(sq / kSub) / mean;
        // causal Hampel on the raw values (window = last 7 incl. this one)
        raw[rawIdx] = spread; rawIdx = (rawIdx + 1) % kHampelN;
        if (rawN < kHampelN) rawN++;
        if (rawN == kHampelN) {
            const double med = median7(raw, kHampelN);
            double dev[kHampelN];
            for (int i = 0; i < kHampelN; i++) dev[i] = fabs(raw[i] - med);
            const double mad = 1.4826 * median7(dev, kHampelN);
            if (mad > 0 && fabs(spread - med) > kHampelK * mad) spread = med;
        }
        hMs[hHead] = ms; hV[hHead] = (float)spread;
        hHead = (hHead + 1) % kHist;
        if (hCount < kHist) hCount++;
        if (spreadOut) *spreadOut = (float)spread;
        return kUsed;
    }

    // Every kTickMs. Updates present / prob / smooth / ready.
    void tick(uint32_t now) {
        // packets of the last 1 s, oldest first
        int n = 0;
        while (n < hCount) {
            const int idx = (hHead - 1 - n + kHist) % kHist;
            if ((int32_t)(now - hMs[idx]) >= (int32_t)kWinMs) break;
            n++;
        }
        nWin = n;
        if (n < kMinPkts) {
            ready = false; present = false; onCnt = offCnt = 0;
            smooth = mvs = prob = 0.0;
            return;
        }
        ready = true;
        double mean = 0.0;
        for (int k = 0; k < n; k++) mean += hV[(hHead - n + k + kHist) % kHist];
        mean /= n;
        double den = 0.0, num = 0.0, prev = 0.0;
        for (int k = 0; k < n; k++) {
            const double d = hV[(hHead - n + k + kHist) % kHist] - mean;
            den += d * d;
            if (k > 0) num += prev * d;
            prev = d;
        }
        mvs    = den / n;
        smooth = den > 0 ? num / den : 0.0;
        prob   = 1.0 / (1.0 + exp(-kSlope * (smooth - cut)));
        const double bar = present ? cut - kHyst : cut;
        if (smooth >= bar) {
            onCnt++; offCnt = 0;
            if (onCnt >= kOnN) present = true;
        } else {
            offCnt++; onCnt = 0;
            if (offCnt >= kOffN) present = false;
        }
    }
};

}  // namespace csiv2
