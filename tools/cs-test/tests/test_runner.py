"""run() end to end against a fake game: no MO2, no game, temp folders only."""

import json
import struct
import zlib

import pytest
from conftest import FIXTURES

from cstest import collect, procs, runner
from cstest.paths import GAME_EXE, STATUS_FILE, TEST_FILE


def tiny_png(value):
    """A 2x1 grey PNG whose pixels are value."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    header = struct.pack(">IIBBBBB", 2, 1, 8, 0, 0, 0, 0)
    return bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]) + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes([0, value, value]))) + chunk(b"IEND", b"")


class FakeGame:
    def __init__(self, paths, status_name="passed", shots=("start",), exit_after_outcome=True, die=False, live=(10, 20)):
        self.paths = paths
        self.live = live  # the two live-image shots' pixel values; None: not written
        self.running = False
        self.status_name = status_name
        self.shots = shots
        self.exit_after_outcome = exit_after_outcome
        self.die = die
        self.seen_test_file = None
        self.seen_modlist = None
        self.report = None  # the status file's exit report, when set
        self.seen_settings = None

    def launch(self, paths):
        contract = json.loads((paths.skse / TEST_FILE).read_text())
        self.seen_test_file = contract
        self.seen_modlist = paths.modlist.read_text()
        settings = paths.overwrite / runner.SETTINGS_USER
        self.seen_settings = json.loads(settings.read_text(encoding="utf-8-sig")) if settings.exists() else None
        self.running = True
        status = json.loads((FIXTURES / f"status-{self.status_name}.json").read_text())
        status["run_id"] = contract["run_id"]
        if self.report is not None:
            status["report"] = self.report
        if not self.die:
            (paths.skse / STATUS_FILE).write_text(json.dumps(status))
        (paths.skse / "CommunityShaders.log").write_text("[15:22:16.927] [1] [I] CommunityShaders v1-9-0-0\n")
        for shot in self.shots:
            (paths.runs / contract["run_id"] / "shots" / f"{shot}.png").write_bytes(b"png")
        if self.live:
            for name, value in zip(("_live-a", "_live-b"), self.live):
                (paths.runs / contract["run_id"] / "shots" / f"{name}.png").write_bytes(tiny_png(value))
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
    result = runner.run(fake, "native-vulkan+quick", ["dclf.plain_snow_sparkle=false"], label="t", stage="p35", log=lambda *_: None)
    assert result["outcome"] == "passed", result["reason"]
    # What the game saw while it ran.
    assert game.seen_test_file["env"]["CS_DCLF_PLAIN_SNOW_SPARKLE"] == "0"
    assert game.seen_test_file["features_disabled"] == []
    assert game.seen_test_file["expect"]["live_frames"] == 0  # the frozen-image check is off by default
    assert "-ImprovedCameraSE" in game.seen_modlist and "+CS AB p35" in game.seen_modlist
    # Everything undone afterwards.
    assert not (fake.skse / TEST_FILE).exists()
    assert (fake.skse / "CommunityShaders.env").read_text() == "MANUAL=1\n"
    assert [l for l in fake.modlist.read_text().splitlines() if l != "-CS AB p35"] == before.splitlines()
    assert not fake.journal.exists() and not fake.lock.exists()
    run_dir = fake.runs / result["run_id"]
    saved = json.loads((run_dir / "result.json").read_text())
    assert saved["shots"] == ["start"] and saved["milestones"]["world-ready"]["frame"] == 352
    assert not saved["live_image"]["checked"]
    assert (run_dir / "resolved.json").exists() and (run_dir / "CommunityShaders.log").exists()
    assert result["summary"] == "native-vulkan+quick [t]: passed; shots: start"


def test_missing_shot_fails(fake, game):
    game.shots = ()
    result = runner.run(fake, "guest+quick", [], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "failed" and "start" in result["reason"]


def test_frozen_image_fails(fake, game):
    game.live = (10, 10)
    result = runner.run(fake, "guest+quick", ["expect.live_frames=30"], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "failed" and "frozen final target" in result["reason"]
    assert result["live_image"]["same"]


def test_missing_live_shots_fail(fake, game):
    game.live = None
    result = runner.run(fake, "guest+quick", ["expect.live_frames=30"], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "failed" and "live-image shots missing" in result["reason"]


def test_live_check_off(fake, game):
    game.live = None
    result = runner.run(fake, "guest+quick", ["expect.live_frames=0"], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "passed", result["reason"]


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


GRAPH_REPORT = {"graph_frames": {"frames": 900, "clients": {
    "cs.ssgi": {"graph": 812, "first_graph_frame": 4100},
    "cs.e11-rays": {"graph": 400, "first_graph_frame": 4000},
}}}


def test_graph_frames_held(fake, game):
    game.report = GRAPH_REPORT
    result = runner.run(fake, "guest+quick", ['expect.graph_frames={min={"cs.ssgi"=300}}'], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "passed", result["reason"]
    assert result["graph_frames"]["failures"] == [] and result["graph_frames"]["report"] == GRAPH_REPORT["graph_frames"]
    # The copy of the final status in the run folder is what the check read.
    assert json.loads((fake.runs / result["run_id"] / "status.json").read_text())["report"] == GRAPH_REPORT


def test_graph_frames_fail(fake, game):
    game.report = GRAPH_REPORT
    sets = ['expect.graph_frames={min={"cs.ssgi"=1000, "cs.fog"=1}}']
    result = runner.run(fake, "guest+quick", sets, stage="p35", log=lambda *_: None)
    assert result["outcome"] == "failed" and result["outcome_by"] == "harness"
    assert result["reason"] == ("cs.ssgi ran on the graph in 812 frames, at least 1000 expected; cs.fog ran on the graph in 0 frames, at least 1 expected "
                                "(harness)")
    saved = json.loads((fake.runs / result["run_id"] / "result.json").read_text())
    assert len(saved["graph_frames"]["failures"]) == 2


def test_graph_frames_missing_report(fake, game):
    result = runner.run(fake, "guest+quick", ['expect.graph_frames={min={"cs.ssgi"=1}}'], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "failed"
    assert result["reason"] == "no graph_frames report in the status file (a Release build or an older build) (harness)"


def test_graph_frames_skipped_in_d3d11_mode(fake, game):
    result = runner.run(fake, "d3d11+quick", ['expect.graph_frames={min={"cs.ssgi"=300}}'], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "passed", result["reason"]
    assert result["graph_frames"]["skipped"] == "skipped (CS_ORG=0)"


def test_graph_frames_unit():
    contract = {"env": {}, "expect": {"graph_frames": {"min": {"cs.e11-rays": 500}}}}
    status = {"report": GRAPH_REPORT}
    got = collect.graph_frames(contract, status)
    assert got["failures"] == ["cs.e11-rays ran on the graph in 400 frames, at least 500 expected"] and got["skipped"] == ""
    assert collect.graph_frames({"env": {}, "expect": {"graph_frames": {}}}, status)["failures"] == []


def test_set_ini_values():
    text = "[Display]\nbFoo=0\n\n[Imagespace]\nbDoDepthOfField=1\n"
    got = runner.set_ini_values(text, {"display": {"BFOO": True, "bSAO_CS_Enable": 1}, "Extra": {"x": "y"}})
    assert got == "[Display]\nBFOO=1\nbSAO_CS_Enable=1\n\n[Imagespace]\nbDoDepthOfField=1\n\n[Extra]\nx=y\n"


SSGI = {"Screen Space GI": {"Enabled": True, "Settings": {"Quality": 3}}}


def test_saved_settings_over_an_existing_file(fake, game):
    target = fake.overwrite / runner.SETTINGS_USER
    target.parent.mkdir(parents=True)
    original = b'\xef\xbb\xbf{\r\n "Screen Space GI": {"Enabled": false, "Settings": {"Quality": 1, "Blur": 2}},\r\n "Other": 5\r\n}'
    target.write_bytes(original)
    messages = []
    sets = ['saved_settings.Screen Space GI={Enabled=true, Settings={Quality=3}}']
    result = runner.run(fake, "guest+quick", sets, stage="p35", log=messages.append)
    assert result["outcome"] == "passed", result["reason"]
    assert game.seen_settings == {"Screen Space GI": {"Enabled": True, "Settings": {"Quality": 3, "Blur": 2}}, "Other": 5}
    assert any(m.startswith("Saved settings: Screen Space GI written into") for m in messages)
    # Put back byte for byte, and the copy set aside is gone.
    assert target.read_bytes() == original
    assert not list((fake.root / "aside").glob("*SettingsUser.json"))


def test_saved_settings_new_file_from_the_staged_mod(fake, game):
    staged = fake.mods / "CS AB p35" / runner.SETTINGS_USER
    staged.parent.mkdir(parents=True)
    staged.write_text('{"Screen Space GI": {"Enabled": false, "Settings": {"Blur": 2}}}', encoding="utf-8")
    result = runner.run(fake, "guest+quick", ['saved_settings.Screen Space GI={Enabled=true, Settings={Quality=3}}'], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "passed", result["reason"]
    assert game.seen_settings == {"Screen Space GI": {"Enabled": True, "Settings": {"Blur": 2, "Quality": 3}}}
    assert not (fake.overwrite / runner.SETTINGS_USER).exists()
    assert staged.read_text(encoding="utf-8") == '{"Screen Space GI": {"Enabled": false, "Settings": {"Blur": 2}}}'


def test_saved_settings_new_file_from_nothing(fake, game):
    result = runner.run(fake, "guest+quick", ['saved_settings.Disable at Boot={"Light Limit Fix"=true}'], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "passed", result["reason"]
    assert game.seen_settings == {"Disable at Boot": {"Light Limit Fix": True}}
    assert not (fake.overwrite / runner.SETTINGS_USER).exists()


def test_saved_settings_unreadable_file_is_left_alone(fake, game):
    target = fake.overwrite / runner.SETTINGS_USER
    target.parent.mkdir(parents=True)
    target.write_text("{ not json", encoding="utf-8")
    result = runner.run(fake, "guest+quick", ['saved_settings.Screen Space GI={Enabled=true}'], stage="p35", log=lambda *_: None)
    assert result["outcome"] == "not-run" and "does not parse" in result["reason"]
    assert target.read_text(encoding="utf-8") == "{ not json"
    assert game.seen_test_file is None and not fake.journal.exists()


def test_format_diff_shows_await():
    steps = [{"at": "world-ready", "frames": 5, "do": "call", "arg": "org.rebuild", "await": "adopted"}]
    assert runner.format_diff([("steps", [], steps)]) == ["  steps: 0 -> 1: world-ready+5 call org.rebuild await adopted"]
