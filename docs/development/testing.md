# Testing in game

Community Shaders runs automated game sessions through two parts:

- **`tools/cs-test`** (Python, out of game). It resolves a test configuration, stages a build into MO2, launches the game, watches it, and collects the results.
- **`src/Testing`** (C++, in game). It applies the configuration, tracks the game's load milestones, runs the configured steps, and reports what happened.

They talk through two JSON files in the SKSE log folder (`Documents\My Games\Skyrim Special Edition\SKSE`):

| File | Written by | Read by |
|---|---|---|
| `CommunityShaders-Test.json` | the harness, before launch | the game, in `SKSEPlugin_Load` |
| `CommunityShaders-Test.status.json` | the game, about once a second and at every milestone | the harness |
| `CommunityShaders-Switches.json` | the game, with `CS_TEST_LIST_SWITCHES=1` | `cs-test check-schema` |

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
cell = ""                     # if set, a coc to this cell once the save is loaded
mods_off = ["Skyrim Engine Telemetry"]

[features]                    # short names; false disables the feature at boot
Upscaling = false

[dclf]                        # typed keys; the schema names each one's switch
shadows = false

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
```

`cs-test run a+b --set dclf.shadows=false --set org.backend=d3d12` merges `a`, then `b`, then each `--set` in order. A `--set` always wins over the files. A key that isn't in `tools/cs-test/schema.toml` is an error before launch.

## `CommunityShaders-Test.json` (resolved configuration)

The harness writes this file. All keys are present; empty means none.

```json
{
  "version": 1,
  "name": "native-vulkan+quick",
  "run_id": "1112-151815-p35-native-quick-refs2",
  "run_dir": "C:\\Modding\\CS-AB\\runs\\1112-151815-p35-native-quick-refs2",
  "env": { "CS_ORG_NATIVE": "1", "CS_DCLF_SHADOWS": "0", "DXVK_DEBUG": "hang" },
  "features_disabled": ["Upscaling"],
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
- **`features_disabled`** joins `CS_DISABLE_FEATURES`.
- **`dclf_ini`** values are served by `Util::SwitchFile` as though they were in `CommunityShaders-DCLF.ini`.
- **`steps[].wait`** (optional) holds the step until a condition is true: `"settled"`, `"shaders"`, `"reload"`, `"signal:<n>"`. **`steps[].wait_s`** overrides `timeouts.step_wait`. A wait that times out fails the run.
- **Shots** are written to `<run_dir>\shots\<arg>.png`.

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
  "config": { "applied": 14, "features_off": ["Upscaling"], "unknown": [] },
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

- **`t`** is seconds since the plugin loaded. **`frame`** counts every Present.
- **`phase`** is one of `boot`, `compiling`, `menu`, `loading`, `world` and `exiting`.
- **Milestones** are recorded the first time each is reached. A save loaded after the first world-ready records `load-start` and `world-ready` again, in a `loads` array of the same shape.
- **world-ready** is recorded when all of these hold:
  - the loading menu is closed and no hold is active;
  - the player's parent cell is attached and their 3D is loaded;
  - 30 frames in a row have been presented in that state.

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

With `CS_TEST_LIST_SWITCHES=1` the game writes every switch it knows and exits. `cs-test check-schema` compares it with `tools/cs-test/schema.toml`.

```json
{
  "version": 1,
  "switches": [
    { "name": "CS_DCLF_SHADOWS", "kind": "feature", "description": "0: DCLF does not draw the shadow views (live toggle's seed)", "family": "dclf" }
  ]
}
```

- **`kind`** is DCLF's kind in lower case (`feature`, `parity`, `diagnostic`, `test`, `other`); `other` for switches outside DCLF's registry.
- **`family`** is the owner: `dclf` (DCLF's registry and its features' `RegisterSwitch` rows, `CS_ORG_EPOCHS` among them), `org` (other `CS_ORG_*`), `cs` (other `CS_*`), `basicrhi`, `dxvk`, `vk`.
- **`description`** is the registry's text, or empty.
- DCLF's features register their switches only when DCLF installs, so dump with DCLF on.

