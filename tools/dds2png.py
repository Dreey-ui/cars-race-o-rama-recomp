# Converts .dds (DXT1/DXT3/DXT5 or uncompressed A8R8G8B8, level 0) to .png, to check the original colors.
# Usage: python -I tools/dds2png.py file.dds [out.png]
import struct
import sys
import zlib


def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def color_block(b):
    c0, c1, bits = struct.unpack("<HHI", b[:8])
    p0, p1 = rgb565(c0), rgb565(c1)
    if c0 > c1:
        p2 = tuple((2 * a + c) // 3 for a, c in zip(p0, p1))
        p3 = tuple((a + 2 * c) // 3 for a, c in zip(p0, p1))
    else:
        p2 = tuple((a + c) // 2 for a, c in zip(p0, p1))
        p3 = (0, 0, 0)
    pal = [p0, p1, p2, p3]
    return [pal[(bits >> (2 * i)) & 3] for i in range(16)]


def alpha5(b):
    a0, a1 = b[0], b[1]
    bits = int.from_bytes(b[2:8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
    else:
        pal = [a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def main():
    data = open(sys.argv[1], "rb").read()
    h, w = struct.unpack("<II", data[12:20])
    fourcc = data[84:88]
    out = bytearray(w * h * 4)
    p = 128
    if fourcc in (b"DXT1", b"DXT3", b"DXT5"):
        bs = 8 if fourcc == b"DXT1" else 16
        for by in range(0, h, 4):
            for bx in range(0, w, 4):
                blk = data[p:p + bs]
                p += bs
                if fourcc == b"DXT1":
                    cols, alph = color_block(blk), [255] * 16
                elif fourcc == b"DXT3":
                    cols = color_block(blk[8:])
                    alph = [((blk[i // 2] >> (4 * (i & 1))) & 15) * 17 for i in range(16)]
                else:
                    cols, alph = color_block(blk[8:]), alpha5(blk)
                for i in range(16):
                    x, y = bx + i % 4, by + i // 4
                    if x < w and y < h:
                        o = (y * w + x) * 4
                        out[o:o + 4] = bytes(cols[i]) + bytes([alph[i]])
    else:   # A8R8G8B8 (bytes B G R A)
        for i in range(w * h):
            b, g, r, a = data[p + 4 * i:p + 4 * i + 4]
            out[4 * i:4 * i + 4] = bytes([r, g, b, a])
    # gray checkerboard background to see the alpha
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            r, g, b, a = out[(y * w + x) * 4:(y * w + x) * 4 + 4]
            bg = 160 if ((x // 8 + y // 8) & 1) else 96
            rows += bytes([(r * a + bg * (255 - a)) // 255, (g * a + bg * (255 - a)) // 255, (b * a + bg * (255 - a)) // 255])
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
          chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    dst = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1].rsplit(".", 1)[0] + ".png"
    open(dst, "wb").write(png)
    print("%s: %dx%d %s -> %s" % (sys.argv[1], w, h, fourcc.decode(errors="replace"), dst))


main()
