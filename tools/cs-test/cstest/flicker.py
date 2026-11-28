"""Bursts: consecutive rendered frames (the game's `burst` step), scored for frame-to-frame change and flicker.

The game reduces each frame of a burst to a grid of cell lumas (240x135 cells, 8x8 pixels at 1920x1080; burst.json and
grid.bin in <run>/bursts/<name>). Here the cells are grouped into tiles (TILE_X x TILE_Y cells, 40x40 pixels at 1080p) and two
measures are taken per tile, in 8-bit luma:

- change: the mean absolute change of the tile's cells from one frame to the next. Motion, noise and flicker all raise it.
- flicker: a jump that comes back. For a cell, frames f..f+k-1 (k = 1 or 2, a one- or two-frame hold) against the frames
  before (b) and after (a): when the held value s is above both or below both, the spike is the smaller of |s - b| and
  |s - a|, less |a - b| (what did not come back). Motion and eye adaptation move one way, so they score 0; noise (TAA
  jitter, water ripples) scores a little everywhere. A tile's spike at a frame is its cells' mean spike.

A tile flickers at a frame when its spike is over SPIKE (the event threshold). The burst's numbers are the worst tile spike,
the tiles with events in at least two frames (repeated flicker), the median tile spike (the noise floor), the frame-mean
spike (whole-frame pumping), and the frame times before and during the burst (what capturing cost).
`expect.flicker` fails a run whose burst is over its limits (see check).
"""

import json
import statistics
from array import array
from pathlib import Path

TILE_X, TILE_Y = 5, 5  # cells per tile: 48x27 tiles, 40x40 pixels at 1920x1080
HOLDS = (1, 2)  # frames a flicker may hold before it comes back
SPIKE = 6.0  # a tile spike over this (8-bit luma) is a flicker event
WORST = 8  # tiles listed in the result
EXPECT_FIELDS = {"max_spike": (int, float), "max_tiles": int, "min_frames": int}


def bursts_in(run_dir):
    """The bursts a run wrote: their names, sorted."""
    folder = Path(run_dir) / "bursts"
    return sorted(p.parent.name for p in folder.glob("*/burst.json")) if folder.exists() else []


def expected_bursts(contract):
    """The bursts the config's steps ask for."""
    return [step["arg"].split(":")[0] for step in contract.get("steps", []) if step.get("do") == "burst"]


def load(burst_dir):
    """(meta, frames, grids): the frames with a grid, and their grids as lists of floats (8-bit luma)."""
    burst_dir = Path(burst_dir)
    meta = json.loads((burst_dir / "burst.json").read_text(encoding="utf-8"))
    width, height = meta["cells"]
    cells = width * height
    raw = array("H")
    raw.frombytes((burst_dir / "grid.bin").read_bytes())
    frames = [f for f in meta["frames"] if f.get("grid", -1) >= 0]
    grids = []
    for frame in frames:
        start = frame["grid"] * cells
        grids.append([v / 256.0 for v in raw[start:start + cells]])
    return meta, frames, grids


