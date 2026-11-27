# Testing in game

Community Shaders runs automated game sessions through two parts:

- **`tools/cs-test`** (Python, out of game). It resolves a test configuration, stages a build into MO2, launches the game, watches it, and collects the results.
- **`src/Testing`** (C++, in game). It applies the configuration, tracks the game's load milestones, runs the configured steps, and reports what happened.

They talk through two JSON files in the SKSE log folder (`Documents\My Games\Skyrim Special Edition\SKSE`):

| File | Written by | Read by |
|---|---|---|
| `CommunityShaders-Test.json` | the harness, before launch | the game, in `SKSEPlugin_Load` |
| `CommunityShaders-Test.status.json` | the game, about once a second and at every milestone | the harness |
| `CommunityShaders-Switches.json` | the game, with `CS_TEST_LIST_SWITCHES=1` | `cs-test schema`, `cs-test check-schema` |

The test runner does not depend on any feature. It works with Drawcall Limit Fix (DCLF) disabled at boot. Features add their own commands and load holds through `Testing::RegisterCommand` and `Testing::Holds`.

## Configurations

A configuration is a TOML file in `tools/cs-test/configs/`. The same table, as resolved, is what the game receives.

```toml
extends = ["base", "native-vulkan"]  # applied in order; this file wins over them

[run]
timeout_s = 180               # the whole run, as a last resort
label = "refs2"

[build]
stage = "p35"                 # a build staged with `cs-test stage`
shader_cache = "warm"         # "warm" keeps the variant's cache; "cold" clears it first

[game]
save = "autoload"             # "autoload" (the save the autoload mod picks) or a save's name
autoload_mod = "Start On Save"   # the MO2 mod that loads a save at the main menu; turned off when save names one
cell = ""                     # if set, a coc to this cell once the save is loaded
mods_off = ["Skyrim Engine Telemetry"]

[features]                    # short names; false disables the feature at boot, true loads it even when the saved settings disable it
Upscaling = false

[dclf]                        # typed keys; the schema names each one's switch
cull = "frustum"

[org]
native = true
backend = "vulkan"            # "vulkan" or "d3d12"

[env]                         # raw switches, still checked against the schema
DXVK_DEBUG = "hang"

[timeouts]                    # seconds per phase (defaults shown)
launch = 60                   # from launch to the first status file
compile = 600                 # from data-loaded to shaders-ready
load = 120                    # from main-menu or load-start to world-ready
step_wait = 60                # a step's wait, unless the step gives its own

[[steps]]
at = "world-ready"            # a milestone, or "previous" (the step before this one)
frames = 60                   # frames after that point
do = "shot"
arg = "start"

[[steps]]
at = "previous"
frames = 10
do = "exit"

[expect]
reach = "world-ready"
shots = ["start"]
no_log = ["device fault"]     # substrings that fail the run if they are logged
live_frames = 30              # base's: two shots this many frames apart at the exit must differ (0: off)
```

`cs-test run a+b --set dclf.cull=frustum --set org.backend=d3d12` merges `a`, then `b`, then each `--set` in order. A `--set` always wins over the files. A key that isn't in `tools/cs-test/switches.json` is an error before launch.

## `CommunityShaders-Test.json` (resolved configuration)

The harness writes this file. All keys are present; empty means none.

```json
{
  "version": 1,
  "name": "native-vulkan+quick",
  "run_id": "1112-151815-p35-native-quick-refs2",
  "run_dir": "C:\\Modding\\CS-AB\\runs\\1112-151815-p35-native-quick-refs2",
  "env": { "CS_ORG_NATIVE": "1", "CS_DCLF_CULL": "frustum", "DXVK_DEBUG": "hang" },
  "features_disabled": ["Upscaling"],
  "features_enabled": [],
  "dclf_ini": { },
  "game": { "save": "autoload", "cell": "" },
  "timeouts": { "launch": 60, "compile": 600, "load": 120, "step_wait": 60 },
  "steps": [
    { "at": "world-ready", "frames": 60, "do": "shot", "arg": "start" },
    { "at": "previous", "frames": 10, "do": "exit", "arg": "" }
  ],
  "expect": { "reach": "world-ready", "shots": ["start"], "no_log": ["device fault"] }
}
```

