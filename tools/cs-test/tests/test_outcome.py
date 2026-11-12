import json
import shutil

from conftest import FIXTURES

from cstest.outcome import Final, Watcher, frames_after, read_status, summary_line

CONTRACT = {
    "run_id": "R1",
    "timeouts": {"launch": 60, "compile": 600, "load": 120, "step_wait": 60},
    "steps": [{"at": "world-ready", "frames": 60, "do": "shot", "arg": "start"}, {"at": "previous", "frames": 10, "do": "exit", "arg": ""}],
    "expect": {"reach": "world-ready", "shots": ["start"], "no_log": []},
}
HARNESS = {"timeout_s": 600, "stall_s": 90}


def status(name, **changes):
    data = json.loads((FIXTURES / f"status-{name}.json").read_text())
    data.update(changes)
    return data


def feed(watcher, observations):
    """observations: (t, status or None, game running). Returns the first Final and its time."""
    for t, s, running in observations:
        final = watcher.observe(t, s, running)
        if final:
            return t, final
    return None, None


def test_read_status_checks_run_id(tmp_path):
    path = tmp_path / "s.json"
    shutil.copy(FIXTURES / "status-passed.json", path)
    assert read_status(path, "R1")["outcome"] == "passed"
    assert read_status(path, "OTHER") is None  # a stale file from another run
    path.write_text('{"half": ')
    assert read_status(path, "R1") is None
    assert read_status(tmp_path / "missing.json", "R1") is None


def test_game_outcomes_pass_through():
    for name, outcome in (("passed", "passed"), ("device-lost", "device-lost"), ("config-error", "config-error")):
        t, final = feed(Watcher(CONTRACT, HARNESS), [(1, None, True), (5, status(name), True)])
        assert final == Final(outcome, status(name)["reason"], "game")


def test_never_launched():
    t, final = feed(Watcher(CONTRACT, HARNESS), [(i, None, False) for i in range(0, 70, 5)])
    assert final.outcome == "never-launched" and t > 60


def test_game_without_status_file():
    t, final = feed(Watcher(CONTRACT, HARNESS), [(i, None, True) for i in range(0, 70, 5)])
    assert final.outcome == "never-launched" and "no status file" in final.reason


def test_crashed_when_the_process_ends_without_an_outcome():
    t, final = feed(Watcher(CONTRACT, HARNESS), [(1, None, True), (10, status("main-menu"), True), (11, status("main-menu"), False)])
    assert final.outcome == "crashed" and "main-menu" in final.reason
    t, final = feed(Watcher(CONTRACT, HARNESS), [(1, None, True), (3, None, False)])
    assert final.outcome == "crashed" and "before it wrote a status file" in final.reason


def test_never_loaded_stuck_at_the_main_menu():
    menu = status("main-menu")
    obs = [(1, None, True)] + [(t, dict(menu, updated=str(t)), True) for t in range(10, 200, 5)]
    t, final = feed(Watcher(CONTRACT, HARNESS), obs)
    assert final.outcome == "never-loaded" and "main-menu" in final.reason
    assert 120 < t - 10 <= 150  # the load timeout plus the harness's grace


def test_compile_is_not_a_hang_while_it_progresses():
    compiling = status("compiling")
    obs = [(1, None, True)] + [(t, dict(compiling, updated=str(t)), True) for t in range(5, 590, 5)]
    t, final = feed(Watcher(CONTRACT, dict(HARNESS, timeout_s=2000)), obs)
    assert final is None
    obs = [(t, dict(compiling, updated=str(t)), True) for t in range(590, 700, 5)]
    watcher = Watcher(CONTRACT, dict(HARNESS, timeout_s=2000))
    feed(watcher, [(1, None, True), (5, dict(compiling, updated="5"), True)])
    t, final = feed(watcher, obs)
    assert final.outcome == "never-loaded" and "compiling" in final.reason and "210/1560" in final.reason


def test_stalled_status_is_a_hang():
    menu = status("main-menu")
    obs = [(1, None, True)] + [(t, menu, True) for t in range(5, 200, 5)]  # "updated" never changes
    harness = dict(HARNESS, stall_s=30)
    t, final = feed(Watcher(CONTRACT, harness), obs)
    assert final.outcome == "hang" and t > 30


def test_run_timeout():
    contract = dict(CONTRACT, steps=[], expect={"reach": "", "shots": [], "no_log": []})
    menu = status("main-menu")
    harness = dict(HARNESS, timeout_s=50)
    t, final = feed(Watcher(contract, harness), [(1, None, True)] + [(t, dict(menu, updated=str(t)), True) for t in range(5, 80, 5)])
    assert final.outcome == "timeout" and t > 50


def test_menu_only_config_is_not_never_loaded():
    contract = dict(CONTRACT, steps=[{"at": "main-menu", "frames": 600, "do": "exit", "arg": ""}], expect={"reach": "main-menu", "shots": [], "no_log": []})
    menu = status("main-menu")
    t, final = feed(Watcher(contract, HARNESS), [(1, None, True)] + [(t, dict(menu, updated=str(t)), True) for t in range(5, 400, 5)])
    assert final is None


def test_summary_line():
    s = status("device-lost")
    line = summary_line("native-vulkan+quick", "refs2", Final("device-lost", "VK_ERROR_DEVICE_LOST", "game"), s, [], s["fault"]["address"])
    assert line.startswith("native-vulkan+quick [refs2]: device-lost 3 frames after world-ready")
    assert "fault 0x1f0380000" in line and line.endswith("shots: none")
    assert frames_after(s) == 3
    line = summary_line("guest+quick", "", Final("passed", "", "game"), status("passed"), ["start"])
    assert line == "guest+quick: passed; shots: start"
