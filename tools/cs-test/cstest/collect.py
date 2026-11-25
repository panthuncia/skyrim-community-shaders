"""Collecting a run's files with bounded sizes, and reading its log once for error kinds.

A lost device once flooded the log to 12 GB, and copying it hung the old harness. Logs are copied in
chunks, and only their first and last 64 MB when they are bigger.
"""

import json
import re
import shutil
import zlib
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


FGTEST = re.compile(r"^\[(?P<time>[0-9:.]+)\] .*\[FGTEST\] (?P<fps>\d+) fps, (?P<presents>\d+) FSR-FG presents")


def fgtest_seconds(path, day):
    """The [FGTEST] seconds in which FSR-FG presented, as (wall time, rendered fps). day: the run's start (epoch seconds)."""
    import datetime

    base = datetime.datetime.fromtimestamp(day).replace(hour=0, minute=0, second=0, microsecond=0)
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FGTEST.search(line)
                if m and int(m["presents"]) > 0:
                    h, mi, s = m["time"].split(":")
                    when = base + datetime.timedelta(hours=int(h), minutes=int(mi), seconds=float(s))
                    out.append((when.timestamp(), int(m["fps"])))
    except OSError:
        pass
    return out


def fgtest_rates(path):
    """Median rendered frames and FSR-FG presents per second over the [FGTEST] seconds in which FSR-FG presented."""
    rows = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FGTEST.search(line)
                if m and int(m["presents"]) > 0:
                    rows.append((int(m["fps"]), int(m["presents"])))
    except OSError:
        pass
    if not rows:
        return {"fgtest_seconds": 0}
    fps = sorted(r[0] for r in rows)
    presents = sorted(r[1] for r in rows)
    return {"fgtest_seconds": len(rows), "rendered_fps_median": fps[len(fps) // 2], "fsrfg_presents_median": presents[len(presents) // 2]}


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
    # Crash Logger's report, and the lab helper's log.
    (re.compile(r"^crash-[\d-]+\.log$"), "copy"),
    (re.compile(r"^CSTestLab\.log$"), "copy"),
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
    """Copies or moves this run's files from the slot's SKSE folder (and the slot's game folder: DXVK logs, the crash dump) into
    run_dir."""
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


def png_pixels(path):
    """A PNG's header and its decompressed image data. Two files with the same pixels and filters give the same bytes, whatever the
    compression did."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    header, idat, at = b"", [], 8
    while at + 8 <= len(data):
        length = int.from_bytes(data[at:at + 4], "big")
        kind = data[at + 4:at + 8]
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            header = body
        elif kind == b"IDAT":
            idat.append(body)
        at += 12 + length
    return header, zlib.decompress(b"".join(idat))


LIVE_SHOTS = ("_live-a", "_live-b")


def live_image(run_dir, expect):
    """expect.live_frames: the game's two shots that many frames apart must differ. A frozen final target gives the same image twice.
    Checked only for a run that expects the world. Returns what was found; "failure" is set when the check fails."""
    frames = expect.get("live_frames", 0)
    if not frames or expect.get("reach") != "world-ready":
        return {"checked": False}
    folder = Path(run_dir) / "shots"
    paths = [folder / f"{name}.png" for name in LIVE_SHOTS]
    missing = [p.stem for p in paths if not p.exists()]
    if missing:
        return {"checked": True, "frames": frames, "failure": f"live-image shots missing: {', '.join(missing)}"}
    try:
        a, b = (png_pixels(p) for p in paths)
    except (OSError, ValueError, zlib.error) as error:
        return {"checked": True, "frames": frames, "failure": f"live-image shots unreadable: {error}"}
    if a[0] != b[0]:
        return {"checked": True, "frames": frames, "same": False, "note": "the two shots differ in size or format"}
    changed = sum(1 for x, y in zip(a[1], b[1]) if x != y)
    result = {"checked": True, "frames": frames, "same": changed == 0, "changed_bytes": changed, "bytes": len(a[1])}
    if changed == 0:
        result["failure"] = f"the final image did not change in {frames} frames (_live-a and _live-b are the same image): a frozen final target"
    return result


def shots_in(run_dir):
    folder = Path(run_dir) / "shots"
    if not folder.exists():
        return []
    # The live-image shots are the check's own (live_image), not the config's.
    return sorted(p.stem for p in folder.iterdir() if p.is_file() and p.suffix.lower() in (".png", ".jpg", ".dds", ".bmp") and p.stem not in LIVE_SHOTS)


def final_status(run_dir, watched):
    """The game's last status: run_dir/status.json (copied after the exit) when it reads, else the one the watch saw."""
    try:
        return json.loads((Path(run_dir) / "status.json").read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return watched


def graph_frames(contract, status):
    """expect.graph_frames against the game's exit report (status "report.graph_frames"). "failures" is empty when it held."""
    expected = contract["expect"].get("graph_frames") or {}
    report = ((status or {}).get("report") or {}).get("graph_frames")
    result = {"expected": expected, "report": report, "failures": [], "skipped": ""}
    if not expected:
        result["skipped"] = "nothing expected"
        return result
    if contract.get("env", {}).get("CS_ORG") == "0":
        # Plain D3D11 mode: nothing runs on the graph.
        result["skipped"] = "skipped (CS_ORG=0)"
        return result
    if not isinstance(report, dict):
        result["failures"].append("no graph_frames report in the status file (a Release build or an older build)")
        return result
    clients = report.get("clients") or {}
    for client, least in expected.get("min", {}).items():
        frames = (clients.get(client) or {}).get("graph", 0)
        if frames < least:
            result["failures"].append(f"{client} ran on the graph in {frames} frames, at least {least} expected")
    return result
