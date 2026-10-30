#!/usr/bin/env python3
"""Summarise a PS2_CAPTURE draws.tsv: draw batches grouped by render target and state, in order.

usage: draws_summary.py draws.tsv [--tail N]
Prints runs of consecutive batches with the same frame/zbuf/test/prim-type, with their box and Z range.
"""
import re
import sys


def main():
    rows = []
    for line in open(sys.argv[1]):
        f = line.rstrip("\n").split("\t")
        if f[0] in ("draw", "image") or len(f) < 14:
            continue
        rows.append(f)
    runs = []
    for f in rows:
        draw, prims, first, ctx, prim, frame, zbuf, tex0, tex1, clamp, alpha, test, box, texels = f[:14]
        z = f[14] if len(f) > 14 else ""
        p = int(prim, 16)
        key = (frame[-8:], zbuf[-8:], test[-5:], ctx, p & 7, (p >> 4) & 1, (p >> 8) & 1)  # TME, FST
        m = re.match(r"(-?[0-9.]+),(-?[0-9.]+)-(-?[0-9.]+),(-?[0-9.]+)", box)
        nums = [float(v) for v in m.groups()] if m else []
        x0, y0, x1, y1 = nums if len(nums) == 4 else (0, 0, 0, 0)
        za, zb = (int(v, 16) for v in z.split("-")) if "-" in z else (0, 0)
        if runs and runs[-1]["key"] == key:
            r = runs[-1]
            r["n"] += 1
            r["prims"] += int(prims)
            r["box"] = [min(r["box"][0], x0), min(r["box"][1], y0), max(r["box"][2], x1), max(r["box"][3], y1)]
            r["z"] = [min(r["z"][0], za), max(r["z"][1], zb)]
            r["tex"].add(tex0[-10:])
        else:
            runs.append({"key": key, "first": int(draw), "n": 1, "prims": int(prims), "box": [x0, y0, x1, y1],
                         "z": [za, zb], "tex": {tex0[-10:]}})
    tail = int(sys.argv[sys.argv.index("--tail") + 1]) if "--tail" in sys.argv else len(runs)
    print(f"{len(rows)} batches, {len(runs)} runs; frame zbuf test ctx prim TME FST")
    for r in runs[-tail:]:
        k = r["key"]
        b = r["box"]
        print(f"#{r['first']:5} x{r['n']:4} prims {r['prims']:6} fr {k[0]} zb {k[1]} test {k[2]} c{k[3]} p{k[4]} "
              f"tme{k[5]} fst{k[6]} box {b[0]:.0f},{b[1]:.0f}-{b[2]:.0f},{b[3]:.0f} z {r['z'][0]:06X}-{r['z'][1]:06X} "
              f"tex {len(r['tex'])}")


if __name__ == "__main__":
    main()
