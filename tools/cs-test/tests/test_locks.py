import json
import os

import pytest

from cstest import commands, locks, mo2


def lines(paths):
    return paths.modlist.read_text(encoding="utf-8").splitlines()


def test_lock(fake):
    path = fake.root / "x.lock"
    lock = locks.Lock(path, "run a")
    lock.acquire(0)
    assert json.loads(path.read_text())["pid"] == os.getpid()
    # A live owner: refused.
    path.write_text(json.dumps({"pid": 4242, "command": "run other", "started": "x"}))
    other = locks.Lock(path, "run b")
    with pytest.raises(locks.LockError, match="4242"):
        other.acquire(0, alive=lambda pid: True, log=lambda *_: None)
    assert not other.try_acquire(alive=lambda pid: True, log=lambda *_: None)
    # A dead owner: taken over.
    other.acquire(0, alive=lambda pid: False, log=lambda *_: None)
    assert json.loads(path.read_text())["command"] == "run b"
    other.release()
    assert not path.exists()


def test_a_lock_this_process_holds_is_not_taken_twice(fake):
    lock = locks.Lock(fake.root / "y.lock", "a")
    assert lock.try_acquire()
    with pytest.raises(locks.LockError, match="already held by this process"):
        locks.Lock(fake.root / "y.lock", "b").try_acquire()
    lock.release()


def test_restore_puts_back_the_old_harness_swap(fake, monkeypatch):
    monkeypatch.setattr(commands.procs, "find", lambda image: [])
    mo2.set_enabled_mod(fake.modlist, "CS AB p35")
    fake.state.write_text(json.dumps({"live": "p35", "originalMod": "CommunityShaders_ORG_Staging"}))
    assert commands.restore(fake, log=lambda *_: None)
    assert mo2.enabled_cs_mod(fake.modlist) == "CommunityShaders_ORG_Staging" and "-CS AB p35" in lines(fake)
    assert not fake.state.exists()
    raw = fake.modlist.read_bytes()
    assert b"\r\n" in raw and not raw.startswith(b"\xef\xbb\xbf")
    # Nothing left: nothing to do.
    messages = []
    assert commands.restore(fake, log=messages.append) and messages == ["Nothing to restore"]


def test_restore_refuses_an_old_journal(fake, monkeypatch):
    monkeypatch.setattr(commands.procs, "find", lambda image: [])
    (fake.root / "journal.json").write_text("{}")
    messages = []
    assert not commands.restore(fake, log=messages.append)
    assert "old MO2 harness" in messages[0]
