"""sweep, compare, status, restore and check-schema."""

import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

from . import collect, journal as journal_mod, mo2, procs
from .config import diff
from .paths import GAME_EXE, LOG_FILE, MO2_EXE, REPO_DIR, SWITCHES_FILE, TEST_FILE
from .runner import run
from .schema import ConfigError, load_schema

CS_AB = REPO_DIR / "tools" / "cs-ab"


# sweep

def sweep_plan(schema, base, vary, mode):
    """The runs a sweep makes: [(label suffix, the --set list)]. The first is the base run."""
    base_sets = [b.strip() for item in base for b in item.split(",") if b.strip()]
    keys = []
    for item in vary:
        for pattern in (p.strip() for p in item.split(",") if p.strip()):
            for key in schema.expand(pattern):
                if key not in keys:
                    keys.append(key)
    if not keys:
        raise ConfigError("--vary names no keys")
    for key in keys:
        if schema.get(key).get("type") != "bool":
            raise ConfigError(f"--vary {key}: only true/false keys can be swept")
    value = "true" if mode == "on-one" else "false"
    plan = [("base", base_sets)]
    for key in keys:
        plan.append((key.split(".", 1)[1].replace("_", "-"), base_sets + [f"{key}={value}"]))
    return plan


def sweep(paths, spec, base, vary, mode="on-one", sets=(), label=None, stage=None, dry_run=False, wait_lock=0, log=print):
    schema = load_schema()
    plan = sweep_plan(schema, base, vary, mode)
    log(f"Sweep of {spec}: {len(plan)} runs ({mode})")
    lock = None
    if not dry_run:
        lock = journal_mod.Lock(paths.lock, f"sweep {spec}")
        lock.acquire(wait_lock, log=log)
    rows = []
    try:
        for suffix, sweep_sets in plan:
            run_label = f"{label}-{suffix}" if label else suffix
            result = run(paths, spec, list(sets) + sweep_sets, label=run_label, stage=stage, dry_run=dry_run, lock=lock, log=log)
            rows.append((suffix, result))
            if result.get("outcome") == "not-run":
                log("Stopping the sweep: a run could not start")
                break
    finally:
        if lock:
            lock.release()
    log("")
    width = max(len(s) for s, _ in rows)
    log(f"{'run'.ljust(width)}  outcome          detail")
    for suffix, result in rows:
        detail = result.get("reason", "")
        if result.get("frames_after_world_ready") is not None:
            detail = f"{result['frames_after_world_ready']} frames after world-ready; {detail}"
        log(f"{suffix.ljust(width)}  {result.get('outcome', '?'):<15}  {detail}  {result.get('run_id', '')}")
    return rows


# compare

def find_run(paths, name):
    path = Path(name)
    if path.is_dir():
        return path
    candidate = paths.runs / name
    if candidate.is_dir():
        return candidate
    matches = sorted(p for p in paths.runs.glob(f"*{name}*") if p.is_dir())
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise SystemExit(f"no run '{name}' in {paths.runs}")
    raise SystemExit(f"'{name}' matches {len(matches)} runs: {', '.join(m.name for m in matches[:6])}")


def run_facts(folder):
    result_path = folder / "result.json"
    if result_path.exists():
        return json.loads(result_path.read_text(encoding="utf-8"))
    # An old cs-ab run: read its log.
    scan = collect.scan_log(folder / LOG_FILE)
    old = {}
    if (folder / "run.json").exists():
        old = json.loads((folder / "run.json").read_text(encoding="utf-8-sig"))
    return {"outcome": old.get("stop", "?"), "milestones": {}, "error_kinds": scan["error_kinds"], "warning_kinds": scan["warning_kinds"],
            "errors": scan["errors"], "warnings": scan["warnings"], "resolved": {}, "first_error": scan["first_error"]}


