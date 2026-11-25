"""The lab's layers, slot builds, homes, slot locks and shader cache, in temp folders."""

import json
import os

import pytest

from cstest import lab as lab_mod, locks, runner


def entries(fake, mods=None):
    lab = lab_mod.Lab(fake)
    return lab, lab.data_entries(mods if mods is not None else lab.enabled_mods(), "CS AB p35")


def test_enabled_mods_lowest_priority_first_without_cs_mods(fake):
    assert lab_mod.Lab(fake).enabled_mods() == ["Another Mod", "Start On Save", "ImprovedCameraSE", "Skyrim Engine Telemetry", "Some Mod"]


def test_layers_follow_mo2_priority(fake):
    _, got = entries(fake)
    # Some Mod is above Another Mod in the list, so its file wins.
    assert got["textures/a.dds"].src.endswith(os.path.join("Some Mod", "textures", "a.dds"))
    # The staged build is in, the other CS mod is not; MO2's meta.ini, captures and the shader cache are left out.
    assert got["skse/plugins/communityshaders.dll"].src.endswith(os.path.join("CS AB p35", "SKSE", "Plugins", "CommunityShaders.dll"))
    assert not any(k.endswith("meta.ini") or k.endswith(".rdc") or k.startswith("shadercache/") for k in got)
    assert "skse/plugins/!cstestlab.dll" in got and got["skyrim.esm"].mode == "link"


def test_copy_rules():
    assert lab_mod.copied("skse/plugins/some.ini", "mod") and lab_mod.copied("skse/plugins/x/data.bin", "mod")
    assert not lab_mod.copied("skse/plugins/some.dll", "mod")
    assert lab_mod.copied("nahlfrod grass.ini", "game") and lab_mod.copied("interface/x.json", "mod")
    assert lab_mod.copied("textures/water/flowmap.dds", "overwrite") and not lab_mod.copied("grass/x.cgid", "overwrite")
    assert not lab_mod.copied("textures/a.dds", "mod")
    assert lab_mod.copied("shaders/common.hlsli", "stage")


def test_slot_build_never_writes_through_a_link(fake):
    lab = lab_mod.Lab(fake)
    slot = lab.slot(0)
    lab_mod.build_slot(lab, slot, lab.enabled_mods(), "CS AB p35", log=lambda *_: None)
    esm = slot.data / "Skyrim.esm"
    assert os.stat(esm).st_ino == os.stat(fake.game / "Data" / "Skyrim.esm").st_ino  # a hard link
    assert (slot.game / "steam_appid.txt").read_text() == lab_mod.APP_ID
    assert not (slot.game / "capture.rdc").exists() and (slot.game / "dxvk.conf").exists()
    # A game writes a config: it is the slot's own copy, and the next build puts the mod's back.
    ini = slot.data / "SKSE" / "Plugins" / "Some.ini"
    ini.write_text("x=2\n")
    assert (fake.mods / "Some Mod" / "SKSE" / "Plugins" / "Some.ini").read_text() == "x=1\n"
    # A new mod file replaces the link by deleting it first: the game's own file stays as it was.
    (fake.mods / "Some Mod" / "Skyrim.esm").write_text("override")
    stray = slot.data / "SKSE" / "Plugins" / "stray.log"
    stray.write_text("left by a game")
    lab_mod.build_slot(lab, slot, lab.enabled_mods(), "CS AB p35", log=lambda *_: None)
    assert ini.read_text() == "x=1\n" and not stray.exists()
    assert esm.read_text() == "override" and (fake.game / "Data" / "Skyrim.esm").read_text() == "esm"


def test_mods_off_leave_the_slot(fake):
    lab = lab_mod.Lab(fake)
    slot = lab.slot(0)
    mods = [m for m in lab.enabled_mods() if m != "ImprovedCameraSE"]
    lab_mod.build_slot(lab, slot, lab.enabled_mods(), "CS AB p35", log=lambda *_: None)
    assert (slot.data / "SKSE" / "Plugins" / "ImprovedCameraSE.dll").exists()
    lab_mod.build_slot(lab, slot, mods, "CS AB p35", log=lambda *_: None)
    assert not (slot.data / "SKSE" / "Plugins" / "ImprovedCameraSE.dll").exists()


def test_mirrors_only_for_another_volume(fake, monkeypatch):
    lab = lab_mod.Lab(fake)
    assert lab.mod_source("Some Mod") == fake.mods / "Some Mod"
    monkeypatch.setattr(lab_mod, "same_volume", lambda a, b: False)
    lab.refresh_mirrors(["Some Mod"], log=lambda *_: None)
    assert (lab.mirrors / "Some Mod" / "SomeMod.esp").read_text() == "some"
    assert not (lab.mirrors / "_overwrite" / "ShaderCache").exists()
    assert not list((lab.mirrors / "_overwrite").rglob("*.rdc"))
    (fake.mods / "Some Mod" / "SomeMod.esp").unlink()
    lab.refresh_mirrors(["Some Mod"], log=lambda *_: None)
    assert not (lab.mirrors / "Some Mod" / "SomeMod.esp").exists()


