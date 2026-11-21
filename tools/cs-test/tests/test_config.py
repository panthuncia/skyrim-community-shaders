import json

import pytest

from cstest.config import diff, parse_value, resolve
from cstest.paths import CONFIG_DIR
from cstest.schema import ConfigError, Schema, load_schema

SCHEMA = Schema({
    "allow": {"env": ["DXVK_*"]},
    "key": {
        "dclf.shadows": {"env": "CS_DCLF_SHADOWS", "type": "bool", "default": True, "sweep": True},
        "dclf.grass": {"env": "CS_DCLF_GRASS", "type": "bool", "default": True, "sweep": True},
        "dclf.enabled": {"env": "CS_DCLF", "type": "bool", "default": True},
        "dclf.cull": {"env": "CS_DCLF_CULL", "type": "enum", "choices": ["off", "frustum", "occlusion"]},
        "cs.hang_watchdog": {"env": "CS_HANG_WATCHDOG", "type": "int"},
        "cs.disable_features": {"env": "CS_DISABLE_FEATURES", "type": "string", "use_instead": "[features]"},
        "org.native": {"env": "CS_ORG_NATIVE", "type": "bool"},
        "org.sky": {"env": "CS_ORG_SKY", "type": "bool", "sweep": True},
        "org.water": {"env": "CS_ORG_WATER", "type": "bool", "sweep": True},
        "org.ports": {"type": "preset", "values": {"all": {"CS_ORG_SKY": "1", "CS_ORG_WATER": "1"}, "none": {}}},
        "features.Upscaling": {"type": "bool", "sweep": True},
        "features.Skylighting": {"type": "bool", "sweep": True},
        "dxvk.debug": {"env": "DXVK_DEBUG", "type": "string"},
    },
})


@pytest.fixture
def configs(tmp_path):
    def write(name, text):
        (tmp_path / f"{name}.toml").write_text(text, encoding="utf-8")

    write("base", '[run]\ntimeout_s = 600\n[game]\nsave = "autoload"\nmods_off = ["Telemetry"]\n[dxvk]\ndebug = "hang"\n')
    write("a", 'extends = ["base"]\n[dclf]\nshadows = false\ngrass = false\n[run]\nlabel = "a"\n')
    write("b", 'extends = ["base"]\n[dclf]\ngrass = true\n[[steps]]\nat = "world-ready"\nframes = 60\ndo = "shot"\narg = "start"\n')
    write("c", 'extends = ["a"]\n[dclf]\nshadows = true\n')
    return tmp_path


def r(spec, configs, sets=(), **kw):
    return resolve(spec, sets, schema=SCHEMA, config_dir=configs, **kw)


def test_extends_order_later_wins(configs):
    contract, harness = r("c", configs)
    assert contract["env"]["CS_DCLF_SHADOWS"] == "1"  # c over a
    assert contract["env"]["CS_DCLF_GRASS"] == "0"    # from a
    assert contract["env"]["DXVK_DEBUG"] == "hang"    # from base
    assert harness["label"] == "a"


def test_composition_applies_base_once(configs):
    # base is pulled in by both a and b. Applied again after a, it would undo nothing here, but its position must stay first.
    contract, _ = r("a+b", configs)
    assert contract["env"]["CS_DCLF_SHADOWS"] == "0"
    assert contract["env"]["CS_DCLF_GRASS"] == "1"  # b wins over a
    assert contract["steps"] == [{"at": "world-ready", "frames": 60, "do": "shot", "arg": "start"}]
    assert contract["name"] == "a+b"


def test_set_wins_over_files(configs):
    contract, harness = r("a", configs, ["dclf.shadows=true", "run.timeout_s=60", "game.save=MySave"])
    assert contract["env"]["CS_DCLF_SHADOWS"] == "1"
    assert harness["timeout_s"] == 60
    assert contract["game"]["save"] == "MySave"


def test_set_in_order_last_wins(configs):
    contract, _ = r("a", configs, ["dclf.shadows=true", "dclf.shadows=false"])
    assert contract["env"]["CS_DCLF_SHADOWS"] == "0"


def test_set_glob_matches_sweep_keys_only(configs):
    contract, _ = r("base", configs, ["dclf.*=false"])
    assert contract["env"]["CS_DCLF_SHADOWS"] == "0" and contract["env"]["CS_DCLF_GRASS"] == "0"
    assert "CS_DCLF" not in contract["env"]  # dclf.enabled is not a sweep key


def test_set_steps_replaces(configs):
    contract, _ = r("b", configs, ['steps=[{at="world-ready", frames=5, do="exit"}]'])
    assert contract["steps"] == [{"at": "world-ready", "frames": 5, "do": "exit", "arg": ""}]


