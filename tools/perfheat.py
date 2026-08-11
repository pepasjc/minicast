#!/usr/bin/env python3
"""
Build a time x SH4-address heat trace from a perf capture of the dynarec.

Where symbolize_perf.py answers "which blocks are hot", this answers "which parts
of guest RAM are hot, and when" - the output is a 2D timeline for tools/perfheat/
(x = wall time, y = SH4 address masked into 32 MB, colour = cycles spent there).

    MINICAST_PERF_MAP=/tmp/jit.map ./minicast.elf ...
    perf record -F 999 -g --tid <tid> ...
    perf script -i perf.data -F time,ip > samples.txt
    tools/perfheat.py samples.txt /tmp/jit.map minicast.elf > heat.txt

`-F time,ip` is the minimum; adding `period` (`-F time,ip,period`) weights each
sample by its period instead of counting it as 1, which is what you want when
recording a cycles event rather than a fixed frequency.

THE WHOLE BLOCK, NOT THE ENTRY POINT
------------------------------------
A sample lands on a *host* pc inside a translated block. That block was compiled
from sh4_code_size bytes of SH4 code, and the time is spent running all of it -
so charging the sample to the block's entry pc alone would pile every block's
cost onto one address and leave the rest of the function looking cold. Each
sample is instead spread uniformly across the block's whole guest range
[pc, pc+size), which is why perf_map.cpp records the span in the symbol.

Spreading conserves total cost: a sample contributes its full weight however many
address buckets it covers. That keeps the map readable as "cycles per region"
rather than "cycles x how big the block happened to be".

OUTPUT FORMAT (text, consumed by tools/perfheat/index.html)
    MINICASTHEAT 1
    M <key> <value>              metadata (t0_ns, mask, samples, jit, ...)
    B <idx> <guest_addr_hex> <guest_size_hex>
    S <t_ns_delta> <block_idx> <weight>    one sample, ascending in time
Block indices are assigned in first-use order; S deltas are from the previous S.
"""

import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symbolize_perf import read_map, sh4_tcb_range   # noqa: E402  (same-dir tool)

# The guest axis: SH4 RAM mirrors repeat every 16 MB inside a 32 MB window, and a
# 32 MB fold puts every alias of a given line on one row.
ADDR_MASK = 0x01FFFFFF

# perf script prints the selected fields space-separated; with -F time,ip the line
# is "  <secs>.<usecs>:  <hex-ip>". Anything else selected (comm/tid/cpu/event)
# still parses because the time field is the one ending in ':' and the ip is the
# first bare hex run after it.
TIME_RE = re.compile(r"(\d+\.\d+):")
HEX_RE = re.compile(r"\b([0-9a-fA-F]{4,})\b")


def parse_sample(line):
    """-> (time_seconds, ip, period) or None."""
    tm = TIME_RE.search(line)
    if not tm:
        return None

    rest = line[tm.end():]
    hm = HEX_RE.search(rest)
    if not hm:
        return None

    ip = int(hm.group(1), 16)

    # an optional period column (-F ...,period) is a DECIMAL run after the ip;
    # treat a bare integer that is not itself the ip as the weight
    period = 1
    tail = rest[hm.end():].split()
    for tok in tail:
        if tok.isdigit():
            period = int(tok)
            break

    return float(tm.group(1)), ip, period


