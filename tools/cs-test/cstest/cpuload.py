"""The machine's CPU load while a run is in the world.

Other processes that keep every CPU busy (a parallel build, another game compiling shaders) starve the game: its update rate falls
to 2-4 a second in every mode, d3d11 and guest too (post-collapse.md). A native run keeps rendering, so it shows as a collapse of
the game updates. The harness counts those seconds and names the busy tools, so a slow run says why.
"""

import collections
import os
from pathlib import Path

from . import procs

SATURATED = 0.95  # the system's busy share of all CPUs
OTHERS = 0.5  # and the other processes' share of all CPUs
# Process names worth naming when the CPUs are saturated: compilers, linkers, build drivers and games.
TOOLS = ("cl.exe", "clang-cl.exe", "clang.exe", "link.exe", "lld-link.exe", "ninja.exe", "msbuild.exe", "cmake.exe", "dxc.exe",
         "fxc.exe", "skyrimse.exe")
REPORT_S = 5  # saturated seconds before the summary line mentions it


def _filetime(value):
    return (value.dwHighDateTime << 32) | value.dwLowDateTime


def system_times():
    """(idle, kernel + user) in 100 ns units over all CPUs; kernel time includes idle time."""
    if not procs.IS_WINDOWS:
        return None
    import ctypes
    from ctypes import wintypes
    idle, kernel, user = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
    if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
        return None
    return _filetime(idle), _filetime(kernel) + _filetime(user)


def process_time(pid):
    """A process's kernel + user time in 100 ns units, or None."""
    if not procs.IS_WINDOWS:
        return None
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.windll.kernel32
    kernel32.OpenProcess.restype = wintypes.HANDLE
    handle = kernel32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return None
    try:
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel32.GetProcessTimes(wintypes.HANDLE(handle), *(ctypes.byref(t) for t in times)):
            return None
        return _filetime(times[2]) + _filetime(times[3])
    finally:
        kernel32.CloseHandle(wintypes.HANDLE(handle))


def busy_tools():
    """Running processes whose names are in TOOLS, counted by name."""
    names = collections.Counter()
    for pid in procs.all_pids():
        name = Path(procs.image_path(pid) or "").name.lower()
        if name in TOOLS:
            names[name] += 1
    return names


class CpuLoad:
    """Samples the system's and the game's CPU time once a poll. Counts the seconds in the world when the CPUs were saturated by other
    processes, the most CPUs they used, and the tools running at the first saturated sample of each stretch."""

    def __init__(self, cpus=None, system=system_times, process=process_time, tools=busy_tools):
        self.cpus = cpus or os.cpu_count() or 1
        self.system, self.process, self.tools = system, process, tools
        self.last = None
        self.saturated_s = 0.0
        self.longest_s = 0.0
        self.stretch_s = 0.0
        self.others_max = 0.0
        self.seen_tools = collections.Counter()

    def sample(self, pid, in_world):
        """One poll. pid is the game's (or None); only samples in the world count."""
        now = self.system()
        game = self.process(pid) if pid else None
        last, self.last = self.last, (now, game, pid)
        if not now or not last or not last[0] or last[2] != pid:
            return
        idle = now[0] - last[0][0]
        total = now[1] - last[0][1]
        if total <= 0:
            return
        busy = 1.0 - idle / total
        game_share = (game - last[1]) / total if game is not None and last[1] is not None else 0.0
        others = max(0.0, busy - game_share)
        seconds = total / 1e7 / self.cpus
        if not in_world:
            self.stretch_s = 0.0
            return
        self.others_max = max(self.others_max, others * self.cpus)
        if busy >= SATURATED and others >= OTHERS:
            if self.stretch_s == 0.0:
                self.seen_tools.update(self.tools())
            self.saturated_s += seconds
            self.stretch_s += seconds
            self.longest_s = max(self.longest_s, self.stretch_s)
        else:
            self.stretch_s = 0.0

    def result(self):
        return {"cpus": self.cpus, "saturated_s": round(self.saturated_s, 1), "longest_s": round(self.longest_s, 1),
                "others_max_cpus": round(self.others_max, 1), "tools": dict(self.seen_tools.most_common())}


def text(result):
    """One clause for the run's summary line, or empty when the CPUs were not saturated for long."""
    if not result or result.get("saturated_s", 0) < REPORT_S:
        return ""
    tools = ", ".join(f"{name} x{count}" if count > 1 else name for name, count in result["tools"].items())
    return (f"CPUs saturated by other processes {result['saturated_s']:.0f} s in the world (up to {result['others_max_cpus']:.1f} of "
            f"{result['cpus']} CPUs{': ' + tools if tools else ''}): game updates starve in every mode")
