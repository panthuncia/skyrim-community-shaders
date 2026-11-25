"""Lock files: each holds its owner's PID, and a lock whose PID is gone is taken over.

Lab runs take one slot lock each (lab.take_slots). The launch lock lets one game start at a time, the base lock guards
the lab's mod mirror, the cache lock the shared shader cache, and a stage lock a staged build while it is copied.
"""

import datetime
import json
import os
import time
from pathlib import Path

from . import procs


class LockError(Exception):
    pass


def write_json(path, data):
    """A temporary file and a rename, so a reader never sees half a file."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    temp.replace(path)


def now():
    return datetime.datetime.now().isoformat(timespec="seconds")


class Lock:
    """A lock file with the owner's PID. A lock whose PID is gone is stale and taken over."""

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

    def try_acquire(self, alive=procs.alive, log=print):
        """Takes the lock if it is free or stale. Returns whether it holds it."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        while True:
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    json.dump({"pid": os.getpid(), "started": now(), "command": self.command}, f)
                self.held = True
                return True
            except FileExistsError:
                owner = self.owner()
                if owner is None:
                    # Being written right now, or unreadable: wait a moment and look again.
                    time.sleep(0.2)
                    owner = self.owner()
                    if owner is None:
                        self.path.unlink(missing_ok=True)
                        continue
                if owner.get("pid") == os.getpid():
                    raise LockError(f"{self.path} is already held by this process ({owner.get('command', '')})")
                if not self.is_live(owner, alive):
                    log(f"Taking over a stale lock {self.path.name} from PID {owner.get('pid')} ({owner.get('command', '')})")
                    self.path.unlink(missing_ok=True)
                    continue
                return False

    def acquire(self, wait_s=0, alive=procs.alive, log=print, poll_s=2):
        deadline = time.monotonic() + wait_s
        announced = False
        while not self.try_acquire(alive, log):
            owner = self.owner() or {}
            if time.monotonic() >= deadline:
                raise LockError(f"another cs-test run holds {self.path}: PID {owner.get('pid')}, {owner.get('command', '')}, since {owner.get('started')}")
            if not announced:
                log(f"Waiting for PID {owner.get('pid')} ({owner.get('command', '')}) to release {self.path.name}")
                announced = True
            time.sleep(poll_s)

    def release(self):
        if self.held:
            owner = self.owner()
            if owner and owner.get("pid") == os.getpid():
                self.path.unlink(missing_ok=True)
            self.held = False
