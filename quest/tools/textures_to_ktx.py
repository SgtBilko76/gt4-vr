#!/usr/bin/env python3
"""Convert PCSX2 replacement texture packs (PNG/DDS) into Quest-ready KTX1 ASTC mip chains.

GT4 Spec II HD 3D (owner 2026-11-02, private build): the PCSX2 GS bridge (ARMSX2 fork) uploads ASTC
replacements without decoding (GSTextureKTX.h: KTX1, linear GL_COMPRESSED_RGBA_ASTC_*, 1..7 levels,
KTXorientation S=r,T=d). PNG would be decoded on the Quest's CPU during the race and needs 4x the memory.

usage: textures_to_ktx.py OUT_DIR PACK=BLOCK:DIR [PACK=BLOCK:DIR ...] [--cap 2048] [--jobs N] [--quality medium]
  Packs are listed in priority order: a texture name that an earlier pack already has is skipped
  (PCSX2 keeps the first file it finds, and Android lists folders in no fixed order).
  OUT_DIR/<pack>/<path in pack>/<texture name>.ktx is written; existing up-to-date files are kept.
"""
import argparse, os, struct, subprocess, sys, tempfile, time
from multiprocessing import Pool

ASTCENC = os.environ.get("ASTCENC", os.path.expanduser("~/tools/astcenc/bin/astcenc-avx2"))
FORMATS = {"4x4": 0x93B0, "5x5": 0x93B2, "6x6": 0x93B4, "8x8": 0x93B7}
KTX_ID = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
KV = b"KTXorientation\0S=r,T=d\0"
MAX_LEVELS = 7


def kv_block():
    entry = struct.pack("<I", len(KV)) + KV
    return entry + b"\0" * ((4 - len(entry) % 4) % 4)


def convert(job):
    src, dst, block, cap, quality = job
    from PIL import Image
    try:
        im = Image.open(src)
        im.load()
        im = im.convert("RGBA")
        w, h = im.size
        if max(w, h) > cap:
            s = cap / max(w, h)
            w, h = max(1, round(w * s)), max(1, round(h * s))
        levels = min(MAX_LEVELS, max(w, h).bit_length())  # floor(log2(max)) + 1
        # Mips from premultiplied alpha, so transparent edges don't turn dark.
        pm = im.convert("RGBa")
        bw, bh = map(int, block.split("x"))
        payloads = []
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as tmp:
            for lv in range(levels):
                lw, lh = max(1, w >> lv), max(1, h >> lv)
                img = pm if (lw, lh) == pm.size else pm.resize((lw, lh), Image.Resampling.LANCZOS)
                png, astc = os.path.join(tmp, f"{lv}.png"), os.path.join(tmp, f"{lv}.astc")
                img.convert("RGBA").save(png, compress_level=1)
                subprocess.run([ASTCENC, "-cl", png, astc, block, "-" + quality, "-silent", "-j", "1"],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                data = open(astc, "rb").read()[16:]
                expect = -(-lw // bw) * -(-lh // bh) * 16
                if len(data) != expect:
                    raise RuntimeError(f"level {lv}: {len(data)} bytes, expected {expect}")
                payloads.append(data)
        kv = kv_block()
        head = KTX_ID + struct.pack("<13I", 0x04030201, 0, 1, 0, FORMATS[block], 0x1908, w, h, 0, 0, 1, levels, len(kv))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        part = dst + ".part"
        with open(part, "wb") as f:
            f.write(head + kv)
            for p in payloads:
                f.write(struct.pack("<I", len(p)) + p + b"\0" * ((4 - len(p) % 4) % 4))
        os.replace(part, dst)
        return src, None, os.path.getsize(dst)
    except Exception as e:  # report and continue: one bad file must not stop 90k others
        return src, f"{type(e).__name__}: {str(e)[:200]}", 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("packs", nargs="+")
    ap.add_argument("--cap", type=int, default=2048)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--quality", default="medium")
    a = ap.parse_args()
    seen, jobs, skipped_dup, kept = set(), [], 0, 0
    for spec in a.packs:
        name, rest = spec.split("=", 1)
        block, root = rest.split(":", 1)
        files = []
        for r, ds, fs in os.walk(root):
            ds.sort()
            files += [os.path.join(r, f) for f in sorted(fs) if f.lower().endswith((".png", ".dds"))]
        for src in files:
            stem = os.path.splitext(os.path.basename(src))[0].lower()
            if stem in seen:
                skipped_dup += 1
                continue
            seen.add(stem)
            rel = os.path.relpath(os.path.splitext(src)[0], root) + ".ktx"
            dst = os.path.join(a.out, name, rel)
            if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
                kept += 1
                continue
            jobs.append((src, dst, block, a.cap, a.quality))
    print(f"{len(jobs)} to convert, {kept} up to date, {skipped_dup} duplicate names skipped", flush=True)
    t0, done, errors, out_bytes = time.time(), 0, [], 0
    with Pool(a.jobs) as pool:
        for src, err, size in pool.imap_unordered(convert, jobs, chunksize=8):
            done += 1
            out_bytes += size
            if err:
                errors.append((src, err))
            if done % 2000 == 0:
                el = time.time() - t0
                print(f"{done}/{len(jobs)} {el:.0f}s eta {el / done * (len(jobs) - done):.0f}s, "
                      f"{out_bytes / 2**30:.2f} GB, errors {len(errors)}", flush=True)
    with open(os.path.join(a.out, "convert-errors.txt"), "w") as f:
        for s, e in errors:
            f.write(f"{s}\t{e}\n")
    print(f"done: {done} converted in {time.time() - t0:.0f}s, {out_bytes / 2**30:.2f} GB new, {len(errors)} errors",
          flush=True)


if __name__ == "__main__":
    main()
