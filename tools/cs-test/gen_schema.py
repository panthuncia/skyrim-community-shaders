"""Regenerates schema.toml from a Community Shaders source tree.

    python tools/cs-test/gen_schema.py [<repo root>] [--out schema.toml]

The switches come from the sources:
- DCLF's registry (kRegistry in src/Features/DrawcallLimitFix/Core/Settings/Switches.cpp) and the
  RegisterSwitch calls under src/Features/DrawcallLimitFix, with their kind and description.
- Every other "CS_*" literal in src, and every switch the cs-ab scenarios set.
- The feature short names (GetShortName in src/Features).

The hand-written parts (typed ORG keys, presets, descriptions the sources lack) are the tables below.
Edit them here and regenerate. Do not edit schema.toml by hand.
"""

import argparse
import re
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Typed keys whose name is not the mechanical one, with what the sources do not say.
SPECIAL = {
    "CS_DCLF": {"key": "dclf.enabled", "description": "false: DCLF does not install"},
    "CS_ORG": {"key": "org.enabled", "type": "bool", "default": True},
    "CS_ORG_NATIVE": {
        "key": "org.native", "type": "bool", "default": False,
        "description": "true: native mode, no DXVK; BasicRHI's device and ORG present (render-graph.md, \"Native mode\")",
    },
    "CS_ORG_NATIVE_BACKEND": {
        "key": "org.backend", "type": "enum", "choices": ["vulkan", "d3d12"], "default": "vulkan",
        "description": "native mode's backend; d3d12 needs org.native",
    },
    "CS_ORG_NATIVE_VALIDATION": {
        "key": "org.validation", "type": "bool", "default": False,
        "description": "true: BasicRHI's API validation in native mode (Vulkan layers or the D3D12 debug layer)",
    },
    "CS_DISABLE_FEATURES": {
        "key": "cs.disable_features", "type": "string",
        "use_instead": "[features] <ShortName> = false",
        "description": "comma-separated feature short names disabled at boot (write [features] instead)",
    },
    "CS_ORG_EPOCH_STATS": {
        "key": "org.epoch_stats", "type": "bool", "default": False,
        "description": "true: render-thread CPU time per epoch, each anchor's peak runs a frame, and the run shapes per window "
                       "(\"[ORG] run shapes\")",
    },
    "CS_ORG_NATIVE_OP_CENSUS": {
        "key": "org.native_op_census", "type": "bool", "default": False,
        "description": "true: native mode's immediate-context operations per frame by kind, target and frame position "
                       "(\"[ORG native] op census\")",
    },
    "CS_ORG_NATIVE_WRITE_CENSUS": {
        "key": "org.native_write_census", "type": "bool", "default": False,
        "description": "true: native SRV-only textures and VB/IB-only buffers written after creation (\"[ORG native] write census\")",
    },
    "CS_ORG_VALIDATE_UNDECLARED": {
        "key": "org.validate_undeclared", "type": "bool", "default": False,
        "description": "true: log raw bindless indices and buffer refs of resources that are not exempt (\"[ORG] undeclared access\")",
    },
    "CS_ORG_VALIDATE_UNDECLARED_LOG": {
        "key": "org.validate_undeclared_log", "type": "int",
        "description": "how many first-access lines the undeclared-access validator logs (default 256)",
    },
    "CS_ORG_FRAME_SEQUENCE": {
        "key": "org.frame_sequence", "type": "int",
        "description": "N: log one frame's ordered anchor runs, epoch submissions and boundaries every N frames per window "
                       "(1 means 600; \"[ORG] frame sequence\")",
    },
    "CS_ORG_CENSUS_FRAMES": {
        "key": "org.census_frames", "type": "int",
        "description": "frames per window report of the native censuses, run shapes and validator (default 600)",
    },
    "CS_ORG_CENSUS_TOP": {
        "key": "org.census_top", "type": "int",
        "description": "entries each of those window reports lists (default 25)",
    },
    "CS_ORG_DEFERRED_CLEARS": {
        "key": "org.deferred_clears", "type": "bool", "default": True,
        "description": "false: the engine clears the six GBuffer targets itself, instead of one clear pass at the Prepass anchor",
    },
    # The epoch and submission switches (src/Testing/SwitchRegistry.cpp, kDescribed).
    "CS_ORG_EPOCHS": {
        "key": "org.epochs", "type": "bool", "default": True,
        "description": "0: the render graph runs without epochs, and DCLF does not install",
    },
    "CS_ORG_ASYNC_EPOCHS": {
        "key": "org.async_epochs", "type": "bool", "default": True,
        "description": "0: epochs are recorded on the render thread",
    },
    "CS_ORG_CLOSED": {
        "key": "org.closed", "type": "bool", "default": False,
        "description": "1: epochs return every resource to its home state and start with a full barrier (closed executions, an "
                       "interop fallback)",
    },
    "CS_ORG_EXTERNAL_HOME": {
        "key": "org.external_home", "type": "bool", "default": True,
        "description": "0: epochs do not return DXVK's images to their home state on the graphics queue (unsafe: DXVK expects them "
                       "there)",
    },
    "CS_ORG_BATCH_SUBMIT": {
        "key": "org.batch_submit", "type": "bool", "default": True,
        "description": "0: an epoch's submissions go to DXVK one at a time",
    },
    "CS_ORG_EARLY_FLUSH": {
        "key": "org.early_flush", "type": "bool", "default": False,
        "description": "1: DXVK's pending work is flushed when an epoch starts (a second DXVK list per epoch)",
    },
    # Written into schema.toml by hand before; kept here so a regeneration does not drop them.
    "CS_ORG_IMAGE_DUPLICATE_CHECK": {
        "key": "org.image_duplicate_check", "type": "bool", "default": False,
        "description": "log when two live graph resources wrap one image (any importer)",
    },
    "CS_ORG_RAW_ACCESS_CHECK": {
        "key": "org.raw_access_check", "type": "bool", "default": False,
        "description": "native: log and count work on graph-tracked images that skips the graph's state tracking ([ORG raw access] lines)",
    },
    "CS_HANG_WATCHDOG": {
        "key": "cs.hang_watchdog", "type": "int",
        "description": "seconds without progress before the hang watchdog writes its report (default 15)",
    },
}

