# csi_lab: record and replay raw CSI from the sensor nodes

Tools to collect real data from your nodes and test motion detectors on it
offline, before changing the node firmware (v2 roadmap, phases 1 to 3).

- `record.py` asks one node to stream every router CSI packet to your PC and saves
  a labelled recording (`.csil`, about 0.9 MB per minute).
- `replay.py` runs a detector over recordings, scores it and can plot it.
  Today it has `v1`, an exact copy of the current node detector; the new one
  will be added next to it.

Needs node firmware **2.0-dev2 or newer** (raw logging; 2.0-dev switched logging
off right away), Python 3.10+, and a PC on the same Wi-Fi network as the nodes.

To update a node without pairing it again, flash `csi-node-esp32-app.bin` from the
release at **0x10000**. The full `csi-node-esp32.bin` at 0x0 also erases the saved
Wi-Fi details.

## Setup (once, PowerShell)

```powershell
cd tools\csi_lab
python -m pip install -r requirements.txt
```

The first time `record.py` runs, Windows asks whether Python may use the network:
allow it on **private** networks, otherwise nothing is received.

## Record

```powershell
python record.py                      # lists the nodes it hears (MAC, IP, mode)
python record.py --node A1B2C3D4E5F6 --label empty --room kitchen --minutes 5
```

You get a 10 s countdown (`--countdown`) to get in place, then a live line with
packets/s, RSSI and losses. Files go to `recordings\` (not committed to git).

Labels:

| Label | What to do during the recording |
|---|---|
| `empty` | Nobody in the room (pets out too). Close the door if you can. |
| `walk` | Walk around the room at a normal pace the whole time, including between the node and the router. |
| `still` | Sit or stand still in the room (reading, phone). Breathing is fine. |
| `other` | Anything else; use `--note` to describe it. |

Suggested first set, **per room**: 2 x `empty`, 2 x `walk`, 1 x `still`, 5 minutes
each, at different times of day if you can. Keep the node where it normally lives.

If nothing arrives after 8 s, `record.py` prints the likely causes. Open the node's
serial monitor (`python -m pip install pyserial`, then
`python -m serial.tools.miniterm COM5 115200`): it should show
`# raw log: on -> <PC IP>:4213`. With several network adapters, add `--bind <PC IP>`.

Logging doesn't stop the node's normal sensing, and the node stops streaming on
its own about 6 s after `record.py` exits.

## Replay

```powershell
python replay.py "recordings\*.csil"
python replay.py "recordings\kitchen_*.csil" --cal "recordings\kitchen_empty_*.csil" --plot
python replay.py "recordings\*.csil" --threshold 0.25
```

For each file it prints the input quality (packets/s, gaps, frame kinds, RSSI),
a parity check for `v1` (the Python copy against the metric the node logged), and
the score:

- `empty`: false contact, % of the time, and the number of alarms.
- `walk`: detection, % of the time.
- `still`: contact, % of the time.

`--cal` uses empty recordings as the calibrated reference (like pressing `c`),
otherwise the detector runs in AUTO. `--plot` saves a PNG next to each recording.

## File format

See `csilog.py`. The node's wire format is documented in
`tools/node_esp32/src/main.cpp` ("raw CSI logging"). Each record holds the radio
timestamp, the node's processing time, frame details (rate, MCS, bandwidth, STBC,
RSSI, noise floor), the v1 metric and the raw 128 CSI bytes.
