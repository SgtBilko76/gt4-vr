#!/usr/bin/env python3
"""Best x or y shift between two ps2run frame dumps (PPM), for checking the VR camera hooks.

usage: frame_shift.py a.ppm b.ppm x|y [max]
Compares the middle of the picture (HUD corners left out) and prints the five best shifts.
"""
import sys


def read(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    return w, h, [[sum(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) / 3 for x in range(w)] for y in range(h)]


def main():
    w, h, a = read(sys.argv[1])
    _, _, b = read(sys.argv[2])
    axis = sys.argv[3]
    top = int(sys.argv[4]) if len(sys.argv) > 4 else (w // 2 if axis == "x" else h // 2)
    best = []
    for s in range(-top, top + 1):
        err = n = 0
        for y in range(h // 6, h - h // 6, 2):
            for x in range(w // 6, w - w // 6, 3):
                x2, y2 = (x + s, y) if axis == "x" else (x, y + s)
                if 0 <= x2 < w and 0 <= y2 < h:
                    err += abs(a[y][x] - b[y2][x2])
                    n += 1
        if n > 300:
            best.append((err / n, s))
    best.sort()
    print(f"{w}x{h}", [(s, round(e, 1)) for e, s in best[:5]])


if __name__ == "__main__":
    main()