# The census and latch lines the census scenarios repeat ("org.census = true").
CENSUS = {
    "CS_ORG_ENGINE_FLUSH_LATCH": "1",
    "CS_ORG_RESIDUE_CENSUS": "1",
    "CS_ORG_RESIDUE_CENSUS_FRAMES": "300",
    "CS_ORG_RESIDUE_CENSUS_TOP": "60",
}

# Env names other software reads (DXVK, the Vulkan loader). Their families cannot be listed whole, so a
# raw [env] name matching one of these passes the schema check. The game checks them again.
ALLOW_ENV = ["DXVK_*", "VK_*", "BASICRHI_*"]

# Literals in src that look like switches but are not read from the environment.
NOT_SWITCHES = {"CS_FFT", "CS_AB_PROBE"}

KIND = {"F": "feature", "P": "parity", "D": "diagnostic", "T": "test", "O": "other"}
STRING = r'"(?:[^"\\]|\\.)*"'


def join_strings(text):
    """The value of adjacent C++ string literals."""
    return "".join(bytes(s[1:-1], "utf-8").decode("unicode_escape") for s in re.findall(STRING, text))


def family_of(name):
    for prefix, family in (("CS_DCLF", "dclf"), ("CS_ORG", "org"), ("ORG_", "org"), ("BASICRHI_", "basicrhi"), ("DXVK_", "dxvk"),
                           ("VK_", "vk"), ("SARP_", "sarp"), ("CS_", "cs")):
        if name.startswith(prefix):
            return family
    return "other"


def key_of(name):
    if name in SPECIAL:
        return SPECIAL[name]["key"]
    for prefix, table in (("CS_DCLF_", "dclf"), ("CS_ORG_", "org"), ("ORG_", "orglib"), ("BASICRHI_", "basicrhi"), ("DXVK_", "dxvk"),
                          ("VK_", "vk"), ("SARP_", "sarp"), ("CS_", "cs")):
        if name.startswith(prefix):
            return f"{table}.{name[len(prefix):].lower()}"
    return f"env.{name.lower()}"


