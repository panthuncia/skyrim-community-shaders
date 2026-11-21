"""The switch schema: switches.json, a copy of the game's switch dump (CommunityShaders-Switches.json).

The game writes the dump from its one switch table (src/Testing/SwitchRegistry.cpp) and DCLF's features'
registered switches. A row with a key is a typed config key. Every other row is known to [env] by name.
Regenerate the copy with "cs_test.py schema" after a dump; never edit it by hand.
"""

import fnmatch
import json
from pathlib import Path

from .paths import SCHEMA_PATH

DUMP_VERSION = 2
TYPES = ("string", "bool", "int", "enum")
KINDS = ("feature", "parity", "diagnostic", "test", "other")
ROW_FIELDS = ("name", "key", "type", "default", "choices", "kind", "family", "description")

# What cs-test asks for instead of a switch it writes itself from a table of the config.
USE_INSTEAD = {"CS_DISABLE_FEATURES": "[features] <ShortName> = false"}


class ConfigError(Exception):
    """A configuration the harness refuses before launch."""


def check_dump(dump):
    """The dump's problems: a list of lines, empty when it can be the schema."""
    if not isinstance(dump, dict):
        return ["the dump is not a JSON object"]
    problems = []
    if dump.get("version") != DUMP_VERSION:
        problems.append(f"version {dump.get('version')!r}, not {DUMP_VERSION}: dump with a build that has the one switch table")
    if not dump.get("dclf_complete"):
        problems.append("dclf_complete is false: DCLF's features' switches are missing (dump with DCLF on)")
    names, keys = set(), set()
    for row in dump.get("switches", []):
        name = row.get("name", "")
        if not name or name in names:
            problems.append(f"switch {name!r} is empty or listed twice")
        names.add(name)
        if row.get("type") not in TYPES:
            problems.append(f"{name}: type {row.get('type')!r}")
        if row.get("kind") not in KINDS:
            problems.append(f"{name}: kind {row.get('kind')!r}")
        if (row.get("type") == "enum") != bool(row.get("choices")):
            problems.append(f"{name}: an enum has choices, and only an enum")
        key = row.get("key", "")
        if key:
            if key in keys or "." not in key or key.startswith("features."):
                problems.append(f"{name}: key {key!r} is taken, has no table or is a feature's")
            keys.add(key)
    if not dump.get("features"):
        problems.append("no features")
    return problems


def _default(row):
    """A row's default as the config value that means it, or None."""
    text = row.get("default", "")
    if text == "":
        return None
    if row.get("type") == "bool":
        return text == "1"
    if row.get("type") == "int":
        try:
            return int(text)
        except ValueError:
            return None
    return text


def from_dump(dump):
    """The schema's tables from a dump: the typed keys, the [env] names and the families read by prefix."""
    keys = {}
    for row in dump.get("switches", []):
        if not row.get("key"):
            continue
        entry = {"env": row["name"], "type": row.get("type", "string"), "family": row.get("family", ""), "kind": row.get("kind", "other"),
                 "description": row.get("description", "")}
        if row.get("choices"):
            entry["choices"] = list(row["choices"])
        default = _default(row)
        if default is not None:
            entry["default"] = default
        if row["name"] in USE_INSTEAD:
            entry["use_instead"] = USE_INSTEAD[row["name"]]
        keys[row["key"]] = entry
    for feature in dump.get("features", []):
        keys[f"features.{feature}"] = {
            "type": "bool", "default": True, "family": "features", "kind": "feature", "sweep": True,
            "description": f"false: {feature} is disabled at boot for this run (features_disabled); true: it loads at boot for this run even "
                           "when the saved settings disable it (features_enabled)"}
    return {
        "key": keys,
        "env": sorted(row["name"] for row in dump.get("switches", [])),
        "allow": {"env": [f"{prefix}*" for prefix in dump.get("prefixes", [])]},
    }


class Schema:
    def __init__(self, data):
        self.keys = data.get("key", {})
        self.allow_env = data.get("allow", {}).get("env", [])
        self.by_env = {}
        for key, entry in self.keys.items():
            if "env" in entry:
                self.by_env.setdefault(entry["env"], key)
        # Every switch the game knows, typed or not: [env] may name any of them.
        self.known_env = set(data.get("env", [])) | set(self.by_env)

    @classmethod
    def load(cls, path=SCHEMA_PATH):
        with open(path, encoding="utf-8") as f:
            return cls(from_dump(json.load(f)))

    def tables(self):
        return sorted({key.split(".", 1)[0] for key in self.keys})

    def get(self, key):
        return self.keys.get(key)

    def expand(self, pattern):
        """The keys a pattern names. "<table>.*" matches only the table's sweep keys."""
        if "*" not in pattern and "?" not in pattern:
            if pattern not in self.keys:
                raise ConfigError(f"unknown key '{pattern}'{self.suggest(pattern)}")
            return [pattern]
        found = sorted(k for k, e in self.keys.items() if e.get("sweep") and fnmatch.fnmatchcase(k, pattern))
        if not found:
            raise ConfigError(f"'{pattern}' matches no sweep key in the schema")
        return found

    def suggest(self, key):
        import difflib

        close = difflib.get_close_matches(key, list(self.keys), n=3, cutoff=0.75)
        return f" (did you mean {', '.join(close)}?)" if close else ""

    def env_allowed(self, name):
        return name in self.known_env or any(fnmatch.fnmatchcase(name, p) for p in self.allow_env)

    def convert(self, key, value):
        """A typed value as its env string. Raises ConfigError on a wrong type."""
        entry = self.keys[key]
        kind = entry.get("type", "string")
        if kind == "bool":
            if not isinstance(value, bool):
                raise ConfigError(f"{key} takes true or false, not {value!r}")
            return "1" if value else "0"
        if kind == "int":
            if isinstance(value, bool) or not isinstance(value, int):
                raise ConfigError(f"{key} takes a whole number, not {value!r}")
            return str(value)
        if kind == "enum":
            if not isinstance(value, str) or value.lower() not in entry.get("choices", []):
                raise ConfigError(f"{key} takes one of {', '.join(entry.get('choices', []))}, not {value!r}")
            return value.lower()
        if kind == "string":
            if isinstance(value, bool):
                return "1" if value else "0"
            if isinstance(value, (int, float)):
                return str(value)
            if not isinstance(value, str):
                raise ConfigError(f"{key} takes a string, not {value!r}")
            return value
        raise ConfigError(f"{key} has type {kind}, which convert does not take")

    def preset(self, key, value):
        """The env lines a preset value stands for."""
        entry = self.keys[key]
        values = entry.get("values", {})
        name = str(value).lower() if isinstance(value, bool) else str(value)
        if name not in values:
            raise ConfigError(f"{key} takes one of {', '.join(values)}, not {value!r}")
        return dict(values[name])


def load_schema(path=None):
    return Schema.load(Path(path) if path else SCHEMA_PATH)


def normalize(dump):
    """The dump as switches.json keeps it: rows by name, each with every field in a fixed order."""
    rows = [{f: row.get(f, [] if f == "choices" else "") for f in ROW_FIELDS} for row in sorted(dump.get("switches", []), key=lambda r: r.get("name", ""))]
    return {
        "version": dump.get("version"),
        "dclf_complete": bool(dump.get("dclf_complete")),
        "prefixes": list(dump.get("prefixes", [])),
        "features": sorted(dump.get("features", [])),
        "switches": rows,
    }


def render(dump):
    """switches.json's text."""
    return json.dumps(normalize(dump), indent="\t", ensure_ascii=False) + "\n"
