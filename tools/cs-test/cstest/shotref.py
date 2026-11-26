"""Reference shots: a run's shots compared with kept references, so a run whose pictures are grossly wrong fails.

The comparison is coarse on purpose. Two runs of one build differ by 9-24 mean (8-bit) in busy scenes: NPC poses, a
camera turned in a fight, wind, particles, eye adaptation. A wrong frame differs in structure: the world in a corner, a
black or white frame, a missing world behind the UI, a large brightness change. So each shot becomes a grid of cells
(CELLS_X x CELLS_Y, each the lightness of its pixels' mean luma), grouped into tiles (TILES_X x TILES_Y). Per tile the
check compares the mean and the detail (the spread of its cells). A tile is wrong when its mean moved far, or its detail
collapsed ("flat") or appeared ("busy"). A shot fails when too many tiles are wrong, or its mean lightness moved far.

References are PNGs in <root>/refs, named by their sha256. tools/cs-test/refs.json, in the repo, maps
"<run name>/<shot>" to the sha256 and the run it came from. "cs_test.py ref accept <run>" adds or replaces them;
nothing accepts a reference by itself.
"""

import datetime
import hashlib
import json
import math
import shutil
import zlib
from dataclasses import dataclass
from pathlib import Path

import shotdiff  # tools/cs-test, beside cs_test.py: the standard library PNG reader

from .collect import shots_in

CELLS_X, CELLS_Y = 96, 54  # 20x20 pixel cells at 1920x1080
TILES_X, TILES_Y = 16, 9  # each tile is 6x6 cells

# Calibrated on 2026-11-26's runs (snap3-shotref.md), in lightness: 741 pairs of good shots of one config (combat, camera
# turns, NPCs, eye adaptation) and 525 pairs of a good shot and a quarter-frame one. No bad pair passes. 11 good pairs
# fail, all between builds that really differ (a darker stats menu, Whiterun's exposure after a coc); the worst same-scene
# pair has 22 wrong tiles, the best bad pair 45.
TILE_MEAN_OFF = 56.0  # a tile whose mean moved more than this is wrong ("dark" or "bright")
DETAIL_MIN = 4.0  # detail below this is flat: wrong when the other side has DETAIL_RATIO times this and times its detail
DETAIL_RATIO = 2.5
WRONG_TILES = 0.23  # more than this share of wrong tiles (33 of 144) fails a shot
LIGHTNESS_OFF = 20.0  # the whole shot's mean lightness may move this much


@dataclass
class Signature:
    width: int
    height: int
    cells: list  # CELLS_Y rows of CELLS_X lightness values, 0-255: 255 * sqrt(mean luma / 255)


