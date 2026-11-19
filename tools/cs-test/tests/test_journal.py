import json
import os

import pytest

from cstest import journal as journal_mod
from cstest import mo2


def lines(paths):
    return paths.modlist.read_text(encoding="utf-8").splitlines()


def test_modlist_keeps_crlf(fake):
    mo2.disable_mods(fake.modlist, ["Some Mod"])
    raw = fake.modlist.read_bytes()
    assert b"\r\n" in raw and b"\n-Some Mod\r\n" in raw and not raw.startswith(b"\xef\xbb\xbf")


def test_set_enabled_mod_takes_the_slot(fake):
    mo2.set_enabled_mod(fake.modlist, "CS AB p35")
    got = lines(fake)
    assert got.index("+CS AB p35") == got.index("-CommunityShaders_ORG_Staging") + 1
    assert "-CS AB old" in got
    assert mo2.enabled_cs_mod(fake.modlist) == "CS AB p35"
    mo2.set_enabled_mod(fake.modlist, "CS AB old")
    assert "+CS AB old" in lines(fake) and "-CS AB p35" in lines(fake)


def test_disable_and_enable_touch_only_named_lines(fake):
    changed, missing = mo2.disable_mods(fake.modlist, ["Skyrim Engine Telemetry", "Already Off", "Not Listed"])
    assert changed == ["Skyrim Engine Telemetry"] and missing == ["Already Off", "Not Listed"]
    mo2.enable_mods(fake.modlist, changed)
    assert "+Skyrim Engine Telemetry" in lines(fake) and "-Already Off" in lines(fake)


def test_use_variant_stashes_and_restore(fake):
    previous = mo2.use_variant(fake, "p35")
    assert previous == "_original"
    assert (fake.variants / "_original" / "ShaderCache" / "original.bin").exists()
    assert not (fake.overwrite / "ShaderCache").exists()
    state = mo2.get_state(fake)
    assert state == {"live": "p35", "originalMod": "CommunityShaders_ORG_Staging"}
    (fake.overwrite / "ShaderCache").mkdir()
    (fake.overwrite / "ShaderCache" / "p35.bin").write_text("p35")
    assert mo2.restore_original(fake) == "CommunityShaders_ORG_Staging"
    assert (fake.overwrite / "ShaderCache" / "original.bin").exists()
    assert (fake.variants / "p35" / "ShaderCache" / "p35.bin").exists()
    assert mo2.enabled_cs_mod(fake.modlist) == "CommunityShaders_ORG_Staging"
    assert not fake.state.exists()


def test_unknown_variant(fake):
    with pytest.raises(mo2.Mo2Error, match="stage it first"):
        mo2.use_variant(fake, "nope")


def simulate_run(fake, test_file=True):
    """The changes a run makes, journaled the way runner.run journals them."""
    journal = journal_mod.Journal(fake)
    journal.begin("R1")
    journal.add({"do": "variant", "from": "_original", "to": "p35"})
    mo2.use_variant(fake, "p35")
    journal.add({"do": "mods_off", "mods": ["Skyrim Engine Telemetry", "ImprovedCameraSE"]})
    mo2.disable_mods(fake.modlist, ["Skyrim Engine Telemetry", "ImprovedCameraSE"])
    env = fake.skse / "CommunityShaders.env"
    env.write_text("MANUAL=1\n")
    aside = fake.root / "aside" / "R1-CommunityShaders.env"
    aside.parent.mkdir()
    journal.add({"do": "move", "from": str(env), "to": str(aside)})
    env.replace(aside)
    test = fake.skse / "CommunityShaders-Test.json"
    journal.add({"do": "file", "path": str(test)})
    test.write_text("{}")
    journal.add({"do": "launch"})
    return journal


