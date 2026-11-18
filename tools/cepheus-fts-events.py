#!/usr/bin/env python3
"""Decode FTS event lines from a shim log: x, y, type, id, and the FIFO byte.

The shim logs one raw frame per line, either
  fts test: ev t=<ms> fts test:  <8 bytes>
  fts test: drain t=<ms> fts test:  <8 bytes>
Bytes: [0] 0x43 status, else type; [1] id<<4 | action; [2] x low,
[3] (x high nibble) | (y low nibble << 4); [4] y high; [5] ?; [6] area;
[7] FIFO counter.  Coordinates only for 0x13/0x23/0x33 events.
"""
import re
import sys

PAT = re.compile(r"t=(\d+).*?((?:[0-9a-f]{1,2} ){7}[0-9a-f]{1,2})")
COORD = re.compile(r"t=(\d+) .*?((?:[0-9a-f]{1,2} ){7}[0-9a-f]{1,2})")

for path in sys.argv[1:]:
    print(f"== {path}")
    counts = {}
    for line in open(path, errors="replace"):
        m = COORD.search(line)
        if not m or "fts" not in line:
            continue
        t = int(m.group(1))
        b = [int(v, 16) for v in m.group(2).split()]
        if len(b) != 8:
            continue
        if b[0] == 0x43:
            print(f"  t={t:6d} status {b[1]:02x} {b[2]:02x} {b[3]:02x}"
                  f" {b[4]:02x} {b[5]:02x} fifo={b[7]:02x}")
            continue
        x = (b[2] | ((b[3] & 0x0F) << 8))
        y = ((b[4] << 4) | ((b[3] & 0xF0) >> 4))
        kind = {0x13: "report", 0x23: "move", 0x33: "leave"}.get(b[0], f"{b[0]:02x}")
        print(f"  t={t:6d} {kind:7s} id={b[1] >> 4} act={b[1] & 0xF}"
              f" x={x:4d} y={y:4d} area={b[6]:02x} fifo={b[7]:02x}")
        if x > 1000:
            counts["right"] = counts.get("right", 0) + 1
        if y < 250:
            counts["top"] = counts.get("top", 0) + 1
    print(f"  -- right-edge {counts.get('right', 0)}, top-row {counts.get('top', 0)}")