- **`env`** is applied before anything reads a switch, at the point the old `CommunityShaders.env` was. A variable already in the real environment wins, and that is logged. Every name is checked against the game's switch registry. An unknown name sets the outcome `config-error`.
- **`features_disabled`** joins `CS_DISABLE_FEATURES`. **`features_enabled`** joins `CS_ENABLE_FEATURES`: those features load at boot for this run even when the saved "Disable at Boot" settings disable them, and the settings are not changed. Either list turns the startup precompile off when a named feature has shader defines (its cache is cold); shaders then compile on demand.
- **`dclf_ini`** values are served by `Util::SwitchFile` as though they were in `CommunityShaders-DCLF.ini`.
- **`steps[].wait`** (optional) holds the step until a condition is true: `"settled"`, `"shaders"`, `"reload"`, `"signal:<n>"`. **`steps[].wait_s`** overrides `timeouts.step_wait`. A wait that times out fails the run.
- **`steps[].await = "adopted"`** (optional) holds the step until the frame revision that holds the previous step's writes is adopted. Two Presents after the previous step ran, the game reads the change serial of the newest frame variables a Begin took (`Revision::Mark().takenVariables`). The step runs once the last adopted revision carried variables at least that new. `frames` counts first, and `wait` after it. A timeout (`wait_s`, else `timeouts.step_wait`) fails the run. Without frame revisions (plain D3D11) it holds at once. The log line is `[TEST] step N: await 'adopted' held at frame F: variables V adopted by revision R`. A build-only change (`org:rebuild`) changes no variable, so it holds at once: use `wait = "settled"` there.
- **`game.save`**, when it names a save, is loaded from the main menu once the shaders are ready; a missing file fails the run ("save '<name>' not found"). **`game.cell`** is a coc once that save's world is ready. Both count as step boundaries, so a step at `world-ready` waits for the world after them. Load milestones count only after `main-menu`: the startup loading screen is not a load.
- **Shots** are written to `<run_dir>\shots\<arg>.png`.
- **`expect.live_frames`** (base sets 30): when the `exit` step runs in the world phase, the game first takes `_live-a.png`, then
  `_live-b.png` that many frames later, then exits. In a run that expects `world-ready`, the harness fails the run when the two have
  the same pixels (a frozen final target: the screen kept one frame) or are missing. `result.json`'s `live_image` holds what it found.
  A config whose last steps leave a still image on screen (a menu that pauses the world) sets it to 0.
- **A malformed configuration or an unknown name** ends the game at once with `config-error`, after the status file is written.
- **`at`** anchors a step at the first time its milestone is reached at or after the previous step ran. Every load records the load milestones again, so a step after a `load` or a coc waits for that load's `world-ready`: `at = "world-ready"` and `frames` to settle (60-120 for a shot). `"previous"` is the frame the previous step ran at. A step waiting for its milestone while the world is ready and no load is under way fails after `wait_s` (else `timeouts.step_wait`), for example after a coc that loaded nothing. Through boot, compiling, a load or a menu the phase timeouts time the wait.

### Commands and waits

`do` names a registered command and `arg` is its argument. An unregistered command fails the step (outcome `failed`).

