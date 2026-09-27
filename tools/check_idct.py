#!/usr/bin/env python3
"""Check the kernel's integer IDCT against a float reference.

Run by tools/run_image_test.sh.  A scaling mistake in an IDCT produces
an image that still looks like *something*, so it is worth comparing
block by block against numpy rather than relying on the end-to-end
image tolerance.
"""
import subprocess
import sys

import numpy as np

BIN = sys.argv[1] if len(sys.argv) > 1 else None
if BIN is None:
    print("usage: check_idct.py <path-to-test_idct>")
    sys.exit(2)

out = subprocess.run([BIN], capture_output=True, text=True).stdout
lines = out.splitlines()

R = 4096
C0 = 1.0 / np.sqrt(2.0)
k = np.arange(8)
# M[x][u] = cos((2x+1) * u * pi / 16)   <- note the /16, not /8
M = np.cos(np.pi / 16 * (2 * k[:, None] + 1) * k[None, :])


def reference(block):
    F = np.array(block, dtype=float).reshape(8, 8)
    s = np.full(8, 1.0)
    s[0] = C0
    # Row pass:  G[v][x] = SUM_u C(u) F[v][u] cos(x,u)
    # C(u) weights the columns, so the vector broadcasts along axis 1.
    G = (s[None, :] * F) @ M.T
    # Column pass: H[y][x] = SUM_v C(v) G[v][x] cos(y,v)
    # Here v is contracted out against the *first* axis of G, which
    # means transposing before and after the product.
    A = s[:, None] * G
    H = (A.T @ M.T).T
    return H / 4.0 + 128.0


i = 0
worst = 0.0
worst_case = -1
for case in range(8):
    if not lines[i].startswith("CASE"):
        print("malformed output at line", i)
        sys.exit(2)
    i += 1
    coef = [int(lines[i + k]) for k in range(64)]
    i += 64
    got = np.array([[int(v) for v in lines[i + r].split()] for r in range(8)])
    i += 8

    ref = np.clip(np.rint(reference(coef)), 0, 255)
    err = np.abs(ref - got)
    mean = err.mean()
    if mean > worst:
        worst, worst_case = mean, case
    flag = "ok " if mean <= 1.0 else "BAD"
    print(f"  {flag} case {case}: mean |err| = {mean:.3f}  max = {err.max()}")

print(f"worst mean error {worst:.3f} (case {worst_case})")
if worst > 1.0:
    print("IDCT DOES NOT MATCH THE REFERENCE")
    sys.exit(1)
print("IDCT matches the float reference")
