"""Reads a Skyrim SE/AE save's screenshot: its size and mean channel values, so a run can tell whether the save got its picture.

python tools/cs-test/essshot.py <save.ess> [...] [--png out_dir]

A save written without its screenshot has a black (all zero) picture. With --png each picture is also written as a PNG.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path


def read_wstring(data, at):
    (length,) = struct.unpack_from("<H", data, at)
    return data[at + 2 : at + 2 + length].decode("cp1252", "replace"), at + 2 + length


def read_shot(path):
    data = Path(path).read_bytes()
    if not data.startswith(b"TESV_SAVEGAME"):
        raise ValueError(f"{path}: not a save")
    at = 13
    (header_size,) = struct.unpack_from("<I", data, at)
    at += 4
    header_start = at
    version, _number = struct.unpack_from("<II", data, at)
    at += 8
    name, at = read_wstring(data, at)
    (_level,) = struct.unpack_from("<I", data, at)
    at += 4
    location, at = read_wstring(data, at)
    _date, at = read_wstring(data, at)
    _race, at = read_wstring(data, at)
    at += 2 + 4 + 4 + 8  # sex, experience, level-up experience, file time
    width, height = struct.unpack_from("<II", data, at)
    at = header_start + header_size
    channels = 4 if version >= 12 else 3
    pixels = data[at : at + width * height * channels]
    sums = [sum(pixels[c::channels]) for c in range(channels)]
    count = max(1, width * height)
    return {"file": str(path), "version": version, "player": name, "location": location, "width": width, "height": height,
            "channels": channels, "means": [round(s / count, 1) for s in sums], "pixels": pixels}


def write_png(shot, out):
    width, height, channels = shot["width"], shot["height"], shot["channels"]
    rows = b"".join(b"\x00" + shot["pixels"][y * width * channels : (y + 1) * width * channels] for y in range(height))
    colour = 6 if channels == 4 else 2

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, colour, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    Path(out).write_bytes(png)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("saves", nargs="+")
    parser.add_argument("--png", help="write each picture as a PNG into this folder")
    args = parser.parse_args()
    black = 0
    for path in args.saves:
        shot = read_shot(path)
        empty = not any(shot["pixels"])
        black += empty
        print(f"{shot['file']}: {shot['width']}x{shot['height']}, means {shot['means']}{', BLACK' if empty else ''} ({shot['location']})")
        if args.png:
            Path(args.png).mkdir(parents=True, exist_ok=True)
            write_png(shot, Path(args.png) / (Path(path).stem + ".png"))
    return 1 if black else 0


if __name__ == "__main__":
    sys.exit(main())
