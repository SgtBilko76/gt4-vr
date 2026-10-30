#!/usr/bin/env python3
"""Passes of a PS2_CAPTURE draws.tsv: consecutive batches into the same frame buffer with the same
Z test, with their union box, Z range and prim count.  usage: draw_passes.py draws.tsv"""
import re
import sys

rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])]
rows = [f for f in rows if len(f) >= 15 and f[0].isdigit()]
passes = []
for f in rows:
    frame, zbuf, test = int(f[5], 16), int(f[6], 16), int(f[11], 16)
    fbp, fbw, psm = frame & 0x1FF, (frame >> 16) & 0x3F, (frame >> 24) & 0x3F
    zte, ztst = (test >> 16) & 1, (test >> 17) & 3
    zcls = "zoff" if not zte else ["never", "always", "gequal", "greater"][ztst]
    m = re.match(r"(-?[0-9.]+),(-?[0-9.]+)-(-?[0-9.]+),(-?[0-9.]+)", f[12])
    nums = [float(v) for v in m.groups()] if m else []
    za, zb = (int(v, 16) for v in f[14].split("-"))
    prim = int(f[4], 16) & 7
    key = (fbp, fbw, psm, zcls)
    if passes and passes[-1]["key"] == key:
        p = passes[-1]
    else:
        p = {"key": key, "first": int(f[0]), "batches": 0, "prims": 0, "box": [1e9, 1e9, -1e9, -1e9],
             "z": [1 << 32, 0], "types": set()}
        passes.append(p)
    p["batches"] += 1
    p["prims"] += int(f[1])
    p["types"].add(prim)
    if len(nums) == 4:
        p["box"] = [min(p["box"][0], nums[0]), min(p["box"][1], nums[1]), max(p["box"][2], nums[2]),
                    max(p["box"][3], nums[3])]
    p["z"] = [min(p["z"][0], za), max(p["z"][1], zb)]
for p in passes:
    fbp, fbw, psm, z = p["key"]
    b = p["box"]
    print(f"#{p['first']:5} fbp {fbp:03X} w{fbw*64:4} psm{psm:02X} {z:7} batches {p['batches']:4} prims {p['prims']:6} "
          f"types {sorted(p['types'])} box {b[0]:.0f},{b[1]:.0f}..{b[2]:.0f},{b[3]:.0f} z {p['z'][0]:X}-{p['z'][1]:X}")