def dclf_rows(root):
    """kRegistry's rows and the RegisterSwitch calls: name -> dict."""
    rows = {}
    text = (root / "src/Features/DrawcallLimitFix/Core/Settings/Switches.cpp").read_text(encoding="utf-8")
    block = text[text.index("kRegistry[] = {"):]
    block = block[:block.index("\n\t\t};")]
    entry = re.compile(r"\{\s*\w+,\s*\"(CS_[A-Z0-9_]+)\",\s*([FPDTO]),\s*(Reduced::\w+|N),\s*(true|false),\s*((?:" + STRING + r"\s*)+)\}")
    for m in entry.finditer(block):
        reduced = m.group(3).replace("Reduced::", "")
        rows[m.group(1)] = {"kind": KIND[m.group(2)], "reduced": "Never" if reduced == "N" else reduced,
                            "description": join_strings(m.group(5)), "owner": "core"}
    call = re.compile(r"RegisterSwitch\(\{(.*?)\}\)", re.S)
    for path in sorted((root / "src/Features/DrawcallLimitFix").rglob("*.cpp")):
        source = path.read_text(encoding="utf-8", errors="replace")
        for m in call.finditer(source):
            body = m.group(1)
            name = re.search(r'\.name\s*=\s*"([A-Z0-9_]+)"', body)
            if not name:
                continue
            kind = re.search(r"SwitchKind::(\w+)", body)
            reduced = re.search(r"SwitchReduced::(\w+)", body)
            description = re.search(r"\.description\s*=\s*((?:" + STRING + r"\s*)+)", body)
            rows[name.group(1)] = {
                "kind": (kind.group(1).lower() if kind else "other"),
                "reduced": reduced.group(1) if reduced else "Never",
                "description": join_strings(description.group(1)) if description else "",
                "owner": path.parent.name,
            }
    return rows


def source_names(root):
    names, excluded = set(), set()
    for path in (root / "src").rglob("*"):
        if path.suffix in (".cpp", ".h", ".hpp", ".inl"):
            text = path.read_text(encoding="utf-8", errors="replace")
            # Shader define names are not switches.
            defines = set(re.findall(r'GetShaderDefineName\(\)[^{]*\{\s*return\s*"([A-Z0-9_]+)"', text))
            for name in re.findall(r'(?<![A-Za-z])"((?:CS|BASICRHI|SARP)_[A-Z0-9_]*[A-Z0-9])"', text):
                if name not in defines:
                    names.add(name)
            names -= defines
            excluded |= defines
    # The render graph library's own switches (ORG_*), read inside the submodule.
    for path in (root / "extern/OpenRenderGraph").glob("*/**/*"):
        if path.suffix in (".cpp", ".h", ".hpp") and "tests" not in path.parts:
            names.update(re.findall(r'(?<![A-Za-z])"(ORG_[A-Z0-9_]*[A-Z0-9])"', path.read_text(encoding="utf-8", errors="replace")))
    return names - excluded - NOT_SWITCHES


def scenario_values(root):
    """Every switch the cs-ab scenarios set, with the values they use."""
    values = {}
    for path in (root / "tools/cs-ab/scenarios").glob("*.env"):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"^([A-Z][A-Z0-9_]+)=(.*)$", line.strip())
            if m:
                values.setdefault(m.group(1), set()).add(m.group(2))
    return values


def guess_type(values):
    if not values:
        return "string"
    if values <= {"0", "1"}:
        return "bool"
    if all(re.fullmatch(r"\d+", v) for v in values):
        return "int"
    return "string"


def dclf_type(row):
    description = row["description"]
    if row["reduced"] in ("UnlessOne", "Zero"):
        return "bool", True
    if row["reduced"] == "Cull":
        return "enum", None
    # "1: ..." describes an off-by-default switch that "1" turns on.
    if description.startswith("1:"):
        return "bool", False
    return "string", None


