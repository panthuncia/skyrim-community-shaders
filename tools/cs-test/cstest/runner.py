"""cs-test run: one game session from a resolved config, with every change journaled and undone."""

import atexit
import datetime
import json
import shutil
import signal
import subprocess
import time
from pathlib import Path

from . import collect, journal as journal_mod, mo2, procs
from .config import diff, resolve
from .outcome import Final, Watcher, frames_after, read_status, summary_line
from .paths import CONFIG_DIR, GAME_EXE, LEGACY_ENV_FILE, LOG_FILE, MO2_EXE, STATUS_FILE, TEST_FILE
from .schema import ConfigError, load_schema
from .stage import load_manifest

POLL_S = 1.0
EXIT_WAIT_S = 15  # how long a game that reported its outcome gets to exit by itself


class RunError(Exception):
    pass


def resolve_with_diff(spec, sets, label=None, stage=None, schema=None, config_dir=CONFIG_DIR):
    """The resolved config and its differences from base."""
    schema = schema or load_schema()
    sets = list(sets)
    if stage:
        sets.insert(0, f"build.stage={stage}")
    contract, harness = resolve(spec, sets, schema=schema, config_dir=config_dir, label=label)
    differences = []
    if (Path(config_dir) / "base.toml").exists():
        base_contract, base_harness = resolve("base", (), schema=schema, config_dir=config_dir)
        differences = diff({**base_contract, "harness": base_harness}, {**contract, "harness": harness})
    return contract, harness, differences


def format_diff(differences):
    if not differences:
        return ["  (same as base)"]
    lines = []
    for path, a, b in differences:
        if path == "harness.autoload_off":
            lines.append(f"  {path}: {json.dumps(a)} -> {json.dumps(b)} (game.save names a save, so the autoload mod is turned off)" if b else f"  {path}: {json.dumps(a)} -> {json.dumps(b)}")
        elif path == "steps":
            lines.append(f"  steps: {len(a or [])} -> {len(b or [])}: " + "; ".join(f"{s['at']}+{s['frames']} {s['do']} {s['arg']}".strip() for s in (b or [])))
        else:
            lines.append(f"  {path}: {json.dumps(a)} -> {json.dumps(b)}")
    return lines


def run_id_for(stage, name, label, when=None):
    when = when or datetime.datetime.now()
    safe = name.replace("+", "-")
    return f"{when.strftime('%m%d-%H%M%S')}-{stage}-{safe}" + (f"-{label}" if label else "")


def _install_signals():
    def handler(signum, frame):
        raise KeyboardInterrupt(f"signal {signum}")

    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, handler)
    signal.signal(signal.SIGINT, handler)


def launch(paths):
    return subprocess.Popen([str(paths.mo2 / MO2_EXE), "-p", paths.profile, "moshortcut://:SKSE"], cwd=str(paths.mo2),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))


def watch(paths, contract, harness, log=print, clock=time.monotonic, sleep=time.sleep):
    """Watches the status file and the game until the run is over. Returns (Final, last status)."""
    watcher = Watcher(contract, harness)
    status_path = paths.skse / STATUS_FILE
    start = clock()
    status = None
    focused = False
    last_phase = None
    while True:
        sleep(POLL_S)
        t = clock() - start
        games = procs.find(GAME_EXE)
        fresh = read_status(status_path, contract["run_id"])
        if fresh is not None:
            status = fresh
        if games and not focused and procs.focus(games[0]):
            focused = True
        if status is not None and status.get("phase") != last_phase:
            last_phase = status.get("phase")
            log(f"  {t:6.1f} s  phase {last_phase}, frame {status.get('frame')}")
        if not games and watcher.game_seen:
            # It may have written its outcome as it ended: read once more.
            fresh = read_status(status_path, contract["run_id"])
            status = fresh or status
        final = watcher.observe(t, status, bool(games))
        if final:
            if final.by == "game" and games:
                procs.wait_gone(games, EXIT_WAIT_S)
                status = read_status(status_path, contract["run_id"]) or status
            return final, status


def check_expectations(contract, run_dir, status, final):
    """The harness's own checks, after a passed run: shots on disk, forbidden log lines, the milestone reached."""
    if final.outcome != "passed":
        return final
    expect = contract["expect"]
    shots = collect.shots_in(run_dir)
    missing = [s for s in expect.get("shots", []) if s not in shots]
    if missing:
        return Final("failed", f"shots missing: {', '.join(missing)} (harness)", "harness")
    found = collect.log_contains(Path(run_dir) / LOG_FILE, expect.get("no_log", []))
    if found:
        return Final("failed", f"logged: {', '.join(sorted(found))} (harness)", "harness")
    reach = expect.get("reach")
    if reach and reach not in ((status or {}).get("milestones") or {}):
        return Final("failed", f"never reached {reach} (harness)", "harness")
    return final


