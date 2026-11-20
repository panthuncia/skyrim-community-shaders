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
- `<table>.*` (in `--set`, `--base` and `--vary`) matches only that table's sweep keys: DCLF's families (the switches on unless set) and the features.
- A key that is not in the schema, a wrong type, or an unknown `[env]` name is a config error before launch.

The shipped configs:

| Config | What |
|---|---|
| `base` | what every run shares: autoload save (`game.autoload_mod`, turned off when `game.save` names a save), Telemetry off, `DXVK_DEBUG=hang`, Nsight layer off, the live-image check (`expect.live_frames = 30`) |
| `guest`, `native-vulkan`, `native-d3d12` | the three modes; guest and native run every port |
| `d3d11` | plain D3D11 mode, the render graph off (`CS_ORG=0`); compose it, `d3d11+quick` |
| `quick` | a shot 60 frames after world-ready, then exit; compose it, `native-vulkan+quick` |
| `guest-quick`, `native-quick`, `native-d3d12-quick` | the old quick scenarios |
| `boot` | exits at shaders-ready (`--set build.shader_cache=cold` for a cold compile) |
| `resize` | Exponential Height Fog turned on, its grid settings changed (`setting` steps) and upscale method switches: graph textures that take a new size mid-session; compose it, `native-vulkan+resize` |
| `llf-local-shadows` | Light Limit Fix's local light shadows on (`cs.llf_local_shadows`, DCLF off) in Dragonsreach, then the cache's resolution, slot count and toggle changed mid-session, a shot after each; D3D11 only, compose it as `d3d11+llf-local-shadows` (guest and native need DCLF and stop at startup without it) |
| `llf-parity` | Light Limit Fix's graph culling compared with D3D11's every 300 frames (`[ORG] LLF parity` lines); guest only; compose it, `guest+upscalers+llf-parity` |
| `view-atmosphere` | Exponential Height Fog turned on, then Atmosphere and Fog's toggle and distant grid changed, then the fog off and on again: the atlas and the post-water composite follow frame revisions, a shot after each; compose it, `native-vulkan+view-atmosphere` |
| `shadows-blend` | Terrain Blending and Screen Space Shadows toggled, and the shadows' sample count changed, mid-session: clients, a program and a depth source that switch with frame revisions; compose it, `native-vulkan+shadows-blend` |
| `ssgi` | Screen Space GI toggled, its GI, specular, denoiser and blur options and its resolution mode changed, and a quality preset written (the `ssgi-preset` step), mid-session: textures, programs and a client that switch with frame revisions; compose it, `native-vulkan+ssgi` |