def test_undo_puts_everything_back(fake):
    before = fake.modlist.read_text()
    journal = simulate_run(fake)
    assert "-ImprovedCameraSE" in lines(fake)
    stopped = []
    ok = journal_mod.undo(fake, journal.load(), log=lambda *_: None, running=lambda: [1], stop=lambda *a, **k: stopped.append(1) or True)
    assert ok and stopped == [1]
    # The variant's line stays in the list, turned off (as cs-ab.ps1 left it); every other line is as before.
    assert [l for l in lines(fake) if l != "-CS AB p35"] == before.splitlines()
    assert (fake.skse / "CommunityShaders.env").read_text() == "MANUAL=1\n"
    assert not (fake.skse / "CommunityShaders-Test.json").exists()
    assert (fake.overwrite / "ShaderCache" / "original.bin").exists()
    assert not fake.journal.exists()


def test_recover_a_killed_run(fake):
    simulate_run(fake)
    data = json.loads(fake.journal.read_text())
    data["pid"] = 999999  # the killed cs-test
    fake.journal.write_text(json.dumps(data))
    assert journal_mod.recover(fake, log=lambda *_: None, running=lambda: [], stop=lambda *a, **k: True)
    assert "+ImprovedCameraSE" in lines(fake) and "+Skyrim Engine Telemetry" in lines(fake)
    assert mo2.get_state(fake)["live"] == "_original"
    assert not fake.journal.exists()


def test_recover_refuses_while_someone_elses_mo2_runs(fake):
    journal = journal_mod.Journal(fake)
    journal.begin("R1")
    journal.add({"do": "mods_off", "mods": ["ImprovedCameraSE"]})
    mo2.disable_mods(fake.modlist, ["ImprovedCameraSE"])
    data = journal.load()
    data["pid"] = 999999
    fake.journal.write_text(json.dumps(data))
    messages = []
    assert not journal_mod.recover(fake, log=messages.append, running=lambda: [42], stop=lambda *a, **k: True)
    assert fake.journal.exists() and "-ImprovedCameraSE" in lines(fake)
    assert any("not from this journal" in m for m in messages)


def test_undo_survives_mo2_rewriting_the_list(fake):
    """MO2 rewrites modlist.txt on exit; the undo edits lines afterwards, so the rewrite cannot undo it."""
    journal = simulate_run(fake)

    def mo2_exit_rewrite(*a, **k):
        fake.modlist.write_bytes(fake.modlist.read_bytes())  # what MO2 had loaded: the run's list
        return True

    assert journal_mod.undo(fake, journal.load(), log=lambda *_: None, running=lambda: [7], stop=mo2_exit_rewrite)
    assert "+ImprovedCameraSE" in lines(fake)


def test_legacy_mods_off(fake):
    mo2.disable_mods(fake.modlist, ["ImprovedCameraSE"])
    fake.mods_off_legacy.write_text("ImprovedCameraSE\nImprovedCameraSE\n")
    assert journal_mod.recover(fake, log=lambda *_: None)
    assert "+ImprovedCameraSE" in lines(fake) and not fake.mods_off_legacy.exists()


def test_legacy_env_detection(fake):
    env = fake.skse / "CommunityShaders.env"
    env.write_text("MANUAL=1\n")
    assert journal_mod.legacy_env_left(fake) is None
    env.write_text("CS_AB_PROBE=1\nCS_DCLF_TEST_EXIT=200\n")
    assert journal_mod.legacy_env_left(fake) == env


def test_lock(fake):
    lock = journal_mod.Lock(fake.lock, "run a")
    lock.acquire(0)
    assert json.loads(fake.lock.read_text())["pid"] == os.getpid()
    # A live owner: refused.
    fake.lock.write_text(json.dumps({"pid": 4242, "command": "run other", "started": "x"}))
    other = journal_mod.Lock(fake.lock, "run b")
    with pytest.raises(journal_mod.LockError, match="4242"):
        other.acquire(0, alive=lambda pid: True, log=lambda *_: None)
    # A dead owner: taken over.
    other.acquire(0, alive=lambda pid: False, log=lambda *_: None)
    assert json.loads(fake.lock.read_text())["command"] == "run b"
    other.release()
    assert not fake.lock.exists()
