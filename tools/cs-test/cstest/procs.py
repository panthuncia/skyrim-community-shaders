"""Processes and windows on Windows, with ctypes (no psutil)."""

import csv
import io
import os
import subprocess
from pathlib import Path
import sys
import time

IS_WINDOWS = sys.platform == "win32"

if IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _kernel32.OpenProcess.restype = wintypes.HANDLE
    _kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    _kernel32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
    _kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    _kernel32.SetThreadExecutionState.restype = wintypes.DWORD
    _kernel32.SetThreadExecutionState.argtypes = (wintypes.DWORD,)
    _WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    _user32.EnumWindows.argtypes = (_WNDENUMPROC, wintypes.LPARAM)
    _user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    _user32.IsWindowVisible.argtypes = (wintypes.HWND,)
    _user32.GetWindow.restype = wintypes.HWND
    _user32.GetWindow.argtypes = (wintypes.HWND, wintypes.UINT)
    _user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    _user32.SwitchToThisWindow.argtypes = (wintypes.HWND, wintypes.BOOL)
    _user32.mouse_event.argtypes = (wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t)
    _psapi = ctypes.WinDLL("psapi", use_last_error=True)
    _psapi.EnumProcesses.argtypes = (ctypes.POINTER(wintypes.DWORD), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD))
    _kernel32.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
    _kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    _kernel32.CreateJobObjectW.argtypes = (wintypes.LPVOID, wintypes.LPCWSTR)
    _kernel32.SetInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD)
    _kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
    _kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    _kernel32.SetPriorityClass.argtypes = (wintypes.HANDLE, wintypes.DWORD)

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_SET_INFORMATION = 0x0200
ABOVE_NORMAL_PRIORITY_CLASS = 0x8000
STILL_ACTIVE = 259
WM_CLOSE = 0x0010
GW_OWNER = 4


def find(image):
    """PIDs of running processes with this image name."""
    if not IS_WINDOWS:
        return []
    try:
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    pids = []
    for row in csv.reader(io.StringIO(out)):
        if len(row) >= 2 and row[0].lower() == image.lower():
            try:
                pids.append(int(row[1]))
            except ValueError:
                pass
    return pids


def image_of(pid):
    """The image name of a running process, or None."""
    if not IS_WINDOWS:
        return None
    try:
        out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    for row in csv.reader(io.StringIO(out)):
        if len(row) >= 2 and row[1] == str(pid):
            return row[0]
    return None


def alive(pid):
    if not pid:
        return False
    if not IS_WINDOWS:
        return False
    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not handle:
        return False
    try:
        code = wintypes.DWORD()
        if not _kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return False
        return code.value == STILL_ACTIVE
    finally:
        _kernel32.CloseHandle(handle)


def raise_priority(pid):
    """Puts a process in the above-normal priority class. Returns whether it worked."""
    if not IS_WINDOWS or not pid:
        return False
    handle = _kernel32.OpenProcess(PROCESS_SET_INFORMATION, False, int(pid))
    if not handle:
        return False
    try:
        return bool(_kernel32.SetPriorityClass(handle, ABOVE_NORMAL_PRIORITY_CLASS))
    finally:
        _kernel32.CloseHandle(handle)


def kill(pid):
    if IS_WINDOWS and pid:
        # A game stuck in a GPU teardown can take longer than this to die: the kill is still requested, and the caller's
        # wait for the process to go is what decides.
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            print(f"taskkill of {pid} did not return in 30 s; carrying on", flush=True)


def windows_of(pid):
    """Visible top-level windows without an owner (main windows) of a process."""
    if not IS_WINDOWS:
        return []
    found = []

    def callback(hwnd, _):
        owner = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and _user32.IsWindowVisible(hwnd) and not _user32.GetWindow(hwnd, GW_OWNER):
            found.append(hwnd)
        return True

    _user32.EnumWindows(_WNDENUMPROC(callback), 0)
    return found


def close_windows(pid):
    """Asks a process to close, as CloseMainWindow does. Returns whether it had a window to ask."""
    windows = windows_of(pid)
    for hwnd in windows:
        _user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    return bool(windows)


def focus(pid):
    """Brings the process's main window forward once. Returns whether it found one."""
    windows = windows_of(pid)
    if windows:
        _user32.SwitchToThisWindow(windows[0], True)
    return bool(windows)


