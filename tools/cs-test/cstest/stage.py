"""cs-test stage: a build into MO2 as the mod "CS AB <name>", mirrored (stale files deleted), with stage.json.

From a worktree: build/<preset>/CommunityShaders.dll, .pdb and CommunityShadersCrashDump.exe into
SKSE/Plugins, and package/Shaders plus every features/*/Shaders into Shaders. The rest of the mod (the
AIO's meshes, interface files and so on) comes from a base AIO folder the first time.
From an AIO folder: the whole folder, as cs-ab.ps1 stage did (PDBs left out unless --pdb).
"""

import datetime
import fnmatch
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

PLUGIN_FILES = ["CommunityShaders.dll", "CommunityShaders.pdb", "CommunityShadersCrashDump.exe"]


class StageError(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(4 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def git_info(folder):
    folder = Path(folder)
    if not folder.exists():
        return None

    def git(*args):
        try:
            result = subprocess.run(["git", "-C", str(folder), *args], capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            return None
        return result.stdout.strip() if result.returncode == 0 else None

    sha = git("rev-parse", "HEAD")
    if not sha:
        return None
    return {
        "sha": sha,
        "branch": git("rev-parse", "--abbrev-ref", "HEAD") or "",
        "dirty": bool(git("status", "--porcelain", "--untracked-files=no")),
    }


def same(src, dst):
    try:
        a, b = src.stat(), dst.stat()
    except OSError:
        return False
    return a.st_size == b.st_size and int(a.st_mtime) == int(b.st_mtime)


def mirror_map(files, dest, exclude=()):
    """Makes dest hold exactly files (relative path -> source path). Files matching exclude are neither copied nor deleted.
    Returns (copied, deleted)."""
    dest = Path(dest)
    copied = deleted = 0
    wanted = {Path(rel).as_posix().lower(): (Path(rel), Path(src)) for rel, src in files.items()}
    for key, (rel, src) in wanted.items():
        target = dest / rel
        if not same(src, target):
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, target)
            copied += 1
    if dest.exists():
        for root, dirs, names in os.walk(dest, topdown=False):
            for name in names:
                path = Path(root) / name
                rel = path.relative_to(dest).as_posix()
                if any(fnmatch.fnmatch(name, p) for p in exclude):
                    continue
                if rel.lower() not in wanted:
                    path.unlink()
                    deleted += 1
            for name in dirs:
                folder = Path(root) / name
                try:
                    folder.rmdir()  # only when empty
                except OSError:
                    pass
    return copied, deleted


def tree(folder, exclude=()):
    folder = Path(folder)
    out = {}
    for root, _, names in os.walk(folder):
        for name in names:
            if any(fnmatch.fnmatch(name, p) for p in exclude):
                continue
            path = Path(root) / name
            out[path.relative_to(folder).as_posix()] = path
    return out


def shader_files(worktree, log=print):
    """package/Shaders, then each features/<Name>/Shaders over it, as relative path -> source."""
    worktree = Path(worktree)
    files = tree(worktree / "package" / "Shaders")
    if not (worktree / "package" / "Shaders" / "OpenRenderGraph").exists():
        log("warning: package/Shaders/OpenRenderGraph is missing (CMake copies it at configure time); ORG's built-in shaders will be missing")
    features = worktree / "features"
    if features.exists():
        for feature in sorted(p for p in features.iterdir() if p.is_dir()):
            for rel, src in tree(feature / "Shaders").items():
                if rel in files and not same(files[rel], src) and files[rel].read_bytes() != src.read_bytes():
                    log(f"warning: {rel} differs between {files[rel]} and {src}; the feature's copy wins")
                files[rel] = src
    return files


def pad(mod, count):
    # usvfs crashes the game when its file table grows while the game reads it, which a cold V1 shader cache
    # does. Empty files make MO2 grow the table before the game starts (cs-ab.ps1 -Pad).
    folder = Path(mod) / "_cs-ab-pad"
    folder.mkdir(parents=True, exist_ok=True)
    for i in range(1, count + 1):
        (folder / f"pad{i}").touch()


def stage(paths, source, name, preset="Dev-Fast", base_aio=None, pad_count=0, pdb=False, log=print):
    source = Path(source).resolve()
    mod = paths.mods / f"CS AB {name}"
    manifest = {"name": name, "source": str(source), "staged_at": datetime.datetime.now().isoformat(timespec="seconds"), "mod": str(mod)}
    if (source / "SKSE" / "Plugins" / "CommunityShaders.dll").exists():
        manifest["mode"] = "aio"
        exclude = () if pdb else ("*.pdb",)
        copied, deleted = mirror_map(tree(source, exclude), mod, exclude)
        manifest["files"] = {"copied": copied, "deleted": deleted}
        dll = mod / "SKSE" / "Plugins" / "CommunityShaders.dll"
    elif (source / "src").exists() and (source / "package").exists():
        manifest["mode"] = "worktree"
        build = source / "build" / preset
        if not (build / "CommunityShaders.dll").exists():
            raise StageError(f"no {build / 'CommunityShaders.dll'}: build it first (BuildDevFast.bat), or pass --preset")
        if not mod.exists():
            base = Path(base_aio) if base_aio else paths.root / "aio" / name
            if not (base / "SKSE" / "Plugins").exists():
                raise StageError(f"{mod} does not exist yet: pass --base-aio <an AIO folder> for the files a DLL build lacks")
            log(f"First stage of {name}: mirroring the base AIO {base}")
            mirror_map(tree(base, ("*.pdb",)), mod, ("*.pdb",))
            manifest["base_aio"] = str(base)
        plugins = mod / "SKSE" / "Plugins"
        plugins.mkdir(parents=True, exist_ok=True)
        for file in PLUGIN_FILES:
            if (build / file).exists():
                shutil.copy2(build / file, plugins / file)
            else:
                log(f"warning: no {build / file}")
        copied, deleted = mirror_map(shader_files(source, log), mod / "Shaders")
        manifest["shaders"] = {"copied": copied, "deleted": deleted}
        manifest["preset"] = preset
        manifest["git"] = {
            "cs": git_info(source),
            "OpenRenderGraph": git_info(source / "extern" / "OpenRenderGraph"),
            "BasicRHI": git_info(source / "extern" / "BasicRHI"),
        }
        dll = plugins / "CommunityShaders.dll"
    else:
        raise StageError(f"{source} is neither a worktree (src, package, build) nor an AIO folder (SKSE/Plugins/CommunityShaders.dll)")
    if pad_count:
        pad(mod, pad_count)
    stat = dll.stat()
    manifest["dll"] = {
        "sha256": sha256(dll),
        "size": stat.st_size,
        "mtime": datetime.datetime.fromtimestamp(stat.st_mtime).isoformat(timespec="seconds"),
    }
    paths.stages.mkdir(parents=True, exist_ok=True)
    (paths.stages / f"{name}.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def load_manifest(paths, name):
    path = paths.stages / f"{name}.json"
    if path.exists():
        return json.loads(path.read_text(encoding="utf-8"))
    return None