def features(root):
    names = set()
    for path in (root / "src/Features").glob("*.h"):
        for m in re.finditer(r"GetShortName\(\)\s*override\s*\{\s*return\s*\"([^\"]+)\"", path.read_text(encoding="utf-8", errors="replace")):
            names.add(m.group(1))
    return sorted(names)


def toml_str(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ") + '"'


def toml_value(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, list):
        return "[" + ", ".join(toml_value(v) for v in value) + "]"
    if isinstance(value, dict):
        return "{ " + ", ".join(f"{k} = {toml_value(v)}" for k, v in value.items()) + " }"
    return toml_str(value)


def build(root):
    rows = dclf_rows(root)
    scenarios = scenario_values(root)
    names = set(rows) | source_names(root) | set(scenarios)
    keys = {}
    for name in sorted(names):
        row = rows.get(name)
        entry = {"env": name, "family": "dclf" if row else family_of(name), "kind": row["kind"] if row else "other"}
        if row:
            type_, default = dclf_type(row)
            entry["type"] = type_
            if type_ == "enum":
                entry["choices"] = ["off", "frustum", "occlusion"]
                default = "occlusion"
            if default is not None:
                entry["default"] = default
            # A family: a DCLF feature switch that is on unless set to something other than "1". Sweeps vary these.
            if row["kind"] == "feature" and row["reduced"] == "UnlessOne":
                entry["sweep"] = True
            entry["owner"] = row["owner"]
            entry["description"] = row["description"]
        else:
            entry["type"] = guess_type(scenarios.get(name, set()))
            if name.startswith("CS_ORG_") and entry["type"] == "bool":
                entry["default"] = False
            entry["description"] = ""
        special = SPECIAL.get(name, {})
        entry.update({k: v for k, v in special.items() if k != "key"})
        keys[key_of(name)] = entry
    for feature in features(root):
        keys[f"features.{feature}"] = {"type": "bool", "default": True, "family": "features", "kind": "feature", "sweep": True,
                                       "description": f"false: {feature} is disabled at boot for this run (features_disabled); true: it loads at boot for this run even when the saved settings disable it (features_enabled)"}
    keys["org.census"] = {"type": "preset", "family": "org", "kind": "preset",
                          "description": "true: the residue census and the flush latch (tools/cs-ab/census.py reads them)",
                          "values": {"true": CENSUS, "false": {}}}
    return keys


def render(keys, root):
    try:
        sha = subprocess.run(["git", "-C", str(root), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    except OSError:
        sha = ""
    out = [
        "# The test switches: one row per typed key (tools/cs-test/README.md).",
        f"# Generated by tools/cs-test/gen_schema.py from {sha or 'the sources'}. Edit gen_schema.py, then regenerate.",
        "#",
        "# env: the switch the key sets. type: bool (\"1\"/\"0\"), int, string, enum (choices), or preset (values: a set of env lines).",
        "# family and kind: as the game's switch dump (CommunityShaders-Switches.json) names them; cs-test check-schema compares.",
        "# sweep: \"<table>.*\" in --base and --vary matches only these keys.",
        "",
        "[allow]",
        f"env = {toml_value(ALLOW_ENV)}",
        "",
    ]
    for key in sorted(keys):
        entry = keys[key]
        out.append(f'[key."{key}"]')
        for field in ("env", "type", "choices", "default", "family", "kind", "sweep", "owner", "use_instead", "description"):
            if field in entry:
                out.append(f"{field} = {toml_value(entry[field])}")
        if "values" in entry:
            for value, env in entry["values"].items():
                out.append(f'values.{toml_str(value)} = {toml_value(env)}')
        out.append("")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", nargs="?", default=str(HERE.parents[1]))
    parser.add_argument("--out", default=str(HERE / "schema.toml"))
    args = parser.parse_args()
    root = Path(args.root)
    keys = build(root)
    Path(args.out).write_text(render(keys, root), encoding="utf-8", newline="\n")
    print(f"{len(keys)} keys -> {args.out}")


if __name__ == "__main__":
    main()