def keep_display_awake(on):
    """Keeps the system awake during a run. The displays may sleep: on 2026-11-27 perf-taa ran at the same frame rate
    with the monitors off (D3) and on (D0). CS_TEST_KEEP_DISPLAY_ON=1 also keeps the displays on, as runs used to."""
    if not IS_WINDOWS:
        return
    ES_CONTINUOUS, ES_SYSTEM_REQUIRED, ES_DISPLAY_REQUIRED = 0x80000000, 0x1, 0x2
    display = ES_DISPLAY_REQUIRED if os.environ.get("CS_TEST_KEEP_DISPLAY_ON") == "1" else 0
    _kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | display if on else ES_CONTINUOUS)


def nudge_mouse():
    """A zero-length mouse move wakes displays that already went to sleep."""
    if IS_WINDOWS:
        _user32.mouse_event(0x0001, 0, 0, 0, 0)


def wait_gone(pids, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if not any(alive(p) for p in pids):
            return True
        time.sleep(0.5)
    return not any(alive(p) for p in pids)


def dump_before_kill(pids, helper, out_dir, log=print, timeout_s=180):
    """Writes every thread's stack and a minidump of each game process (CommunityShadersCrashDump.exe --pid) before it is killed,
    so a run the harness ends still leaves a hang report. Returns the files written."""
    written = []
    pids = [p for p in pids if alive(p)]
    if not pids or not helper or not Path(helper).exists() or not out_dir:
        return written
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    for pid in pids:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        text = Path(out_dir) / f"CommunityShaders-Kill-{stamp}-{pid}.txt"
        dump = Path(out_dir) / f"CommunityShaders-Kill-{stamp}-{pid}.dmp"
        log(f"Writing the game's stacks and a minidump before the kill: {text.name}")
        try:
            subprocess.run([str(helper), "--pid", str(pid), str(text), str(dump)], capture_output=True, timeout=timeout_s)
        except subprocess.TimeoutExpired:
            log(f"The dump of {pid} did not finish in {timeout_s} s; killing anyway")
        written += [p.name for p in (text, dump) if p.exists()]
    return written


def stop(pids, log=print, wait_s=15):
    """Kills these processes (and their children) by PID only, never by name: other lab slots run games too. Returns whether all
    are gone."""
    pids = [p for p in pids if alive(p)]
    for pid in pids:
        kill(pid)
    if pids and not wait_gone(pids, wait_s):
        log(f"Still running after the kill: {', '.join(str(p) for p in pids if alive(p))}")
        return False
    return True


def image_path(pid):
    """The full path of a running process's image, or None."""
    if not IS_WINDOWS:
        return None
    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not handle:
        return None
    try:
        size = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(size.value)
        if not _kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return None
        return buffer.value
    finally:
        _kernel32.CloseHandle(handle)


def all_pids():
    if not IS_WINDOWS:
        return []
    count = 4096
    while True:
        array = (wintypes.DWORD * count)()
        needed = wintypes.DWORD()
        if not _psapi.EnumProcesses(array, ctypes.sizeof(array), ctypes.byref(needed)):
            return []
        if needed.value < ctypes.sizeof(array):
            return [array[i] for i in range(needed.value // ctypes.sizeof(wintypes.DWORD)) if array[i]]
        count *= 2


def pids_of(exe):
    """PIDs of running processes started from this exact file (a lab slot's SkyrimSE.exe)."""
    want = str(Path(exe)).lower()
    return [pid for pid in all_pids() if (image_path(pid) or "").lower() == want]


_job = None


def kill_children_with_this_process():
    """Puts this process in a job that kills every process in it when the last handle closes: when cs-test ends, however it
    ends, the game it started (through skse64_loader.exe, a child) ends too."""
    global _job
    if not IS_WINDOWS or _job is not None:
        return

    class BasicLimits(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64), ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t), ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in ("ReadOps", "WriteOps", "OtherOps", "ReadBytes", "WriteBytes", "OtherBytes")]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BasicLimits), ("IoInfo", IoCounters), ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
    JobObjectExtendedLimitInformation = 9
    job = _kernel32.CreateJobObjectW(None, None)
    if not job:
        raise OSError(ctypes.get_last_error(), "CreateJobObjectW failed")
    limits = ExtendedLimits()
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not _kernel32.SetInformationJobObject(job, JobObjectExtendedLimitInformation, ctypes.byref(limits), ctypes.sizeof(limits)):
        raise OSError(ctypes.get_last_error(), "SetInformationJobObject failed")
    if not _kernel32.AssignProcessToJobObject(job, _kernel32.GetCurrentProcess()):
        raise OSError(ctypes.get_last_error(), "AssignProcessToJobObject failed")
    _job = job
