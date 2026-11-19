"""Counts green-streak pixels in run folders' whiterun shots (configs/green.toml): python greenscore.py <run folder>...

A pixel counts when green leads red and blue by more than 50 and is above 90 (every second pixel in each direction).
Clean shots of that scene score 0; the SSGI streaks scored several hundred to a few thousand.
"""

import sys
from pathlib import Path

from PIL import Image


def score(path):
    image = Image.open(path).convert("RGB")
    width, height = image.size
    pixels = image.load()
    count = 0
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            r, g, b = pixels[x, y]
            if g > r + 50 and g > b + 50 and g > 90:
                count += 1
    return count


def main():
    for folder in sys.argv[1:]:
        shot = Path(folder) / "shots" / "whiterun.png"
        if shot.exists():
            print(f"{score(shot):7d} {Path(folder).name}")
        else:
            print(f"   none {Path(folder).name} (no shots/whiterun.png)")


if __name__ == "__main__":
    main()
