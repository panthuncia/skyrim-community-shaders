# cs-test

Runs Community Shaders game sessions, several at once, and reports a definite outcome for each. Each game runs in a lab slot:
a copy of the game built from the Mod Organizer 2 profile (see Lab).
It replaced the old `tools/cs-ab/cs-ab.ps1`, now deleted. The contract with the game is in `docs/development/testing.md`.

Python 3.13, standard library only. The tests need pytest (`python -m pip install pytest`).

```
python tools/cs-test/cs_test.py <command> ...
```

## Commands

```
stage <worktree|aio folder> --name p35 [--preset Dev-Fast] [--base-aio DIR] [--pad N] [--pdb] [--wait S]
run <config>[+<config>...] [--stage p35] [--set key=value]... [--label x] [--dry-run] [--wait S] [--slots N]
sweep <config> --base "features.*=false" --vary features.Skylighting,features.ScreenSpaceGI [--mode on-one|off-one] [run options]
compare <run A> <run B> [--no-shots] [--no-memory]
status
restore [--journal-only]
schema [<CommunityShaders-Switches.json>] [--out FILE]
check-schema [<CommunityShaders-Switches.json>]
```

- **stage** mirrors a build into the MO2 mod `CS AB <name>` and deletes stale files. A run copies the stage into its slot; the
  stage's own lock keeps the two apart, so staging never waits for a game.
  - From a worktree: `build/<preset>/CommunityShaders.dll`, `.pdb` and `CommunityShadersCrashDump.exe` go to `SKSE/Plugins`; `package/Shaders` and every `features/*/Shaders` go to `Shaders`.
  - The first stage of a name needs `--base-aio` (or `C:\Modding\CS-AB\aio\<name>`) for the files a DLL build lacks.
  - From an AIO folder: the whole folder.
  - It writes `C:\Modding\CS-AB\stages\<name>.json`: the git shas of CS, `extern/OpenRenderGraph` and `extern/BasicRHI`, and the DLL's sha256 and time.
- **run** resolves the config, prints its difference from `base`, and runs it. It prints one summary line and writes `result.json`.
  - `--dry-run` prints the resolved JSON and touches nothing.
  - The exit code is 0 only for `passed`.
- **sweep** runs the base, then one run per varied key: on (`on-one`) or off (`off-one`). It prints an outcome table.
- **compare** shows two runs' outcomes, config differences, milestone times and error kinds. It diffs their common shots (`shotdiff.py`) and memory (`compare.py`, when both runs have `CS-AB-memory.csv`). Both also run alone: `python tools/cs-test/shotdiff.py a.png b.png [diff.png]`, `python tools/cs-test/compare.py <run>...`.
- **status** shows the lab's locks, the runs in its slots with their game PIDs and logs, and the shared shader cache's generation. It changes nothing.
- **restore** puts back the CS mod the old MO2 harness left swapped in `modlist.txt` (its `state.json`). Lab runs change nothing outside the lab.
- **schema** copies the game's switch dump (by default the newest run folder's) to `switches.json`, the schema, and lists what changed. It refuses a dump made with DCLF off.
- **check-schema** compares `switches.json` with the game's switch dump. It changes nothing.

A run's name is `<config>` or `a+b`. A run folder is `C:\Modding\CS-AB\runs\<MMdd-HHmmss>-<stage>-<config>[-<label>]`.

## Configs

Configs are `configs/*.toml`. A config may `extends = ["base", ...]`; each file is applied once, parents first, and later files win. `a+b` applies `a`, then `b`. Each `--set` is applied after the files, in order.

- Tables merge key by key. Lists and `steps` are replaced whole.
- `--set` values are TOML when they parse (`true`, `12`, `[...]`), else plain text: `--set game.save=MySave`, `--set 'steps=[{at="world-ready", frames=5, do="exit"}]'`.
- `<table>.*` (in `--set`, `--base` and `--vary`) matches only that table's sweep keys: the features.
- A key that is not in the schema, a wrong type, or an unknown `[env]` name is a config error before launch.

The shipped configs:

