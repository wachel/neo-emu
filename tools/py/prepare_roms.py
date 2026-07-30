# -*- coding: utf-8 -*-
"""
Extract kof98.zip + neogeo.zip and build CPU/GFX images for the native build.

Outputs into build/rom:
  prom.bin   - decrypted 68k program image (5 MiB used, linear CPU address space content)
  bios.bin   - MVS BIOS (sp-s2.sp1, 128 KiB, word-swapped to 68k big-endian image)
  sfix.bin   - BIOS fix tiles (raw)
  s1.bin     - game fix tiles (raw)
  zoomy.bin  - 000-lo.lo Y-zoom lookup ROM (raw)
  crom.bin   - interleaved sprite graphics (c1..c8)
"""
import os, sys, zipfile, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "rom")


def load_zip_member(zf, name):
    return zf.read(name)


def swap16(data: bytes) -> bytes:
    b = bytearray(len(data))
    b[0::2] = data[1::2]
    b[1::2] = data[0::2]
    return bytes(b)


def crc(b):
    return "%08x" % (zlib.crc32(b) & 0xFFFFFFFF)


def kof98_decrypt_68k(src: bytearray):
    """MAME prot_kof98 decrypt_68k, applied on the big-endian cpurom buffer (p1 2MiB + p2 4MiB)."""
    sec = [0x000000, 0x100000, 0x000004, 0x100004, 0x10000a, 0x00000a, 0x10000e, 0x00000e]
    pos = [0x000, 0x004, 0x00a, 0x00e]
    dst = bytes(src[0:0x200000])

    def put(idx, b2):
        src[idx] = b2[0]
        src[idx + 1] = b2[1]

    for i in range(0x800, 0x100000, 0x200):
        for j in range(0, 0x100, 0x10):
            for k in range(0, 16, 2):
                s = sec[k // 2]
                put(i + j + k, dst[i + j + s + 0x100: i + j + s + 0x102])
                put(i + j + k + 0x100, dst[i + j + s: i + j + s + 2])
            if 0x80000 <= i < 0xC0000:
                for k in range(4):
                    p = pos[k]
                    put(i + j + p, dst[i + j + p: i + j + p + 2])
                    put(i + j + p + 0x100, dst[i + j + p + 0x100: i + j + p + 0x102])
            elif i >= 0xC0000:
                for k in range(4):
                    p = pos[k]
                    put(i + j + p, dst[i + j + p + 0x100: i + j + p + 0x102])
                    put(i + j + p + 0x100, dst[i + j + p: i + j + p + 2])
        put(i + 0x000, dst[i + 0x000000: i + 0x000002])
        put(i + 0x002, dst[i + 0x100000: i + 0x100002])
        put(i + 0x100, dst[i + 0x000100: i + 0x000102])
        put(i + 0x102, dst[i + 0x100100: i + 0x100102])

    src[0x100000:0x500000] = src[0x200000:0x600000]


def main():
    os.makedirs(OUT, exist_ok=True)
    kof = zipfile.ZipFile(os.path.join(ROOT, "roms", "kof98.zip"))
    bio = zipfile.ZipFile(os.path.join(ROOT, "roms", "neogeo.zip"))

    # ---- program ROMs ----
    p1 = load_zip_member(kof, "242-p1.p1")
    p2 = load_zip_member(kof, "242-p2.sp2")
    print("p1 size %06x crc %s" % (len(p1), crc(p1)))
    print("p2 size %06x crc %s" % (len(p2), crc(p2)))
    cpurom = bytearray(swap16(p1) + swap16(p2))
    kof98_decrypt_68k(cpurom)
    prom = bytes(cpurom[:0x500000])
    with open(os.path.join(OUT, "prom.bin"), "wb") as f:
        f.write(prom)
    print("prom.bin written, size %06x crc %s" % (len(prom), crc(prom)))
    # vector info
    def rd32(b, o):
        return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]
    print("cart vectors: SSP=%06x PC=%06x IRQ1=%06x" % (rd32(prom, 0), rd32(prom, 4), rd32(prom, 0x64)))
    print("default_rom[0x100]=%04x %04x" % ((prom[0x100] << 8) | prom[0x101], (prom[0x102] << 8) | prom[0x103]))

    # ---- BIOS ----
    bios = swap16(load_zip_member(bio, "sp-s2.sp1"))
    with open(os.path.join(OUT, "bios.bin"), "wb") as f:
        f.write(bios)
    print("bios.bin size %06x crc %s  PC=%06x SSP=%06x" % (len(bios), crc(bios), rd32(bios, 4), rd32(bios, 0)))

    # ---- gfx + audio bios ----
    for name, out in [("sfix.sfix", "sfix.bin"), ("000-lo.lo", "zoomy.bin"), ("sm1.sm1", "sm1.bin")]:
        d = load_zip_member(bio, name)
        with open(os.path.join(OUT, out), "wb") as f:
            f.write(d)
        print("%s size %06x crc %s" % (out, len(d), crc(d)))

    s1 = load_zip_member(kof, "242-s1.s1")
    with open(os.path.join(OUT, "s1.bin"), "wb") as f:
        f.write(s1)
    print("s1.bin size %06x crc %s" % (len(s1), crc(s1)))

    # ---- sprite ROMs: interleave (c1,c2), (c3,c4), ... ----
    crom = bytearray()
    for pair in (("242-c1.c1", "242-c2.c2"), ("242-c3.c3", "242-c4.c4"),
                 ("242-c5.c5", "242-c6.c6"), ("242-c7.c7", "242-c8.c8")):
        a = load_zip_member(kof, pair[0])
        b = load_zip_member(kof, pair[1])
        print("%s %06x %s / %s %06x %s" % (pair[0], len(a), crc(a), pair[1], len(b), crc(b)))
        assert len(a) == len(b)
        blk = bytearray(len(a) * 2)
        blk[0::2] = a
        blk[1::2] = b
        crom += blk
    with open(os.path.join(OUT, "crom.bin"), "wb") as f:
        f.write(crom)
    print("crom.bin size %06x crc %s" % (len(crom), crc(crom)))

    # ---- audio ----
    m1 = load_zip_member(kof, "242-m1.m1")
    with open(os.path.join(OUT, "m1.bin"), "wb") as f:
        f.write(m1)
    print("m1.bin size %06x crc %s" % (len(m1), crc(m1)))

    vrom = bytearray()
    for n in ("242-v1.v1", "242-v2.v2", "242-v3.v3", "242-v4.v4"):
        d = load_zip_member(kof, n)
        print("%s %06x %s" % (n, len(d), crc(d)))
        vrom += d
    with open(os.path.join(OUT, "vrom.bin"), "wb") as f:
        f.write(vrom)
    print("vrom.bin size %06x crc %s" % (len(vrom), crc(vrom)))


if __name__ == "__main__":
    main()
