"""cs-test run: one game session from a resolved config, in a lab slot (lab.py). Nothing outside the slot changes."""

import copy
import datetime
import json
import os
import shutil
import signal
import time
from pathlib import Path

from . import collect, cpuload, lab as lab_mod, locks, mo2, presentmon, procs, shotref
from .config import diff, resolve
from .outcome import Final, Watcher, frames_after, read_status, summary_line
from .paths import CONFIG_DIR, LOG_FILE, STATUS_FILE
from .schema import ConfigError, load_schema
from .stage import load_manifest

# SSE Display Tweaks reads this file after its own ini; game.uncapped writes it into the slot for one run.
DISPLAY_TWEAKS_FILE = "SSEDisplayTweaks_custom.ini"
DISPLAY_TWEAKS_UNCAPPED = "[Render]\nEnableVSync=false\nEnableTearing=true\nFramerateLimit=0\n"
SETTINGS_USER = Path("SKSE/Plugins/CommunityShaders/SettingsUser.json")
STEAM_EXE = "steam.exe"
CRASH_DUMP_EXE = "CommunityShadersCrashDump.exe"

POLL_S = 1.0
EXIT_WAIT_S = 15  # how long a game that reported its outcome gets to exit by itself

# CS_TEST_BACKGROUND=1: an exclusive run, the only kind that may take the focus, never focuses the game or moves the mouse
# either. Other runs never do: several games run at once, each kept running unfocused (bAlwaysActive=1 in the slot's Skyrim.ini).
BACKGROUND = os.environ.get("CS_TEST_BACKGROUND") == "1"


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


def _step_text(step):
    text = f"{step['at']}+{step['frames']} {step['do']} {step['arg']}".strip()
    return text + (f" await {step['await']}" if step.get("await") else "")


def format_diff(differences):
    if not differences:
        return ["  (same as base)"]
    lines = []
    for path, a, b in differences:
        if path == "harness.autoload_off":
            lines.append(f"  {path}: {json.dumps(a)} -> {json.dumps(b)} (game.save names a save, so the autoload mod is turned off)" if b else f"  {path}: {json.dumps(a)} -> {json.dumps(b)}")
        elif path == "steps":
            lines.append(f"  steps: {len(a or [])} -> {len(b or [])}: " + "; ".join(_step_text(s) for s in (b or [])))
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


def watch(slot, contract, harness, gate=None, background=True, log=print, clock=time.monotonic, sleep=time.sleep):
    """Watches the slot's status file and game until the run is over. Returns (Final, last status, the game PIDs seen)."""
    watcher = Watcher(contract, harness)
    watch.cpu = cpuload.CpuLoad()
    status_path = slot.skse / STATUS_FILE
    start = clock()
    seen = set()
    try:
        final, status = _watch_loop(slot, contract, harness, log, clock, sleep, watcher, status_path, start, gate, background, seen)
        return final, status, sorted(seen)
    finally:
        # Stopped before the game is killed. The samples stay for the result.
        if watch.sampler:
            watch.sampler.stop()


# fg.presentmon's sampler for the run being watched.
watch.sampler = None
# The machine's CPU load during the run being watched (cpuload.py).
watch.cpu = None


def _start_presentmon(contract, harness, games, log):
    """fg.presentmon: samples the game's presents per second from its first poll on."""
    if not harness.get("presentmon") or watch.sampler or not games:
        return
    pid = games[0]
    sampler = presentmon.Sampler(pid, Path(contract["run_dir"]) / "presentmon.jsonl")
    try:
        sampler.start()
        watch.sampler = sampler
        log(f"  PresentMon: sampling process {pid}")
    except presentmon.PresentMonError as error:
        log(f"  PresentMon: {error}")
        harness["presentmon"] = False


def _watch_loop(slot, contract, harness, log, clock, sleep, watcher, status_path, start, gate, background, seen):
    status = None
    focused = False
    last_phase = None
    while True:
        sleep(POLL_S)
        t = clock() - start
        games = procs.pids_of(slot.exe)
        seen.update(games)
        if gate:
            gate.check(games, watcher.game_seen)
        _start_presentmon(contract, harness, games, log)
        fresh = read_status(status_path, contract["run_id"])
        if fresh is not None:
            status = fresh
        if games and not focused and not background and procs.focus(games[0]):
            focused = True
        watch.cpu.sample(games[0] if games else None, (status or {}).get("phase") == "world")
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