| Config | What |
|---|---|
| `base` | what every run shares: autoload save (`game.autoload_mod`, turned off when `game.save` names a save), Telemetry off, `DXVK_DEBUG=hang`, Nsight layer off, the live-image check, off (`expect.live_frames = 0`; its D3D11 fallback shots read black in guest) |
| `guest`, `native-vulkan`, `native-d3d12` | the three modes; guest and native run every port |
| `d3d11` | plain D3D11 mode, the render graph off (`CS_ORG=0`); compose it, `d3d11+quick` |
| `quick` | a shot 60 frames after world-ready, then exit; compose it, `native-vulkan+quick` |
| `guest-quick`, `native-quick`, `native-d3d12-quick` | the old quick scenarios |
| `boot` | exits at shaders-ready (`--set build.shader_cache=cold` for a cold compile) |
| `resize` | Exponential Height Fog turned on, its grid settings changed (`setting` steps) and upscale method switches: graph textures that take a new size mid-session; compose it, `native-vulkan+resize` |
| `llf-local-shadows` | Light Limit Fix's local light shadows on (`saved_settings`) in Dragonsreach, then the cache's resolution, slot count and toggle changed mid-session, a shot after each. In graph modes the cache follows DCLF's shadow views; compose it, `guest+llf-local-shadows`, `native-vulkan+llf-local-shadows`, `d3d11+llf-local-shadows` |
| `llf-slots-live` | Light Limit Fix's local shadow slot count changed live in Dragonsreach (8, 32, then 4 and 24 two frames apart, then 16), a shot when each revision is adopted (`await = "adopted"`) and one later: a new slice count switches in with graphs built for it (ORG array-size versions); compose it, `native-vulkan+llf-slots-live`, `native-d3d12+llf-slots-live`, `guest+llf-slots-live` |
| `llf-local-shadows-fog` | Exponential Height Fog's volumetric fog with Light Limit Fix's local shadow cache in Dragonsreach, two shots; compose it, `guest+llf-local-shadows-fog`, `native-vulkan+llf-local-shadows-fog` |
| `llf-parity` | Light Limit Fix's graph culling compared with D3D11's every 300 frames (`[ORG] LLF parity` lines); guest only; compose it, `guest+upscalers+llf-parity` |
| `view-atmosphere` | Exponential Height Fog turned on, then Atmosphere and Fog's toggle and distant grid changed, then the fog off and on again: the atlas and the post-water composite follow frame revisions, a shot after each; compose it, `native-vulkan+view-atmosphere` |
| `shadows-blend` | Terrain Blending and Screen Space Shadows toggled, and the shadows' sample count changed, mid-session: clients, a program and a depth source that switch with frame revisions; compose it, `native-vulkan+shadows-blend` |
| `ssgi` | Screen Space GI toggled, its GI, specular, denoiser and blur options and its resolution mode changed, and a quality preset written (a `setting` step with an object), mid-session, from the standard preset in `saved_settings`; each shot `await = "adopted"`; `cs.ssgi` on the graph (`graph_frames`): textures, programs and a client that switch with frame revisions; compose it, `native-vulkan+ssgi` |
| `harness-selftest` | the harness's own features: `saved_settings`, `call org.rebuild`, a `setting` object, shots with `await = "adopted"`, `org:refresh` and `expect.graph_frames`; compose it, `native-vulkan+harness-selftest`, `d3d11+harness-selftest` |
| `tripwire-selftest` | the tripwire handles' self-test (`tests/Tripwire/TripwireSelfTest.cpp`, `CS_ORG_TRIPWIRE_SELFTEST=1`) at startup, then exit once the shaders are ready; the log has `[ORG] tripwire self-test: PASS` |

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

