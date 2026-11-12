"""Where things are. Every path can be moved with an environment variable, which the tests use."""

import os
from dataclasses import dataclass
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parents[1]
REPO_DIR = TOOL_DIR.parents[1]
CONFIG_DIR = TOOL_DIR / "configs"
SCHEMA_PATH = TOOL_DIR / "schema.toml"

TEST_FILE = "CommunityShaders-Test.json"
STATUS_FILE = "CommunityShaders-Test.status.json"
SWITCHES_FILE = "CommunityShaders-Switches.json"
LEGACY_ENV_FILE = "CommunityShaders.env"
LOG_FILE = "CommunityShaders.log"

GAME_EXE = "SkyrimSE.exe"
MO2_EXE = "ModOrganizer.exe"


def _documents():
    try:
        import ctypes
        from ctypes import wintypes

        buffer = ctypes.create_unicode_buffer(wintypes.MAX_PATH)
        # CSIDL_PERSONAL = 5: the Documents folder, wherever it was moved.
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buffer) == 0:
            return Path(buffer.value)
    except (AttributeError, OSError):
        pass
    return Path.home() / "Documents"


@dataclass
class Paths:
    root: Path
    mo2: Path
    profile: str
    skse: Path

    @property
    def modlist(self):
        return self.mo2 / "profiles" / self.profile / "modlist.txt"

    @property
    def overwrite(self):
        return self.mo2 / "overwrite"

    @property
    def mods(self):
        return self.mo2 / "mods"

    @property
    def state(self):
        return self.root / "state.json"

    @property
    def journal(self):
        return self.root / "journal.json"

    @property
    def lock(self):
        return self.root / "cs-test.lock"

    @property
    def runs(self):
        return self.root / "runs"

    @property
    def stages(self):
        return self.root / "stages"

    @property
    def variants(self):
        return self.root / "variants"

    @property
    def mods_off_legacy(self):
        # cs-ab.ps1's record of mods a killed run left off.
        return self.root / "mods-off.txt"


def default_paths():
    skse = os.environ.get("CS_TEST_SKSE")
    return Paths(
        root=Path(os.environ.get("CS_TEST_ROOT", r"C:\Modding\CS-AB")),
        mo2=Path(os.environ.get("CS_TEST_MO2", r"C:\Modding\MO2")),
        profile=os.environ.get("CS_TEST_PROFILE", "Default"),
        skse=Path(skse) if skse else _documents() / "My Games" / "Skyrim Special Edition" / "SKSE",
    )
