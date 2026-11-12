import os
import time

from cstest import collect
from cstest.stage import mirror_map, stage, tree


def test_bounded_copy_small_is_whole(tmp_path):
    src = tmp_path / "a.log"
    src.write_bytes(b"x" * 1000)
    size, cut = collect.bounded_copy(src, tmp_path / "out" / "a.log", head=600, tail=600)
    assert (size, cut) == (1000, False)
    assert (tmp_path / "out" / "a.log").read_bytes() == b"x" * 1000


def test_bounded_copy_keeps_head_and_tail(tmp_path):
    src = tmp_path / "big.log"
    src.write_bytes(b"H" * 100 + b"M" * 10_000 + b"T" * 100)
    size, cut = collect.bounded_copy(src, tmp_path / "out.log", head=100, tail=100)
    data = (tmp_path / "out.log").read_bytes()
    assert cut and size == 10_200
    assert data.startswith(b"H" * 100) and data.endswith(b"T" * 100)
    assert b"MM" not in data and b"10000 bytes cut" in data


def test_collect_filters_by_start_time(tmp_path):
    skse, run = tmp_path / "SKSE", tmp_path / "run"
    skse.mkdir()
    old = skse / "SkyrimSE_d3d11.log"
    old.write_text("stale")
    os.utime(old, (time.time() - 3600, time.time() - 3600))
    start = time.time()
    (skse / "CommunityShaders.log").write_text("[15:22:16.927] [1] [I] hello\n")
    (skse / "CommunityShaders-VRAM-1.json").write_text("{}")
    (skse / "CS-AB-memory.csv").write_text("a,b\n")
    (skse / "Unrelated.log").write_text("no")
    got = collect.collect(skse, run, start, log=lambda *_: None)
    assert (run / "CommunityShaders.log").exists() and not (run / "SkyrimSE_d3d11.log").exists()
    assert not (skse / "CommunityShaders-VRAM-1.json").exists() and (run / "CommunityShaders-VRAM-1.json").exists()
    assert (skse / "CS-AB-memory.csv").exists() and (run / "CS-AB-memory.csv").exists()
    assert not (run / "Unrelated.log").exists()
    assert "CommunityShaders.log" in got["logs"]


def test_scan_log_counts_kinds(tmp_path):
    log = tmp_path / "CommunityShaders.log"
    log.write_text(
        "[15:22:16.927] [11148] [I] start\n"
        "[15:22:27.572] [11148] [E] [ORG] poisoned timeline 12 at value 3400\n"
        "[15:22:27.573] [11148] [E] [ORG] poisoned timeline 13 at value 3401\n"
        "[15:22:27.574] [11148] [W] [Settings] file not found: 'D:\\x\\y.ini'\n"
        "[15:22:27.575] [11148] [E] Vulkan device fault: invalid read at 0x1f0380000\n"
    )
    scan = collect.scan_log(log)
    assert scan["errors"] == 3 and scan["warnings"] == 1
    assert scan["error_kinds"]["[ORG] poisoned timeline # at value #"] == 2
    assert scan["fault_address"] == "0x1f0380000"
    assert "poisoned timeline 12" in scan["first_error"]
    assert collect.log_contains(log, ["device fault", "nothing like this"]) == {"device fault"}


def test_mirror_deletes_stale(tmp_path):
    src = tmp_path / "src"
    (src / "Common").mkdir(parents=True)
    (src / "Common" / "A.hlsli").write_text("a")
    dest = tmp_path / "dest"
    (dest / "Common").mkdir(parents=True)
    (dest / "Common" / "DeviceAddress.hlsli").write_text("stale")
    (dest / "Old").mkdir()
    (dest / "Old" / "x.hlsl").write_text("stale")
    (dest / "keep.pdb").write_text("pdb")
    copied, deleted = mirror_map(tree(src), dest, exclude=("*.pdb",))
    assert (copied, deleted) == (1, 2)
    assert (dest / "Common" / "A.hlsli").exists() and not (dest / "Common" / "DeviceAddress.hlsli").exists()
    assert not (dest / "Old").exists() and (dest / "keep.pdb").exists()
    assert mirror_map(tree(src), dest, exclude=("*.pdb",)) == (0, 0)


def test_stage_from_worktree(fake, tmp_path):
    wt = tmp_path / "wt"
    (wt / "src").mkdir(parents=True)
    (wt / "build" / "Dev-Fast").mkdir(parents=True)
    (wt / "build" / "Dev-Fast" / "CommunityShaders.dll").write_bytes(b"MZdll")
    (wt / "package" / "Shaders" / "Common").mkdir(parents=True)
    (wt / "package" / "Shaders" / "Common" / "Color.hlsli").write_text("c")
    (wt / "features" / "Grass Lighting" / "Shaders" / "Features").mkdir(parents=True)
    (wt / "features" / "Grass Lighting" / "Shaders" / "Features" / "GrassLighting.ini").write_text("[Info]")
    mod = fake.mods / "CS AB p35"
    (mod / "Shaders" / "Common").mkdir(parents=True)
    (mod / "Shaders" / "Common" / "DeviceAddress.hlsli").write_text("stale")
    manifest = stage(fake, wt, "p35", log=lambda *_: None)
    assert (mod / "SKSE" / "Plugins" / "CommunityShaders.dll").read_bytes() == b"MZdll"
    assert (mod / "Shaders" / "Features" / "GrassLighting.ini").exists()
    assert not (mod / "Shaders" / "Common" / "DeviceAddress.hlsli").exists()
    assert manifest["mode"] == "worktree" and len(manifest["dll"]["sha256"]) == 64
    assert (fake.stages / "p35.json").exists()