@pytest.mark.parametrize("sets, message", [
    (["dclf.shadowz=false"], "unknown key 'dclf.shadowz'"),
    (["run.timeoutt=5"], "unknown key 'run.timeoutt'"),
    (["env.CS_MADE_UP=1"], "unknown switch"),
    (["dclf.shadows=yes"], "takes true or false"),
    (["dclf.cull=sideways"], "takes one of"),
    (["cs.hang_watchdog=soon"], "whole number"),
    (["org.ports=most"], "takes one of"),
    (["env.CS_DISABLE_FEATURES=Upscaling"], "instead"),
    (["features.NoSuchFeature=false"], "unknown key"),
    (["build.shader_cache=lukewarm"], "warm or cold"),
    (["expect.reach=somewhere"], "milestone"),
    (["expect.live_frames=-1"], "at least 0"),
    (["expect.live_frames=soon"], "whole number"),
    (["noequals"], "key=value"),
])
def test_errors_before_launch(configs, sets, message):
    with pytest.raises(ConfigError, match=message):
        r("a", configs, sets)


def test_unknown_table_and_missing_config(configs, tmp_path):
    (tmp_path / "bad.toml").write_text("[nonsense]\nx = 1\n")
    with pytest.raises(ConfigError, match="unknown table"):
        r("bad", configs)
    with pytest.raises(ConfigError, match="no config 'nope'"):
        r("nope", configs)


def test_extends_loop(configs, tmp_path):
    (tmp_path / "x.toml").write_text('extends = ["y"]\n')
    (tmp_path / "y.toml").write_text('extends = ["x"]\n')
    with pytest.raises(ConfigError, match="loop"):
        r("x", configs)


def test_preset_expands_and_explicit_keys_win(configs):
    contract, harness = r("base", configs, ["org.ports=all", "org.water=false"])
    assert contract["env"]["CS_ORG_SKY"] == "1"
    assert contract["env"]["CS_ORG_WATER"] == "0"
    assert harness["origin"]["CS_ORG_SKY"] == "org.ports"


def test_typed_and_raw_env_conflict(configs):
    with pytest.raises(ConfigError, match="keep one"):
        r("a", configs, ["env.CS_DCLF_SHADOWS=1"])


def test_raw_env_allowed_by_pattern(configs):
    contract, _ = r("base", configs, ["env.DXVK_HUD=fps"])
    assert contract["env"]["DXVK_HUD"] == "fps"


def test_features_disabled(configs):
    contract, _ = r("base", configs, ["features.Upscaling=false", "features.Skylighting=true"])
    assert contract["features_disabled"] == ["Upscaling"]
    assert contract["features_enabled"] == ["Skylighting"]
    contract, _ = r("base", configs, ["features.*=false"])
    assert contract["features_disabled"] == ["Skylighting", "Upscaling"]


def test_step_rules(configs):
    with pytest.raises(ConfigError, match="step 1 cannot be at 'previous'"):
        r("base", configs, ['steps=[{do="exit"}]'])
    with pytest.raises(ConfigError, match="wait takes"):
        r("base", configs, ['steps=[{at="world-ready", do="shot", wait="forever"}]'])
    contract, _ = r("base", configs, ['steps=[{at="world-ready", do="shot", wait="signal:3", wait_s=10}]'])
    assert contract["steps"][0]["wait"] == "signal:3"


def test_contract_shape(configs):
    contract, _ = r("b", configs)
    assert set(contract) == {"version", "name", "run_id", "run_dir", "env", "features_disabled", "features_enabled", "dclf_ini", "game", "timeouts", "steps", "expect"}
    assert contract["timeouts"] == {"launch": 60, "compile": 600, "load": 120, "step_wait": 60}
    assert contract["expect"] == {"reach": "", "shots": [], "no_log": [], "live_frames": 0, "graph_frames": {}}


def test_label_argument(configs):
    _, harness = r("a", configs, label="mine")
    assert harness["label"] == "mine"


def test_diff(configs):
    base, _ = r("base", configs)
    other, _ = r("a", configs)
    paths = [d[0] for d in diff(base, other)]
    assert "env.CS_DCLF_SHADOWS" in paths and "env.DXVK_DEBUG" not in paths


def test_parse_value():
    assert parse_value("true") is True
    assert parse_value("12") == 12
    assert parse_value("hang") == "hang"
    assert parse_value("*nsight*") == "*nsight*"
    assert parse_value('"quoted"') == "quoted"


def test_every_shipped_config_resolves():
    schema = load_schema()
    names = sorted(p.stem for p in CONFIG_DIR.glob("*.toml"))
    assert "base" in names and "quick" in names
    for name in names:
        resolve(name, ["build.stage=x"], schema=schema)
    contract, _ = resolve("native-vulkan+quick", (), schema=schema)
    assert contract["env"]["CS_ORG_NATIVE"] == "1" and contract["features_disabled"] == []
    assert contract["expect"]["live_frames"] == 0
    contract, _ = resolve("native-d3d12-quick", (), schema=schema)
    assert contract["env"]["CS_ORG_NATIVE_BACKEND"] == "d3d12"


