# cs-test

Runs Community Shaders game sessions through Mod Organizer 2 and reports a definite outcome for each.
It replaces `tools/cs-ab/cs-ab.ps1`. The contract with the game is in `docs/development/testing.md`.

Python 3.13, standard library only. The tests need pytest (`python -m pip install pytest`).

```
python tools/cs-test/cs_test.py <command> ...
```

## Commands

```
stage <worktree|aio folder> --name p35 [--preset Dev-Fast] [--base-aio DIR] [--pad N] [--pdb]
run <config>[+<config>...] [--stage p35] [--set key=value]... [--label x] [--dry-run] [--wait S]
sweep <config> --base "dclf.*=false" --vary dclf.shadows,dclf.grass [--mode on-one|off-one] [run options]
compare <run A> <run B> [--no-shots] [--no-memory]
status
restore [--journal-only]
check-schema [<CommunityShaders-Switches.json>]
```

- **stage** mirrors a build into the MO2 mod `CS AB <name>` and deletes stale files.
  - From a worktree: `build/<preset>/CommunityShaders.dll`, `.pdb` and `CommunityShadersCrashDump.exe` go to `SKSE/Plugins`; `package/Shaders` and every `features/*/Shaders` go to `Shaders`.
  - The first stage of a name needs `--base-aio` (or `C:\Modding\CS-AB\aio\<name>`) for the files a DLL build lacks.
  - From an AIO folder: the whole folder, as `cs-ab.ps1 stage` did.
  - It writes `C:\Modding\CS-AB\stages\<name>.json`: the git shas of CS, `extern/OpenRenderGraph` and `extern/BasicRHI`, and the DLL's sha256 and time.
- **run** resolves the config, prints its difference from `base`, and runs it. It prints one summary line and writes `result.json`.
  - `--dry-run` prints the resolved JSON and touches nothing.
  - The exit code is 0 only for `passed`.
- **sweep** runs the base, then one run per varied key: on (`on-one`) or off (`off-one`). It prints an outcome table.
- **compare** shows two runs' outcomes, config differences, milestone times and error kinds. It diffs their common shots (`tools/cs-ab/shotdiff.py`) and memory (`tools/cs-ab/compare.py`).
- **status** shows the lock, a leftover journal, the live variant and cs-ab's leftovers. It changes nothing.
- **restore** undoes leftovers, then puts back the CS mod from before the first swap, as `cs-ab.ps1 restore` did.
- **check-schema** compares `schema.toml` with the game's switch dump.

A run's name is `<config>` or `a+b`. A run folder is `C:\Modding\CS-AB\runs\<MMdd-HHmmss>-<stage>-<config>[-<label>]`.

## Configs

Configs are `configs/*.toml`. A config may `extends = ["base", ...]`; each file is applied once, parents first, and later files win. `a+b` applies `a`, then `b`. Each `--set` is applied after the files, in order.

- Tables merge key by key. Lists and `steps` are replaced whole.
- `--set` values are TOML when they parse (`true`, `12`, `[...]`), else plain text: `--set game.save=MySave`, `--set 'steps=[{at="world-ready", frames=5, do="exit"}]'`.
- `<table>.*` (in `--set`, `--base` and `--vary`) matches only that table's sweep keys: DCLF's families (the switches on unless set), the ports `org.ports = "all"` turns on, and the features.
- A key that is not in the schema, a wrong type, or an unknown `[env]` name is a config error before launch.

The shipped configs:

| Config | What |
|---|---|
| `base` | what every run shares: autoload save (`game.autoload_mod`, turned off when `game.save` names a save), Telemetry off, `DXVK_DEBUG=hang`, Nsight layer off |
| `guest`, `native-vulkan`, `native-d3d12` | the three modes |
| `quick` | a shot 60 frames after world-ready, then exit; compose it, `native-vulkan+quick` |
| `guest-quick`, `native-quick`, `native-d3d12-quick` | the old quick scenarios |
| `boot` | exits at shaders-ready (`--set build.shader_cache=cold` for a cold compile) |
| `latch-census-short`, `vram-all`, `vram-menu-B`, `native-tour`, `native-d3d12-tour`, `native-validation` | ORG tours |
| `rx6600-*`, `sky-river-org0`, `sky-river-org1` | the perf and sky scenarios |

The converted scenarios map old frame N to world-ready + (N - 100) for their first command, then each command at `previous` plus the old gap.

## Schema

`schema.toml` maps each typed key to its switch, type, default and description. It is generated:

```
python tools/cs-test/gen_schema.py
```

It reads DCLF's registry and `RegisterSwitch` calls, every other `CS_*` switch named in `src`, the switches the cs-ab scenarios set, and the feature short names. Hand-written parts (the typed ORG keys, the `org.ports` and `org.census` presets) are tables in `gen_schema.py`. Edit those and regenerate; do not edit `schema.toml` by hand.

## What a run does

1. Takes `C:\Modding\CS-AB\cs-test.lock`. A lock whose PID is gone is taken over. A live one refuses, or waits with `--wait`.
2. Undoes any journal a killed run left.
3. Records each change in `C:\Modding\CS-AB\journal.json` before making it:
   - the variant swap (the stage's mod enabled, its shader cache moved in);
   - the mods turned off;
   - a stray `CommunityShaders.env` moved aside;
   - `CommunityShaders-Test.json` written.
4. Launches MO2 (`-p Default moshortcut://:SKSE`), keeps the displays on, nudges the mouse and focuses the game once.
5. Watches `CommunityShaders-Test.status.json` until an outcome: the game's own, or `never-launched`, `crashed`, `never-loaded`, `hang` (a stalled status file) or `timeout`.
6. Kills the game and closes MO2. Then it undoes the journal, editing single lines of `modlist.txt` only after MO2 has exited.
7. Collects this run's files (logs by time, first and last 64 MB at most), checks the expectations, and writes `result.json`.

The undo also runs on Ctrl+C, Ctrl+Break and at exit. If the harness itself is killed, the next command undoes it.

## Tests

```
python -m pytest tools/cs-test/tests -p no:anyio
```

They use temp folders only: never MO2, the game or the real SKSE folder. The `CS_TEST_ROOT`, `CS_TEST_MO2`, `CS_TEST_PROFILE` and `CS_TEST_SKSE` variables move every path.
