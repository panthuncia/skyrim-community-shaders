"""Reference shots (shotref.py) on synthetic pictures: temp folders only."""

import json
import math
import random
import struct
import zlib

import pytest

from cstest import shotref
from cstest.config import resolve
from cstest.schema import ConfigError

W, H = 480, 270


def write_png(path, pixel):
    """An 8-bit RGB PNG of W x H; pixel(x, y) gives (r, g, b)."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(c for x in range(W) for c in pixel(x, y)) for y in range(H))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))
    return path


def scene(x, y, gain=1.0, shift=0.0):
    """A dim, busy picture: blobs and stripes, like a dark exterior."""
    v = 40 + 30 * math.sin(x / 9.0 + shift) * math.cos(y / 13.0) + 25 * ((x // 23 + y // 17) % 2)
    v = max(0, min(255, int(v * gain)))
    return v, max(0, v - 10), max(0, v - 20)


def noisy(seed):
    rng = random.Random(seed)
    return lambda x, y: tuple(max(0, min(255, c + rng.randint(-12, 12))) for c in scene(x, y, 1.05, 0.3))


def quarter(x, y):
    """The scene drawn in the top-left two thirds, the rest a flat grey-blue: the 2026-11-26 failure."""
    if x < W * 2 // 3 and y < H * 2 // 3:
        return scene(x * 3 // 2, y * 3 // 2, 0.5)
    return 50, 55, 70


@pytest.fixture
def pictures(tmp_path):
    return {
        "ref": write_png(tmp_path / "ref.png", scene),
        "again": write_png(tmp_path / "again.png", noisy(1)),
        "quarter": write_png(tmp_path / "quarter.png", quarter),
        "black": write_png(tmp_path / "black.png", lambda x, y: (1, 1, 1)),
        "bright": write_png(tmp_path / "bright.png", lambda x, y: scene(x, y, 3.0)),
    }


def compare(pictures, name):
    return shotref.compare(shotref.signature(pictures["ref"]), shotref.signature(pictures[name]))


def test_noise_and_a_small_shift_pass(pictures):
    result = compare(pictures, "again")
    assert result["failures"] == [], result


def test_world_in_a_corner_fails_with_the_tiles(pictures):
    result = compare(pictures, "quarter")
    assert any("tiles wrong" in f for f in result["failures"]), result
    # The flat border is named: the right third and the bottom third.
    assert result["map"][-1].count(".") < shotref.TILES_X // 2


def test_black_and_blown_frames_fail(pictures):
    assert any("black frame" in f for f in compare(pictures, "black")["failures"])
    assert any("mean lightness" in f or "tiles wrong" in f for f in compare(pictures, "bright")["failures"])


def test_size_change_fails(pictures):
    ref = shotref.signature(pictures["ref"])
    other = shotref.Signature(W, H - 1, ref.cells)
    assert "size" in shotref.compare(ref, other)["failures"][0]


def make_run(paths, name, run_id, shots, outcome="passed"):
    run_dir = paths.runs / run_id
    (run_dir / "shots").mkdir(parents=True)
    (run_dir / "resolved.json").write_text(json.dumps({"name": name, "harness": {"shot_refs": list(shots)}}))
    (run_dir / "result.json").write_text(json.dumps({"outcome": outcome}))
    for shot, source in shots.items():
        (run_dir / "shots" / f"{shot}.png").write_bytes(source.read_bytes())
    return run_dir


def test_accept_then_check(fake, pictures):
    good = make_run(fake, "native-vulkan+quick", "good", {"start": pictures["ref"]})
    assert shotref.accept(fake, good, log=lambda *_: None) == ["native-vulkan+quick/start"]
    entry = json.loads(fake.refs_manifest.read_text())["native-vulkan+quick/start"]
    assert entry["run"] == "good" and shotref.reference_path(fake, entry).exists()

    again = make_run(fake, "native-vulkan+quick", "again", {"start": pictures["again"], "other": pictures["ref"]})
    result = shotref.check_run(fake, ["start", "other"], "native-vulkan+quick", again)
    assert result["failures"] == [] and result["unreferenced"] == ["other"]

    bad = make_run(fake, "native-vulkan+quick", "bad", {"start": pictures["quarter"]})
    result = shotref.check_run(fake, ["start"], "native-vulkan+quick", bad)
    assert result["failures"] and result["failures"][0].startswith("shot start: ")

    # Not asked for: no check. A reference missing from the store fails, it is never skipped.
    assert not shotref.check_run(fake, [], "native-vulkan+quick", bad)["checked"]
    shotref.reference_path(fake, entry).unlink()
    assert "not in" in shotref.check_run(fake, ["start"], "native-vulkan+quick", again)["failures"][0]


def test_only_passed_runs_are_accepted(fake, pictures):
    run_dir = make_run(fake, "guest+quick", "crashed", {"start": pictures["ref"]}, outcome="crashed")
    with pytest.raises(ValueError, match="only a passed run"):
        shotref.accept(fake, run_dir, log=lambda *_: None)


def test_config_shot_refs(tmp_path):
    (tmp_path / "base.toml").write_text("", encoding="utf-8")
    (tmp_path / "s.toml").write_text('extends = ["base"]\n[expect]\nshot_refs = ["start"]\n', encoding="utf-8")
    contract, harness = resolve("s", config_dir=tmp_path)
    assert harness["shot_refs"] == ["start"] and "shot_refs" not in contract["expect"]
    with pytest.raises(ConfigError, match="list of strings"):
        resolve("s", ["expect.shot_refs=true"], config_dir=tmp_path)
