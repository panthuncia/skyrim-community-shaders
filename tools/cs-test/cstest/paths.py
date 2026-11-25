"""Where things are. Every path can be moved with an environment variable, which the tests use."""

import os
import re
from dataclasses import dataclass, field
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parents[1]
REPO_DIR = TOOL_DIR.parents[1]
CONFIG_DIR = TOOL_DIR / "configs"
SCHEMA_PATH = TOOL_DIR / "switches.json"  # the game's switch dump, copied by "cs_test.py schema"
HELPER_BUILD = TOOL_DIR / "lab" / "plugin" / "build" / "Release" / "!CSTestLab.dll"

TEST_FILE = "CommunityShaders-Test.json"
STATUS_FILE = "CommunityShaders-Test.status.json"
SWITCHES_FILE = "CommunityShaders-Switches.json"
LOG_FILE = "CommunityShaders.log"

GAME_EXE = "SkyrimSE.exe"


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


def game_from_mo2(mo2):
    """The game folder MO2 runs (ModOrganizer.ini's gamePath)."""
    ini = Path(mo2) / "ModOrganizer.ini"
    text = ini.read_text(encoding="utf-8", errors="replace") if ini.exists() else ""
    match = re.search(r"^gamePath=@ByteArray\((.*)\)\s*$", text, re.MULTILINE)
    if not match:
        raise FileNotFoundError(f"no gamePath in {ini}: set CS_TEST_GAME to the game folder")
    return Path(match.group(1).replace("\\\\", "\\"))


@dataclass
class Paths:
    root: Path  # C:\Modding\CS-AB: runs, stages, locks
    mo2: Path
    profile: str
    documents: Path  # the user's Documents: the game's INIs and saves are read from here, never written
    game: Path = None  # the installed game: only read and hard-linked
    lab: Path = None  # D:\CSLab, on the game's volume
    helper: Path = field(default=HELPER_BUILD)

    @property
    def modlist(self):
        return self.mo2 / "profiles" / self.profile / "modlist.txt"

    @property
    def profile_dir(self):
        return self.mo2 / "profiles" / self.profile

    @property
    def overwrite(self):
        return self.mo2 / "overwrite"

    @property
    def mods(self):
        return self.mo2 / "mods"

    @property
    def state(self):
        # The old MO2 harness's record of its variant swap; restore puts the mod from before it back.
        return self.root / "state.json"

    @property
    def runs(self):
        return self.root / "runs"

    @property
    def stages(self):
        return self.root / "stages"

    @property
    def locks(self):
        return self.root / "lab"

    @property
    def game_documents(self):
        return self.documents / "My Games" / "Skyrim Special Edition"


def default_paths():
    mo2 = Path(os.environ.get("CS_TEST_MO2", r"C:\Modding\MO2"))
    game = os.environ.get("CS_TEST_GAME")
    documents = os.environ.get("CS_TEST_DOCUMENTS")
    return Paths(
        root=Path(os.environ.get("CS_TEST_ROOT", r"C:\Modding\CS-AB")),
        mo2=mo2,
        profile=os.environ.get("CS_TEST_PROFILE", "Default"),
        documents=Path(documents) if documents else _documents(),
        game=Path(game) if game else game_from_mo2(mo2),
        lab=Path(os.environ.get("CS_TEST_LAB", r"D:\CSLab")),
        helper=Path(os.environ.get("CS_TEST_LAB_HELPER", str(HELPER_BUILD))),
    )