def check_expectations(contract, run_dir, status, final, refs=None):
    """The harness's own checks, after a passed run: shots on disk, forbidden log lines, the milestone reached, the graph
    frames, and the reference shots (refs, from shotref.check_run)."""
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
    live = collect.live_image(run_dir, expect)
    if live.get("failure"):
        return Final("failed", f"{live['failure']} (harness)", "harness")
    graph = collect.graph_frames(contract, collect.final_status(run_dir, status))
    if graph["failures"]:
        return Final("failed", "; ".join(graph["failures"]) + " (harness)", "harness")
    if refs and refs.get("failures"):
        return Final("failed", "; ".join(refs["failures"]) + " (harness)", "harness")
    return final


def deep_merge(into, layer):
    """Tables merge key by key, recursively; everything else is replaced."""
    for key, value in layer.items():
        if isinstance(value, dict) and isinstance(into.get(key), dict):
            deep_merge(into[key], value)
        else:
            into[key] = value
    return into


def write_saved_settings(slot, saved, log=print):
    """[saved_settings]: merged into the slot's copy of the saved settings for this run. That copy is the one MO2 would serve
    (overwrite's, else the staged build's). The next run's slot build puts it back."""
    target = slot.data / SETTINGS_USER
    settings = {}
    if target.exists():
        try:
            settings = json.loads(target.read_text(encoding="utf-8-sig"))
        except ValueError as error:
            raise RunError(f"{target} does not parse ({error}): fix the file it comes from; saved_settings will not overwrite it") from error
        if not isinstance(settings, dict):
            raise RunError(f"{target} is not a JSON object: fix the file it comes from; saved_settings will not overwrite it")
        target.unlink()
    target.parent.mkdir(parents=True, exist_ok=True)
    deep_merge(settings, copy.deepcopy(saved))
    target.write_text(json.dumps(settings, indent=4), encoding="utf-8")
    log(f"Saved settings: {', '.join(saved)} written into slot {slot.number}'s {SETTINGS_USER.name} for this run")


def ini_text(value):
    """A prefs_ini value as the game's INI writes it: bools as 1 and 0."""
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(value)


