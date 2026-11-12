"""Resolves a test configuration: extends, "a+b" composition, --set, the schema check, the flat JSON.

The result's contract part is what the game reads (docs/development/testing.md). The "harness" part is
only for cs-test: the build to stage, the mods to turn off, the run's own limits.
"""

import copy
import tomllib
from pathlib import Path

from .paths import CONFIG_DIR
from .schema import ConfigError

MILESTONES = ["plugin", "data-loaded", "shaders-ready", "main-menu", "load-start", "loading-screen", "world-ready"]
WAITS = ("settled", "shaders", "reload", "nvperf")
DEFAULT_TIMEOUTS = {"launch": 60, "compile": 600, "load": 120, "step_wait": 60}

# Tables with fixed fields, and each field's type.
FIXED = {
    "run": {"timeout_s": int, "label": str, "stall_s": int},
    "build": {"stage": str, "shader_cache": str},
    "game": {"save": str, "cell": str, "mods_off": list, "autoload_mod": str},
    "timeouts": {"launch": int, "compile": int, "load": int, "step_wait": int},
    "expect": {"reach": str, "shots": list, "no_log": list},
}
STEP_FIELDS = {"at": str, "frames": int, "do": str, "arg": str, "wait": str, "wait_s": int}
RAW_TABLES = ("env", "dclf_ini")


def parse_value(text):
    """A --set value: TOML when it parses (true, 12, "x", [..], {..}), otherwise the text itself."""
    try:
        return tomllib.loads(f"v = {text}")["v"]
    except tomllib.TOMLDecodeError:
        return text


def find_config(name, config_dir=CONFIG_DIR):
    path = Path(name)
    if path.suffix == ".toml" and path.exists():
        return path
    candidate = Path(config_dir) / f"{name}.toml"
    if not candidate.exists():
        known = ", ".join(sorted(p.stem for p in Path(config_dir).glob("*.toml")))
        raise ConfigError(f"no config '{name}' in {config_dir} (known: {known})")
    return candidate


def load_file(path):
    try:
        with open(path, "rb") as f:
            return tomllib.load(f)
    except tomllib.TOMLDecodeError as error:
        raise ConfigError(f"{path}: {error}") from error


def linearize(names, config_dir=CONFIG_DIR, seen=None, chain=()):
    """Every file the names pull in, each once, parents before the files that extend them."""
    seen = [] if seen is None else seen
    for name in names:
        path = find_config(name, config_dir)
        key = path.resolve()
        if key in chain:
            raise ConfigError(f"extends loop: {' -> '.join(p.stem for p in chain)} -> {path.stem}")
        if any(key == s for s, _ in seen):
            continue
        data = load_file(path)
        extends = data.get("extends", [])
        if isinstance(extends, str):
            extends = [extends]
        linearize(extends, config_dir, seen, chain + (key,))
        if not any(key == s for s, _ in seen):
            seen.append((key, data))
    return seen


def merge(into, layer):
    """Tables merge key by key; everything else (values, arrays, steps) is replaced."""
    for key, value in layer.items():
        if key == "extends":
            continue
        if isinstance(value, dict) and isinstance(into.get(key), dict):
            merge(into[key], value)
        else:
            into[key] = copy.deepcopy(value)
    return into


def apply_set(merged, assignment, schema):
    """One --set k=v. A k with "*" sets every sweep key it matches."""
    if "=" not in assignment:
        raise ConfigError(f"--set '{assignment}' is not key=value")
    key, text = assignment.split("=", 1)
    key = key.strip()
    value = parse_value(text.strip())
    if "." not in key:
        if key != "steps":
            raise ConfigError(f"--set '{key}': name a table and key, such as dclf.shadows or run.timeout_s")
        merged["steps"] = value
        return
    table, name = key.split(".", 1)
    if table in FIXED or table in RAW_TABLES:
        merged.setdefault(table, {})[name] = value
        return
    for full in schema.expand(key):
        t, n = full.split(".", 1)
        merged.setdefault(t, {})[n] = value


