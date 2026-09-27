#!/usr/bin/env python3
"""Verify the kernel's initialised image fits the bootloader's load window.

Usage: check_kernel_fit.py <ld-map> <load_addr> <max_sectors>

The MBR reads ``max_sectors`` sectors from LBA 1 to physical ``load_addr``.
Everything the kernel *initialises* at build time (.text, .rodata, .data)
has to live inside that window; anything past it is left as whatever the
firmware left in RAM — in practice zeros.

That failure mode is silent: the kernel runs, but every statically
initialised variable reads back as 0.  Repaint flags, RNG seeds and
lookup tables all quietly become "empty", so features appear to work
while doing nothing.  This check turns that into a build error.
"""

import re
import sys

# Only these sections are written by the loader.  .bss is *not* included:
# RAM is zero on entry, so leaving it unloaded is correct.
INITIALISED = (".text", ".rodata", ".data")

SECTION_RE = re.compile(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s*$")


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2

    map_path, load_addr, max_sectors = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3])
    limit = load_addr + max_sectors * 512

    ends: dict[str, int] = {}
    with open(map_path) as f:
        for line in f:
            m = SECTION_RE.match(line)
            if not m:
                continue
            name, addr, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
            if name in INITIALISED:
                ends[name] = addr + size

    if ".text" not in ends:
        print("check_kernel_fit: no .text section found in %s" % map_path)
        return 2

    end = max(ends.values())
    used = end - load_addr
    pct = 100.0 * used / (max_sectors * 512)

    print(
        "kernel init: 0x%08x..0x%08x  (%d KiB, %.0f%% of the %d KiB load window)"
        % (load_addr, end, used // 1024, pct, max_sectors // 2)
    )
    for name in INITIALISED:
        if name in ends:
            print("  %-8s ends at 0x%08x" % (name, ends[name]))

    if end > limit:
        print("")
        print("ERROR: kernel initialised data overruns the bootloader load window.")
        print("       Raise KERNEL_BLOCKS in both boot/boot.S and the Makefile")
        print("       (they must stay in sync) until it fits.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
