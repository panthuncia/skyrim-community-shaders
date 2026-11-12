"""run() end to end against a fake game: no MO2, no game, temp folders only."""

import json

import pytest
from conftest import FIXTURES

from cstest import procs, runner
from cstest.paths import GAME_EXE, STATUS_FILE, TEST_FILE


class FakeGame:
    def __init__(self, paths, status_name="passed", shots=("start",), exit_after_outcome=True, die=False):
        self.paths = paths
        self.running = False
        self.status_name = status_name
        self.shots = shots
        self.exit_after_outcome = exit_after_outcome
        self.die = die
        self.seen_test_file = None
        self.seen_modlist = None

    def launch(self, paths):
        contract = json.loads((paths.skse / TEST_FILE).read_text())
        self.seen_test_file = contract
        self.seen_modlist = paths.modlist.read_text()
        self.running = True
        status = json.loads((FIXTURES / f"status-{self.status_name}.json").read_text())
        status["run_id"] = contract["run_id"]
        if not self.die:
            (paths.skse / STATUS_FILE).write_text(json.dumps(status))
        (paths.skse / "CommunityShaders.log").write_text("[15:22:16.927] [1] [I] CommunityShaders v1-9-0-0\n")
        for shot in self.shots:
            (paths.runs / contract["run_id"] / "shots" / f"{shot}.png").write_bytes(b"png")
    def find(self, image):
        found = [111] if self.running and image == GAME_EXE else []
        if self.die and found:
            self.running = False  # seen once, then gone
        return found

    def wait_gone(self, pids, seconds):
        if self.exit_after_outcome:
            self.running = False
        return not self.running

    def stop(self, *a, **k):
        self.running = False
        return True


@pytest.fixture
def game(fake, monkeypatch):
    g = FakeGame(fake)
    monkeypatch.setattr(runner, "launch", g.launch)
    monkeypatch.setattr(runner, "POLL_S", 0.001)
    monkeypatch.setattr(procs, "find", g.find)
    monkeypatch.setattr(procs, "wait_gone", g.wait_gone)
    monkeypatch.setattr(procs, "stop_game_and_mo2", g.stop)
    for name in ("focus", "keep_display_awake", "nudge_mouse"):
        monkeypatch.setattr(procs, name, lambda *a, **k: False)
    return g


def test_run_passed_and_cleaned_up(fake, game):
    before = fake.modlist.read_text()
    (fake.skse / "CommunityShaders.env").write_text("MANUAL=1\n")
    result = runner.run(fake, "native-vulkan+quick", ["dclf.shadows=false"], label="t", stage="p35", log=lambda *_: None)
    assert result["outcome"] == "passed", result
    # What the game saw while it ran.
    assert game.seen_test_file["env"]["CS_DCLF_SHADOWS"] == "0"
    assert game.seen_test_file["features_disabled"] == ["Upscaling"]
    assert "-ImprovedCameraSE" in game.seen_modlist and "+CS AB p35" in game.seen_modlist
    # Everything undone afterwards.
    assert not (fake.skse / TEST_FILE).exists()
    assert (fake.skse / "CommunityShaders.env").read_text() == "MANUAL=1\n"
    assert [l for l in fake.modlist.read_text().splitlines() if l != "-CS AB p35"] == before.splitlines()
    assert not fake.journal.exists() and not fake.lock.exists()
    run_dir = fake.runs / result["run_id"]
    saved = json.loads((run_dir / "result.json").read_text())
    assert saved["shots"] == ["start"] and saved["milestones"]["world-ready"]["frame"] == 352
    assert (run_dir / "resolved.json").exists() and (run_dir / "CommunityShaders.log").exists()
    assert result["summary"] == "native-vulkan+quick [t]: passed; shots: start"


def test_missing_shot_fails(fake, game):
    game.shots = ()
    result = runner.run(fake, "guest+quick", [], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "failed" and "start" in result["reason"]


def test_device_lost(fake, game):
    game.status_name = "device-lost"
    game.shots = ()
    result = runner.run(fake, "guest+quick", [], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "device-lost"
    assert result["fault"]["address"] == "0x1f0380000"
    assert "3 frames after world-ready" in result["summary"]


def test_crash_before_status(fake, game):
    game.die = True
    game.shots = ()
    result = runner.run(fake, "guest+quick", [], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "crashed" and "before it wrote a status file" in result["reason"]


def test_config_error_touches_nothing(fake, game):
    before = fake.modlist.read_text()
    result = runner.run(fake, "guest+quick", ["dclf.shadowz=false"], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "config-error"
    assert fake.modlist.read_text() == before and not fake.journal.exists() and not (fake.skse / TEST_FILE).exists()


def test_dry_run_touches_nothing(fake, game):
    before = fake.modlist.read_text()
    result = runner.run(fake, "native-d3d12-quick", [], stage="p35", dry_run=True, log=lambda *_: None)
    assert result["outcome"] == "dry-run" and result["resolved"]["env"]["CS_ORG_NATIVE_BACKEND"] == "d3d12"
    assert fake.modlist.read_text() == before and not fake.runs.exists() and not fake.state.exists()


def test_no_stage(fake, game):
    result = runner.run(fake, "guest+quick", [], log=lambda *_: None)
    assert result["outcome"] == "config-error"
