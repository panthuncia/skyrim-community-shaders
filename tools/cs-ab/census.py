"""The Phase 5.5 gate on one cs-ab run: does any engine draw still reach D3D11?

    python census.py <run folder or CommunityShaders.log> [--top N]

Reads the residue census windows (CS_ORG_RESIDUE_CENSUS) and the flush latch's windows (CS_ORG_ENGINE_FLUSH_LATCH) from the
log. Prints each census window with the tour command before it, its engine draws a frame, and the latch's allowed draws by
reason. Exits 0 only when every window reads 0 engine draws reaching D3D11 and nothing was skipped
(docs/development/org-full-port.md, "Phase 5.5").
"""

import re
import sys
from pathlib import Path

TIME = r"^\[(?P<time>[0-9:.]+)\]"
CENSUS = re.compile(TIME + r".*\[ORG\] residue census: (?P<frames>\d+) frames, .*?: (?P<rate>[0-9.]+) engine draws and dispatches a frame reach D3D11")
LATCH = re.compile(TIME + r".*\[ORG\] flush latch: (?P<frames>\d+) frames: .*?allowed engine draws a frame: (?P<allowed>.*); (?P<skipped>\d+) engine draws and dispatches skipped")
BY_FUNCTION = re.compile(TIME + r".*\[ORG\] residue census: by draw function: (?P<list>.*)")
COMMAND = re.compile(TIME + r".*\[DCLF\] test command at frame (?P<frame>\d+): (?P<command>.*)")
UNVOUCHED = re.compile(TIME + r".*\[E\] \[ORG\] flush latch: engine ")
REASON = re.compile(r"(?P<reason>[^,]+?) (?P<rate>[0-9.]+) \([0-9.]+ passes vouched\)")


def main():
    args = sys.argv[1:]
    top = 3
    if "--top" in args:
        i = args.index("--top")
        top = int(args[i + 1])
        del args[i : i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    path = Path(args[0])
    log = path / "CommunityShaders.log" if path.is_dir() else path
    command = "(start)"
    windows = []
    unvouched = 0
    pending_latch = None
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if m := COMMAND.match(line):
            command = f"{m['frame']}: {m['command']}"
        elif m := LATCH.match(line):
            pending_latch = m
        elif m := CENSUS.match(line):
            windows.append({"time": m["time"], "after": command, "rate": float(m["rate"]), "latch": None, "functions": ""})
        elif m := BY_FUNCTION.match(line):
            if windows:
                windows[-1]["functions"] = m["list"]
        elif UNVOUCHED.match(line):
            unvouched += 1
        # The latch's window ends beside the census's (same frame count): pair it with the census window just read.
        if pending_latch and windows and windows[-1]["latch"] is None:
            windows[-1]["latch"] = pending_latch
            pending_latch = None
    if not windows:
        sys.exit(f"{log}: no residue census windows (is CS_ORG_RESIDUE_CENSUS=1 set?)")

    failing = 0
    for w in windows:
        latch = w["latch"]
        skipped = int(latch["skipped"]) if latch else 0
        ok = w["rate"] == 0.0 and skipped == 0
        failing += not ok
        reasons = []
        if latch:
            reasons = sorted(((float(r["rate"]), r["reason"].strip()) for r in REASON.finditer(latch["allowed"])), reverse=True)
            reasons = [f"{name} {rate:g}" for rate, name in reasons if rate > 0][:top]
        detail = "; ".join(reasons) if reasons else w["functions"][:160]
        print(f"{'ok  ' if ok else 'FAIL'} {w['time']}  {w['rate']:7.1f}/frame  skipped {skipped:5d}  after [{w['after'][:40]}]  {detail}")
    total = len(windows)
    print(f"\n{total - failing} of {total} windows clean; {unvouched} unvouched first-seen lines")
    sys.exit(0 if failing == 0 and unvouched == 0 else 1)


if __name__ == "__main__":
    main()
