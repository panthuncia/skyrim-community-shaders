"""Bursts (flicker.py) on synthetic grids: temp folders only."""

import json
import math
import random
from array import array

import pytest

from cstest import flicker
from cstest.config import resolve
from cstest.schema import ConfigError

W, H = 240, 135


def write_burst(folder, grids, repeats=()):
    """A burst folder as the game writes it, from grids (lists of W*H lumas, 0-255)."""
    folder.mkdir(parents=True)
    raw = array("H", [int(round(v * 256)) for g in grids for v in g])
    (folder / "grid.bin").write_bytes(raw.tobytes())
    frames = [{"index": i, "present": 1000 + i, "render_frame": 500 + i, "snapshot": 200 + i // 2, "renders_of_snapshot": 1, "repeat": i in repeats,
               "t_ms": i * 10.0, "grid": i} for i in range(len(grids))]
    (folder / "burst.json").write_text(json.dumps({"version": 1, "count": len(grids), "width": 1920, "height": 1080, "cells": [W, H],
                                                   "source": "kFRAMEBUFFER", "png": False, "pre_frame_ms": 8.0, "waited_frames": 3,
                                                   "frames": frames, "skipped": []}), encoding="utf-8")
    return folder


def base(x, y, t=0.0):
    return 90 + 40 * math.sin(x / 7.0 + t) * math.cos(y / 5.0)


def frames(n, noise=1.0, motion=0.0, drift=0.0, flick=None, seed=1):
    """n grids: a textured scene with TAA-like noise, an optional pan (motion cells per frame), a brightness drift per frame,
    and flick(i, x, y) added to a cell."""
    rng = random.Random(seed)
    out = []
    for i in range(n):
        g = []
        for y in range(H):
            for x in range(W):
                v = base(x, y, motion * i) + drift * i + rng.gauss(0, noise)
                if flick:
                    v += flick(i, x, y)
                g.append(min(255.0, max(0.0, v)))
        out.append(g)
    return out


def test_still_scene_with_noise_and_drift_does_not_flicker(tmp_path):
    found = flicker.analyse(write_burst(tmp_path / "b", frames(12, noise=1.5, drift=0.8)))
    assert found["frames"] == 12 and found["consecutive_pairs"] == 11
    assert found["max_spike"] < flicker.SPIKE and found["flicker_tiles"] == 0


def test_camera_motion_does_not_flicker(tmp_path):
    found = flicker.analyse(write_burst(tmp_path / "b", frames(10, noise=0.5, motion=0.3)))
    assert found["change"]["mean"] > 3
    assert found["flicker_tiles"] == 0


def test_a_b_a_tile_is_found_where_it_is(tmp_path):
    # Cells x 100-109, y 50-59 (pixels 800-880, 400-480) jump by 40 on every other frame.
    def flick(i, x, y):
        return 40.0 if i % 2 == 1 and 100 <= x < 110 and 50 <= y < 60 else 0.0
    found = flicker.analyse(write_burst(tmp_path / "b", frames(10, noise=1.0, flick=flick)))
    assert found["max_spike"] > 30
    at = found["max_spike_at"]
    assert 760 <= at["x"][0] <= 880 and 360 <= at["y"][0] <= 480
    assert found["flicker_tiles"] >= 4
    assert found["global_spike"] < 1.0


def test_two_frame_hold_is_found(tmp_path):
    def flick(i, x, y):
        return -50.0 if i in (4, 5) and x < 20 and y < 20 else 0.0
    found = flicker.analyse(write_burst(tmp_path / "b", frames(10, noise=1.0, flick=flick)))
    assert found["max_spike"] > 40


def test_check_run_limits(tmp_path):
    def flick(i, x, y):
        return 40.0 if i % 2 and x < 30 and y < 30 else 0.0
    write_burst(tmp_path / "bursts" / "water", frames(8, flick=flick))
    contract = {"steps": [{"do": "burst", "arg": "water:8"}, {"do": "burst", "arg": "sky:8:grid"}]}
    found = flicker.check_run(contract, {"flicker": {"max_spike": 10, "max_tiles": 2}}, tmp_path)
    assert "burst sky was not written" in found["failures"]
    assert any("flicker spike" in f for f in found["failures"]) and any("tiles flicker" in f for f in found["failures"])
    assert "burst water: spike" in flicker.text(found)


def test_config_checks_burst_steps_and_limits():
    with pytest.raises(ConfigError, match="burst takes"):
        resolve("base", ['steps=[{at="world-ready", do="burst", arg="water"}]'])
    with pytest.raises(ConfigError, match="expect.flicker"):
        resolve("base", ["expect.flicker={spike=3}"])
    contract, harness = resolve("base", ['steps=[{at="world-ready", do="burst", arg="water:60:grid"}]', "expect.flicker={max_spike=8, max_tiles=0}"])
    assert contract["steps"][0]["arg"] == "water:60:grid" and harness["flicker"] == {"max_spike": 8, "max_tiles": 0}
