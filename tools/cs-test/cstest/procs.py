"""Processes and windows on Windows, with ctypes (no psutil)."""

import csv
import io
import subprocess
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

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
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


def kill(pid):
    if IS_WINDOWS and pid:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True, timeout=30)


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
    """With the displays off, Present throttles to a few frames a second (cs-ab.ps1 did the same)."""
    if not IS_WINDOWS:
        return
    ES_CONTINUOUS, ES_SYSTEM_REQUIRED, ES_DISPLAY_REQUIRED = 0x80000000, 0x1, 0x2
    _kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED if on else ES_CONTINUOUS)


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


def stop_game_and_mo2(game_image, mo2_image, log=print, close_s=10):
    """Kills the game, then closes MO2 politely and kills it if it stays. Returns once neither runs."""
    games = find(game_image)
    for pid in games:
        kill(pid)
    if games:
        wait_gone(games, 15)
    mo2 = find(mo2_image)
    if mo2:
        for pid in mo2:
            close_windows(pid)
        if not wait_gone(mo2, close_s):
            log(f"MO2 did not close in {close_s} s; killing it")
            for pid in find(mo2_image):
                kill(pid)
            wait_gone(mo2, 15)
    # MO2 writes the mod list as it exits: give the file system a moment before anyone edits it.
    time.sleep(1)
    return not find(game_image) and not find(mo2_image)