def main():
    argv = sys.argv[1:]
    if len(argv) != 3:
        sys.exit(__doc__)

    samples_path, map_path, elf_path = argv

    blocks = read_map(map_path)
    if not blocks:
        sys.exit("no usable entries in %s" % map_path)

    # split "sh4_<pc>+<size>" into the guest range the block covers. An entry
    # without a span is from an older build: it still places on the axis, but only
    # at its entry instruction (4 bytes), which is the honest fallback.
    guest = []
    for start, size, name in blocks:
        if not name.startswith("sh4_") or name == "sh4_<unmapped>":
            guest.append(None)
            continue
        body = name[4:]
        pc, _, span = body.partition("+")
        try:
            guest.append((int(pc, 16), int(span, 16) if span else 4))
        except ValueError:
            guest.append(None)

    tcb_vaddr, tcb_size = sh4_tcb_range(elf_path)
    # identical rebasing to symbolize_perf.py - see the reasoning there
    runtime_base = blocks[0][0] & ~0xFFF
    shift = runtime_base - ((tcb_vaddr + 0xFFF) & ~0xFFF)
    starts = [b[0] for b in blocks]

    def lookup(ip):
        """host ip (elf offset) -> index into blocks, or None."""
        runtime = ip + shift
        i = bisect.bisect_right(starts, runtime) - 1
        if i < 0:
            return None
        start, size, _ = blocks[i]
        return i if runtime < start + size else None

    total = jit = 0
    t0 = None
    out = []                 # (t_ns, block_idx, weight)
    seen = {}                # blocks index -> emitted index
    table = []               # (guest_addr, guest_size)

    for line in open(samples_path):
        s = parse_sample(line)
        if s is None:
            continue
        tsec, ip, period = s
        total += 1

        bi = lookup(ip)
        if bi is None or guest[bi] is None:
            continue

        jit += 1
        if t0 is None:
            t0 = tsec

        gi = seen.get(bi)
        if gi is None:
            gi = len(table)
            seen[bi] = gi
            addr, span = guest[bi]
            table.append((addr & ADDR_MASK, span))

        out.append((int(round((tsec - t0) * 1e9)), gi, period))

    if not total:
        sys.exit("no samples parsed from %s -- was it produced with "
                 "`perf script -F time,ip`?" % samples_path)
    if not out:
        sys.exit("%d samples parsed but none landed in a jit block -- wrong map "
                 "or wrong binary?" % total)

    out.sort()               # perf usually emits in order; multi-cpu can interleave

    w = sys.stdout.write
    # Which SH4 region the axis should be LABELLED with. The fold deliberately
    # merges aliases of the same physical memory (0x8C.. cached RAM and 0xAC..
    # uncached are one band, which is the point), but a row still has to be shown
    # as something a listing will match - and that is whichever region the code
    # actually ran in, not a hardcoded guess. P1 RAM dominates in practice; BIOS
    # at 0x80../0xA0.. folds onto the same offsets, so it is reported rather than
    # silently mislabelled.
    regions = {}
    for start, size, name in blocks:
        if not name.startswith("sh4_"):
            continue
        try:
            pc = int(name[4:].split("+")[0], 16)
        except ValueError:
            continue
        regions[pc & ~ADDR_MASK] = regions.get(pc & ~ADDR_MASK, 0) + 1
    label_base = max(regions, key=regions.get) if regions else 0x8C000000

    w("MINICASTHEAT 1\n")
    w("M t0_sec %.9f\n" % t0)
    w("M mask %X\n" % ADDR_MASK)
    w("M label_base %X\n" % label_base)
    for base, cnt in sorted(regions.items(), key=lambda kv: -kv[1]):
        w("M region %08X %d\n" % (base, cnt))
    w("M samples %d\n" % total)
    w("M jit %d\n" % jit)
    w("M span_ns %d\n" % (out[-1][0] - out[0][0]))
    w("M source %s\n" % os.path.basename(samples_path))
    for i, (addr, span) in enumerate(table):
        w("B %d %X %X\n" % (i, addr, span))

    prev = 0
    for t, gi, weight in out:
        w("S %d %d %d\n" % (t - prev, gi, weight))
        prev = t

    sys.stderr.write("%d samples, %d in jit (%.1f%%), %d distinct blocks, %.3f s\n"
                     % (total, jit, 100.0 * jit / total, len(table),
                        (out[-1][0] - out[0][0]) / 1e9))


if __name__ == "__main__":
    main()