def test_named_save_turns_the_autoload_mod_off(configs):
    sets = ['game.autoload_mod="Start On Save"']
    _, harness = r("base", configs, sets)
    assert harness["mods_off"] == ["Telemetry"] and harness["autoload_off"] == ""
    contract, harness = r("base", configs, sets + ["game.save=MySave"])
    assert contract["game"]["save"] == "MySave"
    assert harness["mods_off"] == ["Telemetry", "Start On Save"]
    assert harness["autoload_off"] == "Start On Save"


def test_step_await(configs):
    contract, _ = r("base", configs, ['steps=[{at="world-ready", do="shot", await="adopted", wait_s=5}]'])
    assert contract["steps"][0] == {"at": "world-ready", "frames": 0, "do": "shot", "arg": "", "wait_s": 5, "await": "adopted"}
    with pytest.raises(ConfigError, match="step 1: await takes adopted, not 'later'"):
        r("base", configs, ['steps=[{at="world-ready", do="shot", await="later"}]'])
    with pytest.raises(ConfigError, match="await takes a string"):
        r("base", configs, ['steps=[{at="world-ready", do="shot", await=true}]'])


@pytest.mark.parametrize("arg", ["org.rebuild", "org.reload-shaders=all", "cs.ssgi_x=a=b c", "Owner-1.name_2="])
def test_call_step_takes_owner_and_name(configs, arg):
    contract, _ = r("base", configs, [f'steps=[{{at="world-ready", do="call", arg="{arg}"}}]'])
    assert contract["steps"][0]["arg"] == arg


@pytest.mark.parametrize("arg", ["", "rebuild", "org.", ".rebuild", "org rebuild", "org.re build", "a.b.c", "org.x\n"])
def test_call_step_errors(configs, arg):
    with pytest.raises(ConfigError, match="step 1: call takes <owner>.<name>"):
        r("base", configs, [f'steps=[{{at="world-ready", do="call", arg={json.dumps(arg)}}}]'])


def test_org_step_needs_no_check(configs):
    contract, _ = r("base", configs, ['steps=[{at="world-ready", do="org:rebuild"}]'])
    assert contract["steps"][0]["do"] == "org:rebuild"


def test_graph_frames_contract(configs, tmp_path):
    contract, _ = r("base", configs)
    assert contract["expect"]["graph_frames"] == {}
    (tmp_path / "g.toml").write_text('extends = ["base"]\n[expect.graph_frames]\nmin = { "cs.ssgi" = 300, "cs.fog" = 1 }\n', encoding="utf-8")
    contract, _ = r("g", configs)
    assert contract["expect"]["graph_frames"] == {"min": {"cs.ssgi": 300, "cs.fog": 1}}


@pytest.mark.parametrize("value, message", [
    ('{min={"cs.ssgi"=0}}', "at least 1"),
    ('{min={"cs.ssgi"=true}}', "at least 1"),
    ('{min={"cs.ssgi"="many"}}', "at least 1"),
    ('{min=["cs.ssgi"]}', "must be a table of client id"),
    ('{no_d3d11=["cs.ssgi"]}', "unknown key 'expect.graph_frames.no_d3d11'"),
    ('{max={"cs.ssgi"=3}}', "unknown key 'expect.graph_frames.max'"),
    ('5', "expect.graph_frames must be a table"),
])
def test_graph_frames_errors(configs, value, message):
    with pytest.raises(ConfigError, match=message):
        r("base", configs, [f"expect.graph_frames={value}"])


def test_saved_settings(configs, tmp_path):
    contract, harness = r("base", configs)
    assert harness["saved_settings"] == {} and "saved_settings" not in contract
    (tmp_path / "s.toml").write_text('extends = ["base"]\n[saved_settings."Screen Space GI"]\nEnabled = true\nQuality = 2\n', encoding="utf-8")
    (tmp_path / "t.toml").write_text('[saved_settings."Screen Space GI"]\nQuality = 3\n[saved_settings."Disable at Boot"]\n"Light Limit Fix" = false\n', encoding="utf-8")
    _, harness = r("s+t", configs)
    assert harness["saved_settings"] == {"Screen Space GI": {"Enabled": True, "Quality": 3}, "Disable at Boot": {"Light Limit Fix": False}}
    _, harness = r("s", configs, ['saved_settings.Light Limit Fix={"Enable Light Limit Visualization"=false}'])
    assert harness["saved_settings"]["Light Limit Fix"] == {"Enable Light Limit Visualization": False}
    with pytest.raises(ConfigError, match="saved_settings.Light Limit Fix must be a table"):
        r("s", configs, ["saved_settings.Light Limit Fix=3"])
