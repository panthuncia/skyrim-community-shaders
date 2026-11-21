"""Pixel difference of two screenshots: python tools/cs-test/shotdiff.py a.png b.png [diff.png]

Prints the share of differing pixels, the mean and max channel difference, and percentiles. With a third path it
writes an amplified grey diff image. Standard library only: it reads 8- and 16-bit non-interlaced PNGs.
"""

import struct
import sys
import zlib

CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}  # PNG colour type -> samples per pixel


def unfilter(raw, height, stride, bpp):
    """The PNG scanlines with their filters undone, as one bytearray per row."""
    rows = []
    prior = bytearray(stride)
    pos = 0
    for _ in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if kind == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif kind == 2:
            line = bytearray((a + b) & 0xFF for a, b in zip(line, prior))
        elif kind == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prior[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prior[i]
                c = prior[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        elif kind != 0:
            raise ValueError(f"bad PNG filter {kind}")
        rows.append(line)
        prior = line
    return rows


def read_rgb(path):
    """(width, height, rows): each row is width * 3 bytes of 8-bit RGB."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, header = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    width, height, depth, colour, _, _, interlace = header
    if colour not in CHANNELS or depth not in (8, 16) or interlace:
        raise ValueError(f"{path}: PNG colour type {colour}, depth {depth}, interlace {interlace} is not read")
    samples = CHANNELS[colour]
    size = depth // 8
    bpp = samples * size
    rows = unfilter(zlib.decompress(b"".join(idat)), height, width * bpp, bpp)
    out = []
    for line in rows:
        if size == 2:
            line = line[0::2]  # the high byte of each sample
        if colour == 2:
            out.append(bytes(line))
        elif colour == 6:
            rgb = bytearray(width * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = line[0::4], line[1::4], line[2::4]
            out.append(bytes(rgb))
        else:
            grey = line if colour == 0 else line[0::2]
            rgb = bytearray(width * 3)
            rgb[0::3] = rgb[1::3] = rgb[2::3] = grey
            out.append(bytes(rgb))
    return width, height, out


def write_grey(path, width, rows):
    """An 8-bit grey PNG from rows of width bytes."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, len(rows), 8, 0, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def percentile(histogram, total, share):
    """The smallest value v with at least share of the counts at or below v."""
    need = share * total
    seen = 0
    for value, count in enumerate(histogram):
        seen += count
        if seen >= need:
            return value
    return len(histogram) - 1


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    wa, ha, a = read_rgb(argv[0])
    wb, hb, b = read_rgb(argv[1])
    if (wa, ha) != (wb, hb):
        sys.exit(f"sizes differ: {wa}x{ha} vs {wb}x{hb}")
    histogram = [0] * 256  # per pixel: the largest channel difference
    channel_sum = 0
    channel_max = 0
    box = None  # x0, x1, y0, y1 of pixels differing by more than 8
    amplified = [] if len(argv) > 2 else None
    for y, (ra, rb) in enumerate(zip(a, b)):
        d = [abs(x - z) for x, z in zip(ra, rb)]
        channel_sum += sum(d)
        per = list(map(max, d[0::3], d[1::3], d[2::3]))
        top = max(per)
        channel_max = max(channel_max, top)
        for value in per:
            histogram[value] += 1
        if top > 8:
            xs = [x for x, value in enumerate(per) if value > 8]
            box = (xs[0], xs[-1], y, y) if box is None else (min(box[0], xs[0]), max(box[1], xs[-1]), box[2], y)
        if amplified is not None:
            amplified.append(bytes(min(value * 8, 255) for value in per))
    n = wa * ha
    print(f"{argv[0]} vs {argv[1]}: {wa}x{ha}")
    print(f"  identical pixels: {100.0 * histogram[0] / n:.3f}%")
    for t in (1, 2, 4, 8, 16, 32):
        print(f"  pixels differing by > {t:>2}: {100.0 * sum(histogram[t + 1:]) / n:.3f}%")
    print(f"  mean channel diff {channel_sum / (n * 3):.4f}, max {channel_max}, p99 {percentile(histogram, n, 0.99)}, p99.9 {percentile(histogram, n, 0.999)}")
    if box:
        print(f"  > 8 within x {box[0]}-{box[1]}, y {box[2]}-{box[3]}")
    if amplified is not None:
        write_grey(argv[2], wa, amplified)


if __name__ == "__main__":
    main(sys.argv[1:])