def _tile_index(width, height):
    """Per cell, its tile; and the tile grid's size."""
    tiles_x, tiles_y = -(-width // TILE_X), -(-height // TILE_Y)
    index = [(i % width) // TILE_X + ((i // width) // TILE_Y) * tiles_x for i in range(width * height)]
    counts = [0] * (tiles_x * tiles_y)
    for t in index:
        counts[t] += 1
    return index, tiles_x, tiles_y, counts


def _tile_means(values, index, counts):
    sums = [0.0] * len(counts)
    for t, v in zip(index, values):
        sums[t] += v
    return [s / c for s, c in zip(sums, counts)]


def _spikes(before, held, after):
    """Per cell: the held value's jump that came back (see the module text)."""
    out = []
    for b, s, a in zip(before, held, after):
        d1, d2 = s - b, s - a
        if d1 * d2 <= 0:
            out.append(0.0)
            continue
        spike = min(abs(d1), abs(d2)) - abs(a - b)
        out.append(spike if spike > 0 else 0.0)
    return out


def _spike1(b, s, a):
    d1, d2 = s - b, s - a
    if d1 * d2 <= 0:
        return 0.0
    return max(0.0, min(abs(d1), abs(d2)) - abs(a - b))


def _median(values):
    return statistics.median(values) if values else 0.0


def analyse(burst_dir):
    """A burst's measures (a dict), for result.json."""
    meta, frames, grids = load(burst_dir)
    width, height = meta["cells"]
    px_w, px_h = meta.get("width", 0) or width * 8, meta.get("height", 0) or height * 8
    index, tiles_x, tiles_y, counts = _tile_index(width, height)
    n = len(grids)
    presents = [f["present"] for f in frames]
    consecutive = sum(1 for a, b in zip(presents, presents[1:]) if b - a == 1)
    intervals = [b["t_ms"] - a["t_ms"] for a, b in zip(frames, frames[1:]) if b["present"] - a["present"] == 1]
    burst_ms = _median(intervals)
    pre_ms = meta.get("pre_frame_ms", 0.0)
    result = {
        "frames": n,
        "asked": meta.get("count", 0),
        "consecutive_pairs": consecutive,
        "skipped": len(meta.get("skipped", [])),
        "skipped_why": sorted({s["why"] for s in meta.get("skipped", [])}),
        "repeats": sum(1 for f in frames if f.get("repeat")),
        "snapshots": len({f["snapshot"] for f in frames}),
        "source": meta.get("source", ""),
        "pre_frame_ms": round(pre_ms, 2),
        "burst_frame_ms": round(burst_ms, 2),
        "slowdown": round(burst_ms / pre_ms, 2) if pre_ms > 0 and burst_ms > 0 else None,
    }
    if n < 3:
        result["note"] = "fewer than 3 frames with a grid"
        return result

    def where(tile):
        tx, ty = tile % tiles_x, tile // tiles_x
        x0, y0 = tx * TILE_X * px_w // width, ty * TILE_Y * px_h // height
        x1, y1 = min(px_w, (tx + 1) * TILE_X * px_w // width), min(px_h, (ty + 1) * TILE_Y * px_h // height)
        return {"x": [x0, x1], "y": [y0, y1]}

    def frame_label(i):
        f = frames[i]
        return {"index": f["index"], "present": f["present"], "render_frame": f["render_frame"], "snapshot": f["snapshot"], "repeat": f["repeat"]}

    # Change: per tile, the mean over frame pairs of its cells' mean absolute change.
    tiles = tiles_x * tiles_y
    change_sum = [0.0] * tiles
    change_max = [0.0] * tiles
    change_frame = [0] * tiles
    pair_change = []
    for i in range(n - 1):
        diff = _tile_means([abs(b - a) for a, b in zip(grids[i], grids[i + 1])], index, counts)
        pair_change.append(sum(diff) / tiles)
        for t, v in enumerate(diff):
            change_sum[t] += v
            if v > change_max[t]:
                change_max[t], change_frame[t] = v, i + 1
    change_mean = [s / (n - 1) for s in change_sum]

    # Flicker: per tile and frame, the largest hold's spike.
    spike = [[0.0] * tiles for _ in range(n)]
    for k in HOLDS:
        for f in range(1, n - k):
            held = grids[f] if k == 1 else [sum(v) / k for v in zip(*grids[f:f + k])]
            tile_spikes = _tile_means(_spikes(grids[f - 1], held, grids[f + k]), index, counts)
            row = spike[f]
            for t, v in enumerate(tile_spikes):
                if v > row[t]:
                    row[t] = v
    events = [[f for f in range(n) if spike[f][t] > SPIKE] for t in range(tiles)]
    peak = [max(spike[f][t] for f in range(n)) for t in range(tiles)]
    peak_frame = [max(range(n), key=lambda f, t=t: spike[f][t]) for t in range(tiles)]
    flagged = [t for t in range(tiles) if len(events[t]) >= 2]
    worst_tile = max(range(tiles), key=lambda t: peak[t])
    # The frame mean's own spike: the whole frame pumping.
    means = [sum(g) / len(g) for g in grids]
    global_spike = max([_spike1(means[f - 1], means[f], means[f + 1]) for f in range(1, n - 1)] or [0.0])
    order = sorted(range(tiles), key=lambda t: -peak[t])[:WORST]
    result.update({
        "tiles": [tiles_x, tiles_y],
        "change": {"mean": round(sum(change_mean) / tiles, 2), "p99_tile": round(sorted(change_mean)[int(0.99 * (tiles - 1))], 2),
                   "max_tile": round(max(change_mean), 2), "max_pair": round(max(pair_change), 2)},
        "noise": round(_median([spike[f][t] for f in range(1, n - 1) for t in range(0, tiles, 7)]), 2),
        "max_spike": round(peak[worst_tile], 2),
        "max_spike_at": {**where(worst_tile), "frame": frame_label(peak_frame[worst_tile])},
        "events": sum(len(e) for e in events),
        "flicker_tiles": len(flagged),
        "global_spike": round(global_spike, 2),
        "worst": [{**where(t), "spike": round(peak[t], 2), "frame": frames[peak_frame[t]]["index"], "events": len(events[t]), "frames": [frames[f]["index"] for f in events[t]][:12],
                   "change": round(change_mean[t], 2)} for t in order],
        "map": _map(peak, events, tiles_x, tiles_y),
    })
    return result


def _map(peak, events, tiles_x, tiles_y):
    """A text map of the tiles: '.' quiet, '+' one event, '#' flickering (two or more), 'o' spike over half the threshold."""
    rows = []
    for ty in range(tiles_y):
        line = ""
        for tx in range(tiles_x):
            t = ty * tiles_x + tx
            line += "#" if len(events[t]) >= 2 else "+" if events[t] else "o" if peak[t] > SPIKE / 2 else "."
        rows.append(line)
    return rows


def prune_pngs(burst_dir, found, keep_worst=3):
    """Deletes a burst's PNGs but the first frame's and those around its worst tiles' spikes (the frame before, the spike and the
    two after). Returns the kept indices."""
    burst_dir = Path(burst_dir)
    keep = {0}
    for w in found.get("worst", [])[:keep_worst]:
        keep.update(range(w["frame"] - 1, w["frame"] + 3))
    for png in burst_dir.glob("f*.png"):
        if png.stem[1:].isdigit() and int(png.stem[1:]) not in keep:
            png.unlink(missing_ok=True)
    return sorted(i for i in keep if (burst_dir / f"f{i:03}.png").exists())


def check_expect(expect):
    """expect.flicker's fields and types; returns the error text, or ''."""
    for key, value in expect.items():
        if key not in EXPECT_FIELDS:
            return f"unknown key 'expect.flicker.{key}' (known: {', '.join(EXPECT_FIELDS)})"
        want = EXPECT_FIELDS[key]
        if isinstance(value, bool) or not isinstance(value, want) or value < 0:
            return f"expect.flicker.{key} takes a number of at least 0, not {value!r}"
    return ""


def check_run(contract, harness, run_dir):
    """Every burst of a run analysed, and expect.flicker checked. "failures" is empty when it held."""
    expected = expected_bursts(contract)
    written = bursts_in(run_dir)
    result = {"bursts": {}, "expect": dict(harness.get("flicker") or {}), "failures": []}
    if not expected and not written:
        return result
    for name in expected:
        if name not in written:
            result["failures"].append(f"burst {name} was not written")
    limits = result["expect"]
    for name in written:
        try:
            found = analyse(Path(run_dir) / "bursts" / name)
        except (OSError, ValueError, KeyError) as error:
            result["failures"].append(f"burst {name} unreadable: {error}")
            continue
        found["pngs_kept"] = prune_pngs(Path(run_dir) / "bursts" / name, found)
        result["bursts"][name] = found
        if "min_frames" in limits and found["frames"] < limits["min_frames"]:
            result["failures"].append(f"burst {name}: {found['frames']} frames, at least {limits['min_frames']} expected")
        if "max_spike" in limits and found.get("max_spike", 0) > limits["max_spike"]:
            at = found["max_spike_at"]
            result["failures"].append(f"burst {name}: flicker spike {found['max_spike']} over {limits['max_spike']} at x {at['x'][0]}-{at['x'][1]}, "
                                      f"y {at['y'][0]}-{at['y'][1]}, frame {at['frame']['index']}")
        if "max_tiles" in limits and found.get("flicker_tiles", 0) > limits["max_tiles"]:
            result["failures"].append(f"burst {name}: {found['flicker_tiles']} tiles flicker in two or more frames, at most {limits['max_tiles']}")
    return result


def text(result):
    """The summary line's clause: one part per burst."""
    parts = []
    for name, found in result.get("bursts", {}).items():
        if "max_spike" not in found:
            parts.append(f"burst {name}: {found['frames']} frames")
            continue
        at = found["max_spike_at"]
        slow = f", x{found['slowdown']} frame time" if found.get("slowdown") else ""
        parts.append(f"burst {name}: spike {found['max_spike']} at {at['x'][0]},{at['y'][0]} f{at['frame']['index']}, "
                     f"{found['flicker_tiles']} flickering tiles, change {found['change']['mean']}{slow}")
    return "; ".join(parts)


def lines(name, found):
    """Lines for a person: the measures and the tile map."""
    out = [f"burst {name}: {found['frames']} frames ({found['consecutive_pairs']} consecutive pairs, {found['repeats']} repeats, "
           f"{found['snapshots']} snapshots, {found['skipped']} skipped {found['skipped_why']}), "
           f"frame {found['pre_frame_ms']} ms before, {found['burst_frame_ms']} ms during"]
    if "max_spike" not in found:
        return out
    at = found["max_spike_at"]
    out.append(f"  spike max {found['max_spike']} at x {at['x']}, y {at['y']}, frame {at['frame']}; noise {found['noise']}, "
               f"events {found['events']}, flickering tiles {found['flicker_tiles']}, frame-mean spike {found['global_spike']}")
    out.append(f"  change mean {found['change']['mean']}, p99 tile {found['change']['p99_tile']}, max tile {found['change']['max_tile']}, "
               f"max pair {found['change']['max_pair']}")
    for w in found["worst"]:
        out.append(f"  tile x {w['x']}, y {w['y']}: spike {w['spike']}, {w['events']} events {w['frames']}, change {w['change']}")
    out += [f"  {row}" for row in found["map"]]
    return out


def main(argv=None):
    """python -m cstest.flicker <run dir or burst dir>...: prints each burst's measures."""
    import sys

    for target in (argv if argv is not None else sys.argv[1:]):
        path = Path(target)
        dirs = [path] if (path / "burst.json").exists() else [path / "bursts" / n for n in bursts_in(path)]
        for d in dirs:
            print("\n".join(lines(f"{path.name}/{d.name}" if d != path else d.name, analyse(d))))


if __name__ == "__main__":
    main()