| `physical-sky` | Physical Sky turned off and on, its volumetric clouds on, off and on, then the sky off and on with the clouds on (`setting` steps): its clients, the clouds' screen targets and Atmosphere and Fog's composite switch with frame revisions, a shot after each; compose it, `native-vulkan+physical-sky`, `d3d11+physical-sky` |
| `clouds`, `clouds-long`, `clouds-off-long` | Physical Sky's volumetric clouds turned on after world-ready, shots 240 frames later and 900 (`clouds-long`: 6000, as plain D3D11 compiles the cloud programs with FXC first); `clouds-off-long` keeps them off, the baseline at the same frames; compose them, `guest+clouds` |
| `quality-burst` | XeSS, then FSR, with the quality mode changed many times a few frames apart (new SDK contexts while frames are in flight); compose it, `native-d3d12+quality-burst` |
| `dynres` | DLSS, FSR and XeSS in turn, each with the quality mode changed through the `setting` step (some on consecutive frames) and shots at qualities 1, 4, 0 and 1 again, time stopped: a ratio change is constants and latched viewports, adopted with no build and no new version; compose it, `native-vulkan+dynres`, `native-d3d12+dynres`, `guest+dynres` |
| `dynres-vram` | FSR at quality 4, then at 0, two VRAM dumps each (`vram-json`, ORG's allocator as the source): the VRAM cost of display-size render textures (decision 6a); run at a 3840x2160 display, compose it, `native-vulkan+dynres-vram` |
| `hdr` | HDR Display turned on mid-session (the `hdr` step), shots in HDR with and without the CS menu, then off again; compose it, `native-d3d12+hdr`, `d3d11+hdr` |
| `fg` | FSR frame generation switched on (the `fg` step), then the CS menu, the pause menu and a coc with it on, driver presents logged; compose it, `guest+fg`, `d3d11+fg` |
| `native-fg` | `fg` on native Vulkan (`native-vulkan` + `fg`); with HDR, `native-fg+hdr` |
| `post-effects` | the post chain's effects toggled and changed (`setting` steps: COD Bloom's blend factors, DoF's gather, Physical Glare's and Lens Flare's FFT, the effect list), a shot after each. Needs the profile's Post Processing mode switched from Vanilla to Post Processing for the run; compose it, `native-vulkan+post-effects` |
| `post-lens` | Physical Lens's parts (ImageEffects, Diffraction, ghosts through Intensity) toggled, its lens model changed and Camera FX on and off (`setting` steps), a shot after each. Needs the profile's Post Processing mode switched to Post Processing for the run; compose it, `native-vulkan+post-lens` |
| `post-programs` | DoF's gather ring count changed and back: the chain's new permutation is built on the programs lane before its revision starts, never on the render thread. Needs the profile's Post Processing mode switched to Post Processing; compose it, `native-vulkan+post-programs` |
| `terrain-shadows` | coc between worldspaces (the save's, Raven Rock, Riverwood), a shot in each: Terrain Shadows' heightmap and shadow height map switch with a frame revision; compose it, `native-vulkan+terrain-shadows` |
| `ibl` | Image Based Lighting on, static IBL off, a DALC mode that reads the environment SH and back, then IBL off (`setting` steps): IBL's client and the fog's IBL inputs switch with frame revisions; compose it, `native-vulkan+ibl` |
| `skylighting-reset` | coc into Dragonsreach and back out to Whiterun: each loading screen resets Skylighting's probe volumes as new versions in a frame revision; compose it, `native-vulkan+skylighting-reset` |
| `dclf-family` | Riverwood in cloudy weather with a melee bandit: screen blood in first person, blood decals in third, then HDR on and off with shots of the blood and the sky: the DCLF family clients (sky, decal occlusion, blood, shadow mask) declared on frame variables; compose it, `native-vulkan+dclf-family` |
| `bink` | BGS_Logo.bik played through the `bink` step in SDR, then again with HDR on: Bink's client declared on its targets' variables; compose it, `native-vulkan+bink` |

| `river-sgtm` | Riverwood's river with the global time multiplier at 4, then 1: the water client's heightmap bound changes with a frame revision; compose it, `native-vulkan+river-sgtm` |

| `sss` | Subsurface Scattering's mode switched (Burley, separable, back) and character lighting toggled in Dragonsreach (`setting` steps): the mode's programs and client switch with frame revisions; compose it, `native-vulkan+sss` |
| `dynamic-cubemaps` | Dynamic Cubemaps with water SSR toggled (`setting` steps), then a coc into Dragonsreach and back: the client follows the reflection cubemap and registers again only for the first colour source; compose it, `native-vulkan+dynamic-cubemaps` |
| `volumetric-shadows` | coc into Dragonsreach and back out to Whiterun: Volumetric Shadows' client, registered once by a frame revision, stays as it is across the interior's shut gate; compose it, `native-vulkan+volumetric-shadows` |
| `cloud-shadows` | Cloud Shadows under a forced storm, its opacity changed and back, then a coc into Dragonsreach and back: the occlusion copy's client, registered once by a frame revision, stays as it is; compose it, `native-vulkan+cloud-shadows` |

| `effects11` | Effects11's rays on, bloom and adaptation off and on, a window resize, a shader cache clear and the rays off (`e11` steps: `<CATEGORY>.<Key>=<value>` or `clear-shader-cache`): the ORG FX chain's plan, its textures and the rays' textures, programs and client switch with frame revisions; needs an Effects11 preset with "Post Processing Mode": "Effects11"; compose it, `native-vulkan+effects11` |
| `effects11-programs` | Bloom off and on and a shader cache clear: the FX chain's programs are built on the programs lane before their revision starts, never on the render thread; needs an Effects11 preset with "Post Processing Mode": "Effects11"; compose it, `native-vulkan+effects11-programs` |

| `shader-reload` | a forced reload of every live pipeline (`dclf:reload-shaders=all`), then an unchanged one-file reload, a shot before and after each (`wait = "reload"`): the swap is a `ShaderGeneration` event, swapped in by the revision that carries it; compose it, `native-vulkan+shader-reload` |
| `rain` | Riverwood's river in a forced rain storm (`fw`), two shots while it falls; compose it, `native-vulkan+rain`, `guest+rain` |
| `latch-census-short`, `vram-all`, `native-tour`, `native-d3d12-tour`, `native-validation` | ORG tours |
| `rx6600-*`, `sky-river-org1` | the perf and sky scenarios |

The converted scenarios map old frame N to world-ready + (N - 100) for their first command, then each command at `previous` plus the old gap.

## PresentMon

`--set fg.presentmon=true` samples the game's presents per second from the PresentMon service (Intel PresentMon's SDK; the console
PresentMon needs elevation). Each second goes to `presentmon.jsonl` in the run folder. `result.json`'s `presents` has the medians,
and over the `[FGTEST]` seconds in which FSR-FG presented, PresentMon's presents per second and their ratio to the rendered rate
(`fg_multiplier_median`).

## Schema

`schema.toml` maps each typed key to its switch, type, default and description. It is generated:

```
python tools/cs-test/gen_schema.py
```

It reads DCLF's registry and `RegisterSwitch` calls, every other `CS_*` switch named in `src`, the switches the cs-ab scenarios set, and the feature short names. Hand-written parts (the typed ORG keys, the `org.census` preset) are tables in `gen_schema.py`. Edit those and regenerate; do not edit `schema.toml` by hand.

## What a run does

1. Takes `C:\Modding\CS-AB\cs-test.lock`. A lock whose PID is gone is taken over. A live one refuses, or waits with `--wait`.
2. Undoes any journal a killed run left.
3. Records each change in `C:\Modding\CS-AB\journal.json` before making it:
   - the variant swap (the stage's mod enabled). Every stage shares one shader cache, `overwrite\ShaderCache`;
   - with `build.shader_cache = cold`, the shared cache set aside to `C:\Modding\CS-AB\ShaderCache.cold-hold` and put back after;
   - the mods turned off;
   - a stray `CommunityShaders.env` moved aside;
   - `CommunityShaders-Test.json` written.
4. Launches MO2 (`-p Default moshortcut://:SKSE`), keeps the displays on, nudges the mouse and focuses the game once. With `CS_TEST_BACKGROUND=1` it does neither (the game needs `bAlwaysActive=1`), so the computer stays usable during a run. It also sets `CS_UPSCALE_UNFOCUSED=1` unless the config sets it: Upscaling stops while another process has the foreground, so an unfocused run would never run DLSS, FSR or XeSS.
5. Watches `CommunityShaders-Test.status.json` until an outcome: the game's own, or `never-launched`, `crashed`, `never-loaded`, `hang` (a stalled status file) or `timeout`.
6. Kills the game and closes MO2. Then it undoes the journal, editing single lines of `modlist.txt` only after MO2 has exited.
7. Collects this run's files (logs by time, first and last 64 MB at most), checks the expectations, and writes `result.json`.
   - The live-image check (`expect.live_frames`): in a run that expects `world-ready`, the game's two shots `_live-a` and `_live-b`,
     taken that many frames apart at the exit, must differ in their pixels. Two identical ones mean a frozen final target and fail
     the run. They are not listed among the run's shots; `result.json`'s `live_image` has the comparison.

The undo also runs on Ctrl+C, Ctrl+Break and at exit. If the harness itself is killed, the next command undoes it.

## Tests

```
python -m pytest tools/cs-test/tests -p no:anyio
```

They use temp folders only: never MO2, the game or the real SKSE folder. The `CS_TEST_ROOT`, `CS_TEST_MO2`, `CS_TEST_PROFILE` and `CS_TEST_SKSE` variables move every path.