def signature(path):
    """The shot's cell grid: the lightness of each cell's mean luma (Rec. 601)."""
    width, height, rows = shotdiff.read_rgb(path)
    if width < CELLS_X or height < CELLS_Y:
        raise ValueError(f"{path}: {width}x{height} is smaller than the {CELLS_X}x{CELLS_Y} cell grid")
    xs = [x * width // CELLS_X for x in range(CELLS_X + 1)]
    ys = [y * height // CELLS_Y for y in range(CELLS_Y + 1)]
    cells = []
    for cy in range(CELLS_Y):
        sums = [0.0] * CELLS_X
        for row in rows[ys[cy]:ys[cy + 1]]:
            for cx in range(CELLS_X):
                a, b = xs[cx] * 3, xs[cx + 1] * 3
                sums[cx] += 0.299 * sum(row[a:b:3]) + 0.587 * sum(row[a + 1:b:3]) + 0.114 * sum(row[a + 2:b:3])
        count = ys[cy + 1] - ys[cy]
        # Lightness, not luma: the square root spreads the dark range, where most of a dim scene's detail is.
        cells.append([math.sqrt(s / (count * (xs[cx + 1] - xs[cx])) / 255.0) * 255.0 for cx, s in enumerate(sums)])
    return Signature(width, height, cells)


def _tile(cells, tx, ty):
    """(mean, detail) of a tile: its cells' mean and their standard deviation."""
    per_x, per_y = CELLS_X // TILES_X, CELLS_Y // TILES_Y
    values = [v for row in cells[ty * per_y:(ty + 1) * per_y] for v in row[tx * per_x:(tx + 1) * per_x]]
    mean = sum(values) / len(values)
    return mean, math.sqrt(sum((v - mean) ** 2 for v in values) / len(values))


def _correlation(a, b):
    fa = [v for row in a for v in row]
    fb = [v for row in b for v in row]
    ma, mb = sum(fa) / len(fa), sum(fb) / len(fb)
    da, db = [v - ma for v in fa], [v - mb for v in fb]
    sa, sb = math.sqrt(sum(v * v for v in da)), math.sqrt(sum(v * v for v in db))
    if sa < 1e-6 or sb < 1e-6:
        return 1.0 if sa < 1e-6 and sb < 1e-6 else 0.0
    return sum(x * y for x, y in zip(da, db)) / (sa * sb)


def _span(values, size, count):
    """Pixel range text for tile indices along one axis."""
    return f"{min(values) * size // count}-{(max(values) + 1) * size // count}"


def compare(ref, shot):
    """The shot against its reference. Returns a dict: the measures, the tile map, and "failures" (empty when it passed)."""
    if (ref.width, ref.height) != (shot.width, shot.height):
        return {"failures": [f"size {shot.width}x{shot.height}, reference {ref.width}x{ref.height}"]}
    light_ref = sum(map(sum, ref.cells)) / (CELLS_X * CELLS_Y)
    light = sum(map(sum, shot.cells)) / (CELLS_X * CELLS_Y)
    wrong = []  # (tx, ty, why, mean ref, mean shot, detail ref, detail shot)
    rows = []
    for ty in range(TILES_Y):
        line = ""
        for tx in range(TILES_X):
            mr, dr = _tile(ref.cells, tx, ty)
            ms, ds = _tile(shot.cells, tx, ty)
            why = ""
            if abs(ms - mr) > TILE_MEAN_OFF:
                why = "dark" if ms < mr else "bright"
            elif ds < DETAIL_MIN and dr >= DETAIL_RATIO * max(DETAIL_MIN, ds):
                why = "flat"
            elif dr < DETAIL_MIN and ds >= DETAIL_RATIO * max(DETAIL_MIN, dr):
                why = "busy"
            if why:
                wrong.append((tx, ty, why, mr, ms, dr, ds))
            line += {"dark": "D", "bright": "B", "flat": "F", "busy": "X"}.get(why, ".")
        rows.append(line)
    tiles = TILES_X * TILES_Y
    # The correlation is reported, not judged: a camera that turns in a fight moves it as much as a wrong frame does.
    result = {"lightness": round(light, 1), "lightness_ref": round(light_ref, 1), "correlation": round(_correlation(ref.cells, shot.cells), 3),
              "wrong_tiles": len(wrong), "tiles": tiles, "map": rows, "failures": []}
    failures = result["failures"]
    if light < 20 and light_ref >= 50:  # mean luma under about 1.6
        failures.append(f"black frame (mean lightness {light:.1f}, reference {light_ref:.1f})")
    elif light > 250 and light_ref <= 235:  # mean luma over about 245
        failures.append(f"white frame (mean lightness {light:.1f}, reference {light_ref:.1f})")
    elif abs(light - light_ref) > LIGHTNESS_OFF:
        failures.append(f"mean lightness {light:.1f}, reference {light_ref:.1f} (at most {LIGHTNESS_OFF:.0f} apart)")
    if len(wrong) > WRONG_TILES * tiles:
        kinds = {}
        for w in wrong:
            kinds.setdefault(w[2], []).append(w)
        parts = []
        for why, group in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
            xs, ys = [w[0] for w in group], [w[1] for w in group]
            where = f"x {_span(xs, shot.width, TILES_X)}, y {_span(ys, shot.height, TILES_Y)}"
            if why in ("dark", "bright"):
                off = sum(abs(w[4] - w[3]) for w in group) / len(group)
                parts.append(f"{len(group)} {why} by {off:.0f} ({where})")
            else:
                dr, ds = (sum(w[i] for w in group) / len(group) for i in (5, 6))
                parts.append(f"{len(group)} {why} (detail {ds:.1f}, reference {dr:.1f}; {where})")
        failures.append(f"{len(wrong)} of {tiles} tiles wrong: " + ", ".join(parts))
    return result


# The manifest (paths.refs_manifest, tools/cs-test/refs.json) and the store (paths.refs, <root>/refs).

def load_manifest(paths):
    return json.loads(paths.refs_manifest.read_text(encoding="utf-8")) if paths.refs_manifest.exists() else {}


def save_manifest(paths, manifest):
    paths.refs_manifest.write_text(json.dumps(dict(sorted(manifest.items())), indent=2) + "\n", encoding="utf-8", newline="\n")


def reference_path(paths, entry):
    return paths.refs / f"{entry['sha256']}.png"


def run_name(run_dir):
    """The run's config name ("native-vulkan+quick"), from its resolved.json."""
    return json.loads((Path(run_dir) / "resolved.json").read_text(encoding="utf-8"))["name"]


def accept(paths, run_dir, shots=None, log=print):
    """Copies a passed run's shots (all, or the named ones) into the store and points the manifest at them. Returns the keys."""
    run_dir = Path(run_dir)
    result = json.loads((run_dir / "result.json").read_text(encoding="utf-8")) if (run_dir / "result.json").exists() else {}
    if result.get("outcome") != "passed":
        raise ValueError(f"{run_dir.name}: only a passed run's shots can be references (outcome {result.get('outcome', 'unknown')})")
    name = run_name(run_dir)
    available = [s for s in shots_in(run_dir) if (run_dir / "shots" / f"{s}.png").exists()]
    chosen = shots or available
    missing = [s for s in chosen if s not in available]
    if missing:
        raise ValueError(f"{run_dir.name} has no shot {', '.join(missing)} (it has {', '.join(available) or 'none'})")
    manifest = load_manifest(paths)
    paths.refs.mkdir(parents=True, exist_ok=True)
    keys = []
    for shot in chosen:
        source = run_dir / "shots" / f"{shot}.png"
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        target = paths.refs / f"{digest}.png"
        if not target.exists():
            shutil.copy2(source, target)
        sig = signature(source)
        key = f"{name}/{shot}"
        manifest[key] = {"sha256": digest, "size": f"{sig.width}x{sig.height}", "run": run_dir.name,
                         "accepted": datetime.date.today().isoformat()}
        keys.append(key)
        log(f"accepted {key}: {digest[:12]} from {run_dir.name}")
    save_manifest(paths, manifest)
    return keys


def check_shot(paths, entry, shot_path):
    """One shot against its manifest entry. A reference missing from the store fails; it is never skipped."""
    ref = reference_path(paths, entry)
    if not ref.exists():
        return {"failures": [f"reference {ref.name} is not in {ref.parent} (accept it again from {entry.get('run', '?')})"]}
    try:
        return compare(signature(ref), signature(shot_path))
    except (OSError, ValueError, zlib.error) as error:
        return {"failures": [f"unreadable: {error}"]}


def check_run(paths, wanted, name, run_dir):
    """expect.shot_refs (wanted): each of those shots is compared with its reference. "failures" is empty when they held.
    A shot without a reference is listed in "unreferenced", not failed: "ref accept" adds it."""
    if not wanted:
        return {"checked": False}
    manifest = load_manifest(paths)
    result = {"checked": True, "shots": {}, "unreferenced": [], "failures": []}
    for shot in wanted:
        path = Path(run_dir) / "shots" / f"{shot}.png"
        entry = manifest.get(f"{name}/{shot}")
        if not path.exists():
            continue  # expect.shots reports a missing shot
        if not entry:
            result["unreferenced"].append(shot)
            continue
        found = check_shot(paths, entry, path)
        result["shots"][shot] = {**found, "reference": entry["sha256"], "reference_run": entry.get("run", "")}
        result["failures"] += [f"shot {shot}: {f}" for f in found["failures"]]
    return result


def text(result):
    """Lines for a person: the measures and the tile map."""
    if "map" not in result:
        return list(result.get("failures", []))
    lines = [f"mean lightness {result['lightness']} (reference {result['lightness_ref']}), cell correlation {result['correlation']}, "
             f"{result['wrong_tiles']} of {result['tiles']} tiles wrong (D dark, B bright, F flat, X busy)"]
    lines += [f"  {row}" for row in result["map"]]
    lines += [f"FAIL: {f}" for f in result["failures"]] or ["pass"]
    return lines
