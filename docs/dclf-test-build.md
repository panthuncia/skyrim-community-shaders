# Drawcall Limit Fix: test build notes

Drawcall Limit Fix (DCLF) is a Beta feature of this Community Shaders build. It takes over drawing most of the world: statics,
terrain, trees, grass, LOD, actors, effects and their shadows. It draws them from the GPU in large indirect batches instead of one
engine draw call per object, which removes the game's CPU cost per object in busy scenes.

This is a test build. Expect bugs, and please report them (see "What to send" below).

This build draws the world through a render graph. By default the game still runs on DXVK (bundled), as in earlier test builds.
Two native modes are new and optional: they run without DXVK, on Vulkan or on Direct3D 12 (see "Render modes" below).

## Requirements

- **Skyrim SE 1.5.97, AE 1.6.1170 or AE 1.7.104.** Most testing has been on 1.6.1170, the current Steam build. On any other
  game version, DCLF turns itself off and the game renders as usual. The menu says why.
- **A Vulkan 1.3 driver.** Community Shaders renders through Vulkan in the default mode (DXVK, bundled). Drivers with descriptor
  heaps (`VK_EXT_descriptor_heap`) or descriptor buffers with mutable descriptors use those; older GPUs such as the GTX 10 series
  use plain descriptor sets, which work but are slower. The GPU builds most of the draws on any driver with
  counted indirect draws (`drawIndirectCount`). With `VK_EXT_device_generated_commands` and its pipeline sets (NVIDIA), the colour
  draws use them; without (AMD, Intel), they are plain indirect draws, one per pipeline. Without counted draws, the CPU records
  them; this is slower, but draws the same scene. DCLF has been tested on NVIDIA (RTX 50 series, driver 621; GTX 1070 Ti,
  driver 582) and AMD (RX 6600, and a Radeon integrated GPU on driver 2.0.353). On GTX 10 series and older GPUs DCLF records
  its draws on the CPU, which is faster there. If your driver lacks what it needs, DCLF turns itself off and the game renders as usual.
- **RenderDoc turns DCLF off** while it is loaded: its capture layer hides the descriptor heap extension.

If DCLF is unavailable, the Drawcall Limit Fix page in the Community Shaders menu shows "Unavailable this session" and the reason.

## First launch

The first launch compiles every shader and can take several minutes on the loading screens (about 9 minutes on a GTX 1070 Ti).
Later launches reuse the shader cache and start in under a minute. Changing a Community Shaders setting that needs new shader
variants compiles only those.

## Render modes

Set the mode in `CommunityShaders.env`, a text file next to `CommunityShaders.log` in
`Documents\My Games\Skyrim Special Edition\SKSE\`. Create it if it does not exist. Put one setting per line:

| Mode | Lines in `CommunityShaders.env` |
| --- | --- |
| Default: DXVK, with the render graph and DCLF | none |
| Native Vulkan (no DXVK) | `CS_ORG_NATIVE=1` |
| Native Direct3D 12 (no DXVK) | `CS_ORG_NATIVE=1` and `CS_ORG_NATIVE_BACKEND=d3d12` |
| Without the render graph and DCLF, for comparison | `CS_ORG=0` |

The native modes are newer and less tested than the default. If a feature you use does not work in native mode yet, the game
stops at startup with a message that names it; remove the lines to go back. Please say which mode you used in every report.

## Turning it off

In this build the render graph draws the world through DCLF, so DCLF cannot be switched off while playing. Its switch on the
Drawcall Limit Fix page applies at the next launch. To see whether DCLF causes a problem, start the game once with `CS_ORG=0`
(above): the game then draws everything itself, as it does without DCLF.

## Known issues

- **One frame with objects missing** after a resolution or upscaler change, or after a change of the render target formats.
  After a format change the game can also pause for a few seconds while the new pipelines compile.
- **Physical Sky looks washed out without Linear Lighting.** With Post Processing in Vanilla mode, Physical Sky covers the scene
  in a bright haze. This also happens without DCLF and in upstream Community Shaders; turn Linear Lighting on with Physical Sky.
- **Native Vulkan on AMD RX 6000 GPUs: Physical Lens's ghosts and starburst can stop the game** (the GPU is lost). Turn those two
  effects off in native Vulkan on these GPUs, or use the default mode.
- **Light Limit Fix's local shadow cache stays off** while DCLF is loaded. The two would choose different shadow-casting lights,
  so Light Limit Fix's menu shows the cache as unavailable.
- **Frame generation (FSR frame generation, DLSS-G) is lightly tested** with DCLF: short runs in Whiterun and an interior, with HDR off. Please mention it in a report if you use it.
- **Occasional crashes** are under investigation, most often during long walks through busy areas with cells loading, and at
  load. A crash log helps a lot (see below).
- **Plugins that change scene objects directly** (node flags, fades, materials or alpha set without the game's own functions)
  can leave DCLF drawing an object as it was before the change. The compatibility monitor below finds these.

## The compatibility monitor

On DCLF's menu page, "Watch for changes other plugins make" turns on a check that runs while you play. It compares a slice of the
scene each frame with what DCLF holds, and lists every object that changed with no hook DCLF sees. Each entry names the
object's reference and its plugin. It does not change what is drawn. It costs a little CPU, so leave it off unless you are
testing.

A diagnostic capture (below) includes the monitor's report when the monitor is on. The report has every offender with its
reference and plugin.

## The diagnostic capture

When something looks wrong (slow frames, characters you can see through, objects in the wrong place), press "Create diagnostic
capture" on DCLF's menu page while the problem is visible, then close the menu. Two seconds later DCLF records what it is doing
for the time the slider sets (10 s by default), then writes `DCLF-Capture-<date>-<time>.zip` next to the log. Explorer opens
with the file selected, and the file is on the clipboard, so you can paste it into a message.

The zip holds DCLF's state at the start and the end, what it did during the recording, a screenshot, your logs, your
Community Shaders settings, your load order and your SKSE plugins. Paths under your user folder are written as
`%USERPROFILE%`.

## What to send

For anything you can see while the game runs, a diagnostic capture is the one file to send. Otherwise, send these files from
`Documents\My Games\Skyrim Special Edition\SKSE\`:

- `CommunityShaders.log` from the session with the problem. Send it before you start the game again, because the next start
  replaces it.
- After a crash: `CommunityShaders.dmp` if there is one, and your crash logger's log (for example Crash Logger's).
- After the game froze or the screen went black: any `gpu-crash-*.nv-gpudmp` and `.nvdbg` files with that session's time.
- If the game hangs (a load that never ends, or a frozen picture): after 15 seconds Community Shaders writes
  `CommunityShaders-Hang-<date>-<time>.txt` and `.dmp` next to the log. They show what the game was waiting on, which no log
  can. Wait about 20 seconds before you close the game, then send both. If neither file appears, open Task Manager before you
  close the game, right-click `SkyrimSE.exe` on the Details tab and choose "Create memory dump file". Send the `.DMP` file it
  names.
- For a drawing problem tied to a mod: a diagnostic capture taken with the compatibility monitor on, after you saw the problem.

Please also say where you were (the cell or location), what you did just before, and whether turning DCLF off makes the
problem go away.