- **Generic** (`src/Testing/Commands.cpp`, any feature set): `shot`, `exit`, `console` (a console line), `loadscreen` (`<model path substring>`: every load screen shows that model), `menu` (`<menu name>=show|hide`), `cs-menu` (`open|close`), `save`, `load` (`<name>|recent`), `weather` (`<editor id or form id>|off`), `mark`, `vram-json`, `window`, `capture`, `telemetry`, `feature` (`<ShortName>=0|1`), `setting` (`<ShortName>.<key>=<JSON value>`: one setting, as its menu widget writes it. The key may be a path, `PostProcessing.CODBloom.MipBlendFactor[3]=0.2`: `.` steps into an object, `[n]` into an array, a name matches with its spaces dropped, and a sub-effect's `settings` object is passed through), `shot-ui`, `upscale`, `pov`, `draw`, `face-npc`, `face-female`, `morph-female`, `npc-alpha`, `cast`, `fg`, `pp`, `hdr`, `bink`, `timestep` (`<ms>`: the game timer's fixed step, 0 for the wall clock), `signal`, `move` and `turn` (`<frames>:<amount per frame>`), `spill`, `demote`, `frame-window`, `ballast`.
- **`setting`** also takes `<ShortName>={<key>: <value>, ...}`: several top-level settings written at once and applied once, as a preset button writes them.
- **`call`** (`<owner>.<name>[=<arg>]`) runs a feature's test command, registered with `Testing::RegisterFeatureCommand(owner, name, command)`. An unknown one fails the step and names the owner's commands.
- **The render graph's** (`src/RenderGraph/Testing/OrgCommands.h`, owner `org`, not in Release builds): `org:<name>` or `call org.<name>`. `rebuild`, `refresh[=<graph id>]`, `reload-shaders[=all|<file>]`, `gpu-window`, `nvperf`, `ts-dump` and the build switches (`pass-queries`, `heap-dedupe`, `list-heaps`, `list-barriers`, `rhi-globals`, `queue-boundary`). Builds are asked at the next adoption point, never inside a frame.
- **DCLF's** are `dclf:<name>` (`dclf:cull` with arg `frustum`, `dclf:plain snow sparkle` with arg `0` for a toggle). They exist only when DCLF installs.
- **Waits:** `shaders` and `signal:<n>` always; `reload` and `nvperf` with the render graph's commands; `settled` only when DCLF installs. An unknown wait fails the step.
- **Graph frames:** each render graph client's graph frames are counted (`src/RenderGraph/Testing/GraphFrames.h`): the frames its work committed in a stage. A ported client that reaches its D3D11 path in a graph mode raises a tap error at the site. The exit logs `[ORG] graph frames at exit` and writes them to the status file's `report.graph_frames`, which `expect.graph_frames` checks.

## `CommunityShaders-Test.status.json` (status)

The game rewrites this file about once a second and at every milestone and outcome. It writes a temporary file, then renames it, so the harness never reads half a file.

```json
{
  "version": 1,
  "run_id": "1112-151815-p35-native-quick-refs2",
  "pid": 12345,
  "updated": "2026-11-12T15:18:58.335",
  "phase": "world",
  "frame": 412,
  "fps": 58.2,
  "milestones": {
    "plugin":         { "t": 0.0,  "frame": 0 },
    "data-loaded":    { "t": 6.1,  "frame": 0 },
    "shaders-ready":  { "t": 9.8,  "frame": 31 },
    "main-menu":      { "t": 10.2, "frame": 40 },
    "load-start":     { "t": 12.0, "frame": 95 },
    "loading-screen": { "t": 12.1, "frame": 96 },
    "world-ready":    { "t": 31.5, "frame": 352 }
  },
  "config": { "applied": 14, "features_off": ["Upscaling"], "features_on": [], "unknown": [] },
  "step": { "index": 1, "do": "exit", "state": "waiting" },
  "compile": { "done": 1544, "total": 1560 },
  "holds": ["dclf:load-hold"],
  "last_error": "[E] Vulkan device fault: invalid read at 0x1f0380000",
  "outcome": "",
  "reason": "",
  "fault": { "address": "0x1f0380000", "markers": "" },
  "shots": ["start"]
}
```

- **`t`** is seconds since the plugin loaded. **`frame`** is the step clock: it counts game updates (one per published snapshot,
  `Testing::OnGameUpdate`) and every Present outside a game update (the main menu and loading screens publish no snapshot).
  **`render_frame`** counts every Present. Steps and milestones count `frame`, so with `snapshot.cadence` set a run takes its
  steps at the same game time as a normal run. Shots log the snapshot their frame rendered (`[TEST] shot <name> rendered
  snapshot <id> (render frame <n>)`): a shot asked at game frame N is the first render of snapshot N or later.
  `expect.live_frames` and the graph-frames report count rendered frames.
