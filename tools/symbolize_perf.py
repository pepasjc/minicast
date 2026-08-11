#!/usr/bin/env python3
"""
Symbolize minicast jit frames in perf output.

perf cannot do this itself: the code cache lives inside SH4_TCB, a static array in
.text of minicast.elf, so perf resolves jit addresses against the binary and never
consults a perf-<pid>.map. This joins `perf script` text output against the map
written by perf_map.cpp instead, which avoids perf's symbolization entirely.

    perf script -i perf.data -F ip,sym,dso > samples.txt
    symbolize_perf.py samples.txt /tmp/jit.map minicast.elf

The map records *runtime* addresses; perf script reports offsets within the binary.
The two differ by a constant, which is recovered from the ELF rather than assumed:
SH4_TCB's vaddr is known from the symbol table, and the map's addresses all fall
inside SH4_TCB, so the lowest map address anchors the runtime base.

With --by-shop the third argument is a shop map (MINICAST_SHOP_MAP) instead of a
perf map, and cost is totalled per shil opcode across every block rather than per
block:

    MINICAST_SHOP_MAP=/tmp/shop.map ./minicast.elf ...
    symbolize_perf.py --by-shop samples.txt /tmp/shop.map minicast.elf
"""

import bisect
import collections
import re
import subprocess
import sys


def read_map(path):
    """Parse '<start-hex> <size-hex> <name>' lines into a sorted list."""
    blocks = []

    with open(path) as f:
        for line in f:
            parts = line.split(None, 2)
            if len(parts) != 3:
                continue
            try:
                start = int(parts[0], 16)
                size = int(parts[1], 16)
            except ValueError:
                continue
            blocks.append((start, size, parts[2].strip()))

    blocks.sort()
    return blocks


def read_shop_map(path):
    """Flatten a shop map's block/op lines into the same sorted block list.

    Each op owns the host code from its own offset up to the next op's offset.
    The two regions that belong to no op get their own synthetic names rather
    than being folded into a neighbouring op: everything before the first op is
    the block prologue (smc check, cycle counter, intc_sched call), and
    everything after the last op is the relink/block-exit tail. Charging the
    tail to whichever shop happened to end the block would inflate exactly the
    branch-ish opcodes (jcond, jdyn) that tend to sit there.

    The emitter records offsets in emission order, but a block whose ops all sit
    at one offset (or a truncated final block from a kill -9) would otherwise
    produce negative sizes, so degenerate entries are dropped rather than
    trusted.
    """
    blocks = []
    pending = []        # (offset, name) for the block being read
    start = size = relink = None

    def flush():
        if not pending:
            return

        if pending[0][0] > 0:
            blocks.append((start, pending[0][0], "<block prologue>"))

        # ops stop at the relink tail, so a branchy final shop is not charged for
        # the block-exit code that follows it
        for j, (off, name) in enumerate(pending):
            end = pending[j + 1][0] if j + 1 < len(pending) else relink
            if end > off:
                blocks.append((start + off, end - off, name))

        if size > relink:
            blocks.append((start + relink, size - relink, "<relink/block exit>"))

    with open(path) as f:
        for line in f:
            parts = line.split()

            if len(parts) == 5 and parts[0] == "block":
                if start is not None:
                    flush()
                try:
                    start = int(parts[1], 16)
                    size = int(parts[2], 16)
                    relink = int(parts[4], 16)
                    # a truncated or nonsensical record must not swallow the block
                    if not 0 < relink <= size:
                        relink = size
                except ValueError:
                    start = None
                pending = []
            elif len(parts) == 3 and parts[0] == "op" and start is not None:
                try:
                    pending.append((int(parts[1], 16), parts[2]))
                except ValueError:
                    pass

    if start is not None:
        flush()

    blocks.sort()
    return blocks


def sh4_tcb_range(elf):
    """(vaddr, size) of SH4_TCB from the ELF symbol table."""
    try:
        out = subprocess.run(["readelf", "-sW", elf], capture_output=True,
                             text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        sys.exit("could not read symbols from %s: %s" % (elf, e))

    for line in out.splitlines():
        parts = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(parts) >= 8 and parts[-1] == "SH4_TCB":
            value = int(parts[1], 16)
            size = int(parts[2], 16) if parts[2].startswith("0x") else int(parts[2])
            return value, size

    sys.exit("SH4_TCB not found in %s -- wrong binary?" % elf)


def main():
    argv = sys.argv[1:]

    by_shop = "--by-shop" in argv
    if by_shop:
        argv.remove("--by-shop")

    if len(argv) != 3:
        sys.exit(__doc__)

    samples_path, map_path, elf_path = argv

    blocks = read_shop_map(map_path) if by_shop else read_map(map_path)
    if not blocks:
        sys.exit("no usable entries in %s" % map_path)

    tcb_vaddr, tcb_size = sh4_tcb_range(elf_path)

    # The map holds runtime addresses; perf reports addresses relative to the elf.
    # For a non-PIE binary these are the same, and measured perf output does land
    # directly inside SH4_TCB's vaddr range -- so the shift is normally zero. It is
    # still computed rather than assumed, so a PIE/ASLR build stays correct: the
    # code cache starts at the page-aligned base of SH4_TCB, which anchors the
    # lowest recorded block to a known elf address.
    runtime_base = blocks[0][0] & ~0xFFF
    shift = runtime_base - ((tcb_vaddr + 0xFFF) & ~0xFFF)

    starts = [b[0] for b in blocks]

    def lookup(addr):
        """Map an elf-offset sample address to a block name, or None."""
        runtime = addr + shift

        i = bisect.bisect_right(starts, runtime) - 1
        if i < 0:
            return None

        start, size, name = blocks[i]
        if runtime < start + size:
            return name

        # inside the cache but not in any block: stale code from before a cache
        # clear, or padding between blocks. In shop mode every byte of a live
        # block is attributed (prologue and relink tail have their own buckets),
        # so anything landing here is genuinely stale.
        if tcb_vaddr <= addr < tcb_vaddr + tcb_size:
            return "<stale/unmapped>" if by_shop else "sh4_<unmapped>"
        return None

    counts = collections.Counter()
    total = 0
    jit = 0

    addr_re = re.compile(r"^\s*([0-9a-fA-F]+)\s")

    for line in open(samples_path):
        m = addr_re.match(line)
        if not m:
            continue

        total += 1
        addr = int(m.group(1), 16)

        name = lookup(addr)
        if name is None:
            continue

        jit += 1
        # Block symbols carry their guest span as "sh4_<pc>+<bytes>" (perfheat.py
        # needs it). Costs are still totalled per entry pc here, so drop the span
        # rather than splitting one block across rows when a recompile changes its
        # length. Shop names never contain '+'.
        if name.startswith("sh4_"):
            name = name.split("+", 1)[0]
        counts[name] += 1

    if not total:
        sys.exit("no sample addresses parsed from %s -- "
                 "was it produced with `perf script -F ip,sym,dso`?" % samples_path)

    print("# %d samples, %d in jit (%.1f%%)" % (total, jit, 100.0 * jit / total))
    print("# SH4_TCB vaddr %08x, runtime base %08x, shift %+#x"
          % (tcb_vaddr, runtime_base, shift))
    print()

    for name, n in counts.most_common():
        print("%8d  %5.2f%%  %s" % (n, 100.0 * n / total, name))


if __name__ == "__main__":
    main()
