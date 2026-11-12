"""MO2's mod list and the variants: ported from cs-ab.ps1's Set-EnabledMod, Disable-Mods, Stash-Live and Use-Variant.

Only single lines of modlist.txt change. A whole-file backup is never written back, because that would
also undo a variant swap made meanwhile.
"""

import json
import re
import shutil
from pathlib import Path

# The CS mods: one of them is enabled at a time (cs-ab.ps1's $isCs).
CS_MOD = re.compile(r"^[+-](CS AB .+|CommunityShaders_AIO|CommunityShaders_AIO_native|CommunityShaders_ORG_Staging|Community Shaders DXVK ORG|CS ORG DXVK Optimized)$")

# What a build writes that another build must not see: moved, not copied, so a swap is instant.
LIVE_STATE = [("ShaderCache", "ShaderCache")]  # (folder under overwrite, folder in the variant's stash)


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


def disable_mods(modlist, mods):
    """Turns each named "+mod" line into "-mod". Returns the names it changed and the names it did not find enabled."""
    if not mods:
        return [], []
    lines = read_lines(modlist)
    changed = []
    for i, line in enumerate(lines):
        if line.startswith("+") and line[1:] in mods and line[1:] not in changed:
            changed.append(line[1:])
            lines[i] = "-" + line[1:]
    if changed:
        write_lines(modlist, lines)
    return changed, [m for m in mods if m not in changed]


def enable_mods(modlist, mods):
    """Turns each named "-mod" line back into "+mod"."""
    if not mods:
        return
    lines = read_lines(modlist)
    touched = False
    for i, line in enumerate(lines):
        if line.startswith("-") and line[1:] in mods:
            lines[i] = "+" + line[1:]
            touched = True
    if touched:
        write_lines(modlist, lines)


def mods_state(modlist, mods):
    """Each named mod's state: '+', '-' or None when it is not listed."""
    state = {m: None for m in mods}
    for line in read_lines(modlist):
        if line[1:] in state and line[:1] in "+-":
            state[line[1:]] = line[0]
    return state


# Variants (cs-ab.ps1's state.json is kept, so cs-ab and cs-test agree on what is live).

def get_state(paths):
    if paths.state.exists():
        data = json.loads(paths.state.read_text(encoding="utf-8-sig"))
        return {"live": data.get("live"), "originalMod": data.get("originalMod")}
    return {"live": None, "originalMod": None}


def set_state(paths, state):
    paths.root.mkdir(parents=True, exist_ok=True)
    paths.state.write_text(json.dumps(state, indent=2), encoding="utf-8")


def stash_live(paths, variant):
    folder = paths.variants / variant
    for live_name, stash_name in LIVE_STATE:
        live = paths.overwrite / live_name
        stash = folder / stash_name
        if stash.exists():
            raise Mo2Error(f"{stash} already exists; refusing to overwrite a stash")
        if live.exists():
            folder.mkdir(parents=True, exist_ok=True)
            shutil.move(str(live), str(stash))


def unstash(paths, variant):
    folder = paths.variants / variant
    for live_name, stash_name in LIVE_STATE:
        live = paths.overwrite / live_name
        stash = folder / stash_name
        if stash.exists():
            live.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(stash), str(live))


def mod_for(paths, variant):
    """A staged variant ("CS AB <name>"), or an installed CS mod named in full."""
    mod = f"CS AB {variant}"
    if not (paths.mods / mod).exists() and (paths.mods / variant).exists():
        mod = variant
    if not (paths.mods / mod).exists():
        raise Mo2Error(f"no variant '{variant}': stage it first (cs-test stage)")
    return mod


def use_variant(paths, variant):
    """Makes the variant the enabled CS mod with its shader cache live. Returns the variant that was live before."""
    mod = mod_for(paths, variant)
    state = get_state(paths)
    if not state["originalMod"]:
        state["originalMod"] = enabled_cs_mod(paths.modlist)
        # The state from before the first swap is a variant of its own, so restore can bring it back.
        state["live"] = "_original"
    previous = state["live"]
    if state["live"] != variant:
        stash_live(paths, state["live"])
        unstash(paths, variant)
        set_enabled_mod(paths.modlist, mod)
        state["live"] = variant
    set_state(paths, state)
    return previous


def restore_original(paths):
    """cs-ab.ps1's restore: the mod and shader cache from before the first swap. Returns what it restored."""
    state = get_state(paths)
    if not state["originalMod"]:
        return None
    if state["live"] != "_original":
        stash_live(paths, state["live"])
        unstash(paths, "_original")
    set_enabled_mod(paths.modlist, state["originalMod"])
    paths.state.unlink()
    return state["originalMod"]


def list_stashes(paths):
    out = {}
    if paths.variants.exists():
        for folder in sorted(p for p in paths.variants.iterdir() if p.is_dir()):
            out[folder.name] = sorted(p.name for p in folder.iterdir() if p.is_dir())
    return out