- **`phase`** is one of `boot`, `compiling`, `menu`, `loading`, `world` and `exiting`.
- **Milestones** are recorded the first time each is reached. Every later load records `load-start` and `world-ready` again (and `loading-screen` and `held-frame` when it shows them), in a `loads` array of the same shape. A load is any ServingThread::SetLoadingScreenActive call, which the runner counts: a loading menu (a save loaded, a coc into another cell) or a coc nearby, which calls only the false half and shows no loading menu.
- **`config.unchecked`** lists `CS_DCLF_*` names that may be DCLF feature switches but could not be checked, because DCLF did not install.
- **held-frame** is recorded at the first world frame of a load drawn under the loading menu DCLF's load hold keeps up.
- **world-ready** is recorded when all of these hold:
  - the loading menu is closed and no hold is active;
  - the player's parent cell is attached and their 3D is loaded;
  - 30 game frames (`frame`, the step clock) in a row have passed in that state.

### Outcomes

| Outcome | Set by | When |
|---|---|---|
| `passed` | game | the `exit` step ran and every expectation held |
| `failed` | game | the `exit` step ran and an expectation failed, a step failed, or a wait timed out (see `reason`) |
| `config-error` | game or harness | an unknown key or switch, or a malformed configuration |
| `never-loaded` | game or harness | a phase timeout ran out; `reason` names the last milestone reached |
| `device-lost` | game | the GPU device was lost; `fault` holds the fault address and crash markers. The game exits two seconds later. |
| `crashed` | game or harness | an unhandled exception, or the process ended without an outcome |
| `hang` | game or harness | the hang watchdog fired, or the status file was not rewritten for `run.stall_s` (harness) |
| `never-launched` | harness | no status file within `timeouts.launch` |
| `timeout` | harness | `run.timeout_s` ran out with no other outcome |

The harness writes the final `result.json` in the run folder. It holds the outcome, the reason, the milestones, the stage manifest, the resolved configuration, the shots, the first error and the error counts by message kind.

## `CommunityShaders-Switches.json` (switch dump)

With `CS_TEST_LIST_SWITCHES=1` the game writes every switch it knows and exits; in a test run (`cs-test run list-switches`) the outcome is `passed`. `cs-test schema` copies it to `tools/cs-test/switches.json`, cs-test's schema. `cs-test check-schema` compares the two.

```json
{
  "version": 2,
  "dclf_complete": true,
  "prefixes": ["CS_GPU_COUNTERS_", "CS_NVPERF_", "DXVK_", "VK_"],
  "features": ["CloudShadows", "..."],
  "switches": [
    { "name": "CS_DCLF_CULL", "key": "dclf.cull", "type": "enum", "default": "occlusion", "choices": ["off", "frustum", "occlusion"],
      "kind": "feature", "family": "dclf", "description": "off|frustum|occlusion (default): GPU culling, the live toggle's seed" }
  ]
}
```

- The rows are the switch table (`kSwitches` in `src/Testing/SwitchRegistry.cpp`) and DCLF's features' `RegisterSwitch` rows.
- **`key`** is cs-test's typed config key, or empty: the switch is set by name in `[env]`.
- **`type`** is `bool` (`"1"`/`"0"`), `int`, `string` or `enum` (`choices`). **`default`** is the text the switch acts as when unset, or empty.
- **`kind`** is `feature`, `parity`, `diagnostic`, `test` or `other`.
- **`family`** is the owner: `dclf` (`CS_DCLF*`), `org` (`CS_ORG*`, `ORG_*`), `cs` (other `CS_*`), `basicrhi` (`BASICRHI_*`, `SARP_*`), `dxvk`, `vk`.
- **`prefixes`** are families read by a computed name: any name with one is known.
- **`features`** are the feature short names, for `[features]`.
- DCLF's features register their switches only when DCLF installs, so dump with DCLF on.

