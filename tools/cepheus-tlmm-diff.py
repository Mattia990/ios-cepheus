#!/usr/bin/env python3
"""Diff the entry TLMM dumps of two shim logs.

    tools/cepheus-tlmm-diff.py WARM.txt COLD.txt

Reads lines written by -DSHIM_FTS_TLMM_ALL:

    tlmm[12] N cfg 3 (pull U mux 0 2mA in) io 0

and prints every pad whose cfg or io differs, then a per-tile summary.  Pads
missing from either log are reported separately so a truncated log is not
mistaken for a clean comparison.
"""
import re
import sys

LINE = re.compile(r"tlmm\[(\d+)\]\s+([NSEW])\s+cfg\s+([0-9a-f]+).*?\bio\s+([0-9a-f]+)")


def parse(path):
    pins = {}
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if not m:
                continue
            pin, tile, cfg, io = m.groups()
            # The scrub area is block-granular: if a run's log is shorter
            # than the previous one, stale text follows it.  The new run's
            # lines always come first, so keep the first occurrence.
            pins.setdefault(int(pin), (tile, int(cfg, 16), int(io, 16)))
    return pins


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    warm, cold = parse(sys.argv[1]), parse(sys.argv[2])
    if not warm or not cold:
        sys.exit("no TLMM dump in %s" % (sys.argv[1] if not warm else sys.argv[2]))

    common = sorted(set(warm) & set(cold))
    diffs = [(p, warm[p], cold[p]) for p in common if warm[p][1:] != cold[p][1:]]
    only_warm = sorted(set(warm) - set(cold))
    only_cold = sorted(set(cold) - set(warm))

    for pin, w, c in diffs:
        print("pin %3d %s  warm cfg %08x io %08x   cold cfg %08x io %08x"
              % (pin, w[0], w[1], w[2], c[1], c[2]))
    if only_warm:
        print("only in warm: %s" % only_warm)
    if only_cold:
        print("only in cold: %s" % only_cold)
    print("%d pads compared, %d differ" % (len(common), len(diffs)))

    # A differing cfg is common for pads the other boot has not configured;
    # io differences are the interesting retained-output state.
    io = [p for p, w, c in diffs if w[2] != c[2]]
    print("io differs on: %s" % (io if io else "none"))


if __name__ == "__main__":
    main()
