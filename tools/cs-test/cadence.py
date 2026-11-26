"""The cadence shot comparison: the same steps with CS_SNAPSHOT_CADENCE unset and set, shots compared.

    python tools/cs-test/cadence.py --stage s2-cadence [--mode native-vulkan] [--config cadence-still]
                                    [--cadence repeat:3 --cadence drop:3] [--label s2-cadence-cmp] [--wait 3600]
    python tools/cs-test/cadence.py --runs <normal A> <normal B> [<cadence run>...]

The first form runs, one after the other: two normal runs (the noise floor), then one run per cadence (snapshot.cadence set),
each `<mode>+<config>` on the stage. The second form compares runs that already ran (folders or unique name parts): the first
two are normal runs, the rest cadence runs.

Steps count game frames (snapshots), so each shot is taken at the same game time with any cadence. For every shot it prints
the mean channel difference and the share of pixels differing by more than 8 for: B against A (the noise floor) and each
cadence run against A and B. A cadence shot is within the noise when its difference from A is at most the larger of
twice the floor and the floor plus 1.5 (mean channel difference). The exit code is 0 only when every run passed and every
cadence shot is within the noise.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import shotdiff  # noqa: E402
from cstest.commands import find_run, run_facts  # noqa: E402
from cstest.paths import default_paths  # noqa: E402

SHOT_LINE = re.compile(r"\[TEST\] shot (?P<name>\S+) rendered snapshot (?P<snapshot>\d+) \(render frame (?P<frame>\d+)\)")


def difference(path_a, path_b):
    """(mean channel difference, percent of pixels differing by more than 8) of two shots."""
    wa, ha, a = shotdiff.read_rgb(path_a)
    wb, hb, b = shotdiff.read_rgb(path_b)
    if (wa, ha) != (wb, hb):
        return None
    total = 0
    over = 0
    for ra, rb in zip(a, b):
        d = [abs(x - y) for x, y in zip(ra, rb)]
        total += sum(d)
        over += sum(1 for p in map(max, d[0::3], d[1::3], d[2::3]) if p > 8)
    n = wa * ha
    return total / (n * 3), 100.0 * over / n


def shots(folder):
    return {p.stem: p for p in (folder / "shots").glob("*.png") if not p.stem.startswith("_")} if (folder / "shots").exists() else {}


def rendered_snapshots(folder):
    """Shot name -> (snapshot, render frame) from the run's log, when the build logs it."""
    log = folder / "CommunityShaders.log"
    found = {}
    if log.exists():
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            m = SHOT_LINE.search(line)
            if m:
                found[Path(m["name"]).stem] = (int(m["snapshot"]), int(m["frame"]))
    return found


def run_one(cs_test, mode, config, stage, label, cadence, wait):
    command = [sys.executable, str(cs_test), "run", f"{mode}+{config}", "--stage", stage, "--label", label, "--wait", str(wait)]
    if cadence:
        command += ["--set", f"snapshot.cadence={cadence}"]
    print(" ".join(command), flush=True)
    subprocess.run(command)
    return label


def within(diff, floor):
    # Two normal runs of one build differ by 0.1 to 1.6 (TAA and the other temporal filters settle differently).
    return diff <= max(2.0 * floor, floor + 1.5)


def compare(folders):
    names = ["A", "B"] + [f"C{i}" for i in range(1, len(folders) - 1)]
    ok = True
    for name, folder in zip(names, folders):
        facts = run_facts(folder)
        cadence = ((facts.get("resolved") or {}).get("env") or {}).get("CS_SNAPSHOT_CADENCE") or "off"
        print(f"{name}: {folder.name}: {facts.get('outcome')} (cadence {cadence}) {facts.get('reason', '')}")
        ok = ok and facts.get("outcome") == "passed"
    a, b = shots(folders[0]), shots(folders[1])
    logs = [rendered_snapshots(f) for f in folders]
    print()
    print(f"{'shot':<14} {'run':<4} {'vs A: mean':>11} {'>8 %':>7} {'vs B: mean':>11} {'>8 %':>7}  snapshot (render frame)  verdict")
    for shot in sorted(set(a) & set(b)):
        floor = difference(a[shot], b[shot])
        def where(i):
            s = logs[i].get(shot)
            return f"{s[0]} ({s[1]})" if s else "-"
        print(f"{shot:<14} {'B':<4} {floor[0]:>11.3f} {floor[1]:>7.2f} {'':>11} {'':>7}  {where(1):<23}  noise floor (A: {where(0)})")
        for i, folder in enumerate(folders[2:], start=2):
            c = shots(folder).get(shot)
            if not c:
                print(f"{shot:<14} {names[i]:<4} missing")
                ok = False
                continue
            da, db = difference(a[shot], c), difference(b[shot], c)
            if da is None or db is None:
                print(f"{shot:<14} {names[i]:<4} size differs")
                ok = False
                continue
            good = within(min(da[0], db[0]), floor[0])
            ok = ok and good
            print(f"{shot:<14} {names[i]:<4} {da[0]:>11.3f} {da[1]:>7.2f} {db[0]:>11.3f} {db[1]:>7.2f}  {where(i):<23}  {'within noise' if good else 'OUTSIDE noise'}")
    for shot in sorted(set(a) ^ set(b)):
        print(f"shot {shot} is only in {'A' if shot in a else 'B'}")
    return ok


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", nargs="+", help="compare these runs: two normal ones, then cadence ones")
    parser.add_argument("--stage")
    parser.add_argument("--mode", default="native-vulkan")
    parser.add_argument("--config", default="cadence-still")
    parser.add_argument("--cadence", action="append", help="a CS_SNAPSHOT_CADENCE value; repeatable (default repeat:3 and drop:3)")
    parser.add_argument("--label", help="the label prefix (default <stage>-cadence)")
    parser.add_argument("--wait", type=int, default=3600)
    parser.add_argument("--cs-test", default=str(TOOLS / "cs_test.py"), help="the cs_test.py to run (default: the one beside this script)")
    args = parser.parse_args(argv)

    paths = default_paths()
    if args.runs:
        if len(args.runs) < 3:
            parser.error("--runs needs two normal runs and at least one cadence run")
        labels = args.runs
    else:
        if not args.stage:
            parser.error("--stage is needed to run")
        prefix = args.label or f"{args.stage}-cadence"
        cadences = args.cadence or ["repeat:3", "drop:3"]
        labels = [run_one(args.cs_test, args.mode, args.config, args.stage, f"{prefix}-normal-a", None, args.wait),
                  run_one(args.cs_test, args.mode, args.config, args.stage, f"{prefix}-normal-b", None, args.wait)]
        for cadence in cadences:
            labels.append(run_one(args.cs_test, args.mode, args.config, args.stage, f"{prefix}-{cadence.replace(':', '')}", cadence, args.wait))
    folders = [find_run(paths, label) for label in labels]
    return 0 if compare(folders) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
