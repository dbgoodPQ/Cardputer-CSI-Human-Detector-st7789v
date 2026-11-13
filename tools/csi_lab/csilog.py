"""Raw CSI log format shared by record.py and replay.py.

On the wire (node -> PC, UDP): one datagram = LogHdr + count * LogRec, little-endian,
as defined in tools/node_esp32/src/main.cpp (search "raw CSI logging").

On disk (.csil): a text line b"CSILOG 1\\n", one JSON metadata line, then the
LogRec records back to back (152 bytes each). record.py also writes a .json
sidecar with the run summary.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

import numpy as np

MAGIC = b"CSIL"
WIRE_VERSIONS = (1, 2, 3)   # 3 = firmware 2.1+: metric is the v2 spread
FILE_MAGIC = b"CSILOG 1\n"

HDR = struct.Struct("<4sBB6sII")  # magic, ver, count, mac, dseq, drops
HDR2_EXTRA = struct.Struct("<BBBB")  # v2: ping_hz, v1_win, slot_ms, flags

# Node settings for recordings made before wire version 2 (firmware 2.0-dev2).
V1_STREAM = {"ping_hz": 50, "v1_win": 25, "slot_ms": 0, "flags": 0}
REC_DTYPE = np.dtype([
    ("rx_us", "<u4"),     # radio timestamp, us
    ("ms", "<u4"),        # node millis() when processed
    ("metric", "<f4"),    # node's v1 metric after this packet
    ("rssi", "i1"),
    ("noise", "i1"),
    ("rate", "u1"),
    ("sig_mode", "u1"),
    ("mcs", "u1"),
    ("cwb", "u1"),
    ("stbc", "u1"),
    ("channel", "u1"),
    ("status", "u1"),     # 0 kind skipped, 1 weak, 2 window filling, 3 metric
    ("sgi", "u1"),
    ("lock_cat", "u1"),   # locked frame kind, 255 = none
    ("cat", "u1"),        # this packet's frame kind = sig_mode | cwb<<2 | stbc<<3
    ("buf", "i1", (128,)),
])
assert REC_DTYPE.itemsize == 152

STATUS_NAMES = {0: "kind skipped", 1: "weak", 2: "filling", 3: "used", 4: "slot skipped"}
LABELS = ("empty", "walk", "still", "other")


def norm_mac(mac: str) -> str:
    """'aa:bb:cc:dd:ee:ff' / 'AABBCCDDEEFF' -> 'AABBCCDDEEFF'."""
    m = "".join(c for c in mac if c.isalnum()).upper()
    if len(m) != 12 or any(c not in "0123456789ABCDEF" for c in m):
        raise ValueError(f"not a MAC address: {mac!r}")
    return m


def parse_datagram(data: bytes):
    """Returns (mac_hex, dseq, drops, records ndarray, stream dict) or None if not a log datagram.

    stream = node settings: wire_version, ping_hz, v1_win, slot_ms.
    """
    if len(data) < HDR.size:
        return None
    magic, ver, count, mac, dseq, drops = HDR.unpack_from(data)
    if magic != MAGIC or ver not in WIRE_VERSIONS:
        return None
    hsize = HDR.size
    stream = dict(V1_STREAM, wire_version=ver)
    if ver >= 2:
        if len(data) < HDR.size + HDR2_EXTRA.size:
            return None
        ping_hz, v1_win, slot_ms, flags = HDR2_EXTRA.unpack_from(data, HDR.size)
        stream.update(ping_hz=ping_hz, v1_win=v1_win, slot_ms=slot_ms, flags=flags)
        hsize += HDR2_EXTRA.size
    body = data[hsize:]
    if len(body) != count * REC_DTYPE.itemsize:
        return None
    recs = np.frombuffer(body, dtype=REC_DTYPE, count=count)
    return mac.hex().upper(), dseq, drops, recs, stream


class Recording:
    def __init__(self, path: Path, meta: dict, recs: np.ndarray, summary: dict | None):
        self.path = path
        self.meta = meta
        self.recs = recs
        self.summary = summary or {}

    @property
    def stream(self) -> dict:
        """Node settings the recording was made with (older files: firmware 2.0-dev2 values)."""
        return {k: self.meta.get(k, v) for k, v in V1_STREAM.items()}

    @property
    def label(self) -> str:
        return self.meta.get("label", "other")

    @property
    def name(self) -> str:
        return self.path.stem


def write_header(f, meta: dict) -> None:
    f.write(FILE_MAGIC)
    f.write(json.dumps(meta, separators=(",", ":")).encode() + b"\n")


def load(path: str | Path) -> Recording:
    path = Path(path)
    raw = path.read_bytes()
    if not raw.startswith(FILE_MAGIC):
        raise ValueError(f"{path}: not a .csil recording")
    nl = raw.index(b"\n", len(FILE_MAGIC))
    meta = json.loads(raw[len(FILE_MAGIC):nl])
    body = raw[nl + 1:]
    n = len(body) // REC_DTYPE.itemsize          # ignore a torn last record
    recs = np.frombuffer(body[: n * REC_DTYPE.itemsize], dtype=REC_DTYPE)
    side = path.with_suffix(".json")
    summary = json.loads(side.read_text()) if side.exists() else None
    return Recording(path, meta, recs, summary)