def run(paths, spec, sets=(), label=None, stage=None, dry_run=False, lock=None, wait_lock=0, log=print, config_dir=CONFIG_DIR):
    """One run. Returns its result dict (or the resolved config on a dry run)."""
    schema = load_schema()
    try:
        contract, harness, differences = resolve_with_diff(spec, sets, label, stage, schema, config_dir)
    except ConfigError as error:
        log(f"config-error: {error}")
        return {"outcome": "config-error", "reason": str(error)}
    log(f"{contract['name']}: difference from base")
    for line in format_diff(differences):
        log(line)
    if dry_run:
        log(json.dumps({"resolved": contract, "harness": harness}, indent=2))
        return {"outcome": "dry-run", "resolved": contract, "harness": harness}
    stage = harness["stage"]
    if not stage:
        log("config-error: no build.stage: pass --stage <name> or set it in the config")
        return {"outcome": "config-error", "reason": "no build.stage"}

    own_lock = lock is None
    if own_lock:
        lock = journal_mod.Lock(paths.lock, f"run {spec} {label or ''}".strip())
        lock.acquire(wait_lock, log=log)
    journal = journal_mod.Journal(paths)

    def undo_at_exit():
        if journal.owned():
            journal_mod.undo(paths, journal.load(), log=log)

    atexit.register(undo_at_exit)
    _install_signals()
    final = None
    status = None
    started = None
    run_dir = None
    try:
        if not journal_mod.recover(paths, log=log):
            raise RunError("leftover state could not be undone (cs-test status shows it)")
        if procs.find(GAME_EXE) or procs.find(MO2_EXE):
            raise RunError("the game or MO2 is running: close them first (MO2 rewrites the mod list)")
        mo2.mod_for(paths, stage)
        run_id = run_id_for(stage, contract["name"], harness["label"])
        run_dir = paths.runs / run_id
        (run_dir / "shots").mkdir(parents=True, exist_ok=True)
        contract["run_id"], contract["run_dir"] = run_id, str(run_dir)
        manifest = load_manifest(paths, stage)
        (run_dir / "resolved.json").write_text(json.dumps({**contract, "harness": harness, "stage": manifest}, indent=2), encoding="utf-8")

        journal.begin(run_id)
        state = mo2.get_state(paths)
        previous = state["live"] if state["originalMod"] else "_original"
        journal.add({"do": "variant", "from": previous, "to": stage})
        mo2.use_variant(paths, stage)
        if harness["shader_cache"] == "cold":
            cache = paths.overwrite / "ShaderCache"
            if cache.exists():
                log(f"Clearing {stage}'s shader cache (build.shader_cache = cold)")
                shutil.rmtree(cache)

        wanted = harness["mods_off"]
        current = mo2.mods_state(paths.modlist, wanted)
        for mod in wanted:
            if current[mod] != "+":
                log(f"warning: no enabled mod '{mod}' in the mod list")
        to_change = [m for m in wanted if current[m] == "+"]
        if to_change:
            journal.add({"do": "mods_off", "mods": to_change})
            mo2.disable_mods(paths.modlist, to_change)

        legacy_env = paths.skse / LEGACY_ENV_FILE
        if legacy_env.exists():
            aside = paths.root / "aside" / f"{run_id}-{LEGACY_ENV_FILE}"
            aside.parent.mkdir(parents=True, exist_ok=True)
            journal.add({"do": "move", "from": str(legacy_env), "to": str(aside)})
            shutil.move(str(legacy_env), str(aside))
            log(f"Moved {legacy_env} aside for this run")

        test_file = paths.skse / TEST_FILE
        journal.add({"do": "file", "path": str(test_file)})
        test_file.write_text(json.dumps(contract, indent=2), encoding="utf-8")
        (paths.skse / STATUS_FILE).unlink(missing_ok=True)
        log_path = paths.skse / LOG_FILE
        if log_path.exists():
            log_path.replace(paths.skse / "CommunityShaders.previous.log")

        started = time.time()
        journal.add({"do": "launch", "at": datetime.datetime.now().isoformat(timespec="seconds")})
        log(f"{run_id}: launching through MO2")
        launch(paths)
        procs.keep_display_awake(True)
        procs.nudge_mouse()
        final, status = watch(paths, contract, harness, log=log)
    except KeyboardInterrupt:
        final = Final("interrupted", "cs-test was interrupted", "harness")
        log("Interrupted: stopping the game and undoing the run's changes")
    except (RunError, mo2.Mo2Error) as error:
        final = Final("config-error" if isinstance(error, mo2.Mo2Error) else "not-run", str(error), "harness")
        log(f"{final.outcome}: {error}")
    finally:
        procs.keep_display_awake(False)
        if journal.owned():
            data = journal.load()
            if any(e.get("do") == "launch" for e in data.get("entries", [])):
                procs.stop_game_and_mo2(GAME_EXE, MO2_EXE, log=log)
            journal_mod.undo(paths, data, log=log)
        if own_lock:
            lock.release()

    if run_dir is None or started is None:
        return {"outcome": final.outcome, "reason": final.reason}

    collected = collect.collect(paths.skse, run_dir, started, log=log)
    status_copy = paths.skse / STATUS_FILE
    if status_copy.exists() and collect.new_since(status_copy, started):
        shutil.copy2(status_copy, run_dir / "status.json")
    final = check_expectations(contract, run_dir, status, final)
    scan = collect.scan_log(run_dir / LOG_FILE)
    fault = ((status or {}).get("fault") or {}).get("address") or scan["fault_address"]
    shots = collect.shots_in(run_dir)
    line = summary_line(contract["name"], harness["label"], final, status, shots, fault)
    result = {
        "run_id": contract["run_id"],
        "name": contract["name"],
        "label": harness["label"],
        "outcome": final.outcome,
        "reason": final.reason,
        "outcome_by": final.by,
        "seconds": int(time.time() - started),
        "milestones": (status or {}).get("milestones", {}),
        "frames_after_world_ready": frames_after(status),
        "last_status": status,
        "stage": load_manifest(paths, stage),
        "resolved": contract,
        "harness": harness,
        "shots": shots,
        "first_error": scan["first_error"] or (status or {}).get("last_error", ""),
        "fault": (status or {}).get("fault") or ({"address": fault} if fault else {}),
        "errors": scan["errors"],
        "warnings": scan["warnings"],
        "error_kinds": scan["error_kinds"],
        "warning_kinds": scan["warning_kinds"],
        "collected": collected,
        "summary": line,
    }
    (run_dir / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    log(line)
    return result