| `imagespace-transitions` | Whiterun, Dragonsreach and Whiterun again by coc, each with a loading screen, a shot after each settles: ImageSpace's step groups must lag across the shape changes, never raise a tap error; compose it, `guest+imagespace-transitions`, `native-vulkan+imagespace-transitions` |
| `ssr-divider4` | Riverwood's river with `iReflectionResolutionDivider:Display` set to 4 (`setini`), two shots: the SSR mask's compute downsample (ISDownsampleIgnoreBrightestCS) is a world scope step, no `frame tap error`; compose it, `guest+ssr-divider4`, `native-vulkan+ssr-divider4` |
| `iblf` | Whiterun at noon looking toward the sun, a shot before `ToggleIBLF`, with it, and after toggling it back: the image-based lens flare chain (with ISDownsampleCS) and the composite's lens flare forms are post chain steps, no `frame tap error`; compose it, `guest+iblf`, `native-vulkan+iblf`, `d3d11+iblf` |
| `taa-ui`, `taa-water` | TAA's UI form (Upscaling on TAA, the inventory opened over the world, shots in it) and its water form (Riverwood's river with `ttaaw`, the console's water-blending toggle): their CS steps, and `CheckStepState`'s once-a-session comparison with the engine's state; compose them, `guest+taa-ui`, `native-vulkan+taa-water` |
| `river-sgtm` | Riverwood's river with the global time multiplier at 4, then 1: the water client's heightmap bound changes with a frame revision; compose it, `native-vulkan+river-sgtm` |

| `sss` | Subsurface Scattering's mode switched (Burley, separable, back) and character lighting toggled in Dragonsreach (`setting` steps): the mode's programs and client switch with frame revisions; compose it, `native-vulkan+sss` |
| `dynamic-cubemaps` | Dynamic Cubemaps with water SSR toggled (`setting` steps), then a coc into Dragonsreach and back: the client follows the reflection cubemap and registers again only for the first colour source; compose it, `native-vulkan+dynamic-cubemaps` |
| `gpu-block` | Exponential Height Fog's toggles, View Atmosphere's grid, Terrain Blending's Enabled and Screen Space Shadows' Enable changed mid-session (`setting` steps), a shot on the adopting frame (`await = "adopted"`) and one on the next: the GPU settings blocks read the adopted values, so constants and the frame's shape switch together and each pair of shots agrees; compose it, `native-vulkan+gpu-block` |
| `volumetric-shadows` | coc into Dragonsreach and back out to Whiterun: Volumetric Shadows' client, registered once by a frame revision, stays as it is across the interior's shut gate; compose it, `native-vulkan+volumetric-shadows` |
| `cloud-shadows` | Cloud Shadows under a forced storm, its opacity changed and back, then a coc into Dragonsreach and back: the occlusion copy's client, registered once by a frame revision, stays as it is; compose it, `native-vulkan+cloud-shadows` |
| `sky-cold-programs` | the save outside Whiterun with `CS_DCLF_PROGRAM_MANIFEST=0`, so the sky's programs build at their first draws in the load's first world frames; those draws are left out, never the engine's, so no `frame tap error`; compose it, `native-vulkan+sky-cold-programs` |

| `effects11` | Effects11's rays on, bloom and adaptation off and on, a window resize, a shader cache clear and the rays off (`e11` steps: `<CATEGORY>.<Key>=<value>` or `clear-shader-cache`): the ORG FX chain's plan, its textures and the rays' textures, programs and client switch with frame revisions; needs an Effects11 preset with "Post Processing Mode": "Effects11"; compose it, `native-vulkan+effects11` |
| `effects11-programs` | Bloom off and on and a shader cache clear: the FX chain's programs are built on the programs lane before their revision starts, never on the render thread; needs an Effects11 preset with "Post Processing Mode": "Effects11"; compose it, `native-vulkan+effects11-programs` |

| `shader-reload` | a forced reload of every live pipeline (`org:reload-shaders=all`), then an unchanged one-file reload, a shot before and after each (`wait = "reload"`): the new versions are a `ShaderGeneration` event; every graph is built again off the render thread, and the revision that carries it adopts them with DCLF's scene recorded on them; compose it, `native-vulkan+shader-reload` |
| `shader-reload-resize` | a forced reload of every live pipeline with a fog grid resize every 15 frames while it compiles and its graphs are built again, so the reload's revision and resize revisions with DCLF epochs follow each other, each adopted whole (cleanup plan A4); compose it, `native-vulkan+shader-reload-resize` |
| `dclf-toggles` | DCLF's cull mode (frustum, off, occlusion) and then its pipeline (CPU, GPU) changed during play, a shot after each (`wait = "settled"`): each change rebuilds DCLF's epochs, staged and attached with the snapshot recorded on them at a Begin, with no frame-start wait (plan step 10a); compose it, `native-vulkan+dclf-toggles` |
| `sao-cs`, `sao-cs-downscaled` | the engine's compute SAO (`bSAO_CS_Enable`, then also `bSAO_CS_Downscaled`, set through `[prefs_ini]`), a shot in third and first person: its four dispatches are post chain steps; compose it, `guest+sao-cs`, `native-vulkan+sao-cs` |
| `rain` | Riverwood's river in a forced rain storm (`fw`), two shots while it falls; compose it, `native-vulkan+rain`, `guest+rain` |
| `latch-census-short`, `vram-all`, `native-tour`, `native-d3d12-tour`, `native-validation` | ORG tours |
| `rx6600-*`, `sky-river-org1` | the perf and sky scenarios |

