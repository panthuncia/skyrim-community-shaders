"""Lab slots: copies of the game that run side by side, one game each (README "Lab").

The lab folder (D:\\CSLab) sits on the game's volume, so the game's files hard-link into it:

    mods/<mod>/        copies of the MO2 mods that live on another volume; mods/_overwrite/ is MO2's overwrite folder
    ShaderCache/       the shared shader cache; ShaderCache.json holds its generation
    slots/<n>/game/    a slot's game folder: the game folder's own files, and Data built from the layers
    slots/<n>/home/    the slot's Documents and local AppData: INIs, plugins.txt, the save, the SKSE logs
    slots/<n>/manifest.json   what each file in the slot's Data came from
    slots/<n>/slot.json       the shader cache generation it holds, and its last game's PID

Data is MO2's view of the game: the game's own Data, then each enabled mod by priority, then the staged build, then
overwrite. Every CS mod in the mod list is left out but the staged one. A file the game or a plugin may write is copied
into the slot; every other file is a hard link. Nothing is ever written through a hard link: a file is deleted before it
is replaced, so the installed game and the mirrors never change.
"""

import fnmatch
import json
import os
import shutil
import time
from dataclasses import dataclass
from pathlib import Path

from . import locks, mo2, procs
from .paths import GAME_EXE, LOG_FILE, STATUS_FILE, TEST_FILE

APP_ID = "489830"
LOADER = "skse64_loader.exe"
HELPER_NAME = "!CSTestLab.dll"
HELPER_ERROR = "CSTestLab-error.txt"
HELPER_MARKER = "cs-test-lab.json"
OVERWRITE = "_overwrite"
DEFAULT_SLOTS = 3

# Copied into a slot, never linked: files a game or plugin writes. Configs anywhere, every SKSE file but code, and everything in
# overwrite, which holds what the games wrote (Community Shaders rewrites its water flowmap textures there at each start). Its
# grass cache is linked: No Grass In Objects writes it only in its precache mode (PrecacheGrass.txt), which no run uses.
CONFIG_EXT = (".ini", ".json", ".toml", ".yaml", ".yml", ".txt", ".cfg", ".config", ".xml", ".log")
CODE_EXT = (".dll", ".pdb", ".exe")
# Never in a slot: RenderDoc captures (14 GB in overwrite), MO2's meta.ini, cs-ab's usvfs padding, and the shader cache,
# which has its own copy per slot (prepare_cache).
EXCLUDE_NAMES = ("*.rdc",)
EXCLUDE_ROOT = ("meta.ini", "_cs-ab-pad", "shadercache")
# The game folder's own files: copied, but these are left out (captures, dumps, screenshots, logs) or hard-linked.
ROOT_SKIP = ("*.rdc", "*.dmp", "*.log", "*.png", "*.bak", "*.unpacked")
ROOT_LINK = ("*.pdb",)


class LabError(Exception):
    pass


@dataclass
class Entry:
    rel: str
    src: str
    size: int
    mtime: int  # ns
    mode: str  # "link" or "copy"


