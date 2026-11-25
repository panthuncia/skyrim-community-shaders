"""sweep, compare, status, restore, schema and check-schema."""

import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

from . import collect, lab as lab_mod, locks, mo2, procs
from .config import diff
from . import schema as schema_mod
from .paths import LOG_FILE, REPO_DIR, SCHEMA_PATH, SWITCHES_FILE
from .runner import run
from .schema import ConfigError, load_schema

TOOLS = REPO_DIR / "tools" / "cs-test"


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


def sweep(paths, spec, base, vary, mode="on-one", sets=(), label=None, stage=None, dry_run=False, wait_lock=0, slots=None, log=print):
    schema = load_schema()
    plan = sweep_plan(schema, base, vary, mode)
    log(f"Sweep of {spec}: {len(plan)} runs ({mode})")
    rows = []
    for suffix, sweep_sets in plan:
        run_label = f"{label}-{suffix}" if label else suffix
        # Each run takes a lab slot of its own, and waits for one.
        result = run(paths, spec, list(sets) + sweep_sets, label=run_label, stage=stage, dry_run=dry_run, wait_lock=wait_lock, slots=slots, log=log)
        rows.append((suffix, result))
        if result.get("outcome") == "not-run":
            log("Stopping the sweep: a run could not start")
            break
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
                result = subprocess.run([sys.executable, str(TOOLS / "shotdiff.py"), str(sa[name]), str(sb[name]), str(out)], capture_output=True, text=True)
                log((result.stdout or result.stderr).rstrip())
        for name in sorted(set(sa) ^ set(sb)):
            log(f"  shot {name} only in {'A' if name in sa else 'B'}")
    if memory and (run_a / "CS-AB-memory.csv").exists() and (run_b / "CS-AB-memory.csv").exists():
        log("Memory (compare.py):")
        result = subprocess.run([sys.executable, str(TOOLS / "compare.py"), str(run_a), str(run_b)], capture_output=True, text=True)
        log((result.stdout or result.stderr).rstrip())


# status and restore

def status(paths, log=print):
    lab = lab_mod.Lab(paths)
    count = lab_mod.slot_count(paths)
    log(f"Lab: {lab.root}, {count} slots (game {paths.game})")
    for name in ["exclusive", "launch", "base", "cache"] + [f"slot-{n}" for n in range(max(count, lab_mod.existing_slots(lab)))]:
        lock = lab.lock(name)
        owner = lock.owner()
        if owner:
            live = locks.Lock.is_live(owner)
            log(f"  {name}: PID {owner.get('pid')} {owner.get('command', '')} since {owner.get('started')} ({'running' if live else 'stale'})")
        else:
            log(f"  {name}: free")
    for active in lab_mod.active_runs(paths):
        games = procs.pids_of(lab.slot(active["slot"]).exe)
        log(f"Run {active['run_id']} in slot {active['slot']} (PID {active['pid']}); game {', '.join(map(str, games)) or 'not running'}; log {active['log']}")
    log(f"Shader cache: generation {lab_mod.cache_generation(lab)} at {lab.cache}")
    if paths.modlist.exists():
        state = mo2.get_state(paths)
        if state["originalMod"]:
            log(f"The old MO2 harness left {mo2.enabled_cs_mod(paths.modlist)} enabled in the mod list; restore puts {state['originalMod']} back")


def restore(paths, log=print):
    """Lab runs change nothing outside the lab. This undoes what the old MO2 harness left: its variant swap in the mod list."""
    if (paths.root / "journal.json").exists():
        log(f"{paths.root / 'journal.json'} is the old MO2 harness's record of a killed run: undo it with that harness, or by hand, then delete it")
        return False
    if procs.find("ModOrganizer.exe"):
        log("MO2 is running: close it first (it rewrites the mod list as it exits)")
        return False
    restored = mo2.restore_original(paths)
    log(f"Restored {restored} in the mod list" if restored else "Nothing to restore")
    return True


# schema and check-schema

def newest_dump(paths):
    """The newest run folder's switch dump (a run collects it from its slot)."""
    dumps = sorted(paths.runs.glob(f"*/{SWITCHES_FILE}"), key=lambda p: p.stat().st_mtime) if paths.runs.exists() else []
    return dumps[-1] if dumps else None


def _read_dump(paths, dump, log):
    path = Path(dump) if dump else newest_dump(paths)
    if path is None or not path.exists():
        log(f"No {path or SWITCHES_FILE + ' in any run folder'}: make one with 'cs_test.py run list-switches --stage <name>' (DCLF on, for its features' switches)")
        return None
    log(f"Switch dump: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def schema_differences(old, new):
    """Lines naming what differs between two dumps: switches added or removed, fields changed, features."""
    old, new = schema_mod.normalize(old), schema_mod.normalize(new)
    lines = []
    mine = {row["name"]: row for row in old["switches"]}
    theirs = {row["name"]: row for row in new["switches"]}
    for name in sorted(set(theirs) - set(mine)):
        lines.append(f"added: {name} ({theirs[name]['family']}, {theirs[name]['kind']}){' key ' + theirs[name]['key'] if theirs[name]['key'] else ''}")
    for name in sorted(set(mine) - set(theirs)):
        lines.append(f"removed: {name}{' (key ' + mine[name]['key'] + ')' if mine[name]['key'] else ''}")
    for name in sorted(set(mine) & set(theirs)):
        for field in schema_mod.ROW_FIELDS[1:]:
            if mine[name][field] != theirs[name][field]:
                lines.append(f"{field} differs for {name}: {mine[name][field]!r} -> {theirs[name][field]!r}")
    for feature in sorted(set(new["features"]) - set(old["features"])):
        lines.append(f"feature added: {feature}")
    for feature in sorted(set(old["features"]) - set(new["features"])):
        lines.append(f"feature removed: {feature}")
    if old["prefixes"] != new["prefixes"]:
        lines.append(f"prefixes differ: {old['prefixes']} -> {new['prefixes']}")
    return lines


def write_schema(paths, dump=None, out=None, log=print):
    """Copies the game's switch dump to switches.json. Returns False when the dump cannot be the schema."""
    data = _read_dump(paths, dump, log)
    if data is None:
        return False
    problems = schema_mod.check_dump(data)
    for line in problems:
        log(f"dump: {line}")
    if problems:
        return False
    target = Path(out) if out else SCHEMA_PATH
    if target.exists():
        for line in schema_differences(json.loads(target.read_text(encoding="utf-8")), data):
            log(line)
    target.write_text(schema_mod.render(data), encoding="utf-8", newline="\n")
    typed = sum(1 for row in data["switches"] if row.get("key"))
    log(f"{len(data['switches'])} switches ({typed} typed keys) and {len(data['features'])} features -> {target}")
    return True


def check_schema(paths, dump=None, log=print):
    """Compares switches.json with the game's switch dump. Returns the number of differences, or -1 without a dump."""
    data = _read_dump(paths, dump, log)
    if data is None:
        return -1
    lines = [f"dump: {line}" for line in schema_mod.check_dump(data)]
    lines += schema_differences(json.loads(SCHEMA_PATH.read_text(encoding="utf-8")), data)
    for line in lines:
        log(line)
    log(f"{len(lines)} differences ({len(data.get('switches', []))} switches in the dump); 'cs_test.py schema' copies the dump")
    return len(lines)