The converted scenarios map old frame N to world-ready + (N - 100) for their first command, then each command at `previous` plus the old gap.

### Steps

- A step after a `shot` holds until the shot is saved (timeout as for `await`). A run's first shot waits several frames for
  the revision that switches its copy in, so a coc right after it would otherwise capture the loading screen.
- `await = "adopted"` holds a step until the frame revision that holds the previous step's writes is adopted. Two frames after
  the previous step ran, the game reads the newest variable serial and holds until the adopted revision carries it. `frames`
  still counts first. A timeout uses `wait_s` (else `timeouts.step_wait`) and fails the run. In plain D3D11 mode it holds at once.
  It is the only value; anything else is a config error.
- `do = "call"` runs a feature's registered test command (`Testing::RegisterFeatureCommand` in the game). Its `arg` is
  `<owner>.<name>` or `<owner>.<name>=<arg>`; owner and name are letters, digits, `_` and `-`. The render graph's commands
  have owner `org`: `call org.rebuild`, `call org.reload-shaders=all`. They are also steps of their own, `do = "org:rebuild"`.

### Graph frames

`[expect.graph_frames]` checks, after a passed run, which clients ran on the render graph:

```toml
[expect.graph_frames]
min = { "cs.ssgi" = 300 }   # at least this many frames on the graph
```

- The game writes `report.graph_frames` into its status file at exit: frames since start, and per client its graph frames
  and first graph frame. A client that never ran is absent (0 frames). A ported client that reaches its D3D11 path in a graph
  mode raises a tap error (`D3D11 path in a graph mode`) at the site instead.
- A run without the report fails (a Release build or an older build). A run with `CS_ORG=0` (plain D3D11) skips the check.
- Each failure is one clause: `cs.ssgi ran on the graph in 12 frames, at least 300 expected`.
- `result.json`'s `graph_frames` has the expectation, the report, the failures and why it was skipped.

### Prefs INI

`[prefs_ini]` sets values in the slot's `SkyrimPrefs.ini` for one run. Its tables are the file's sections.
Use it for a pref the engine reads only at boot, such as `bSAO_CS_Enable:Display`.

```toml
[prefs_ini.Display]
bSAO_CS_Enable = true
```

- The slot's INIs are copies of the game's (the profile's own with LocalSettings=true, else Documents). Names match without case;
  a missing key or section is added.
- Bools are written as 1 and 0, other values as they are.

### Saved settings

`[saved_settings]` is merged into the slot's saved settings (`SettingsUser.json`) for one run. Its tables are the
file's own sections: a feature's display name (`"Screen Space GI"`, `"Light Limit Fix"`) or `"Disable at Boot"`. Each must be a table.

```toml
[saved_settings."Screen Space GI"]
Enabled = true
```

- The start is the copy MO2 would serve: overwrite's, else the staged mod's, else nothing. A file that does not parse stops the run.
- MO2's files are never written. The next run's slot build puts the slot's copy back.
- Tables merge key by key, at any depth; other values are replaced. `--set 'saved_settings.Screen Space GI={Enabled=true}'` sets a whole section.

## PresentMon

`--set fg.presentmon=true` samples the game's presents per second from the PresentMon service (Intel PresentMon's SDK; the console
PresentMon needs elevation). Each second goes to `presentmon.jsonl` in the run folder. `result.json`'s `presents` has the medians,
and over the `[FGTEST]` seconds in which FSR-FG presented, PresentMon's presents per second and their ratio to the rendered rate
(`fg_multiplier_median`).

## Schema

`switches.json` is the schema: a copy of the game's switch dump. The game writes the dump from its one switch table,
`kSwitches` in `src/Testing/SwitchRegistry.cpp`, and DCLF's features' registered switches. cs-test reads only the copy,
so it works without the game.