def compare(paths, a, b, log=print, shots=True, memory=True):
    run_a, run_b = find_run(paths, a), find_run(paths, b)
    fa, fb = run_facts(run_a), run_facts(run_b)
    log(f"A {run_a.name}: {fa.get('outcome')} {fa.get('reason', '')}")
    log(f"B {run_b.name}: {fb.get('outcome')} {fb.get('reason', '')}")
    if fa.get("stage") or fb.get("stage"):
        sa, sb = fa.get("stage") or {}, fb.get("stage") or {}
        log(f"  build: A {((sa.get('git') or {}).get('cs') or {}).get('sha', '?')[:10]} dll {(sa.get('dll') or {}).get('sha256', '?')[:12]}"
            f"  B {((sb.get('git') or {}).get('cs') or {}).get('sha', '?')[:10]} dll {(sb.get('dll') or {}).get('sha256', '?')[:12]}")

    differences = diff(fa.get("resolved") or {}, fb.get("resolved") or {})
    if differences:
        log("Config differences:")
        for path, x, y in differences:
            if path != "steps":
                log(f"  {path}: {json.dumps(x)} -> {json.dumps(y)}")

    ma, mb = fa.get("milestones") or {}, fb.get("milestones") or {}
    if ma or mb:
        log("Milestones (seconds, frame):")
        for name in dict.fromkeys(list(ma) + list(mb)):
            x, y = ma.get(name), mb.get(name)
            fmt = lambda m: f"{m.get('t', 0):7.1f} s {m.get('frame', 0):6}" if m else "      -        "
            delta = f"  {y['t'] - x['t']:+.1f} s" if x and y else ""
            log(f"  {name:<15} A {fmt(x)}   B {fmt(y)}{delta}")

    log(f"Errors: A {fa.get('errors', 0)}, B {fb.get('errors', 0)}; warnings: A {fa.get('warnings', 0)}, B {fb.get('warnings', 0)}")
    for title, key in (("Error kinds", "error_kinds"), ("Warning kinds", "warning_kinds")):
        ka, kb = Counter(fa.get(key) or {}), Counter(fb.get(key) or {})
        only_a = [k for k in ka if k not in kb]
        only_b = [k for k in kb if k not in ka]
        if only_a or only_b:
            log(f"{title} only in one run:")
            for k in only_a[:20]:
                log(f"  A only ({ka[k]}): {k}")
            for k in only_b[:20]:
                log(f"  B only ({kb[k]}): {k}")

    if shots:
        sa = {p.stem: p for p in (run_a / "shots").glob("*.png")} if (run_a / "shots").exists() else {}
        sb = {p.stem: p for p in (run_b / "shots").glob("*.png")} if (run_b / "shots").exists() else {}
        common = sorted(set(sa) & set(sb))
        if common:
            log("Shots:")
            for name in common:
                out = run_b / "shots" / f"{name}.diff-vs-{run_a.name}.png"
                result = subprocess.run([sys.executable, str(CS_AB / "shotdiff.py"), str(sa[name]), str(sb[name]), str(out)], capture_output=True, text=True)
                log((result.stdout or result.stderr).rstrip())
        for name in sorted(set(sa) ^ set(sb)):
            log(f"  shot {name} only in {'A' if name in sa else 'B'}")
    if memory and (run_a / "CS-AB-memory.csv").exists() and (run_b / "CS-AB-memory.csv").exists():
        log("Memory (compare.py):")
        result = subprocess.run([sys.executable, str(CS_AB / "compare.py"), str(run_a), str(run_b)], capture_output=True, text=True)
        log((result.stdout or result.stderr).rstrip())


# status and restore