def check_fixed(merged):
    for table, fields in FIXED.items():
        section = merged.get(table, {})
        if not isinstance(section, dict):
            raise ConfigError(f"[{table}] must be a table")
        for name, value in section.items():
            if name not in fields:
                raise ConfigError(f"unknown key '{table}.{name}' (known: {', '.join(fields)})")
            want = fields[name]
            if want is int and (isinstance(value, bool) or not isinstance(value, int)):
                raise ConfigError(f"{table}.{name} takes a whole number, not {value!r}")
            if want is str and not isinstance(value, str):
                raise ConfigError(f"{table}.{name} takes a string, not {value!r}")
            if want is list and not (isinstance(value, list) and all(isinstance(v, str) for v in value)):
                raise ConfigError(f"{table}.{name} takes a list of strings, not {value!r}")
    shader_cache = merged.get("build", {}).get("shader_cache", "warm")
    if shader_cache not in ("warm", "cold"):
        raise ConfigError(f"build.shader_cache takes warm or cold, not {shader_cache!r}")
    reach = merged.get("expect", {}).get("reach", "")
    if reach and reach not in MILESTONES:
        raise ConfigError(f"expect.reach takes a milestone ({', '.join(MILESTONES)}), not {reach!r}")


def check_steps(steps):
    if not isinstance(steps, list):
        raise ConfigError("steps must be a list of tables")
    out = []
    for i, step in enumerate(steps):
        if not isinstance(step, dict):
            raise ConfigError(f"step {i + 1} must be a table")
        for name, value in step.items():
            if name not in STEP_FIELDS:
                raise ConfigError(f"step {i + 1}: unknown key '{name}' (known: {', '.join(STEP_FIELDS)})")
            want = STEP_FIELDS[name]
            if want is int and (isinstance(value, bool) or not isinstance(value, int) or value < 0):
                raise ConfigError(f"step {i + 1}: {name} takes a whole number of at least 0, not {value!r}")
            if want is str and not isinstance(value, str):
                raise ConfigError(f"step {i + 1}: {name} takes a string, not {value!r}")
        if not step.get("do"):
            raise ConfigError(f"step {i + 1} has no 'do'")
        at = step.get("at", "previous")
        if at not in MILESTONES and at != "previous":
            raise ConfigError(f"step {i + 1}: at takes a milestone or 'previous', not {at!r}")
        if i == 0 and at == "previous":
            raise ConfigError("step 1 cannot be at 'previous': anchor it to a milestone")
        wait = step.get("wait")
        if wait is not None and wait not in WAITS and not (wait.startswith("signal:") and wait[7:].isdigit()):
            raise ConfigError(f"step {i + 1}: wait takes {', '.join(WAITS)} or signal:<n>, not {wait!r}")
        normal = {"at": at, "frames": step.get("frames", 0), "do": step["do"], "arg": step.get("arg", "")}
        for optional in ("wait", "wait_s"):
            if optional in step:
                normal[optional] = step[optional]
        out.append(normal)
    return out


