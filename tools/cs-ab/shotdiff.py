# Pixel difference of two dclf:shot screenshots: python tools/cs-ab/shotdiff.py a.png b.png [diff.png]
# Prints the share of differing pixels, the mean and max channel difference, and percentiles; writes an amplified diff image.
import sys
import numpy as np
from PIL import Image

a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(np.int16)
if a.shape != b.shape:
    sys.exit(f"sizes differ: {a.shape} vs {b.shape}")
d = np.abs(a - b)
per = d.max(axis=2)
n = per.size
print(f"{sys.argv[1]} vs {sys.argv[2]}: {a.shape[1]}x{a.shape[0]}")
print(f"  identical pixels: {100.0 * (per == 0).sum() / n:.3f}%")
for t in (1, 2, 4, 8, 16, 32):
    print(f"  pixels differing by > {t:>2}: {100.0 * (per > t).sum() / n:.3f}%")
print(f"  mean channel diff {d.mean():.4f}, max {d.max()}, p99 {np.percentile(per, 99):.1f}, p99.9 {np.percentile(per, 99.9):.1f}")
# Where the differences are: the bounding box of pixels over 8.
ys, xs = np.nonzero(per > 8)
if len(ys):
    print(f"  > 8 within x {xs.min()}-{xs.max()}, y {ys.min()}-{ys.max()}")
if len(sys.argv) > 3:
    Image.fromarray(np.clip(per * 8, 0, 255).astype(np.uint8)).save(sys.argv[3])
