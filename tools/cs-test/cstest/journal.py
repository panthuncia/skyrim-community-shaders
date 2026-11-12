"""The run lock and the journal of every change a run makes, so any exit can undo them.

The journal is written before each change. It is undone in the run's finally, at exit, on Ctrl+C and
Ctrl+Break, and at the start of the next cs-test command when a killed run left one behind.
"""

import datetime
import json
import os
import shutil
import time
from pathlib import Path

from . import mo2, procs
from .paths import GAME_EXE, LEGACY_ENV_FILE, MO2_EXE


class LockError(Exception):
    pass


def _write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    temp.replace(path)


def now():
    return datetime.datetime.now().isoformat(timespec="seconds")


class Lock:
    """C:\\Modding\\CS-AB\\cs-test.lock with the owner's PID. A lock whose PID is gone is stale and taken over."""

    def __init__(self, path, command=""):
        self.path = Path(path)
        self.command = command
        self.held = False

    def owner(self):
        try:
            return json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None

    @staticmethod
    def is_live(owner, alive=procs.alive):
        return bool(owner) and owner.get("pid") != os.getpid() and alive(owner.get("pid"))

    def acquire(self, wait_s=0, alive=procs.alive, log=print):
        deadline = time.monotonic() + wait_s
        self.path.parent.mkdir(parents=True, exist_ok=True)
        announced = False
        while True:
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    json.dump({"pid": os.getpid(), "started": now(), "command": self.command}, f)
                self.held = True
                return
            except FileExistsError:
                owner = self.owner()
                if owner is not None and not self.is_live(owner, alive):
                    log(f"Taking over a stale lock from PID {owner.get('pid')} ({owner.get('command', '')})")
                    self.path.unlink(missing_ok=True)
                    continue
                if owner is None:
                    # Being written right now, or unreadable: wait a moment and look again.
                    time.sleep(0.2)
                    owner = self.owner()
                    if owner is None:
                        self.path.unlink(missing_ok=True)
                        continue
                if time.monotonic() >= deadline:
                    raise LockError(f"another cs-test run holds {self.path}: PID {owner.get('pid')}, {owner.get('command', '')}, since {owner.get('started')}")
                if not announced:
                    log(f"Waiting for PID {owner.get('pid')} ({owner.get('command', '')}) to finish")
                    announced = True
                time.sleep(2)

    def release(self):
        if self.held:
            owner = self.owner()
            if owner and owner.get("pid") == os.getpid():
                self.path.unlink(missing_ok=True)
            self.held = False


class Journal:
    def __init__(self, paths):
        self.paths = paths
        self.path = paths.journal
        self.data = None

    def exists(self):
        return self.path.exists()

    def load(self):
        try:
            return json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None

    def begin(self, run_id):
        self.data = {"pid": os.getpid(), "started": now(), "run_id": run_id, "entries": []}
        _write_json(self.path, self.data)

    def add(self, entry):
        self.data["entries"].append(entry)
        _write_json(self.path, self.data)

    def owned(self):
        data = self.load()
        return bool(data) and data.get("pid") == os.getpid()


def _swap_back(paths, entry):
    state = mo2.get_state(paths)
    if state["live"] != entry["to"]:
        return f"variant already {state['live']}, not {entry['to']}: nothing to swap back"
    back = entry["from"]
    mod = state["originalMod"] if back == "_original" else mo2.mod_for(paths, back)
    mo2.stash_live(paths, entry["to"])
    mo2.unstash(paths, back)
    mo2.set_enabled_mod(paths.modlist, mod)
    state["live"] = back
    mo2.set_state(paths, state)
    return f"variant {entry['to']} -> {back}"


def undo(paths, data, log=print, stop=procs.stop_game_and_mo2, running=None):
    """Undoes a journal's entries, newest first. Returns False (and keeps the journal) if it could not."""
    running = running or (lambda: procs.find(GAME_EXE) + procs.find(MO2_EXE))
    entries = data.get("entries", [])
    launched = any(e.get("do") == "launch" for e in entries)
    if running():
        if not launched:
            log("The game or MO2 is running, and not from this journal's run: close them, then run cs-test restore")
            return False
        log("Stopping the game and MO2 the journal's run started")
        if not stop(GAME_EXE, MO2_EXE, log=log):
            log("The game or MO2 is still running; the journal is kept")
            return False
    failed = False
    for entry in reversed(entries):
        kind = entry.get("do")
        try:
            if kind == "mods_off":
                mo2.enable_mods(paths.modlist, entry["mods"])
                log(f"Turned back on: {', '.join(entry['mods'])}")
            elif kind == "file":
                Path(entry["path"]).unlink(missing_ok=True)
            elif kind == "move":
                src, dst = Path(entry["to"]), Path(entry["from"])
                if src.exists() and not dst.exists():
                    shutil.move(str(src), str(dst))
                    log(f"Put back {dst}")
            elif kind == "variant":
                log(_swap_back(paths, entry))
        except Exception as error:  # Keep going: every other entry should still be undone.
            failed = True
            log(f"Could not undo {entry}: {error}")
    if failed:
        log(f"Some changes were not undone; the journal stays at {paths.journal}")
        return False
    paths.journal.unlink(missing_ok=True)
    return True


def recover(paths, log=print, **kwargs):
    """Undoes a journal a killed run left, and cs-ab.ps1's leftover mods-off.txt. Returns False if something is left."""
    ok = True
    journal = Journal(paths)
    data = journal.load()
    if data is not None:
        pid = data.get("pid")
        if pid != os.getpid() and procs.alive(pid) and "python" in (procs.image_of(pid) or "").lower():
            log(f"The journal belongs to a running cs-test (PID {data.get('pid')}); leaving it")
            return False
        log(f"Undoing what run {data.get('run_id')} (PID {data.get('pid')}, {data.get('started')}) left behind")
        ok = undo(paths, data, log=log, **kwargs)
    elif journal.exists():
        log(f"{paths.journal} is unreadable; look at it, then delete it")
        ok = False
    if ok and paths.mods_off_legacy.exists():
        left = list(dict.fromkeys(l.strip() for l in paths.mods_off_legacy.read_text(encoding="utf-8-sig").splitlines() if l.strip()))
        log(f"Turning back on mods a cs-ab run left off: {', '.join(left)}")
        mo2.enable_mods(paths.modlist, left)
        paths.mods_off_legacy.unlink()
    return ok


def legacy_env_left(paths):
    """cs-ab.ps1's CommunityShaders.env, if a killed cs-ab run left it (it starts with its CS_AB_PROBE line)."""
    env = paths.skse / LEGACY_ENV_FILE
    if env.exists():
        try:
            return env if env.read_text(encoding="ascii", errors="replace").startswith("CS_AB_PROBE=1") else None
        except OSError:
            return None
    return None
