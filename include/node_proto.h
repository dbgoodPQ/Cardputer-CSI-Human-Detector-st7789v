#ifndef NODE_PROTO_H
#define NODE_PROTO_H
//
// Sensor-node protocol, shared by the Cardputer (src/main.cpp) and the ESP32
// node firmware (tools/node_esp32).
//
// Pairing (ESP-NOW, only while the Cardputer's "Add node" screen is open):
//   node (setup mode, hopping channels 1-13)  --HELLO (broadcast)-->  Cardputer
//   Cardputer                                  --SEEN  (unicast)  -->  node   (node stops hopping)
//   Cardputer                                  --PAIR  (unicast)  -->  node   (Wi-Fi SSID + password,
//                                                                              AES-128-GCM encrypted)
//   node                                       --PAIR_OK          -->  Cardputer, then reboots and joins Wi-Fi
//
// Running (UDP broadcast on the home Wi-Fi):
//   node -> Cardputer, port 4211, 10 Hz:
//     N,<mac 12 hex>,<seq>,<presence 0|1>,<motion 0.000-1.000>,<rssi>,<frames/s>,<mode>
//     mode = AUTO | CAL | RUN | NO CSI
//   Cardputer -> nodes, port 4212:
//     C,CAL | C,CALSTOP | C,UNCAL | C,THR,<0.05-0.95> | C,BLINK,<mac> | C,FORGET,<mac>
//
// The pairing key is built into both firmwares (override with -DNODE_PAIR_KEY="16 chars",
// the same on both builds). It keeps the Wi-Fi password away from casual listeners, but
// anyone who knows this public key and records the pairing exchange could decrypt it.
//
#include <stdint.h>
#include <string.h>
#include "mbedtls/gcm.h"

namespace nodeproto {

static const uint16_t kReportPort = 4211;   // node -> Cardputer
static const uint16_t kCmdPort    = 4212;   // Cardputer -> nodes
static const uint8_t  kVer        = 1;

enum : uint8_t { HELLO = 1, SEEN = 2, PAIR = 3, PAIR_OK = 4 };

struct __attribute__((packed)) Hdr {
    char    magic[4];   // "CSIN"
    uint8_t type;
    uint8_t ver;
};

struct __attribute__((packed)) Hello {
    Hdr  h;
    char fw[8];
};

struct __attribute__((packed)) PairPlain {
    char ssid[33];
    char pass[65];
};

struct __attribute__((packed)) PairMsg {
    Hdr     h;
    uint8_t nonce[12];
    uint8_t ct[sizeof(PairPlain)];
    uint8_t tag[16];
};

#ifndef NODE_PAIR_KEY
#define NODE_PAIR_KEY "CardputerCSInode"
#endif
static_assert(sizeof(NODE_PAIR_KEY) - 1 == 16, "NODE_PAIR_KEY must be exactly 16 characters");

inline void mkHdr(Hdr& h, uint8_t type) {
    memcpy(h.magic, "CSIN", 4);
    h.type = type;
    h.ver  = kVer;
}

inline bool hdrOk(const uint8_t* d, int len, uint8_t type) {
    if (!d || len < (int)sizeof(Hdr)) return false;
    const Hdr* h = (const Hdr*)d;
    return memcmp(h->magic, "CSIN", 4) == 0 && h->type == type && h->ver == kVer;
}

// aad = node MAC (6 bytes) + Cardputer MAC (6 bytes); caller fills out.nonce randomly.
inline bool sealPair(const PairPlain& in, const uint8_t aad[12], PairMsg& out) {
    mkHdr(out.h, PAIR);
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, (const unsigned char*)NODE_PAIR_KEY, 128);
    if (rc == 0)
        rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, sizeof(PairPlain),
                                       out.nonce, sizeof(out.nonce), aad, 12,
                                       (const unsigned char*)&in, out.ct, sizeof(out.tag), out.tag);
    mbedtls_gcm_free(&ctx);
    return rc == 0;
}

inline bool openPair(const PairMsg& in, const uint8_t aad[12], PairPlain& out) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, (const unsigned char*)NODE_PAIR_KEY, 128);
    if (rc == 0)
        rc = mbedtls_gcm_auth_decrypt(&ctx, sizeof(PairPlain), in.nonce, sizeof(in.nonce), aad, 12,
                                      in.tag, sizeof(in.tag), in.ct, (unsigned char*)&out);
    mbedtls_gcm_free(&ctx);
    if (rc != 0) return false;
    out.ssid[sizeof(out.ssid) - 1] = '\0';
    out.pass[sizeof(out.pass) - 1] = '\0';
    return true;
}

inline void macHex(const uint8_t mac[6], char out[13]) {
    static const char* hx = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) { out[2*i] = hx[mac[i] >> 4]; out[2*i + 1] = hx[mac[i] & 15]; }
    out[12] = '\0';
}

}  // namespace nodeproto

#endif  // NODE_PROTO_H