class Slot:
    def __init__(self, lab, number):
        self.number = number
        self.dir = Path(lab) / "slots" / str(number)
        self.game = self.dir / "game"
        self.data = self.game / "Data"
        self.home = self.dir / "home"
        self.documents = self.home / "Documents" / "My Games" / "Skyrim Special Edition"
        self.skse = self.documents / "SKSE"
        self.saves = self.documents / "Saves"
        self.local = self.home / "AppData" / "Local" / "Skyrim Special Edition"
        self.manifest = self.dir / "manifest.json"
        self.state_path = self.dir / "slot.json"
        self.exe = self.game / GAME_EXE
        self.cache = self.data / "ShaderCache"

    def state(self):
        try:
            return json.loads(self.state_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def set_state(self, **values):
        state = self.state()
        state.update(values)
        locks.write_json(self.state_path, state)


def same_volume(a, b):
    return os.path.splitdrive(str(Path(a).resolve()))[0].lower() == os.path.splitdrive(str(Path(b).resolve()))[0].lower()


def same_file(size_a, mtime_a, size_b, mtime_b):
    # Times in nanoseconds: a copy keeps its source's time exactly (NTFS keeps 100 ns), and a game's rewrite changes it.
    return size_a == size_b and mtime_a == mtime_b


def excluded(rel_lower):
    first = rel_lower.split("/", 1)[0]
    if first in EXCLUDE_ROOT:
        return True
    name = rel_lower.rsplit("/", 1)[-1]
    return any(fnmatch.fnmatch(name, p) for p in EXCLUDE_NAMES)


def scan(root, skip_top=()):
    """Every file under root: lower-case relative path -> (relative path, full path, size, mtime in ns). Missing root: empty."""
    out = {}
    root = Path(root)
    if not root.exists():
        return out
    stack = [("", str(root))]
    while stack:
        prefix, folder = stack.pop()
        with os.scandir(folder) as entries:
            for entry in entries:
                rel = f"{prefix}{entry.name}"
                if entry.is_dir(follow_symlinks=False):
                    if not prefix and entry.name.lower() in skip_top:
                        continue
                    stack.append((rel + "/", entry.path))
                else:
                    stat = entry.stat(follow_symlinks=False)
                    out[rel.lower()] = (rel, entry.path, stat.st_size, stat.st_mtime_ns)
    return out


def copied(rel_lower, kind):
    """Whether a file is copied into a slot rather than hard-linked."""
    if kind in ("stage", "helper"):
        return True
    if kind == "overwrite":
        return not rel_lower.startswith("grass/")
    name = rel_lower.rsplit("/", 1)[-1]
    ext = os.path.splitext(name)[1]
    if rel_lower.split("/", 1)[0] == "skse":
        return ext not in CODE_EXT
    return ext in CONFIG_EXT


def replace(src, dst, mode):
    """Puts src at dst as a hard link or a copy. An existing dst is deleted first, so nothing is written through a hard link."""
    dst = Path(dst)
    try:
        if dst.exists() or dst.is_symlink():
            dst.unlink()
    except OSError as error:
        raise LabError(f"cannot replace {dst} ({error}): a running game has it open") from error
    dst.parent.mkdir(parents=True, exist_ok=True)
    if mode == "link":
        os.link(src, dst)
    else:
        shutil.copy2(src, dst)


def mirror(src_root, dst_root, exclude=excluded, log=print):
    """Makes dst_root hold copies of src_root's files (only changed ones are copied). Returns (copied, deleted)."""
    wanted = {k: v for k, v in scan(src_root).items() if not exclude(k)}
    have = scan(dst_root)
    copied_n = deleted = 0
    for key, (_, path, _, _) in have.items():
        if key not in wanted:
            try:
                os.unlink(path)
            except OSError as error:
                raise LabError(f"cannot delete {path} ({error}): a running game has it open") from error
            deleted += 1
    for key, (rel, path, size, mtime) in wanted.items():
        old = have.get(key)
        if old and same_file(old[2], old[3], size, mtime):
            continue
        replace(path, Path(dst_root) / rel, "copy")
        copied_n += 1
    return copied_n, deleted


class Lab:
    """The lab folder and its shared parts."""

    def __init__(self, paths):
        self.paths = paths
        self.root = Path(paths.lab)
        self.mirrors = self.root / "mods"
        self.cache = self.root / "ShaderCache"
        self.cache_state = self.root / "ShaderCache.json"
        self.locks = paths.locks

    def slot(self, number):
        return Slot(self.root, number)

    def lock(self, name, command=""):
        return locks.Lock(self.locks / f"{name}.lock", command)

    # The mod list.

    def enabled_mods(self):
        """MO2's enabled mods, lowest priority first, without the CS mods (one of them, the staged build, goes on top)."""
        lines = mo2.read_lines(self.paths.modlist)
        mods = [line[1:] for line in lines if line.startswith("+") and not mo2.CS_MOD.match(line)]
        return list(reversed(mods))

    def mod_source(self, name):
        """Where a mod's files are linked from: MO2's own folder on the lab's volume, else the lab's copy."""
        folder = self.paths.overwrite if name == OVERWRITE else self.paths.mods / name
        return folder if same_volume(folder, self.root) else self.mirrors / name

    def refresh_mirrors(self, mods, log=print):
        """Brings the lab's copies of the mods on another volume (and overwrite) up to date with MO2."""
        started = time.monotonic()
        total_copied = total_deleted = 0
        for name in list(mods) + [OVERWRITE]:
            folder = self.paths.overwrite if name == OVERWRITE else self.paths.mods / name
            if not folder.exists():
                raise LabError(f"the mod list names {name}, but {folder} does not exist")
            if same_volume(folder, self.root):
                continue
            copied_n, deleted = mirror(folder, self.mirrors / name)
            if copied_n or deleted:
                log(f"  lab copy of {name}: {copied_n} files copied, {deleted} deleted")
            total_copied += copied_n
            total_deleted += deleted
        if total_copied or total_deleted:
            log(f"Lab mod copies refreshed in {time.monotonic() - started:.1f} s")

    def data_entries(self, mods, stage_mod):
        """The slot's Data: lower-case relative path -> Entry, from the layers in MO2's order."""
        game_data = Path(self.paths.game) / "Data"
        layers = [(game_data, "game")]
        layers += [(self.mod_source(m), "mod") for m in mods]
        layers += [(self.paths.mods / stage_mod, "stage"), (self.mod_source(OVERWRITE), "overwrite")]
        entries = {}
        for root, kind in layers:
            linkable = same_volume(root, self.root)
            for key, (rel, path, size, mtime) in scan(root).items():
                if excluded(key):
                    continue
                mode = "copy" if copied(key, kind) or not linkable else "link"
                entries[key] = Entry(rel, path, size, mtime, mode)
        helper = Path(self.paths.helper)
        if not helper.exists():
            raise LabError(f"no lab helper at {helper}: build it with tools/cs-test/lab/plugin/build.cmd")
        stat = helper.stat()
        rel = f"SKSE/Plugins/{HELPER_NAME}"
        entries[rel.lower()] = Entry(rel, str(helper), stat.st_size, stat.st_mtime_ns, "copy")
        return entries

    def root_entries(self):
        """The game folder's own files (not its folders), and steam_appid.txt."""
        entries = {}
        for entry in os.scandir(self.paths.game):
            if not entry.is_file() or any(fnmatch.fnmatch(entry.name.lower(), p) for p in ROOT_SKIP):
                continue
            stat = entry.stat()
            mode = "link" if any(fnmatch.fnmatch(entry.name.lower(), p) for p in ROOT_LINK) else "copy"
            entries[entry.name.lower()] = Entry(entry.name, entry.path, stat.st_size, stat.st_mtime_ns, mode)
        if not (Path(self.paths.game) / LOADER).exists():
            raise LabError(f"no {LOADER} in {self.paths.game}")
        # Steam starts the installed game instead of this one without it.
        appid = self.root / "steam_appid.txt"
        if not appid.exists() or appid.read_text(encoding="ascii") != APP_ID:
            appid.parent.mkdir(parents=True, exist_ok=True)
            appid.write_text(APP_ID, encoding="ascii")
        stat = appid.stat()
        entries["steam_appid.txt"] = Entry("steam_appid.txt", str(appid), stat.st_size, stat.st_mtime_ns, "copy")
        return entries


def sync(target, entries, manifest_path, skip_top=(), top_only=False, log=print):
    """Makes target hold exactly entries: unchanged files stay, changed ones are replaced, extra ones deleted.
    Returns (placed, deleted)."""
    try:
        manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        manifest = {}
    target = Path(target)
    if top_only:
        have = {}
        if target.exists():
            for entry in os.scandir(target):
                if entry.is_file():
                    stat = entry.stat()
                    have[entry.name.lower()] = (entry.name, entry.path, stat.st_size, stat.st_mtime_ns)
    else:
        have = scan(target, skip_top)
    deleted = placed = 0
    for key, (_, path, _, _) in have.items():
        if key not in entries:
            try:
                os.unlink(path)
            except OSError as error:
                raise LabError(f"cannot delete {path} ({error})") from error
            deleted += 1
    record = {}
    for key, e in entries.items():
        old = have.get(key)
        known = manifest.get(key)
        record[key] = [e.src, e.size, e.mtime, e.mode]
        if old and known and known[0] == e.src and known[3] == e.mode and same_file(known[1], known[2], e.size, e.mtime) \
                and same_file(old[2], old[3], e.size, e.mtime):
            continue
        replace(e.src, target / e.rel, e.mode)
        placed += 1
    locks.write_json(manifest_path, record)
    return placed, deleted


def build_slot(lab, slot, mods, stage_mod, log=print):
    """Brings a slot's game folder to this run's mod list and staged build."""
    started = time.monotonic()
    root_placed, root_deleted = sync(slot.game, lab.root_entries(), slot.dir / "manifest-root.json", top_only=True)
    entries = lab.data_entries(mods, stage_mod)
    placed, deleted = sync(slot.data, entries, slot.manifest, skip_top=("shadercache",))
    copies = sum(1 for e in entries.values() if e.mode == "copy")
    log(f"Slot {slot.number}: {len(entries)} files in Data ({copies} copied, the rest hard links); {placed + root_placed} placed, "
        f"{deleted + root_deleted} deleted, in {time.monotonic() - started:.1f} s")


def plugins_of(folder):
    """The plugin files at a mod's root."""
    folder = Path(folder)
    if not folder.exists():
        return []
    return [p.name for p in folder.iterdir() if p.is_file() and p.suffix.lower() in (".esp", ".esm", ".esl")]


def profile_flag(paths, name):
    settings = paths.profile_dir / "settings.ini"
    text = settings.read_text(encoding="latin-1") if settings.exists() else ""
    return any(line.strip().lower().replace(" ", "") == f"{name.lower()}=true" for line in text.splitlines())


def ini_folder(paths):
    """Where the game's INIs come from: the profile's own with LocalSettings=true, else the user's Documents."""
    return paths.profile_dir if profile_flag(paths, "LocalSettings") else paths.game_documents


def saves_folder(paths):
    return paths.profile_dir / "saves" if profile_flag(paths, "LocalSaves") else paths.game_documents / "Saves"


def pick_save(paths, save):
    """The save a run loads: the newest one for "autoload" (what Start On Save picks), else the named one."""
    folder = saves_folder(paths)
    if save == "autoload":
        saves = sorted(folder.glob("*.ess"), key=lambda p: p.stat().st_mtime) if folder.exists() else []
        if not saves:
            raise LabError(f"no saves in {folder} for the autoload mod to load")
        return saves[-1]
    path = folder / f"{save}.ess"
    if not path.exists():
        raise LabError(f"save '{save}' not found in {folder}")
    return path


def write_home(paths, slot, contract, prefs, plugins_off, set_ini_values, log=print):
    """The slot's Documents and local AppData for one run: INIs (kept running unfocused), the one save it loads, plugins.txt and
    the test file. The SKSE folder starts empty, so the run's logs are its own."""
    for folder in (slot.skse, slot.saves):
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
    source = ini_folder(paths)
    for name in ("Skyrim.ini", "SkyrimPrefs.ini", "SkyrimCustom.ini"):
        src = source / name
        dst = slot.documents / name
        dst.unlink(missing_ok=True)
        if src.exists():
            shutil.copy2(src, dst)
    skyrim = slot.documents / "Skyrim.ini"
    if not skyrim.exists():
        raise LabError(f"no Skyrim.ini in {source}")
    # Several games run at once and none has the focus.
    skyrim.write_text(set_ini_values(skyrim.read_text(encoding="latin-1"), {"General": {"bAlwaysActive": True}}), encoding="latin-1", newline="\r\n")
    target = slot.documents / "SkyrimPrefs.ini"
    if not target.exists():
        raise LabError(f"no SkyrimPrefs.ini in {source}")
    # The game loads plugins.txt's plugins only with bEnableFileSelection=1 in SkyrimPrefs.ini's [Launcher]. MO2 does not need
    # it, so the user's file may lack it.
    values = {"Launcher": {"bEnableFileSelection": True}}
    for section, keys in prefs.items():
        values.setdefault(section, {}).update(keys)
    target.write_text(set_ini_values(target.read_text(encoding="latin-1"), values), encoding="latin-1", newline="\r\n")
    if prefs:
        log(f"Prefs INI: {', '.join(f'{s}.{k}' for s, keys in prefs.items() for k in keys)} written into the slot's SkyrimPrefs.ini")
    save = pick_save(paths, contract["game"]["save"])
    shutil.copy2(save, slot.saves / save.name)
    if save.with_suffix(".skse").exists():
        shutil.copy2(save.with_suffix(".skse"), slot.saves / save.with_suffix(".skse").name)
    slot.local.mkdir(parents=True, exist_ok=True)
    off = {p.lower() for p in plugins_off}
    lines = (paths.profile_dir / "plugins.txt").read_text(encoding="utf-8-sig").splitlines()
    kept = [line for line in lines if line.lstrip("*").strip().lower() not in off]
    # Bytes, not text: text mode would turn each \r\n into \r\r\n, and the game would find no plugin by those names.
    (slot.local / "plugins.txt").write_bytes(("\r\n".join(kept) + "\r\n").encode("utf-8"))
    order = paths.profile_dir / "loadorder.txt"
    if order.exists():
        shutil.copy2(order, slot.local / "loadorder.txt")
    (slot.home / HELPER_MARKER).unlink(missing_ok=True)
    (slot.data / "SKSE" / "Plugins" / HELPER_ERROR).unlink(missing_ok=True)
    (slot.skse / TEST_FILE).write_text(json.dumps(contract, indent=2), encoding="utf-8")
    return save.stem


# The shader cache. The store keeps one writer per folder (a named mutex from the folder's path), and every game rewrites its
# pipeline cache whole, so two games never share a folder. Each slot has its own copy. The shared one is the newest a run
# finished with: a run starts from it and, ending cleanly, puts its copy back when no other run did so meanwhile.

def cache_generation(lab):
    try:
        return json.loads(lab.cache_state.read_text(encoding="utf-8"))["generation"]
    except (OSError, ValueError, KeyError):
        return 0


def prepare_cache(lab, slot, cold, log=print):
    """Before a run: the slot's cache from the shared one (or empty for a cold run)."""
    if cold:
        if slot.cache.exists():
            shutil.rmtree(slot.cache)
        slot.set_state(cache=-1)
        log(f"Slot {slot.number}: empty shader cache for this run (build.shader_cache = cold)")
        return
    lock = lab.lock("cache", f"slot {slot.number} cache")
    lock.acquire(600, log=log)
    try:
        generation = cache_generation(lab)
        if generation == 0:
            seed = lab.paths.overwrite / "ShaderCache"
            log(f"Making the lab's shared shader cache from {seed}")
            started = time.monotonic()
            mirror(seed, lab.cache, exclude=lambda _: False)
            generation = 1
            locks.write_json(lab.cache_state, {"generation": generation})
            log(f"  {time.monotonic() - started:.1f} s")
        if slot.state().get("cache") != generation:
            started = time.monotonic()
            copied_n, deleted = mirror(lab.cache, slot.cache, exclude=lambda _: False)
            slot.set_state(cache=generation)
            log(f"Slot {slot.number}: shader cache from the shared one (generation {generation}): {copied_n} files copied, {deleted} deleted, "
                f"in {time.monotonic() - started:.1f} s")
    finally:
        lock.release()


def merge_cache(lab, slot, clean, log=print):
    """After a run: the slot's cache becomes the shared one, if the run ended cleanly and no other run's cache went in since it
    started. A killed game may have left a pipeline cache half written, so its slot starts from the shared one next time."""
    held = slot.state().get("cache")
    if held is None or held < 0:
        return
    if not clean:
        slot.set_state(cache=-1)
        return
    lock = lab.lock("cache", f"slot {slot.number} cache")
    lock.acquire(600, log=log)
    try:
        generation = cache_generation(lab)
        if held != generation:
            log(f"Shader cache: slot {slot.number}'s not kept: another run's (generation {generation}) went in after this run started from {held}")
            slot.set_state(cache=-1)
            return
        started = time.monotonic()
        copied_n, deleted = mirror(slot.cache, lab.cache, exclude=lambda _: False)
        if copied_n or deleted:
            generation += 1
            locks.write_json(lab.cache_state, {"generation": generation})
            log(f"Shader cache: slot {slot.number}'s is the shared one now (generation {generation}): {copied_n} files copied, {deleted} deleted, "
                f"in {time.monotonic() - started:.1f} s")
        slot.set_state(cache=generation)
    finally:
        lock.release()


# Slots.

def slot_count(paths, override=None):
    """--slots, else CS_TEST_SLOTS, else <root>\\lab\\settings.json's "slots", else 3."""
    if override:
        return override
    if os.environ.get("CS_TEST_SLOTS"):
        return int(os.environ["CS_TEST_SLOTS"])
    try:
        return int(json.loads((paths.locks / "settings.json").read_text(encoding="utf-8"))["slots"])
    except (OSError, ValueError, KeyError):
        return DEFAULT_SLOTS


class Claim:
    """The slots a run holds. An exclusive run holds all of them, so no other game runs beside it."""

    def __init__(self, lab, numbers, held, exclusive_lock=None):
        self.lab = lab
        self.numbers = numbers
        self.held = held
        self.exclusive_lock = exclusive_lock

    @property
    def slot(self):
        return self.lab.slot(self.numbers[0])

    def release(self):
        for lock in self.held:
            lock.release()
        if self.exclusive_lock:
            self.exclusive_lock.release()


def existing_slots(lab):
    folder = lab.root / "slots"
    numbers = [int(p.name) for p in folder.iterdir() if p.is_dir() and p.name.isdigit()] if folder.exists() else []
    return max(numbers) + 1 if numbers else 0


def take_slots(lab, count, exclusive, wait_s, command, log=print, poll_s=2, alive=procs.alive):
    """A free slot (or, exclusive, every slot). Waits up to wait_s; raises LockError when it runs out."""
    deadline = time.monotonic() + wait_s
    exclusive_lock = lab.lock("exclusive", command)
    if exclusive:
        exclusive_lock.acquire(wait_s, alive=alive, log=log, poll_s=poll_s)
        held = []
        try:
            for number in range(max(count, existing_slots(lab))):
                lock = lab.lock(f"slot-{number}", command)
                lock.acquire(max(0, deadline - time.monotonic()), alive=alive, log=log, poll_s=poll_s)
                held.append(lock)
        except BaseException:
            for lock in held:
                lock.release()
            exclusive_lock.release()
            raise
        return Claim(lab, list(range(len(held))), held, exclusive_lock)
    announced = False
    while True:
        owner = exclusive_lock.owner()
        if not (owner and locks.Lock.is_live(owner, alive)):
            for number in range(count):
                lock = lab.lock(f"slot-{number}", command)
                if lock.try_acquire(alive, log):
                    return Claim(lab, [number], [lock])
        if time.monotonic() >= deadline:
            raise locks.LockError(f"all {count} lab slots are busy (cs_test.py status shows them); pass --wait S to queue")
        if not announced:
            log(f"Waiting for a free lab slot ({count} slots)")
            announced = True
        time.sleep(poll_s)


def kill_orphans(slot, log=print):
    """A game still running from this slot belongs to a cs-test that was killed: its slot lock is stale, and this run holds it now."""
    orphans = procs.pids_of(slot.exe)
    if orphans:
        log(f"Slot {slot.number}: stopping a game left by a killed run: {', '.join(map(str, orphans))}")
        if not procs.stop(orphans, log=log):
            raise LabError(f"slot {slot.number}'s old game {orphans} does not stop")


def launch(slot, log=print):
    """Starts skse64_loader.exe in the slot, with the slot's home. Returns the loader's process."""
    import subprocess

    env = dict(os.environ)
    env["CS_TEST_LAB_HOME"] = str(slot.home)
    env["SteamAppId"] = APP_ID
    env["SteamGameId"] = APP_ID
    return subprocess.Popen([str(slot.game / LOADER)], cwd=str(slot.game), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def helper_marker(slot):
    try:
        return json.loads((slot.home / HELPER_MARKER).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def helper_error(slot):
    path = slot.data / "SKSE" / "Plugins" / HELPER_ERROR
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


class LaunchGate:
    """One game starts at a time: CommonLib's shared Address Library table fails when two games fill it at once. The lock is
    held from the launch until the helper reports kPostLoad (every plugin's SKSEPlugin_Load has returned, so each CommonLib
    has filled its table) or the game is gone."""

    def __init__(self, lab, slot, command, wait_s=900, log=print):
        self.slot = slot
        self.lock = lab.lock("launch", command)
        self.lock.acquire(wait_s, log=log, poll_s=0.5)
        self.started = time.monotonic()

    def check(self, games, game_seen):
        if not self.lock.held:
            return
        marker = helper_marker(self.slot)
        if marker and marker.get("phase") in ("plugins-loaded", "data-loaded") and (not games or marker.get("pid") in games):
            self.release()
        elif game_seen and not games:
            self.release()

    def release(self):
        self.lock.release()


def write_active(paths, run_id, label, slot, numbers):
    """C:\\Modding\\CS-AB\\lab\\active\\<run id>.json: which slot a run uses and where its files are (perf-tools' trun.sh reads it)."""
    path = paths.locks / "active" / f"{run_id}.json"
    locks.write_json(path, {"pid": os.getpid(), "run_id": run_id, "label": label, "slot": slot.number, "slots": numbers,
                            "home": str(slot.home), "skse": str(slot.skse), "log": str(slot.skse / LOG_FILE),
                            "status": str(slot.skse / STATUS_FILE), "game": str(slot.game)})
    return path


def active_runs(paths, alive=procs.alive):
    out = []
    folder = paths.locks / "active"
    if folder.exists():
        for path in sorted(folder.glob("*.json")):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            if alive(data.get("pid")):
                out.append(data)
            else:
                path.unlink(missing_ok=True)
    return out
