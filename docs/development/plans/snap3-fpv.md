# snap3-fpv: native's post-RenderWorld path without engine calls (phase 3)

Lead: `fpv`, branch `snap3/fpv` (base `org_integration` c4088e6e2), stage `s7-fpv`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 66, 70, 122-145 of
[render-thread-decisions.md](render-thread-decisions.md); follows `snap3-accum.md`, `snap3-post.md`, `snap3-depth.md`.

| Item | State |
|---|---|
| 1. Main::RenderFirstPersonView in native, then the leave-state writes | done: CS driver, no engine call; `LeaveResolveState`, `LeaveDepthState`, `LeaveMaskState` deleted |
| 2. The current camera global (414580) | done: native readers use the frame camera; the render side writes it nowhere; the game side writes it for the DOF modifiers |
| 3. The lens flare gather (AE 106988) | done: CS port from the snapshot and the caster selection's queue (`ImageSpace/LensFlares.inl`) |
| 4. Decision 145's non-fatal mask path | done: the masks skip lights whose maps RenderShadowMaps did not draw; a refused world batch is fatal |

## 1. Main::RenderFirstPersonView (SE 100411, AE 107129)

What the engine function does (AE 0x1414cb6d0): unhides the first-person root, sets the first-person depth range, unbinds RT2 and
binds the main depth, renders the first-person accumulator (`SubmitAccumulator`, flags `p0 << 4 | 0x43`: DCLF's colour epoch hook sits
on this call), restores the depth range, finishes the accumulator's blended groups (AE 0x1414b4860), unbinds RT1 and RT2, then, when
the sun glare node is not hidden, culls it into the main accumulator and submits it (the glare over first person), and submits the
sun's second occlusion query (`Sun::SubmitOcclusionQuery(2, main accumulator, world camera)`). Main::RenderPlayerView calls it from its
first-person block (+0x954), after Main::ClearMasks (+0x8CE: registers the first-person list process's entries into the first-person
accumulator) and before AE 107124 (the root shift back, paired with AE 107123).

In native (base run `s7-fpv-a-base-fp`, 258 first-person frames): `SubmitAccumulator #1` and the occlusion query ran every first-person
frame; the glare branch (cull and `SubmitAccumulator #2`) never ran (the glare node stays hidden); the pre/post-resolve sites counted
0.21 per world frame (the first-person accumulator's and the mist menu's).