def status(paths, log=print):
    lock = journal_mod.Lock(paths.lock)
    owner = lock.owner()
    if owner:
        live = journal_mod.Lock.is_live(owner)
        log(f"Lock: PID {owner.get('pid')} {owner.get('command', '')} since {owner.get('started')} ({'running' if live else 'stale'})")
    else:
        log("Lock: free")
    data = journal_mod.Journal(paths).load()
    if data:
        log(f"Journal: run {data.get('run_id')} (PID {data.get('pid')}, {data.get('started')}) left these changes; the next command undoes them:")
        for entry in data.get("entries", []):
            log(f"  {json.dumps(entry)}")
    else:
        log("Journal: none")
    if paths.modlist.exists():
        state = mo2.get_state(paths)
        log(f"Variant: live {state['live']}; enabled CS mod {mo2.enabled_cs_mod(paths.modlist)}; original {state['originalMod']}")
    for name, stash in mo2.list_stashes(paths).items():
        log(f"  {name}: stash {', '.join(stash) or '(empty)'}")
    if paths.mods_off_legacy.exists():
        log(f"cs-ab left mods off ({paths.mods_off_legacy}): {', '.join(l for l in paths.mods_off_legacy.read_text(encoding='utf-8-sig').splitlines() if l)}")
    legacy = journal_mod.legacy_env_left(paths)
    if legacy:
        log(f"cs-ab left {legacy} (a manual launch would run its test commands)")
    if (paths.skse / TEST_FILE).exists():
        log(f"Leftover {paths.skse / TEST_FILE}")
    running = [f"{image} {pid}" for image in (GAME_EXE, MO2_EXE) for pid in procs.find(image)]
    log(f"Running: {', '.join(running) if running else 'neither the game nor MO2'}")


def restore(paths, original=True, log=print):
    lock = journal_mod.Lock(paths.lock, "restore")
    lock.acquire(0, log=log)
    try:
        if not journal_mod.recover(paths, log=log):
            return False
        if procs.find(GAME_EXE) or procs.find(MO2_EXE):
            log("The game or MO2 is running: close them first")
            return False
        legacy = journal_mod.legacy_env_left(paths)
        if legacy:
            legacy.unlink()
            log(f"Deleted cs-ab's leftover {legacy}")
        leftover = paths.skse / TEST_FILE
        if leftover.exists():
            leftover.unlink()
            log(f"Deleted leftover {leftover}")
        if original:
            restored = mo2.restore_original(paths)
            log(f"Restored {restored}" if restored else "Variant: nothing to restore")
        return True
    finally:
        lock.release()


# check-schema

def check_schema(paths, dump=None, log=print):
    """Compares schema.toml with the game's switch dump. Returns the number of differences."""
    path = Path(dump) if dump else paths.skse / SWITCHES_FILE
    if not path.exists():
        log(f"No {path}: run the game once with CS_TEST_LIST_SWITCHES=1 (and DCLF on, for its features' switches)")
        return -1
    data = json.loads(path.read_text(encoding="utf-8"))
    game = {s["name"]: s for s in data.get("switches", [])}
    schema = load_schema()
    ours = {}
    for key, entry in schema.keys.items():
        if "env" in entry:
            ours[entry["env"]] = (key, entry)
    problems = 0
    for name in sorted(set(game) - set(ours)):
        log(f"missing from the schema: {name} ({game[name].get('family')}, {game[name].get('kind')}): {game[name].get('description', '')[:100]}")
        problems += 1
    for name in sorted(set(ours) - set(game)):
        key, entry = ours[name]
        if entry.get("family") in ("dxvk", "vk"):
            continue
        log(f"not known to the game: {name} ({key})")
        problems += 1
    for name in sorted(set(ours) & set(game)):
        key, entry = ours[name]
        for field in ("kind", "family", "description"):
            theirs = game[name].get(field, "")
            mine = entry.get(field, "")
            if field == "description" and not theirs:
                continue
            if theirs != mine:
                log(f"{field} differs for {name} ({key}): schema {mine!r}, game {theirs!r}")
                problems += 1
    log(f"{problems} differences ({len(game)} switches in the dump, {len(ours)} in the schema)")
    return problems