def test_home_holds_the_runs_files(fake):
    lab = lab_mod.Lab(fake)
    slot = lab.slot(0)
    contract = {"game": {"save": "autoload"}, "run_id": "R"}
    slot.skse.mkdir(parents=True)
    (slot.skse / "CommunityShaders.log").write_text("an old run's log")
    save = lab_mod.write_home(fake, slot, contract, {"Display": {"iSize W": 1280}}, ["StartOnSave.esp"], runner.set_ini_values, log=lambda *_: None)
    assert save == "Newest" and sorted(p.name for p in slot.saves.iterdir()) == ["Newest.ess", "Newest.skse"]
    assert not (slot.skse / "CommunityShaders.log").exists()
    assert json.loads((slot.skse / "CommunityShaders-Test.json").read_text())["run_id"] == "R"
    assert "bAlwaysActive=1" in (slot.documents / "Skyrim.ini").read_text(encoding="latin-1")
    prefs = (slot.documents / "SkyrimPrefs.ini").read_text(encoding="latin-1")
    assert "iSize W=1280" in prefs and "bEnableFileSelection=1" in prefs
    plugins = (slot.local / "plugins.txt").read_text()
    assert "StartOnSave.esp" not in plugins and "*TerrainHelper.esp" in plugins
    assert b"\r\r" not in (slot.local / "plugins.txt").read_bytes()
    # The user's files are only read.
    assert (fake.game_documents / "SkyrimPrefs.ini").read_text() == "[Display]\niSize W=1920\n"


def test_local_settings_and_saves_come_from_the_profile(fake):
    (fake.profile_dir / "settings.ini").write_text("[General]\nLocalSaves=true\nLocalSettings=true\n")
    (fake.profile_dir / "Skyrim.ini").write_text("[General]\nsLocal=1\n")
    (fake.profile_dir / "SkyrimPrefs.ini").write_text("[Display]\n")
    (fake.profile_dir / "saves").mkdir()
    (fake.profile_dir / "saves" / "P.ess").write_text("p")
    slot = lab_mod.Lab(fake).slot(1)
    save = lab_mod.write_home(fake, slot, {"game": {"save": "autoload"}}, {}, [], runner.set_ini_values, log=lambda *_: None)
    assert save == "P" and "sLocal=1" in (slot.documents / "Skyrim.ini").read_text(encoding="latin-1")


def test_slots_are_taken_one_each(fake):
    lab = lab_mod.Lab(fake)
    a = lab_mod.take_slots(lab, 2, False, 0, "a", log=lambda *_: None)
    # Another process holds slot 0 (a lock owned by this PID counts as taken).
    (fake.locks / "slot-0.lock").write_text(json.dumps({"pid": 4242, "command": "a"}))
    b = lab_mod.take_slots(lab, 2, False, 0, "b", log=lambda *_: None, alive=lambda pid: True)
    assert a.numbers == [0] and b.numbers == [1]
    (fake.locks / "slot-1.lock").write_text(json.dumps({"pid": 4243, "command": "b"}))
    with pytest.raises(locks.LockError, match="all 2 lab slots are busy"):
        lab_mod.take_slots(lab, 2, False, 0, "c", log=lambda *_: None, alive=lambda pid: True)
    # A dead owner's slot is taken over.
    c = lab_mod.take_slots(lab, 2, False, 0, "c", log=lambda *_: None, alive=lambda pid: False)
    assert c.numbers == [0]
    c.release()


def test_an_exclusive_run_takes_every_slot_and_others_wait(fake):
    lab = lab_mod.Lab(fake)
    claim = lab_mod.take_slots(lab, 3, True, 0, "perf", log=lambda *_: None)
    assert claim.numbers == [0, 1, 2]
    claim.release()
    # A live exclusive run (waiting for its slots) keeps others from taking new ones.
    (fake.locks / "exclusive.lock").write_text(json.dumps({"pid": 4242, "command": "perf"}))
    with pytest.raises(locks.LockError):
        lab_mod.take_slots(lab, 3, False, 0, "quick", log=lambda *_: None, alive=lambda pid: pid == 4242)


def test_shader_cache_seed_merge_and_conflict(fake):
    lab = lab_mod.Lab(fake)
    a, b = lab.slot(0), lab.slot(1)
    for slot in (a, b):
        slot.data.mkdir(parents=True, exist_ok=True)
        lab_mod.prepare_cache(lab, slot, cold=False, log=lambda *_: None)
    assert (a.cache / "v3" / "index.a").read_text() == "original" and lab_mod.cache_generation(lab) == 1
    # Slot 0 compiles something and ends cleanly: its cache is the shared one now.
    (a.cache / "v3" / "packs").mkdir()
    (a.cache / "v3" / "packs" / "00000001.pack").write_text("new")
    lab_mod.merge_cache(lab, a, clean=True, log=lambda *_: None)
    assert lab_mod.cache_generation(lab) == 2 and (lab.cache / "v3" / "packs" / "00000001.pack").exists()
    # Slot 1 started from generation 1: its cache is not kept, and its next run starts from generation 2.
    messages = []
    lab_mod.merge_cache(lab, b, clean=True, log=messages.append)
    assert "not kept" in messages[0] and b.state()["cache"] == -1
    lab_mod.prepare_cache(lab, b, cold=False, log=lambda *_: None)
    assert (b.cache / "v3" / "packs" / "00000001.pack").read_text() == "new"
    # MO2's cache was only read.
    assert not (fake.overwrite / "ShaderCache" / "v3" / "packs").exists()


def test_a_killed_or_cold_run_keeps_nothing(fake):
    lab = lab_mod.Lab(fake)
    slot = lab.slot(0)
    slot.data.mkdir(parents=True)
    lab_mod.prepare_cache(lab, slot, cold=False, log=lambda *_: None)
    lab_mod.merge_cache(lab, slot, clean=False, log=lambda *_: None)
    assert slot.state()["cache"] == -1 and lab_mod.cache_generation(lab) == 1
    lab_mod.prepare_cache(lab, slot, cold=True, log=lambda *_: None)
    assert not slot.cache.exists()
    lab_mod.merge_cache(lab, slot, clean=True, log=lambda *_: None)
    assert lab_mod.cache_generation(lab) == 1
