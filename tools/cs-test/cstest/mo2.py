"""MO2's mod list, read for the lab's layers, and the old harness's variant swap, which restore undoes once.

Lab runs never change MO2. Only restore writes modlist.txt, and only single lines of it.
"""

import json
import re
from pathlib import Path

# The CS mods. The lab leaves every one of them out, and puts the staged build in their place.
CS_MOD = re.compile(r"^[+-](CS AB .+|CommunityShaders_AIO|CommunityShaders_AIO_native|CommunityShaders_ORG_Staging|Community Shaders DXVK ORG|CS ORG DXVK Optimized)$")


class Mo2Error(Exception):
    pass


def read_lines(modlist):
    text = Path(modlist).read_text(encoding="utf-8-sig")
    return text.splitlines()


def write_lines(modlist, lines):
    # MO2 reads UTF-8 with CRLF line ends. A temporary file and a rename, so a kill never leaves half a list.
    path = Path(modlist)
    temp = path.with_suffix(".txt.cs-test")
    temp.write_bytes(("\r\n".join(lines) + "\r\n").encode("utf-8"))
    temp.replace(path)


def enabled_cs_mod(modlist):
    for line in read_lines(modlist):
        if line.startswith("+") and CS_MOD.match(line):
            return line[1:]
    return None


def set_enabled_mod(modlist, mod):
    """Turns every CS mod off and this one on, in the enabled one's place if it is not listed yet."""
    lines = read_lines(modlist)
    slot = -1
    for i, line in enumerate(lines):
        if CS_MOD.match(line):
            if line.startswith("+") and slot < 0:
                slot = i
            lines[i] = "-" + line[1:]
    if f"-{mod}" in lines:
        lines[lines.index(f"-{mod}")] = f"+{mod}"
    elif slot >= 0:
        lines.insert(slot + 1, f"+{mod}")
    else:
        raise Mo2Error(f"no enabled CS mod in {modlist} to take the place of")
    write_lines(modlist, lines)


def mod_for(paths, variant):
    """A staged variant ("CS AB <name>"), or an installed CS mod named in full."""
    mod = f"CS AB {variant}"
    if not (paths.mods / mod).exists() and (paths.mods / variant).exists():
        mod = variant
    if not (paths.mods / mod).exists():
        raise Mo2Error(f"no variant '{variant}': stage it first (cs-test stage)")
    return mod


def get_state(paths):
    if paths.state.exists():
        data = json.loads(paths.state.read_text(encoding="utf-8-sig"))
        return {"live": data.get("live"), "originalMod": data.get("originalMod")}
    return {"live": None, "originalMod": None}


def restore_original(paths):
    """The old MO2 harness left a staged build enabled in the mod list (state.json): the CS mod from before its first swap goes
    back. Returns what it restored, or None."""
    state = get_state(paths)
    if not state["originalMod"]:
        return None
    set_enabled_mod(paths.modlist, state["originalMod"])
    paths.state.unlink()
    return state["originalMod"]
