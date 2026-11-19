"""Frame rate between a run's "start" and "end" marks: python perfwindow.py <run folder>...

Reads CommunityShaders.log: the [TEST] mark lines and the [Perf] "rendered N fps (M ms)" lines between them.
"""

import re
import sys
from pathlib import Path

PERF = re.compile(r"\[Perf\] rendered ([0-9.]+) fps \(([0-9.]+) ms\)")


def window(log):
    inside, samples = False, []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if "[TEST]" in line and "mark" in line:
            if "start" in line:
                inside, samples = True, []
            elif "end" in line and inside:
                break
        elif inside:
            m = PERF.search(line)
            if m:
                samples.append(float(m.group(1)))
    return samples


def main():
    for folder in sys.argv[1:]:
        samples = window(Path(folder) / "CommunityShaders.log")
        if samples:
            print(f"{sum(samples) / len(samples):6.1f} fps mean of {len(samples)} [{', '.join(f'{s:.1f}' for s in samples)}]  {Path(folder).name}")
        else:
            print(f"  none  {Path(folder).name}")


if __name__ == "__main__":
    main()
