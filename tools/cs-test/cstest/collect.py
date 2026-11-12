"""Collecting a run's files with bounded sizes, and reading its log once for error kinds.

A lost device once flooded the log to 12 GB, and copying it hung the old harness. Logs are copied in
chunks, and only their first and last 64 MB when they are bigger.
"""

import re
import shutil
from collections import Counter
from pathlib import Path

HEAD_TAIL = 64 * 1024 * 1024
CHUNK = 4 * 1024 * 1024
LEVEL = re.compile(r"^\[[0-9:.]+\] \[\d+\] \[(?P<level>[A-Z])\] (?P<message>.*)$")


def bounded_copy(src, dst, head=HEAD_TAIL, tail=HEAD_TAIL):
    """Copies src to dst whole if it is at most head + tail bytes, else its head, a note, and its tail.
    Returns (bytes in the source, whether it was cut)."""
    src, dst = Path(src), Path(dst)
    size = src.stat().st_size
    dst.parent.mkdir(parents=True, exist_ok=True)
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        if size <= head + tail:
            while chunk := fin.read(CHUNK):
                fout.write(chunk)
            return size, False
        left = head
        while left > 0 and (chunk := fin.read(min(CHUNK, left))):
            fout.write(chunk)
            left -= len(chunk)
        cut = size - head - tail
        fout.write(f"\n[cs-test] ... {cut} bytes cut: the log was {size} bytes; first and last {head // (1024 * 1024)} MB kept ...\n".encode())
        fin.seek(size - tail)
        while chunk := fin.read(CHUNK):
            fout.write(chunk)
    return size, True


def new_since(path, start_ts):
    """Whether a file was written at or after the run started (minus 2 s for file system time rounding)."""
    try:
        return Path(path).stat().st_mtime >= start_ts - 2
    except OSError:
        return False


NUMBERS = re.compile(r"0x[0-9a-fA-F]+|\b\d+(\.\d+)?\b")
PATHS = re.compile(r"[A-Za-z]:\\[^ '\"]*|'[^']*'")


def message_kind(message):
    """A message with its numbers, addresses and paths blanked, so repeats of one kind count together."""
    text = PATHS.sub("<path>", message)
    text = NUMBERS.sub("#", text)
    return text[:120]


def scan_log(path, limit_lines=5_000_000):
    """Error and warning counts by kind, the first error, and a device-fault address if one is logged."""
    counts = {"E": Counter(), "W": Counter(), "C": Counter()}
    first_error = ""
    fault = ""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f):
                if i >= limit_lines:
                    break
                m = LEVEL.match(line.rstrip("\n"))
                if not m:
                    continue
                level = m.group("level")
                if level not in counts:
                    continue
                message = m.group("message")
                counts[level][message_kind(message)] += 1
                if level in ("E", "C") and not first_error:
                    first_error = line.strip()
                if not fault and "fault" in message.lower():
                    address = re.search(r"0x[0-9a-fA-F]{6,}", message)
                    if address:
                        fault = address.group(0)
    except OSError:
        pass
    return {
        "first_error": first_error,
        "fault_address": fault,
        "errors": sum(counts["E"].values()) + sum(counts["C"].values()),
        "warnings": sum(counts["W"].values()),
        "error_kinds": dict((counts["E"] + counts["C"]).most_common(50)),
        "warning_kinds": dict(counts["W"].most_common(50)),
    }


def log_contains(path, needles):
    """Which of the substrings appear in the log."""
    found = set()
    if not needles:
        return found
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                for needle in needles:
                    if needle in line:
                        found.add(needle)
                if len(found) == len(needles):
                    break
    except OSError:
        pass
    return found


GAME_DIR = re.compile(r"Applied user settings from: (?P<dir>.+?)\\Data\\SKSE\\")
# SKSE-folder files a run writes, and whether they move (only this run needs them) or are copied.
SKSE_FILES = [
    (re.compile(r"^CS-AB-memory\.csv$"), "copy"),
    (re.compile(r"^CommunityShaders-VRAM-\d+\.json$"), "move"),
    (re.compile(r"^CommunityShaders-Hang-[\d-]+\.(txt|dmp)$"), "move"),
    (re.compile(r"^CommunityShaders-GpuCounters\.csv$"), "copy"),
    (re.compile(r"^CommunityShaders-DCLF-Compat-.*\.txt$"), "copy"),
    (re.compile(r"^CommunityShaders-Switches\.json$"), "copy"),
]
BOUNDED_LOGS = [re.compile(r"^CommunityShaders\.log$"), re.compile(r"^SkyrimSE_[a-z0-9]+\.log$")]


def game_dir_from_log(log):
    try:
        with open(log, encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f):
                m = GAME_DIR.search(line)
                if m:
                    return Path(m.group("dir"))
                if i > 2000:
                    break
    except OSError:
        pass
    return None


def collect(skse, run_dir, start_ts, game_dir=None, log=print):
    """Copies or moves this run's files from the SKSE folder (and the game folder's crash dump) into run_dir."""
    skse, run_dir = Path(skse), Path(run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    collected = {"logs": {}, "files": [], "cut": []}
    if not skse.exists():
        return collected
    for entry in sorted(skse.iterdir()):
        if not entry.is_file() or not new_since(entry, start_ts):
            continue
        name = entry.name
        if any(p.match(name) for p in BOUNDED_LOGS):
            try:
                size, cut = bounded_copy(entry, run_dir / name)
                collected["logs"][name] = size
                if cut:
                    collected["cut"].append(name)
                    log(f"{name} was {size // (1024 * 1024)} MB: kept its first and last {HEAD_TAIL // (1024 * 1024)} MB")
            except OSError as error:
                log(f"Could not copy {name}: {error}")
            continue
        for pattern, how in SKSE_FILES:
            if pattern.match(name):
                try:
                    if how == "move":
                        shutil.move(str(entry), str(run_dir / name))
                    else:
                        bounded_copy(entry, run_dir / name)
                    collected["files"].append(name)
                except OSError as error:
                    log(f"Could not collect {name}: {error}")
                break
    # The old DCLF shot command writes to <SKSE>\dclf-shots; the new runner writes to <run_dir>\shots.
    legacy = skse / "dclf-shots"
    if legacy.exists():
        for shot in legacy.iterdir():
            if shot.is_file() and new_since(shot, start_ts):
                (run_dir / "shots").mkdir(exist_ok=True)
                shutil.copy2(shot, run_dir / "shots" / shot.name)
    game_dir = game_dir or game_dir_from_log(run_dir / "CommunityShaders.log")
    if game_dir:
        # DXVK writes its logs beside the game unless DXVK_LOG_PATH moves them.
        for entry in Path(game_dir).glob("SkyrimSE_*.log"):
            if entry.name not in collected["logs"] and new_since(entry, start_ts):
                try:
                    collected["logs"][entry.name] = bounded_copy(entry, run_dir / entry.name)[0]
                except OSError as error:
                    log(f"Could not copy {entry}: {error}")
        dump = Path(game_dir) / "CommunityShaders.dmp"
        if dump.exists() and new_since(dump, start_ts):
            try:
                shutil.move(str(dump), str(run_dir / dump.name))
                collected["files"].append(dump.name)
            except OSError as error:
                log(f"Could not move {dump}: {error}")
    return collected


def shots_in(run_dir):
    folder = Path(run_dir) / "shots"
    if not folder.exists():
        return []
    return sorted(p.stem for p in folder.iterdir() if p.is_file() and p.suffix.lower() in (".png", ".jpg", ".dds", ".bmp"))
