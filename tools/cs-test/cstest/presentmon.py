"""fg.presentmon: presents per second of the game's process, from the PresentMon service (Intel PresentMon's SDK).

The console PresentMon needs elevation to see presents; the service does not. A thread polls one dynamic query a second
(presented and displayed frames per second over the last second, per swap chain) and writes a line per poll to
presentmon.jsonl in the run folder. Frame generation's presents are real presents, so presented / rendered is the multiplier.
"""

import ctypes
import json
import threading
import time
from pathlib import Path

LOADER = Path(r"C:\Program Files\Intel\PresentMon\SDK\PresentMonAPI2Loader.dll")

# PresentMonAPI.h enums.
PM_METRIC_DISPLAYED_FPS = 11
PM_METRIC_PRESENTED_FPS = 12
PM_STAT_AVG = 1
MAX_SWAP_CHAINS = 8


class QueryElement(ctypes.Structure):
    _fields_ = [("metric", ctypes.c_int), ("stat", ctypes.c_int), ("deviceId", ctypes.c_uint32), ("arrayIndex", ctypes.c_uint32),
                ("dataOffset", ctypes.c_uint64), ("dataSize", ctypes.c_uint64)]


class PresentMonError(Exception):
    pass


class Sampler:
    """Polls presents per second for one process on a thread until stop()."""

    def __init__(self, pid, out_path, period_s=1.0):
        self.pid = pid
        self.out_path = Path(out_path)
        self.period_s = period_s
        self.samples = []
        self.error = None
        self._stop = threading.Event()
        self._thread = None
        self._api = None
        self._session = ctypes.c_void_p()
        self._query = ctypes.c_void_p()

    def start(self):
        if not LOADER.exists():
            raise PresentMonError(f"no PresentMon SDK loader at {LOADER}")
        api = ctypes.CDLL(str(LOADER))
        self._api = api
        status = api.pmOpenSession(ctypes.byref(self._session))
        if status != 0:
            raise PresentMonError(f"pmOpenSession failed ({status}): is the PresentMon service running?")
        status = api.pmStartTrackingProcess(self._session, ctypes.c_uint32(self.pid))
        if status != 0:
            api.pmCloseSession(self._session)
            raise PresentMonError(f"pmStartTrackingProcess({self.pid}) failed ({status})")
        self._elements = (QueryElement * 2)(QueryElement(PM_METRIC_PRESENTED_FPS, PM_STAT_AVG, 0, 0, 0, 0),
                                            QueryElement(PM_METRIC_DISPLAYED_FPS, PM_STAT_AVG, 0, 0, 0, 0))
        status = api.pmRegisterDynamicQuery(self._session, ctypes.byref(self._query), self._elements, ctypes.c_uint64(2),
                                            ctypes.c_double(self.period_s * 1000.0), ctypes.c_double(0.0))
        if status != 0:
            api.pmCloseSession(self._session)
            raise PresentMonError(f"pmRegisterDynamicQuery failed ({status})")
        self._blob_size = max(e.dataOffset + e.dataSize for e in self._elements)
        self._thread = threading.Thread(target=self._run, name="presentmon", daemon=True)
        self._thread.start()

    def _poll(self):
        blob = (ctypes.c_uint8 * (self._blob_size * MAX_SWAP_CHAINS))()
        count = ctypes.c_uint32(MAX_SWAP_CHAINS)
        status = self._api.pmPollDynamicQuery(self._query, ctypes.c_uint32(self.pid), blob, ctypes.byref(count))
        if status != 0:
            return None
        chains = []
        for i in range(count.value):
            base = ctypes.addressof(blob) + i * self._blob_size
            presented = ctypes.c_double.from_address(base + self._elements[0].dataOffset).value
            displayed = ctypes.c_double.from_address(base + self._elements[1].dataOffset).value
            chains.append({"presented_fps": round(presented, 2), "displayed_fps": round(displayed, 2)})
        return chains

    def _run(self):
        start = time.monotonic()
        with open(self.out_path, "w", encoding="utf-8") as out:
            while not self._stop.wait(self.period_s):
                try:
                    chains = self._poll()
                except OSError as error:
                    self.error = str(error)
                    return
                if chains is None:
                    continue
                sample = {"t": round(time.monotonic() - start, 1), "wall": time.time(), "chains": chains}
                self.samples.append(sample)
                out.write(json.dumps(sample) + "\n")
                out.flush()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(5)
        if self._api:
            if self._query:
                self._api.pmFreeDynamicQuery(self._query)
            self._api.pmStopTrackingProcess(self._session, ctypes.c_uint32(self.pid))
            self._api.pmCloseSession(self._session)
            self._api = None


def summarize(samples, fgtest=()):
    """Presented and displayed frames per second over the samples where the process presented: per second, the busiest chain.
    With fgtest (the log's [FGTEST] seconds in which FSR-FG presented, as (wall time, rendered fps)), also PresentMon's
    presents per second in those seconds and their ratio to the rendered rate: the multiplier."""
    rows = []
    for sample in samples:
        if sample["chains"]:
            best = max(sample["chains"], key=lambda c: c["presented_fps"])
            if best["presented_fps"] > 0:
                rows.append((sample["t"], best["presented_fps"], best["displayed_fps"]))
    if not rows:
        return {"samples": 0}
    presented = sorted(r[1] for r in rows)
    displayed = sorted(r[2] for r in rows)
    out = {"samples": len(rows), "presented_fps_median": presented[len(presented) // 2], "displayed_fps_median": displayed[len(displayed) // 2],
           "presented_fps_max": presented[-1]}
    # A sample averages the second before it, so it matches an [FGTEST] line logged within half a second of it.
    timed = [(s["wall"], max(c["presented_fps"] for c in s["chains"])) for s in samples if s.get("wall") and s["chains"]]
    ratios, fg_presents = [], []
    for wall, rendered in fgtest:
        near = [p for w, p in timed if abs(w - wall) <= 0.5]
        if near and rendered > 0:
            fg_presents.append(near[0])
            ratios.append(near[0] / rendered)
    if ratios:
        ratios.sort()
        fg_presents.sort()
        out.update({"fg_seconds": len(ratios), "fg_presented_fps_median": fg_presents[len(fg_presents) // 2],
                    "fg_multiplier_median": round(ratios[len(ratios) // 2], 2)})
    return out


if __name__ == "__main__":
    import sys

    sampler = Sampler(int(sys.argv[1]), sys.argv[2] if len(sys.argv) > 2 else "presentmon-probe.jsonl")
    sampler.start()
    time.sleep(float(sys.argv[3]) if len(sys.argv) > 3 else 5)
    sampler.stop()
    print(json.dumps(sampler.samples, indent=1))
    print(summarize(sampler.samples))