def flatten(merged, schema, name):
    """The merged tables as the game's JSON (contract) and the harness's own part."""
    known_tables = set(FIXED) | set(RAW_TABLES) | {"steps", "features"} | set(schema.tables())
    for table in merged:
        if table not in known_tables:
            raise ConfigError(f"unknown table [{table}]")
    check_fixed(merged)

    env, origin = {}, {}
    features_disabled = []
    typed = []
    for table in sorted(merged):
        if table in FIXED or table in RAW_TABLES or table == "steps":
            continue
        section = merged[table]
        if not isinstance(section, dict):
            raise ConfigError(f"[{table}] must be a table")
        for key, value in section.items():
            full = f"{table}.{key}"
            if schema.get(full) is None:
                raise ConfigError(f"unknown key '{full}'{schema.suggest(full)}")
            typed.append((full, value))

    # Presets first, so explicit keys win over the lines they expand to.
    for full, value in typed:
        if schema.get(full).get("type") == "preset":
            for var, text in schema.preset(full, value).items():
                env[var] = text
                origin[var] = full
    for full, value in typed:
        entry = schema.get(full)
        if entry.get("type") == "preset":
            continue
        if full.startswith("features."):
            if not isinstance(value, bool):
                raise ConfigError(f"{full} takes true or false, not {value!r}")
            if not value:
                features_disabled.append(full.split(".", 1)[1])
            continue
        if entry.get("use_instead"):
            raise ConfigError(f"{full}: write {entry['use_instead']} instead")
        var = entry["env"]
        env[var] = schema.convert(full, value)
        origin[var] = full

    for var, value in merged.get("env", {}).items():
        if not schema.env_allowed(var):
            raise ConfigError(f"unknown switch [env] {var}: add it to the schema (gen_schema.py) or fix the name")
        key = schema.by_env.get(var)
        if key and schema.get(key).get("use_instead"):
            raise ConfigError(f"[env] {var}: write {schema.get(key)['use_instead']} instead")
        if var in origin and not origin[var].startswith(("org.ports", "org.census")):
            raise ConfigError(f"{var} is set by {origin[var]} and by [env]: keep one")
        if isinstance(value, bool):
            value = "1" if value else "0"
        env[var] = str(value)
        origin[var] = "env"

    dclf_ini = {}
    for var, value in merged.get("dclf_ini", {}).items():
        if not schema.env_allowed(var):
            raise ConfigError(f"unknown switch [dclf_ini] {var}")
        dclf_ini[var] = "1" if value is True else "0" if value is False else str(value)

    game = merged.get("game", {})
    timeouts = dict(DEFAULT_TIMEOUTS)
    timeouts.update(merged.get("timeouts", {}))
    expect = merged.get("expect", {})
    run = merged.get("run", {})
    build = merged.get("build", {})
    contract = {
        "version": 1,
        "name": name,
        "run_id": "",
        "run_dir": "",
        "env": dict(sorted(env.items())),
        "features_disabled": sorted(set(features_disabled)),
        "dclf_ini": dclf_ini,
        "game": {"save": game.get("save", "autoload"), "cell": game.get("cell", "")},
        "timeouts": timeouts,
        "steps": check_steps(merged.get("steps", [])),
        "expect": {"reach": expect.get("reach", ""), "shots": list(expect.get("shots", [])), "no_log": list(expect.get("no_log", []))},
    }
    # A named save: the autoload mod would load its own save first, so it is turned off too.
    mods_off = list(game.get("mods_off", []))
    autoload_mod = game.get("autoload_mod", "")
    autoload_off = game.get("save", "autoload") != "autoload" and bool(autoload_mod)
    if autoload_off and autoload_mod not in mods_off:
        mods_off.append(autoload_mod)
    harness = {
        "stage": build.get("stage", ""),
        "shader_cache": build.get("shader_cache", "warm"),
        "mods_off": mods_off,
        "autoload_off": autoload_mod if autoload_off else "",
        "timeout_s": run.get("timeout_s", 600),
        "stall_s": run.get("stall_s", 90),
        "label": run.get("label", ""),
        "origin": dict(sorted(origin.items())),
    }
    return contract, harness


def resolve(spec, sets=(), schema=None, config_dir=CONFIG_DIR, label=None):
    """Resolves "a+b" plus --set assignments. Returns (contract, harness)."""
    if schema is None:
        from .schema import load_schema

        schema = load_schema()
    names = [n for n in spec.split("+") if n]
    if not names:
        raise ConfigError("no config named")
    merged = {}
    for _, data in linearize(names, config_dir):
        merge(merged, data)
    for assignment in sets:
        apply_set(merged, assignment, schema)
    if label is not None:
        merged.setdefault("run", {})["label"] = label
    return flatten(merged, schema, "+".join(Path(n).stem for n in names))


def diff(base, other, prefix=""):
    """Flat differences between two resolved dicts: [(path, base value, other value)]."""
    out = []
    keys = list(dict.fromkeys(list(base) + list(other))) if isinstance(base, dict) and isinstance(other, dict) else []
    if not keys:
        if base != other:
            out.append((prefix, base, other))
        return out
    for key in keys:
        if key in ("name", "run_id", "run_dir", "origin"):
            continue
        a, b = base.get(key, None), other.get(key, None)
        path = f"{prefix}.{key}" if prefix else key
        if isinstance(a, dict) and isinstance(b, dict):
            out.extend(diff(a, b, path))
        elif a != b:
            out.append((path, a, b))
    return out