def set_ini_values(text, values):
    """Sets key=value lines in an INI's text, by section. Names match without case, as the game reads them. A missing key goes at the
    end of its section, a missing section at the end of the file. Every other line stays as it is."""
    lines = text.splitlines()
    for section, keys in values.items():
        header = f"[{section.lower()}]"
        start = next((i for i, line in enumerate(lines) if line.strip().lower() == header), None)
        if start is None:
            if lines and lines[-1].strip():
                lines.append("")
            lines.append(f"[{section}]")
            start = len(lines) - 1
        for key, value in keys.items():
            end = next((i for i in range(start + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
            line = f"{key}={ini_text(value)}"
            found = next((i for i in range(start + 1, end) if "=" in lines[i] and lines[i].split("=", 1)[0].strip().lower() == key.lower()), None)
            if found is not None:
                lines[found] = line
                continue
            # After the section's last line that is not blank.
            at = end
            while at > start + 1 and not lines[at - 1].strip():
                at -= 1
            lines.insert(at, line)
    return "\n".join(lines) + "\n"


def mods_for_run(lab, harness, log=print):
    """The enabled mods, those without the run's mods_off, and the plugin files of the mods it turns off."""
    mods = lab.enabled_mods()
    off = []
    for mod in harness["mods_off"]:
        if mod in mods:
            off.append(mod)
        else:
            log(f"warning: no enabled mod '{mod}' in the mod list")
    plugins_off = [p for mod in off for p in lab_mod.plugins_of(lab.paths.mods / mod)]
    return mods, [m for m in mods if m not in off], plugins_off


def prepare_slot(paths, lab, slot, stage, stage_mod, contract, harness, command, log=print):
    """The slot's game folder, its run-only files, its shader cache and its home. Returns the save it loads."""
    all_mods, mods, plugins_off = mods_for_run(lab, harness, log)
    base = lab.lock("base", command)
    base.acquire(900, log=log, poll_s=0.5)
    try:
        lab.refresh_mirrors(all_mods, log=log)
        stage_lock = lab.lock(f"stage-{stage}", command)
        stage_lock.acquire(900, log=log, poll_s=0.5)
        try:
            lab_mod.build_slot(lab, slot, mods, stage_mod, log=log)
        finally:
            stage_lock.release()
    finally:
        base.release()
    # game.uncapped: SSE Display Tweaks' override file in the slot, for this run only. It turns off its vsync and its frame-rate
    # limiter, so a timing run is not held at the display's rate or at the limiter's.
    if harness["uncapped"]:
        tweaks = slot.data / "SKSE" / "Plugins" / DISPLAY_TWEAKS_FILE
        tweaks.unlink(missing_ok=True)
        tweaks.write_text(DISPLAY_TWEAKS_UNCAPPED, encoding="utf-8")
        log("Uncapped: SSE Display Tweaks' vsync and limiter off for this run")
    if harness["saved_settings"]:
        write_saved_settings(slot, harness["saved_settings"], log)
    lab_mod.prepare_cache(lab, slot, harness["shader_cache"] == "cold", log=log)
    save = lab_mod.write_home(paths, slot, contract, harness["prefs_ini"], plugins_off, set_ini_values, log=log)
    log(f"Slot {slot.number}: home ready, save {save}" + (f"; plugins off: {', '.join(plugins_off)}" if plugins_off else ""))
    return save


def run(paths, spec, sets=(), label=None, stage=None, dry_run=False, wait_lock=0, slots=None, log=print, config_dir=CONFIG_DIR):
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
    try:
        stage_mod = mo2.mod_for(paths, stage)
    except mo2.Mo2Error as error:
        log(f"config-error: {error}")
        return {"outcome": "config-error", "reason": str(error)}

    lab = lab_mod.Lab(paths)
    exclusive = harness["exclusive"]
    command = f"run {spec} {label or ''}".strip()
    claim = lab_mod.take_slots(lab, lab_mod.slot_count(paths, slots), exclusive, wait_lock, command, log=log)
    slot = claim.slot
    # However cs-test ends, the game it started ends with it.
    procs.kill_children_with_this_process()
    _install_signals()
    background = not exclusive or BACKGROUND
    final = None
    status = None
    started = None
    run_dir = None
    active = None
    gate = None
    launched = False
    killed = False
    pids = []
    lab_info = {"slot": slot.number, "slots": claim.numbers, "exclusive": exclusive, "home": str(slot.home), "game": str(slot.game)}
    try:
        if not procs.find(STEAM_EXE):
            raise RunError("Steam is not running: start it first (steam.exe -silent)")
        lab_mod.kill_orphans(slot, log=log)
        run_id = run_id_for(stage, contract["name"], harness["label"])
        run_dir = paths.runs / run_id
        (run_dir / "shots").mkdir(parents=True, exist_ok=True)
        contract["run_id"], contract["run_dir"] = run_id, str(run_dir)
        # Upscaling stops while another process has the foreground (Upscaling::IsWindowUnusable). A background run never
        # takes the foreground, so without this switch DLSS, FSR and XeSS never run and their Upscale client never registers.
        if background:
            contract["env"].setdefault("CS_UPSCALE_UNFOCUSED", "1")
        manifest = load_manifest(paths, stage)
        (run_dir / "resolved.json").write_text(json.dumps({**contract, "harness": harness, "stage": manifest, "lab": lab_info}, indent=2), encoding="utf-8")
        active = lab_mod.write_active(paths, run_id, harness["label"], slot, claim.numbers)
        log(f"{run_id}: lab slot {slot.number}{' (exclusive: every slot)' if exclusive else ''}")
        prepare_slot(paths, lab, slot, stage, stage_mod, contract, harness, command, log)

        gate = lab_mod.LaunchGate(lab, slot, command, log=log)
        started = time.time()
        launched = True
        log(f"{run_id}: launching slot {slot.number}")
        lab_mod.launch(slot, log=log)
        procs.keep_display_awake(True)
        if not background:
            procs.nudge_mouse()
        final, status, pids = watch(slot, contract, harness, gate=gate, background=background, log=log)
        helper_error = lab_mod.helper_error(slot)
        if final.outcome in ("crashed", "never-launched") and helper_error:
            final = Final(final.outcome, f"the lab helper stopped the game: {helper_error}", "harness")
    except KeyboardInterrupt:
        final = Final("interrupted", "cs-test was interrupted", "harness")
        log("Interrupted: stopping the game")
    except (RunError, lab_mod.LabError, locks.LockError, mo2.Mo2Error, OSError) as error:
        final = Final("not-run", str(error), "harness")
        log(f"not-run: {error}")
    finally:
        procs.keep_display_awake(False)
        if gate:
            gate.release()
        games = procs.pids_of(slot.exe)
        if games:
            # A game still running here is one the harness ends: its stacks first, beside the run's other files.
            killed = True
            if run_dir is not None:
                procs.dump_before_kill(games, slot.data / "SKSE" / "Plugins" / CRASH_DUMP_EXE, run_dir, log=log)
            procs.stop(games, log=log)
        # A game that has exited can hold its files a moment longer while it is torn down. The next run in this slot
        # deletes them, so the slot is free only once every game it saw is gone.
        if pids and not procs.wait_gone(pids, 60):
            log(f"Slot {slot.number}: game {pids} still shutting down after 60 s")
        if launched:
            lab_mod.merge_cache(lab, slot, clean=not killed and final is not None and final.outcome != "interrupted", log=log)
        if active:
            active.unlink(missing_ok=True)
        claim.release()

    if run_dir is None or started is None:
        return {"outcome": final.outcome, "reason": final.reason}

    collected = collect.collect(slot.skse, run_dir, started, game_dir=slot.game, log=log)
    status_copy = slot.skse / STATUS_FILE
    if status_copy.exists() and collect.new_since(status_copy, started):
        shutil.copy2(status_copy, run_dir / "status.json")
    refs = shotref.check_run(paths, harness["shot_refs"], contract["name"], run_dir) if final.outcome == "passed" else {"checked": False}
    for shot in refs.get("unreferenced", []):
        log(f"shot {shot}: no reference for {contract['name']}/{shot} (cs_test.py ref accept {run_dir.name})")
    final = check_expectations(contract, run_dir, status, final, refs)
    scan = collect.scan_log(run_dir / LOG_FILE)
    fault = ((status or {}).get("fault") or {}).get("address") or scan["fault_address"]
    shots = collect.shots_in(run_dir)
    presents = None
    if watch.sampler:
        fgtest = collect.fgtest_seconds(run_dir / LOG_FILE, started)
        presents = {**presentmon.summarize(watch.sampler.samples, fgtest), **collect.fgtest_rates(run_dir / LOG_FILE)}
        log(f"PresentMon: {presents}")
        watch.sampler = None
    line = summary_line(contract["name"], harness["label"], final, status, shots, fault)
    detected = collect.detectors(collect.final_status(run_dir, status))
    if collect.detectors_text(detected):
        line += f" ({collect.detectors_text(detected)})"
    cpu = watch.cpu.result() if watch.cpu else None
    if cpuload.text(cpu):
        line += f"; {cpuload.text(cpu)}"
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
        "lab": {**lab_info, "game_pids": pids, "killed": killed, "helper": lab_mod.helper_marker(slot)},
        "shots": shots,
        "live_image": collect.live_image(run_dir, contract["expect"]),
        "graph_frames": collect.graph_frames(contract, collect.final_status(run_dir, status)),
        "shot_refs": refs,
        "detectors": detected,
        "first_error": scan["first_error"] or (status or {}).get("last_error", ""),
        "fault": (status or {}).get("fault") or ({"address": fault} if fault else {}),
        "errors": scan["errors"],
        "warnings": scan["warnings"],
        "error_kinds": scan["error_kinds"],
        "warning_kinds": scan["warning_kinds"],
        "collected": collected,
        "presents": presents,
        "cpu_load": cpu,
        "summary": line,
    }
    (run_dir / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    log(line)
    return result
