"""From what the harness sees (the status file, the game process, the clock) to a run's outcome.

The game sets its own outcomes. The harness adds the ones the game cannot report (never-launched, a
process that died with no outcome, the run's own timeout) and backstops the game's phase timeouts.
"""

import json
from dataclasses import dataclass, field
from pathlib import Path

from .config import MILESTONES

GAME_OUTCOMES = ("passed", "failed", "config-error", "never-loaded", "device-lost", "crashed", "hang")
HARNESS_OUTCOMES = ("never-launched", "timeout", "crashed", "never-loaded", "hang", "config-error", "failed")
# The harness's phase checks wait this much longer than the game's own, so the game's outcome wins when it can report.
GRACE_S = 20


def read_status(path, run_id):
    """The status file if it is this run's and whole, else None."""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or (run_id and data.get("run_id") != run_id):
        return None
    return data


def last_milestone(status):
    reached = (status or {}).get("milestones", {}) or {}
    best = None
    for name in MILESTONES:
        if name in reached:
            best = name
    return best


def needed_milestone(contract):
    """The latest milestone the config needs: expect.reach or a step's anchor."""
    names = [contract.get("expect", {}).get("reach", "")] + [s.get("at", "") for s in contract.get("steps", [])]
    best = -1
    for name in names:
        if name in MILESTONES:
            best = max(best, MILESTONES.index(name))
    return MILESTONES[best] if best >= 0 else None


@dataclass
class Final:
    outcome: str
    reason: str
    by: str  # "game" or "harness"


@dataclass
class Watcher:
    """Fed one observation at a time; returns a Final once the run is over."""

    contract: dict
    harness: dict
    grace_s: float = GRACE_S
    game_seen: bool = False
    status_seen_at: float | None = None
    seen: dict = field(default_factory=dict)  # milestone -> harness time first seen
    last_updated: object = None
    last_update_at: float = 0.0

    def timeouts(self):
        return self.contract.get("timeouts", {})

    def observe(self, t, status, game_running):
        """t: seconds since launch. status: this run's status dict or None. game_running: the game process exists."""
        timeouts = self.timeouts()
        if game_running:
            self.game_seen = True
        if status is not None:
            if self.status_seen_at is None:
                self.status_seen_at = t
            for name in (status.get("milestones") or {}):
                self.seen.setdefault(name, t)
            if status.get("updated") != self.last_updated:
                self.last_updated = status.get("updated")
                self.last_update_at = t
            if status.get("outcome"):
                return Final(status["outcome"], status.get("reason", ""), "game")

        if t > self.harness.get("timeout_s", 600):
            return Final("timeout", f"run.timeout_s {self.harness.get('timeout_s')} s ran out at {self.where(status)}", "harness")
        launch = timeouts.get("launch", 60)
        if not self.game_seen:
            if t > launch:
                return Final("never-launched", f"no {self.game_name()} process within {launch} s", "harness")
            return None
        if not game_running:
            if status is None:
                return Final("crashed", "the game ended before it wrote a status file", "harness")
            return Final("crashed", f"the game ended without an outcome at {self.where(status)}", "harness")
        if status is None:
            if self.status_seen_at is None and t > launch:
                return Final("never-launched", f"the game runs but wrote no status file within {launch} s (an old build, or the test config was not read)", "harness")
            return None

        stall = self.harness.get("stall_s", 90)
        if stall and t - self.last_update_at > stall:
            return Final("hang", f"the status file was not updated for {stall} s at {self.where(status)} (harness)", "harness")

        reached = status.get("milestones") or {}
        needed = needed_milestone(self.contract)
        needed_index = MILESTONES.index(needed) if needed else -1
        if "data-loaded" in reached and "shaders-ready" not in reached and needed_index >= MILESTONES.index("shaders-ready"):
            limit = timeouts.get("compile", 600) + self.grace_s
            if t - self.seen["data-loaded"] > limit:
                compile_ = status.get("compile") or {}
                return Final("never-loaded", f"still compiling after {timeouts.get('compile', 600)} s ({compile_.get('done', '?')}/{compile_.get('total', '?')}); last milestone data-loaded (harness)", "harness")
        if needed_index >= MILESTONES.index("data-loaded") and "data-loaded" not in reached:
            limit = timeouts.get("load", 120) + self.grace_s
            if t - (self.status_seen_at or 0) > limit:
                return Final("never-loaded", f"no data-loaded within {timeouts.get('load', 120)} s; last milestone {last_milestone(status) or 'none'} (harness)", "harness")
        if needed_index >= MILESTONES.index("load-start") and "world-ready" not in reached:
            anchors = [self.seen[n] for n in ("main-menu", "load-start", "loading-screen") if n in self.seen and n in reached]
            if anchors:
                limit = timeouts.get("load", 120) + self.grace_s
                if t - max(anchors) > limit:
                    return Final("never-loaded", f"no world-ready within {timeouts.get('load', 120)} s; last milestone {last_milestone(status)} (harness)", "harness")
        return None

    def game_name(self):
        from .paths import GAME_EXE

        return GAME_EXE

    def where(self, status):
        if status is None:
            return "no status"
        last = last_milestone(status)
        phase = status.get("phase", "?")
        return f"phase {phase}, last milestone {last or 'none'}"


def frames_after(status, milestone="world-ready"):
    reached = (status or {}).get("milestones") or {}
    if milestone in reached and isinstance(status.get("frame"), int):
        return status["frame"] - reached[milestone].get("frame", 0)
    return None


def summary_line(name, label, final, status, shots, fault_address=""):
    where = ""
    after = frames_after(status)
    if after is not None and final.outcome != "passed":
        where = f" {after} frames after world-ready"
    elif status is not None and final.outcome not in ("passed",):
        last = last_milestone(status)
        where = f" at {last}" if last else " before the first milestone"
    text = f"{name}{f' [{label}]' if label else ''}: {final.outcome}{where}"
    if final.reason and final.outcome != "passed":
        text += f" ({final.reason})"
    if fault_address:
        text += f", fault {fault_address}"
    text += f"; shots: {', '.join(shots) if shots else 'none'}"
    return text