- A row with a `key` is a typed config key (`[org] native = true`), with its type, default and choices.
- Every other row can be set by name in `[env]`. Names in the dump's `prefixes` families are known too.
- `features.<ShortName>` comes from the dump's feature list.

After a change to the switch table, or to a feature's `RegisterSwitch`, regenerate it from a build:

```
python tools/cs-test/cs_test.py run list-switches --stage p35
python tools/cs-test/cs_test.py schema
```

Do not edit `switches.json` by hand. A new typed key is a `.key` on the table's row.

## Lab

Each game runs from a lab slot, so several run at once (the idea comes from Oculory's Save Lab). The lab folder is
`D:\CSLab` (`CS_TEST_LAB`), on the game's volume so the game's files hard-link:

- `mods\<mod>\`: copies of the profile's enabled mods (and `_overwrite\`, MO2's overwrite folder, without its shader cache and
  RenderDoc captures), because the mods are on C: and a hard link cannot cross volumes. Each run brings them up to date; with
  nothing changed that takes about a second. The first build copied 13 GB in about 4 minutes.
- `slots\<n>\game\`: the slot's game folder. The game folder's own files are copied (captures, dumps, logs and screenshots left
  out), with `steam_appid.txt`. `Data` is MO2's view of the game: the game's own Data, then each enabled mod by priority, then
  the staged build, then overwrite. Every CS mod in the mod list is left out but the staged one, and the run's `mods_off` are
  left out too.
- What a game or plugin may write is copied: configs anywhere (`.ini`, `.json`, `.toml`, `.yaml`, `.txt`, ...), every SKSE file but
  code, overwrite's files but its grass cache, and the staged build. Everything else is a hard link. A file is deleted before it is
  replaced, so nothing is ever written through a link into the installed game or a mod copy.
- `slots\<n>\home\`: the slot's Documents and local AppData. Each run writes the INIs (with `bAlwaysActive=1`), the one save it
  loads (the newest for `autoload`, as Start On Save picks), the profile's `plugins.txt` without the plugins of its `mods_off`,
  and the test file. The SKSE folder starts empty, so the logs in it are the run's own.
- `ShaderCache\`: the shared shader cache. Every slot has its own copy in `Data\ShaderCache`, because the shader store allows one
  writer per folder and each game rewrites its pipeline cache whole. A run starts from the shared one, and a run that ends cleanly
  puts its copy back, unless another run did so after it started (the log says "not kept"). The first run seeds it from MO2's
  `overwrite\ShaderCache`. `build.shader_cache = cold` empties the slot's copy and keeps nothing.

The helper plugin `lab\plugin` (`!CSTestLab.dll`, built with `lab\plugin\build.cmd`; plain Win32 and Detours, no CommonLib)
goes into every slot's `SKSE\Plugins`. It loads first, since SKSE loads plugins in directory order and `!` sorts first:

- The game exits at start-up if a window of its class exists: `FindWindowA` and `FindWindowW` answer "none" for it.
- `SHGetFolderPathA/W`, `SHGetKnownFolderPath` and `SHGetSpecialFolderPathA/W` answer Documents and local AppData with the slot's
  home (`CS_TEST_LAB_HOME`), for every module and thread. So the game, SKSE's co-saves, Community Shaders' log, status and test
  files, and the other plugins' logs stay in the slot. SKSE's own `skse64.log` and `skse64_loader.log` are opened before any plugin
  loads, so they still go to the user's Documents.
- Without `CS_TEST_LAB_HOME` it stops the game and writes `CSTestLab-error.txt` beside itself; the run's reason quotes it.
- It writes `home\cs-test-lab.json` when it loads, at kPostLoad and at kDataLoaded.

It is not part of Community Shaders: a run may stage another branch or an old revision, and those would lack it.

**Slots.** `--slots N` (else `CS_TEST_SLOTS`, else `C:\Modding\CS-AB\lab\settings.json`'s `"slots"`, else 3) is how many runs
go at once. A run takes the first free slot (`lab\slot-<n>.lock`); `--wait S` waits for one. An exclusive run takes every slot,
so no other game runs beside it: `run.exclusive = true` (the perf and VRAM configs, `fg-dlss`), and every run with `game.uncapped` or
`fg.presentmon`. Only an exclusive run focuses its game, and not with `CS_TEST_BACKGROUND=1`.

**Starting.** One game starts at a time (`lab\launch.lock`), until its helper reports kPostLoad (every plugin's `SKSEPlugin_Load` has
returned) or it ends. CommonLib keeps
the Address Library table in shared memory that the first game fills, and a second game that opens it while it is being filled
reads it empty. Every plugin has its own CommonLib build, so the gate is what makes starting safe.

`lab\active\<run id>.json` names a running run's slot, home, log and status file (perf-tools' `trun.sh` reads it).

**SE 1.5.97.** The same tool runs the SE instance with its own lab and documents, sharing the locks:
`CS_TEST_MO2="D:\MO2 installations\MO2 1.5.97 Dev" CS_TEST_LAB=D:\CSLab-SE CS_TEST_DOCUMENTS=D:\CSLab-SE\documents`. Stage and run
as usual. The SE profile has no Start On Save, so name a save in the private documents (`--set game.save=SETest`), or coc from the
main menu in the steps. The SE game folder ships its own `d3dcompiler_47.dll`, so its first run compiles every shader again. Turn
off TressFXSSE there (`game.mods_off`): it crashed the game at start-up. Add `*TerrainHelper.esp` to the SE profile's
`plugins.txt` for the run, and take it out after. Notes: `docs/development/plans/perf3-se.md`.

## What a run does

1. Takes a lab slot (or, exclusive, every slot). A lock whose PID is gone is taken over. With none free it refuses, or waits with `--wait`.
2. Puts itself in a job object that kills its children when it ends, so however cs-test ends, its game ends too. Stops a game a
   killed run left running in the slot.
3. Builds the slot: brings the lab's mod copies up to date, then the slot's game folder and Data to this run's mods and staged
   build (under the stage's lock). Writes the run-only files into the slot: `game.uncapped`'s SSE Display Tweaks override and
   `[saved_settings]`. Copies the shared shader cache into the slot when it changed. Writes the home.
4. Waits for the launch lock, then starts `skse64_loader.exe` in the slot with `CS_TEST_LAB_HOME` and `SteamAppId`. Steam must be
   running. It keeps the displays on. It sets `CS_UPSCALE_UNFOCUSED=1` unless the config sets it, except for a focused exclusive
   run: Upscaling stops while another process has the foreground, so an unfocused run would never run DLSS, FSR or XeSS.
5. Watches the slot's `CommunityShaders-Test.status.json` and the slot's game, found by its image path, until an outcome: the
   game's own, or `never-launched`, `crashed`, `never-loaded`, `hang` (a stalled status file) or `timeout`. The launch lock goes
   at the helper's kPostLoad.
6. Kills its game by PID, never by name. A game still running is dumped first: `CommunityShadersCrashDump.exe --pid` from the
   slot writes every thread's stack and a minidump into the run folder (`CommunityShaders-Kill-*.txt`, `.dmp`). A run whose game
   ended by itself puts its shader cache back (see Lab).
7. Collects this run's files from the slot's SKSE folder and game folder (logs by time, first and last 64 MB at most), checks the
   expectations, and writes `result.json` (its `lab` has the slot, the game's PIDs and the helper's report).
   - The live-image check (`expect.live_frames`, off unless a config sets it): in a run that expects `world-ready`, the game's two shots `_live-a` and `_live-b`,
     taken that many frames apart at the exit, must differ in their pixels. Two identical ones mean a frozen final target and fail
     the run. They are not listed among the run's shots; `result.json`'s `live_image` has the comparison.
   - The graph-frames check (`expect.graph_frames`), read from the final status (`status.json` in the run folder).

Nothing outside the lab changes: not MO2, not the installed game, not the user's INIs or saves.

## Tests

```
python -m pytest tools/cs-test/tests -p no:anyio
```

They use temp folders only: never MO2, the game or the lab. The `CS_TEST_ROOT`, `CS_TEST_MO2`, `CS_TEST_PROFILE`, `CS_TEST_GAME`,
`CS_TEST_DOCUMENTS`, `CS_TEST_LAB` and `CS_TEST_LAB_HELPER` variables move every path.
