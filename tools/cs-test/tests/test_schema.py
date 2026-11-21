import json

import pytest

from cstest import commands
from cstest.config import resolve
from cstest.paths import SCHEMA_PATH
from cstest.schema import ConfigError, Schema, check_dump, from_dump, load_schema, render

DUMP = {
    "version": 2,
    "dclf_complete": True,
    "prefixes": ["DXVK_"],
    "features": ["Skylighting", "Upscaling"],
    "switches": [
        {"name": "CS_DCLF_CULL", "key": "dclf.cull", "type": "enum", "default": "occlusion", "choices": ["off", "frustum", "occlusion"],
         "kind": "feature", "family": "dclf", "description": "culling"},
        {"name": "CS_ORG_NATIVE", "key": "org.native", "type": "bool", "default": "0", "choices": [], "kind": "other", "family": "org", "description": ""},
        {"name": "CS_HANG_WATCHDOG", "key": "cs.hang_watchdog", "type": "int", "default": "", "choices": [], "kind": "other", "family": "cs", "description": ""},
        {"name": "CS_DISABLE_FEATURES", "key": "cs.disable_features", "type": "string", "default": "", "choices": [], "kind": "other", "family": "cs",
         "description": ""},
        {"name": "CS_ORG_TAP_ERROR_TEST", "key": "", "type": "string", "default": "", "choices": [], "kind": "other", "family": "org", "description": ""},
    ],
}


def test_the_dump_is_the_schema():
    schema = Schema(from_dump(DUMP))
    assert schema.get("dclf.cull")["choices"] == ["off", "frustum", "occlusion"] and schema.get("dclf.cull")["default"] == "occlusion"
    assert schema.get("org.native")["default"] is False and schema.get("org.native")["env"] == "CS_ORG_NATIVE"
    assert "default" not in schema.get("cs.hang_watchdog")
    assert schema.get("cs.disable_features")["use_instead"].startswith("[features]")
    assert schema.get("features.Skylighting")["sweep"] and schema.expand("features.*") == ["features.Skylighting", "features.Upscaling"]
    # A row without a key is set by name; a prefix family by any name.
    assert schema.get("org.tap_error_test") is None
    assert schema.env_allowed("CS_ORG_TAP_ERROR_TEST") and schema.env_allowed("DXVK_HUD") and not schema.env_allowed("CS_ORG_NO_SUCH")


@pytest.mark.parametrize("change, problem", [
    ({"version": 1}, "version"),
    ({"dclf_complete": False}, "DCLF"),
    ({"features": []}, "no features"),
])
def test_a_dump_that_cannot_be_the_schema(change, problem):
    assert any(problem in line for line in check_dump({**DUMP, **change}))


def test_bad_rows_are_named():
    rows = [dict(r) for r in DUMP["switches"]]
    rows[1]["key"] = "dclf.cull"
    rows[2]["type"] = "float"
    rows[4]["choices"] = ["a"]
    problems = check_dump({**DUMP, "switches": rows})
    assert any("CS_ORG_NATIVE: key" in p for p in problems) and any("CS_HANG_WATCHDOG: type" in p for p in problems)
    assert any("CS_ORG_TAP_ERROR_TEST: an enum" in p for p in problems)


def test_schema_copies_a_dump_and_check_schema_compares(fake, tmp_path):
    out = tmp_path / "switches.json"
    (fake.skse / "CommunityShaders-Switches.json").write_text(json.dumps(DUMP), encoding="utf-8")
    lines = []
    assert commands.write_schema(fake, out=out, log=lines.append)
    assert out.read_text(encoding="utf-8") == render(DUMP)
    changed = json.loads(json.dumps(DUMP))
    changed["switches"][1]["default"] = "1"
    changed["switches"].append({"name": "CS_NEW", "key": "", "type": "string", "kind": "other", "family": "cs"})
    lines.clear()
    assert commands.schema_differences(DUMP, changed) == ["added: CS_NEW (cs, other)", "default differs for CS_ORG_NATIVE: '0' -> '1'"]
    (fake.skse / "CommunityShaders-Switches.json").write_text(json.dumps({**DUMP, "dclf_complete": False}), encoding="utf-8")
    assert not commands.write_schema(fake, out=out, log=lines.append)
    assert any("DCLF" in line for line in lines)


def test_the_checked_in_schema_is_a_whole_dump():
    data = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    assert check_dump(data) == []
    assert SCHEMA_PATH.read_text(encoding="utf-8") == render(data), "switches.json is written by 'cs_test.py schema', not by hand"
    schema = load_schema()
    assert schema.get("dclf.enabled")["env"] == "CS_DCLF" and schema.get("org.backend")["choices"] == ["vulkan", "d3d12"]
    contract, _ = resolve("quick", ["build.stage=x", "dclf.cull=frustum"], schema=schema)
    assert contract["env"]["CS_DCLF_CULL"] == "frustum"
    with pytest.raises(ConfigError, match="unknown switch"):
        resolve("quick", ["build.stage=x", "env.CS_ORG_DEFERRED_CLEARS=0"], schema=schema)