Now (`Native::RenderFirstPersonView`, `RenderGraph/Engine/Native/RenderDepth.cpp`, from Deferred's call-site thunk): the perf event
and DCLF's colour draw point (`FirstPerson::ColourDrawPoint`, shared with the guest hook). Not made: the first-person accumulator's
render and blended groups (nothing registers into it in native: FirstPersonCulling does not run), the depth range and target writes,
the glare (no CS family draws the sun glare; the engine's draw was dropped), and the second sun query. Main::ClearMasks is skipped in
native (`FirstPersonHooks.cpp`, runtime check): its list is empty and the root is already unhidden. Light Limit Fix's and the frame
annotations' detours on the function body do not run in native: LLF's flag is read only by the engine's Lighting setup and the water's
strict lights, neither of which records inside the first-person view.

Kept: AE 107123 (first-person camera and root shift to the origin, before RenderDepth) and AE 107124 (the shift back). They are a
pair of game-visible node writes; DCLF reads the first-person camera 107123 builds. Porting them together (the first-person camera from
the snapshot, no root shift) is the phase 7 skeleton's work.

### The leave-state writes

Observe (`s7-fpv-b-obs-interior`, 1,800 world renders): which shadow state fields each write changed.

| Writer | Fields it changed | Readers after it in native |
|---|---|---|
| `LeaveDepthState` | depth target (every call), its mode (97%), viewport width and height (every call), stencil mode (4%), write mode (1%) | the masks (CS, constant modes), the precipitation occlusion map (the engine sets its own targets; DCLF's occlusion view reads no shadow state), AE 107131 (sets RT0 and the depth target), RenderWorld (CS's frame) |
| `LeaveMaskState` | RT0, the depth target, RT0's mode, the depth mode (every call) | the same |
| `LeaveResolveState` | RT1 and RT2 (every call), slots 3 and 4's modes, the alpha reference (3%) | RenderPostProcess (CS, reads none), the native post chain (its steps set every target and mode: `KeepTargets`), screen blood (from game data), DrawInterface's menu scenes (their engine setup binds their targets and alpha reference) |

No new dirty bit was ever set by any of them (the bits were already set). The engine code that read them (RenderFirstPersonView's
flush, RenderPostProcess's flush, RenderDepth's and the masks' followers) no longer runs in native. All three are deleted. Also
deleted: native RenderWorld's clears of 414748 and 414749 (no xref reads them in SE or AE; Main::Draw's setup sets them).

## 2. The current camera (SE 527667, AE 414580)

Readers (Ghidra xrefs): the engine's RenderSSAO, water and effect shader setup, utility shader setup, the VL and DOF effects, the world
map effect (render functions), and **game code**: `ImageSpaceModifierInstanceDOF`'s constructor and Apply take a target's distance
from it (AE 0x141482cd0, called from 0x14029eea0 and the modifier's Apply).

- CS readers in native: the effect camera frame row (`EffectFrame.cpp`) now reads the world camera's planes outside native's
  RenderWorld (`WorldPass::CameraPlanes(false)`), except inside a reflection face, whose camera the engine's face render still makes
  current. The water capture, RenderSSAO, the water blend and the masks already read the snapshot in native.
- Writes removed: RenderShadowMasks' driver, RenderPostProcess's driver (`RunNativeBody`), and ShadowMapsNative's last-light camera in
  native (guest's VL render reads it next; native's VL reads the snapshot).
- The game side writes it: `SideEffects::AfterRenderHalf` sets it to the world camera after the render half, as RenderPostProcess left
  it in the engine's world frame, for the DOF modifier instances.

## 3. The lens flare gather (SE 100274, AE 106988)

`ImageSpace/LensFlares.inl`, called from `RunNativeBody` where the engine's gather was:

- **Game side** (the ImageSpace block's `LensFlareInputs`, filled in the capture): the toggle (391128), the weather's sun flare data
  (414867), Sun::Update's direction and strength (414871, 414878), the sky's sun flare colour (414879), the fade distance (391131),
  the global intensity (391134). The sun light's diffuse is the block's `world.sunColour`.
- **Queue**: native's caster selection queues each kept light with lens flare data into a CS-owned list (`ImageSpace::QueueLensFlare`,
  references held), not the engine's array; the gather lets the references go through the retire queue's engine lane; the selection's
  start lets go of a queue no gather took.
- **Render side**: the gather's port (AE 0x1414c2a90: in view, close enough, a history texel continued or assigned; the last texel
  never handed out), each flare's placement (0x1414c33b0) and each sprite's constants (0x1414c3810), staged as the lens flare steps
  with `NativeSteps` armed (`BuildRowsFrom`, `BuildSpriteFrom`: the param forms now share them). Camera: the snapshot's main camera
  (its world-to-camera matrix and port, as WorldPtToScreenPt3 reads them). Aspect: kMAIN's adopted shape. The lights' positions, radii,
  colours and flare data are read on the render thread (decision 105's rule).
- A flare's history is kept per light here, by render frame (the engine kept it in the light, by game frame).

Observe (`s7-fpv-b-obs-interior`, CS's port computed beside the engine's gather each frame, 1,800 frames): draw counts equal in all
but 2 frames; draws exact 17, within float rounding 112 (one ulp in the sprite placement), different 51. All 51 differences were in
the first frames (a history the engine had before CS's started) and only in the sprites' history weights. The visibility pass's write
mode: the engine rotates it 5 to 8 a frame (`0x1420358ac`, 10 of 10 compared). CS's visibility step had a single form with write mode 5,
so the history channels 1-3 were never written; the step now has four forms (`lensflare.visibility.c0..c3`), and native's gather picks
the channel. The engine's draw path (guest) keeps channel 0: changing guest is out of scope (open item).

## 4. Decision 145

The refusal was "a shadow map in depth target 4294967295": a light kept on a load's first world frame whose maps RenderShadowMaps could
not draw (its descriptors never had a target). `Shadows::Native::RenderShadowMaps` now records the lights it drew (`DrewMaps`); the
world masks skip a kept light it did not draw (it casts no shadow that frame, decision 67, so its channel stays lit). A world mask
batch the build refuses is fatal (`ShadowMaskHooks.cpp`). The family not yet registered stays a logged skip.

## Detector

Engine render calls per native world frame (Dev-Fast). "Before" is this branch's base (c4088e6e2) where measured here, else the last
lead's number for that config (`snap3-depth.md`, `snap3-post.md`, `snap3-accum.md`; facegen and post merged since, so those read high).

| Config | Before | After |
|---|---|---|
| first-person (third then first person) | 18.2 (`-a-base-fp`) | 16.6 (`-c-fp`) |
| native-interior | 13.0 (`-a-base-interior2`) | 12.9 (`-e-...-native-interior`), 12.1 (`-d-interior`) |
| native-vulkan+quick | 30.7 (depth) | 27.2 |
| native-d3d12-quick | 30.2 (depth) | 27.4 |
| post-effects | 21 (post) | 13.9 |
| lens | 24 (accum) | 14.9 |
| shadows-blend | 18.5 (depth) | 14.5 |
| upscalers | 24 (accum) | 14.0 |
| window-resize-stress-taa | 19 (depth) | 14.1 |
| cadence-drop-cells | 24.4 (depth) | 18.7 |

In first-person frames the sites inside RenderFirstPersonView (`SubmitAccumulator #1` and the sun query, 0.39 each) read 0; its row
and Main::ClearMasks' row are removed from the table like RenderPostProcess's (decision 136). The lens flare gather was a direct CS call
of an engine function, which the detector does not count (decision 57), so its port does not move the number. Live engine reads per
world frame: first-person 592 -> 587, interior 569 -> 533-558 (scene noise).

What is left after RenderWorld in a native world frame: the skeleton (Main::RenderPlayerView's own code, its SetRenderTarget calls,
AE 107124, AE 39034's thunk, AE 107143 and the screen splatter's hooked call), and Main::RenderSaoCs when bSAO_CS_Enable is on.

## Runs (stage `s7-fpv`, Dev-Fast, detector on)

| Config | Label | Result |
|---|---|---|
| native-vulkan+first-person, base build | `s7-fpv-a-base-fp` | passed (18.2) |
| native-vulkan+native-interior, base build | `s7-fpv-a-base-interior`, `-interior2` | never loaded (stuck before data-loaded, before any CS code ran) / passed (13.0) |
| native-vulkan+native-interior, observe | `s7-fpv-b-obs-interior` | passed; lens flare and leave-state observations above |
| native-vulkan+first-person | `s7-fpv-c-fp` | passed (16.6); first person drawn, shots differ from the base only by pose |
| native-vulkan+native-interior | `s7-fpv-c-interior` | fatal "lens flares: a sprite: no output target" (the sprite effect's output is set by the engine's gather); fixed |
| native-vulkan+native-interior | `s7-fpv-d-interior` | passed (12.1); a hostile guard opened the Health Low tutorial (decision 92's test-setup problem) |
| guest+quick | `s7-fpv-e-guest-quick` | passed |
| native-vulkan+quick | `s7-fpv-e-native-vulkan-quick` | passed (27.2) |
| native-d3d12-quick | `s7-fpv-e-native-d3d12-quick` | passed (27.4) |
| native-vulkan+post-effects | `s7-fpv-e-native-vulkan-post-effects` | passed (13.9) |
| native-vulkan+lens | `s7-fpv-e-native-vulkan-lens` | passed (14.9) |
| native-vulkan+shadows-blend | `s7-fpv-e-native-vulkan-shadows-blend` | passed (14.5) |
| native-vulkan+upscalers | `s7-fpv-e-native-vulkan-upscalers` | passed (14.0) |
| native-vulkan+window-resize-stress-taa | `s7-fpv-e-native-vulkan-window-resize-stress-taa` | passed (12 resizes, 14.1) |
| native-vulkan+cadence-drop-cells | `s7-fpv-e-native-vulkan-cadence-drop-cells` | passed (18.7) |
| native-vulkan+native-interior | `s7-fpv-e-native-vulkan-native-interior` | passed (12.9) |

No run logged a refused world mask batch. The new config `tools/cs-test/configs/first-person.toml` (third then first person, a shot
of each, tgm first) is snap3-depth's scratch pov config.

## Decided by the agent, to confirm

- FP-a. The sun glare over first person and the sun's second occlusion query are not made in native (the glare is not drawn by any CS
  family in either view; the first query already tests against DCLF's first-person depth, so the second would read the same).
- FP-b. Main::ClearMasks is skipped in native; AE 107123 and AE 107124 stay (a pair of node shifts DCLF's first-person camera reads).
- FP-c. The current camera is written game side after the render half (world camera). In a frame with a menu scene the engine left the
  menu scene's camera there; native leaves the world camera.
- FP-d. The lens flares' history counts render frames (visual-only animation by render frame, decision 27's rule).
- FP-e. The lens flare visibility pass writes its channel by the gather's rotation in native; guest keeps channel 0.
- FP-f. Lights kept but not drawn are left out of the masks; the mask build's other refusals are fatal.

## Open items

- Guest's lens flare visibility pass writes channel 0 only (the history weights read stale channels); the fix is the same variant pick
  from the engine's write mode, a guest behaviour change.
- AE 107123/107124 (first-person camera and root shift) with the phase 7 skeleton.
- Main::RenderSaoCs is still the engine's function when bSAO_CS_Enable is on (its dispatches are CS's steps).
- SE was not run. SE ids are pairs from the SE disassembly (SE 100274's globals; SE computes the sprite angle as (y - 0.5) / (W / H)).
