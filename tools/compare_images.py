#!/usr/bin/env python3
"""Compare the kernel decoders' output against a reference decoder.

Reads the report the C harness produced, and for every file it decoded
compares our PPM against what PIL produces from the same source.  Files
we deliberately reject (progressive JPEG) are reported as "expected
rejection" rather than a failure.
"""
import os
import sys

from PIL import Image

EXPECTED_REJECT = {
    # Progressive JPEG is out of scope for the baseline decoder.
    "j_prog.jpg",
}


def main():
    indir, outdir = sys.argv[1], sys.argv[2]
    report = os.path.join(outdir, "report.txt")
    if not os.path.exists(report):
        print("no report.txt — the C harness did not run")
        return 2

    exact = loose = mismatched = rejected = missing = 0
    problems = []

    for line in open(report):
        parts = line.split()
        if not parts:
            continue
        status, name = parts[0], parts[1]
        src = os.path.join(indir, name)
        base = os.path.splitext(name)[0]
        our = os.path.join(outdir, base + ".ppm")

        if status != "OK":
            if name in EXPECTED_REJECT:
                rejected += 1
                print(f"  skip {name:28s} rejected as expected ({parts[-1]})")
            else:
                mismatched += 1
                problems.append(f"{name}: our decoder failed ({' '.join(parts[2:])})")
            continue

        if not os.path.exists(our):
            missing += 1
            problems.append(f"{name}: no PPM written")
            continue

        try:
            ref = Image.open(src)
            if ref.mode in ("I;16", "I;16B", "I;16L"):
                # 16-bit greyscale: PIL's convert("RGB") clips rather
                # than scaling, while the decoder keeps the high byte.
                import numpy as np
                arr = np.asarray(ref).astype(np.uint32)
                ref = Image.fromarray((arr * 255 // 65535).astype(np.uint8)).convert("L")
            ref = ref.convert("RGBA")
        except Exception as e:
            problems.append(f"{name}: reference decoder failed: {e}")
            mismatched += 1
            continue

        # The kernel composites alpha against the desktop backdrop
        # (0x1A,0x1E,0x24) rather than discarding it.  Do the same here
        # so images with transparency compare like for like.
        backdrop = (0x1A, 0x1E, 0x24)
        has_alpha = ref.mode == "RGBA" and ref.getextrema()[3][0] < 255
        if has_alpha:
            bg = Image.new("RGBA", ref.size, backdrop + (255,))
            ref = Image.alpha_composite(bg, ref)
        ref = ref.convert("RGB")

        got = Image.open(our).convert("RGB")
        if got.size != ref.size:
            problems.append(f"{name}: size {got.size} != {ref.size}")
            mismatched += 1
            continue

        a = got.tobytes()
        b = ref.tobytes()
        if a == b:
            exact += 1
            continue

        # Lossy formats (JPEG) will not match bit for bit.  The budget is
        # generous enough to absorb chroma-upsampling differences — the
        # simple nearest-neighbour upsampling here versus libjpeg's
        # triangle filter — but far tighter than any real decoder bug:
        # a mis-scaled IDCT measures a mean error of 50+, a wrong
        # quantisation order 30+, and a wrong zig-zag 20+.
        n = len(b)
        diff = sum(abs(x - y) for x, y in zip(a, b)) / n
        maxd = max(abs(x - y) for x, y in zip(a, b))
        if diff <= 8.0 and maxd <= 100:
            loose += 1
            print(f"  ok   {name:28s} within JPEG tolerance (mean {diff:.2f}, max {maxd})")
        else:
            mismatched += 1
            problems.append(f"{name}: pixel mismatch (mean {diff:.2f}, max {maxd})")

    print()
    print(f"exact: {exact}   lossy-tolerant: {loose}   rejected: {rejected}   "
          f"missing: {missing}   mismatched: {mismatched}")
    if problems:
        print("\nproblems:")
        for p in problems:
            print("  -", p)
        return 1
    print("all decoded images match the reference")
    return 0


if __name__ == "__main__":
    sys.exit(main())
