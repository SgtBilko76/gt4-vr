#!/usr/bin/env python3
"""Compare PS2_CAPTURE draws.tsv runs of quest/tools/renderer_attrib.sh against the base run.

Per run: draws, prims, render-target reads (a textured draw whose TEX0.TBP0 is the start of a frame or
Z buffer drawn earlier in the captured frame: the effect passes that break a tiled GPU's render pass),
target changes, and how much of the picture differs from the base run.

usage: renderer_attrib.py <dir> base name...
"""
import os
import sys


def stats(path):
    rows = []
    for line in open(path):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 15 and f[0].isdigit():
            rows.append(f)
    drawn = set()  # block addresses (64-word units) of frame / Z buffers drawn so far
    draws = prims = reads = changes = 0
    read_boxes = []
    last = None
    for f in rows:
        prim = int(f[4], 16)
        frame, zbuf, tex0 = int(f[5], 16), int(f[6], 16), int(f[7], 16)
        fbp = (frame & 0x1FF) * 32
        zbp = (zbuf & 0x1FF) * 32
        tbp = tex0 & 0x3FFF
        draws += 1
        prims += int(f[1])
        if (prim >> 4) & 1 and tbp in drawn:
            reads += 1
            read_boxes.append(f[12])
        target = (fbp, zbp)
        if target != last:
            changes += 1
            last = target
        drawn.add(fbp)
        drawn.add(zbp)
    return draws, prims, reads, changes, read_boxes


def picture(path):
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    head = data.split(b"\n", 3)
    return head[3]


def main():
    d, names = sys.argv[1], sys.argv[2:]
    def ppm(name):
        files = [x for x in sorted(os.listdir(os.path.join(d, name))) if x.endswith(".ppm")]
        return os.path.join(d, name, files[-1]) if files else ""
    base = stats(os.path.join(d, names[0], "draws.tsv"))
    base_pic = picture(ppm(names[0]))
    print(f"{'run':10} {'draws':>6} {'prims':>7} {'rt-reads':>8} {'targets':>7} {'picture':>8}")
    for n in names:
        p = os.path.join(d, n, "draws.tsv")
        if not os.path.exists(p):
            print(f"{n:10} (no capture)")
            continue
        s = stats(p)
        pic = picture(ppm(n))
        if pic is not None and base_pic is not None and len(pic) == len(base_pic):
            diff = sum(1 for k in range(0, len(pic), 3) if pic[k:k + 3] != base_pic[k:k + 3]) / (len(pic) / 3)
            dtxt = f"{diff * 100:7.1f}%"
        else:
            dtxt = "     n/a"
        mark = "" if n == names[0] else (
            f"   d draws {s[0] - base[0]:+d}, rt-reads {s[2] - base[2]:+d}, targets {s[3] - base[3]:+d}")
        print(f"{n:10} {s[0]:6d} {s[1]:7d} {s[2]:8d} {s[3]:7d} {dtxt}{mark}")


if __name__ == "__main__":
    main()
