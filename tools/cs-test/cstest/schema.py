"""The switch schema (schema.toml): typed keys, their env names, types and presets."""

import fnmatch
import tomllib
from pathlib import Path

from .paths import SCHEMA_PATH


class ConfigError(Exception):
    """A configuration the harness refuses before launch."""


class Schema:
    def __init__(self, data):
        self.keys = data.get("key", {})
        self.allow_env = data.get("allow", {}).get("env", [])
        self.by_env = {}
        for key, entry in self.keys.items():
            if "env" in entry:
                self.by_env.setdefault(entry["env"], key)

    @classmethod
    def load(cls, path=SCHEMA_PATH):
        with open(path, "rb") as f:
            return cls(tomllib.load(f))

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
        return name in self.by_env or any(fnmatch.fnmatchcase(name, p) for p in self.allow_env)

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
