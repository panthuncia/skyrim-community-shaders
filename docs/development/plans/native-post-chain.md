# Native post chain

Status: 2026-11-25, families 0-6 built and the only path in graph modes, on SE and AE (see "Milestone 1, as built", "Round 2,
as built" and "Families 5 and 6, as built"); every end-of-frame slot CS draws is native ("Slot effects, as built"). The snow window
and families 7-8 are design only. Owner: the ImageSpace shim (`src/RenderGraph/Engine/ImageSpace/`) and the
post chain hooks (`src/RenderGraph/Frame/PostChain.cpp`). Follows [dclf-native-imagespace.md](./dclf-native-imagespace.md) (steps
2a-2c, the formulas in "Where the values come from"). Background: [p5/imagespace.md](../p5/imagespace.md),
[imagespace-remaining-ports.md](./imagespace-remaining-ports.md).

Ids are AE 1.6.1170 unless an SE id is given. Addresses are AE.

## Goal

CS runs the whole ImageSpace chain from its own frame schedule. The engine's ImageSpace container functions are no longer called
in graph mode. CS computes every constant row, the adaptation timing, the history parities, the TAA jitter and the render target
bookkeeping itself.

The engine still supplies data. Once per frame, at a fixed point, CS copies the engine's ImageSpace inputs into a snapshot: the
blended ImageSpaceBaseData and the modifier state from ImageSpaceManager, the INI settings, the camera, the sun, the fog and the
frame time. After that point no CS post code reads engine memory. The chain becomes a function of the snapshot and of state CS
owns.

Why:

- The user's direction: everything native, no engine reads at draw time.
- Rendering will move off the main thread. A chain that needs engine containers to run on the render thread at fixed points cannot
  move. A chain that needs only a snapshot can be recorded on any thread, as soon as the snapshot exists.
- It removes the last engine CPU work and most of the shim bookkeeping from the ImageSpace path (see "CPU and threading").

Non-goals:

- Menu 3D scenes, face generation and the frozen world's composite. `UI3DSceneManager::Render` (51855 / 52727) calls
  `RenderEndOfFrameEffects` for these. They stay on the engine path and on capture (`MenuSteps.inl`).
- ImageSpace modifier *blending*. ImageSpaceManager blends weather, cell and modifier data on the game side. CS reads the result.
  It does not re-implement the blend.
- The water system's simulation inputs and BGSAutoWater's load requests. They are produced by the game. CS reads them.

## Today

### Who calls what

The engine calls these containers in a world frame. CS hooks each one (`ImageSpace.cpp` Install, `PostChain.cpp` Install).

| Stage (CS segment) | Caller | Container | What it does |
| --- | --- | --- | --- |
| Start | water update | water displacement update (31385 / 32176) | sim timer, 0 to N fixed 0.025 s steps, height map swap, ripples |
| Start | 35561 / 36560 | flow copies (`DispatchCopySubRegion` 107217) | drains BGSAutoWater's request ring |
| Deferred | `Main::Draw` | compute SAO (100526 / 107252) | when `bSAO_CS_Enable` |
| Deferred | `Main::RenderWorld` (107142) | `RenderReflections` (100428 / 107146) | SSR, stores the world camera (`DAT_14338c970`) and the normals target (+0x34) |
| Deferred | `Main::RenderWorld` | `RenderSSAO` first call (100520 / 107245) | SAO levels, bumps its counter (+0x54) |
| Deferred | `Main::RenderWorld`, inline | snow window: `FUN_1414d4a00`, `FUN_1414d4a80`, `RenderSnowSSS` | kMAIN into `kSNOW_SWAP` |
| Deferred | `Main::RenderWorld` | `RenderSSAO` second call | the SAO / fog composite |
| PostWater / SunWater | accumulator batch render (106584) | water blend (107285) | water history, clear request (+0x0C) |
| Post | `Main::Draw` | `RenderWorldPostProcess` (100430 / 107148) | the post chain's body, below |
| Post | `RenderWorldPostProcess` | ApplyVL (107193), VL blur (107206) | CS's ORG VL already replaces both |
| Post | `RenderWorldPostProcess` | lens flare gather (106988 `0x1414c2600`) | lights into `DAT_14338c2b0`, visibility, sprites |
| Post | `RenderWorldPostProcess` | `RenderPreTonemapComposites` (99024 / 105675) | SIL, IBLF, VL / lens flare composite, a copy |
| Post | `RenderWorldPostProcess` | `RenderEndOfFrameEffects` (99023 / 105674) | slots 0-2, HDR or copy, TAA, LDR slots 3-7, screenshot, upsample |
| Post | `RenderEndOfFrameEffects` via `RenderEffect` (99025 / 105676) | `ImageSpaceEffectHDR::Render` (100465 / 107183) | luminance chain, adaptation, bloom, tonemap |

`RenderWorldPostProcess` itself (`isdec/postproc.c`) also: sets the current camera `DAT_14332a328` to the world camera and leaves it
set, saves and restores the world camera's +0x174/+0x17c, copies `DAT_14338c820`'s near and far into the DoF effect (+0x14C,
+0x150), unbinds RT0-3 and the depth target and applies state, and sets the thread's render context id (TLS +0x768 = 0x21) for its
duration.

`RenderEndOfFrameEffects` (`dec/eof.c`) also: counts the active slots into manager +0x3C and the last one into +0x40, routes the
targets through the slots, and on a screenshot request (manager +0x211) clears target 0x4B, copies the frame into it, clears the
flag and stores `gFrameCounter` at +0x218.

### What CS does inside them

Since steps 2a-2c every per-effect `Render`, explicit `Render` and `DispatchComputeShader` in these scopes is staged natively from
the param rows or the bound buffer. The engine's per-draw code no longer runs. But the containers still run on the render thread,
and CS depends on them:

- **Step selection.** The frame's description at Begin picks rows from engine state (`PostChain::Read`, `ReadWorld`). The variant
  (normals parity, history parity, luminance level, the IBLF repeat) is still decided late, at the hooks.
- **Constant rows.** `NativeSteps::BuildRows` copies the param that the effect's `UpdateParams` and the container filled.
  `BoundRows` copies the PS b2 the explicit setter bound.
- **Parities and history.** Luminance parity (`DAT_14338c960`), SAO counter (+0x54), SIL counter (+0x14), TAA parity (+0x34),
  water blend parity (`gFrameCounter & 1`), the adapted texture swap (`LAB_142033da8`).
- **Resets.** The adaptation reset (`DAT_14338c95f`), the TAA reset (`DAT_1420353a8`), the water blend clear (+0x0C), the VL history
  reset (VL object +9).
- **Target bookkeeping.** `KeepRenderTargets` sets the shadow state as base Render would. `PreRender` and `PostRender` run around
  each step.
- **Feature hooks on the inner path.** The tonemap call site in `RenderEndOfFrameEffects` (`Main_HDRTonemapBlendCinematic_Render`,
  Hooks.cpp: Tonemap anchor, Effects11, Post Processing's fallback), the EOF call site in `RenderWorldPostProcess` (Upscaling's and
  HDR Display's `Main_PostProcessing`: Post Processing's pre-upscale draw, Reflex markers, the upscaler's evaluate, `SetTemporal`,
  the HDR framebuffer redirect, the clean-scene screenshot, frame generation), the VL composite's colour conversion (Linear
  Lighting), the water blend clear (`WaterBlendHistory`), Post Processing's DoF `IsActive`, blur `Render` and refraction
  `SetupTechnique` hooks, and Legacy Graphics Compatibility's full-screen blur slot.

### Cost

Render thread, Riverwood, 960x540, Dev-Fast (/Od), sampled:

- The engine's container code: about 0.75%.
- CS's shim bookkeeping around it: about 2.8%. Scope enter and leave, `WithEffect`, `InScope`, `EngineCall`, `NativeStepNow`,
  `BuildRows` / `BoundRows`, `PreRender` / `PostRender`, `KeepRenderTargets`, `TakeDraw`, the `DrawTriShape` checks, the
  `RaiseShape` checks in the HDR and SAO thunks, and the anchor runs at scope ends.

## Design

### Data in, frame out

Three parts:

1. **The snapshot** (`ImageSpaceFrame`). A plain struct filled once per frame from engine memory. It holds values, not pointers.
2. **CS's post state** (`PostState`). Everything the containers kept between frames: parities, counters, the adaptation pair, TAA
   history, reset edges. CS owns it. It lives with the frame slot that uses it.
3. **The chain.** One function per family turns (snapshot, post state) into the frame's step list, each step's rows, and the next
   post state. It runs inside CS's stages (Deferred, PostWater/SunWater, Post) as part of their recording. It reads nothing else.

The step tables, passes, gates, latches and HLSL do not change. Only the source of the step list and the rows changes, and the
trigger: the stage opens its steps itself instead of waiting for the engine's calls.

### The snapshot point

**Read point A: Renderer::Begin** (`ImageSpace::OnFrameBegin`, where `PostChain::OnBegin` already reads `PostFields`). By then
`Main::Update` has run: the ImageSpace blend, `Sky::Update` (sun, fog, the adaptation reset), the frame time, the camera update,
the water system's offsets, the INI values. Almost every input is final here. Post Processing already copies `gameISData` at the
same time in its `Prepass`.

**Read point B: the post chain's entry** (`RenderWorldPostProcess`'s thunk). Only for values written during the frame by engine
code CS does not own yet: the VL state counter (VL Render 107023 bumps it), the lens flare gather's results while lens flares stay
on the engine, and the camera that is current at end of frame (TAA's near and far, see Risks). Each of these is a named field of
the snapshot with "read at B" in its comment. The plan is to empty B over time.

Later, when rendering leaves the main thread, read point A becomes the hand-off: the game thread fills the snapshot after
`Main::Update`, and the render side only consumes it.

### Not calling the containers

Each container hook gets a native form. In native mode the thunk does not call `func`. It runs CS's equivalent and the write-back
for that container (below), then returns what the engine's function returns (`RenderSSAO` returns a target index;
`RenderPreTonemapComposites`' helpers return the target they wrote).

- **The post chain.** `RenderWorldPostProcess`'s thunk replaces the whole body. CS stages VL (its ORG VL steps, called directly),
  the composites, the HDR chain, the slots, TAA, the upsample and the screenshot copy from the snapshot.
- **Deferred containers.** `RenderReflections`, both `RenderSSAO` calls, the snow window helpers, the water blend and the compute
  SAO are called from engine functions CS cannot skip (`Main::RenderWorld` and the accumulator also do world work). Each is
  replaced at its own hook. The snow window needs hooks on `FUN_1414d4a00` / `FUN_1414d4a80` / `RenderSnowSSS` or one patch of
  the inline block in `Main::RenderWorld`.
- **Start water.** The displacement update and the flow copies have step counts that depend on the frame time and on loading. They
  are replaced last (see the conversion order). Their native forms take the count at their own hook from snapshot fields read
  there.

### Keeping the call-site hooks

Upscaling and HDR Display hook the call to `RenderEndOfFrameEffects` inside `RenderWorldPostProcess` (107148 +0x1E7, a
`write_thunk_call`). Other mods may hook the same site. If CS stops running the body, that call never happens.

So the native body calls through the call site's current target: read the rel32 at 107148 +0x1E7 at setup and call whatever chain
is installed there. The chain ends in CS's own `RenderEndOfFrameEffects` detour, which in native mode stages the native
end-of-frame steps and does not call the engine. Upscaling, HDR Display and any other site hook run in the same order as today.

The tonemap call site (`RenderEndOfFrameEffects` +0x178, `Main_HDRTonemapBlendCinematic_Render`) is CS's own code. The native HDR
family calls `RunTonemapAnchor`, `State::HandlePostProcessing` and Post Processing's `PreProcess` directly, in the same order.

Hooks on effect vfuncs (`UpdateParams`, `Render`, `IsActive`, `SetupTechnique`) no longer run. Each CS one moves into the native
family (list under "What the engine still owns"). For other mods see Risks.

### Write-back

Some container side effects are read by engine code that keeps running, or by other mods. CS writes these back after its native
form runs. Only these. Everything else the containers kept becomes CS state and the engine copy goes stale.

| Engine state | Written by | Read by (outside the containers) | CS writes back |
| --- | --- | --- | --- |
| Adapted luminance texture `LAB_142033da8` (391302) | HDR Render's swap (`FUN_1414e8d60`) | `BSLightingShader` (`0x1414dced2`), the sun query (`FUN_1414c1e00`, called by `Main::RenderWorld`), CS's `AdaptedLuminanceOf` | Point it at the engine stand-in of CS's current adapted target, each frame |
| Adaptation reset `DAT_14338c95f` | `Sky::Update` sets, HDR clears | the HDR effect only | Clear it after CS consumes it, so the next set is an edge |
| TAA reset `DAT_1420353a8` | `LoadingMenu::ProcessMessage` sets, the TAA helper clears | TAA only | Clear it after CS consumes it |
| Water blend clear (object +0x0C) | the engine on history loss | the water blend only | Clear it after CS consumes it |
| Current camera `DAT_14332a328` (414580) | `RenderWorldPostProcess` sets the world camera | engine code after the chain | Set it to the world camera |
| Shadow state RT0-3, depth, viewport | each container's last Render, `Renderer::UpdateViewPort` | Scaleform displays, menus, the end-of-frame copies | Final target as RT0, RT1-3 and depth unbound, viewport updated, dirty flags set (`KeepRenderTargets`, once per container) |
| Screenshot: target 0x4B, manager +0x211, +0x218 | `RenderEndOfFrameEffects` | the save game's thumbnail | Copy into 0x4B's stand-in, clear +0x211, store `gFrameCounter` at +0x218 |
| Manager +0x3C / +0x40 (active slot count, last slot) | `RenderEndOfFrameEffects` | unknown (open question) | Write them; it is two ints |
| Water displacement pair and timer | displacement update | the water shader binds the current height map | Keep the pair index the engine's water reads (until DCLF's water reads CS's) |
| `State` jitter (+0x44 / +0x48) | `UpdateJitter`, Upscaling | `SetCameraData` (PerFrame), DCLF's `ViewEpoch`, the water port | See "TAA jitter" |

## Containers, one by one

What replacing each implies. "Internal" means no reader outside the container was found (Ghidra xrefs, 2026-11-24), so CS keeps
its own copy and stops maintaining the engine's.

- **HDR effect** (107183). Native: the downsample, luminance levels, adaptation, bright pass blur and tonemap from the formulas in
  dclf-native-imagespace.md ("HDR chain, per level"). CS owns the luminance parity (internal: only HDR Render reads
  `DAT_14338c960`), the adapted pair (CS already owns the targets, `kAdaptedIds`), and the adaptation rate from the snapshot's frame
  time and menu flag. Write-back: the adapted texture pointer, the reset flag. The tonemap call-site features run from the family.
  The HDR effect's first-frame flag (+9, it calls vfunc 0x38) needs a decompile of that vfunc.
- **Pre-tonemap composites** (105675). SIL (`0x1414d79a0`, counter +0x14 internal), IBLF (`0x1414d21b0`, forced off by CS, so its
  native form is "nothing"), the VL / lens flare composite (`0x1414d45f0`) and the final copy when the target differs. Linear
  Lighting's colour conversion of `VolumetricLightingColor` must be applied to the native row (call its conversion, do not hook).
- **End-of-frame effects** (105674). Slots 0-2, then HDR or ISCopy (`DAT_142033d84`), TAA, slots 3-7 (skipped with `usesLdr`), the
  screenshot copy, the upsample. The slot routing (which target each slot reads and writes) is a small pure function of the active
  slots, the TAA flag and the dynamic resolution flag. CS owns the TAA parity (+0x34) and row 0's copy at +0x1C. Each slot's
  `UpdateParams` (DoF, radial blur, refraction, blurs) becomes a native row writer from snapshot fields.
- **Post chain body** (107148). VL apply and blur go straight to CS's ORG VL. The lens flare gather stays an engine call at its
  place until lens flares are ported (it keeps light history in `DAT_14338c230..260`). Camera, DoF near/far and target unbinding
  become write-back or snapshot reads.
- **RenderReflections** (107146). Writes `DAT_14338c970` (read only by `FUN_1414d54b0`, an SSR helper) and the normals target
  index at +0x34. Both internal once SSR and SAO are native. The normals parity follows `gFrameCounter`, an input.
- **RenderSSAO** (107245), both calls. The counter (+0x54) is internal. The composite's rows read `DAT_14338c99d`, which
  `Main::RenderWorld` writes before the second call: read it at that hook, or compute it from the reflections' active flag in the
  snapshot (the same value). `ToggleFog` writes `DAT_14338c99b`: an input.
- **Snow window.** Two copies, no state.
- **Water blend** (107285). Parity is `gFrameCounter & 1` (input). Clear request is a reset edge. `WaterBlendHistory`'s RT1 clear
  moves into the family.
- **Compute SAO** (107252). Its first dispatch checks the bound views today. Native: from the snapshot's sizes and INI.
- **Water displacement** (32176). A simulation with a timer on the effect (+0x98, +0x9C, +0xA8). CS owns the timer and the step
  count. The ripple quads are geometry from the water system's queue (`0x1433dcf28`, count `0x1433dcf38`), read at the hook.
- **Flow copies** (36560). The ring's read pointer (+0x1808) must advance, or BGSAutoWater's queue fills. CS drains it at the hook
  and writes the pointer back. Sources are loaded textures (by heap index, as today).

## What the engine still owns and must be preserved

- **The ImageSpace blend.** `ImageSpaceManager` data (+0xC8 base data, `overrideBaseData`, `currentBaseData`), modifier instances,
  weather and cell ImageSpaces, `TESImageSpaceModifier` updates. Mods that apply modifiers or override base data (photo modes,
  weather mods) work through this, so they keep working.
- **Effect activity.** Each slot's `IsActive` (vfunc 0x30). Post Processing hooks DoF's to skip vanilla DoF. CS calls these vfuncs
  at read point A and stores the bits. That runs engine code, but at the snapshot, not at draw time, and it keeps other hooks on
  `IsActive` working.
- **Jitter source, unless CS takes it** (see below), and `SetCameraData`'s PerFrame buffer until DCLF owns every PerFrame.
- **The water system and BGSAutoWater**: offsets, wading, the ripple queue, flow requests.
- **VL Render** (107023), through CS's ORG VL detour: it bumps the VL counter the composite's target choice follows.
- **Lens flare gathering** until it is ported.
- **Menus**: `UI3DSceneManager::Render`, face gen, the frozen world, the world map (open question), loading screens.
- **Scaleform and the HUD**: they draw after the chain and read the shadow state's current target. CS must leave it as the engine
  did (write-back table).
- **Console commands and settings writers**: `ToggleFog`, `ToggleSAOCS`, `ConfigureTAA`, INI changes. They write globals CS reads
  at point A.
- **Save game thumbnails**: the screenshot request and target 0x4B.

## Inputs and when they are read

All at point A unless marked B or "hook".

| Input | Where | Notes |
| --- | --- | --- |
| ImageSpace data | manager +0xC8..0x148 (HDR, cinematic, tint, fade, DoF, radial blur), refraction tint +0x98, bloom radius +0xCC | already read by `PostChain::Read` for selection |
| Effect activity | each slot's `IsActive`, VL (+0x170 +8), lens flares (+0x180 +0x30), SIL (+0x1D8 +0x10), SSR (+0x1C0 +0x10), snow (+0x1E8 +8), SAO (+0x1C8 +0x50), TAA object (+0x1F0) | |
| Engine switches | `gEnableEOFImageSpace`, HDR on (391301), VL on (414913), SAO and SSR globals (`PostChain.cpp`), `bSAO_CS_Enable` | |
| INI values | HDR curves, adaptation scales, bloom boost, SAO, SSR, snow sparkles, TAA threshold, VL, DoF (`DAT_14338ca90..aa8`) | read each frame; a few dozen floats |
| Frame time | `DAT_142033084` (390954) | `Main::Update` writes it |
| Menu flag | `UI+0x160` | |
| Camera | world camera (`gWorldSceneGraph+0x128`): translate, frustum, near, far; posAdjust current and previous | CS's world camera snapshot, the same one DCLF's views use |
| End-of-frame camera | `DAT_14332a328` near and far | B (TAA row 5) |
| Sun | sun shadow light direction and diffuse, sun light diffuse | after `Sky::Update` |
| Fog | the portal graph's fog property (near, far, colours, power, clamp), framebuffer range `DAT_1420330fc` | |
| VL settings | `FUN_1414c4820` block (intensity, custom colour, contribution) | colour at A, counter at B |
| Target sizes | RenderTargetManager (411451) widths and heights | change only with a revision |
| Dynamic resolution | `State` ratios (+0x104/+0x108), lock (+0x118) | |
| Jitter | `State` +0x44/+0x48, or CS's own | see below |
| Frame counter | `gFrameCounter` (411489) | parities that follow it stay engine-defined |
| Resets | `DAT_14338c95f`, `DAT_1420353a8`, water blend +0x0C, VL object +9 | consumed and cleared by CS |
| Screenshot request | manager +0x211 | |
| Water | `0x1433dcf20/24`, rain and wading vars, the ripple queue, flow request ring | hook (Start) |
| `DAT_14338c99d` | written by `Main::RenderWorld` | hook (second `RenderSSAO`), or computed from SSR's flag |

## State CS must own

**Adaptation.** The adapted pair (CS's targets), which one is current, the reset edge, and the rate. The rate is
`1 - pow(max(0, 1 - 0.01 s), 30 dt)` and its strength-scaled twin, with dt the snapshot's frame time or `fConstHDRAdaptTimerForMenu`
in menus. CS must apply a reset on the frame the engine would: `Sky::Update` sets the flag during `Main::Update`, so point A sees it
on the same frame. Write-back: the engine's adapted texture pointer, so `BSLightingShader` and the sun query read this frame's
value.

**Parities.** Luminance (flips once per HDR run), TAA (flips per run, not in the UI form), SAO (+0x54, wraps at 2), SIL (+0x14).
They live in `PostState` per frame slot and advance only when the step that flips them is in the frame. Parities that follow
`gFrameCounter` (normals, water blend) stay engine-defined and are read.

**Histories.** TAA history and its water mask, the SAO temporal history, the water blend pair, the VL history. Clears come from
the reset inputs and from a revision that changes their size. The shape check (`RaiseShape`) moves from the container thunks to the
snapshot: a change in luminance levels, SAO levels or the water textures is seen at point A.

**TAA jitter.** Today the engine's `UpdateJitter` writes `State` +0x44/+0x48 and Upscaling overwrites them when an upscaler is on
(`Upscaling::ConfigureUpscaling`, Halton by `frameCount`). The engine's `SetCameraData` builds PerFrame's jittered matrices from
them, DCLF's `ViewEpoch` adds them to its projections, and TAA's rows read them. Design: CS computes the jitter for every method
in the existing `Main_UpdateJitter` hook (after the engine's call) and writes it to `State`. The snapshot takes it from CS, not
from `State`. The engine is then a reader of CS's jitter, like DCLF. Open question: match the vanilla sequence bit for bit, or use
CS's Halton everywhere.

**Render target bookkeeping.** CS decides each step's targets from the step table and the slot routing, never from the shadow
state. At each replaced container's end it writes the shadow state as that container leaves it (the write-back table). After the
post chain: the final target (normally kFRAMEBUFFER) as RT0, nothing else bound, the viewport reset. HDR Display's framebuffer
redirect happens in the call-site chain around it, as today.

**Camera.** The world camera's values come from CS's camera snapshot. The current-camera global is written back after the chain.

## Per-family conversion order

Each family goes observe first, then native, behind the switch below. Earlier families keep the engine's containers for the rest.

0. **Snapshot and post state.** Fill `ImageSpaceFrame` at A (and B), keep `PostState`. In observe mode compute every family's step
   list, rows and next state from them and compare (see below). No behaviour change.
1. **HDR.** The most steps in every frame, formulas read. Owns the adaptation and the luminance parity. Write-back of the adapted
   texture and the reset flag. Tonemap call-site features run from the family.
2. **End-of-frame effects.** Slot routing, TAA (parity, jitter rows, reset), the upsample, the screenshot copy, the final copy.
   The modifier slots' rows (refraction, DoF, radial blur, blurs) come with it; FXAA and the world map need their decompile.
3. **Pre-tonemap composites.** SIL, the VL / lens flare composite with Linear Lighting's conversion.
4. **Post chain body.** `RenderWorldPostProcess` no longer called. ORG VL called directly, lens flare gather kept as an engine call,
   the call-site chain called through 107148 +0x1E7, camera and target write-back. From here the Post stage depends only on the
   snapshot (plus B).
5. **SAO** (both calls, compute SAO), then **SSR**, then the **snow window**.
6. **Water blend.**
7. **Start water**: displacement and flow copies. Ties into the water family's port.
8. **Lens flares**, when ported, empty most of read point B.

## Switch and parity (historical: the switch and observe mode were deleted on 2026-11-25, see "Round 2, as built")

`CS_ORG_POST_NATIVE`, a family list (`hdr,eof,composites,post,sao,ssr,snow,waterblend,water`), with a mode:

- 0: the engine's containers run, steps native per effect as today (`CS_ORG_IMAGESPACE_NATIVE=2`).
- 1 (observe): the containers run and draw as in 0. Before each container, CS computes that container's step list, rows and next
  state from the snapshot and `PostState`. It compares:
  - the step list and order against the steps the hooks take (effect, variant, targets from the effect's texture list),
  - each step's rows against `BuildRows` / `BoundRows` (the engine's values, 41,733 and 118,162 draws already matched them to the
    capture), each float within 1e-5 relative, plus the PerFrame float4s,
  - after the container: every `PostState` field against the engine's (parities, counters, the adapted texture pointer, the TAA
    object's +0x1C..+0x34, manager +0x3C/+0x40/+0x211/+0x218, the reset flags), the shadow state's targets and viewport, the current
    camera.
  Mismatches log once per family, field or row, with both values.
- 2 (native): the family's containers are not called. Its native form and its write-back run instead.

Observe runs, as for the per-effect port: Riverwood noon and night, Dragonsreach, a weather change, a modifier (potion, hit: DoF,
radial blur, get hit, full-screen blur), a teleport and a load (resets), a pause menu (menu flag, adaptation timer), a save
(screenshot), dynamic resolution on, Upscaling TAA, DLSS and off, Post Processing on and off, Effects11 on, HDR Display on, Linear
Lighting on.

## CPU and threading

**CPU, render thread, 960x540 Dev-Fast.** Today about 3.55% (0.75% engine containers, 2.8% shim).

- Goes away: the engine containers (0.75%) and the per-effect shim (scopes, `WithEffect`, `InScope`, `EngineCall`, row copies,
  `PreRender` / `PostRender`, per-step `KeepRenderTargets`, `TakeDraw`, the `DrawTriShape` checks): about 1.6-1.8% of the 2.8%.
- Stays or is new: the native row writers (0.2-0.3%, a few dozen floats a step), gate opening and anchor runs (about 1.0% today,
  less once a stage opens all its steps at once instead of at each scope's end), the snapshot and write-back (under 0.1%).
- Net: about 3.55% down to about 1.2-1.5%, a saving of roughly 2-2.3% of the render thread. Release shrinks CS's /Od-inflated
  share more than the engine's, so expect about half that in absolute terms.

**Threading.** This is the larger benefit.

- The Post stage's whole recording (rows, step list, gate entries) can run as soon as the snapshot exists, on any thread, in
  parallel with the engine's world rendering. Today it can only run while the render thread is inside the engine's containers.
- The render thread's ImageSpace work shrinks to the snapshot copy, the write-back and the stage taps.
- No engine D3D11 state is touched by the post chain except the write-back. That removes the shadow-state coupling that tied the
  chain to the render thread.
- With rendering off the main thread, point A is the hand-off between game and render. The Deferred containers inside
  `Main::RenderWorld` still need their hooks until the world draw itself leaves the engine, but their native forms only consult the
  snapshot.

## Risks

- **Unknown readers of container side effects.** The write-back table lists the readers found. A missed one shows as a stale value
  in engine code after the chain (as the Scaleform stale target did in step 2a). Observe mode compares the state CS knows about,
  not the state it does not. Mitigation: Ghidra xrefs for every field each container writes before its family goes native; log
  the shadow state after the chain in both modes.
- **Other mods hooking containers or effect vfuncs.** Their changes stop applying in native mode. Known: Legacy Graphics
  Compatibility's full-screen blur slot (adds an ISCopyDynamicFetchDisabled stage) and its viewport patch. Mitigation: at setup,
  compare each container's prologue and each effect vtable slot with the expected bytes or CS's own thunk. A family with a foreign
  hook stays on the engine path, with one log line naming the hook.
- **CS's own inner-path hooks.** Every CS hook listed under "What CS does inside them" must move into a family. A missed one fails
  silently (for example Linear Lighting's VL colour: wrong colour, no error).
- **One-frame offsets.** Adaptation, parities and resets must change on the same frame as the engine's. Observe mode catches it,
  but only in the scenarios run. Loads and teleports are the risky edges.
- **The current camera at end of frame.** TAA's ThresholdParams read whatever camera was submitted last, which can be the screen
  splatter's. Reproducing that needs read point B. Using the world camera is simpler and arguably correct, but differs from vanilla.
- **Read point B never empties** if lens flares and VL Render stay on the engine. The chain then still waits for the render thread's
  post chain entry for a few values. Acceptable at first; it blocks recording the Post stage at Begin.
- **Formula gaps.** FXAA, the world map and the HDR effect's first-frame vfunc are not decompiled. Rare modifiers (DoF, radial
  blur, get hit) are seen only in observe runs that trigger them.
- **Step counts that are not known at A.** The displacement sim and the flow copies. They stay at their hooks.
- **Dynamic resolution and Upscaling.** The slot routing, the upsample and the TAA rows depend on both. The call-site chain must
  keep running in the same order relative to the native end-of-frame steps.
- **SE.** The decompiles and most addresses are AE. Every id the families read needs its SE pair before SE can use native mode.
- **More engine writes.** The write-back makes CS a writer of engine globals. A wrong write (for example the adapted texture
  pointer) breaks engine draws that are still captured. Keep the write-back list short and observed.

## Open questions

1. Which read points: A only (accept small differences in TAA's near and far, VL parity from CS's own counter) or A and B.
2. Parity policy: match vanilla bit for bit (end-of-frame camera, vanilla jitter sequence) or define CS's values and accept
   documented differences.
3. Jitter owner: CS for every method, or the engine's `UpdateJitter` when no upscaler is on.
4. Foreign hooks: fall back per family (as proposed), or document them as incompatible with native mode.
5. Scope of the first milestone: the Post stage only (families 1-4), or also the Deferred containers inside `Main::RenderWorld`.
6. Who reads manager +0x3C / +0x40 / +0x60, and does the compute SAO or a menu path read the SAO counter (+0x54)?
7. The world map and FXAA: native, or left on the engine path with the menus.
8. Should the write-back of the adapted texture be replaced by routing DCLF's Lighting and the sun query to CS's target directly
   (fewer engine writes, but the captured sun query still needs the engine pointer)?

## Decisions (2026-11-25, main session; the user delegated them)

1. Read points: A and B. B goes away once lens flares and VL Render leave the engine.
2. Parity: match vanilla, including the end-of-frame camera and the vanilla jitter sequence.
3. Jitter: CS owns it for every method.
4. Foreign hooks: decided per family at setup. A family whose container prologue or effect vtable slot holds a foreign hook stays
   on the engine path, with one log line naming the hook. Nothing switches at runtime.
5. First milestone: families 0-4 (the Post stage). The Deferred containers come later.
6. The lead answers it from Ghidra (xrefs to manager +0x3C/+0x40/+0x60/+0x54) and records the readers here.
7. The world map and FXAA stay on the engine path. A frame that needs either runs the end-of-frame family on the engine.
8. Keep the write-back of the adapted texture.

## Milestone 1, as built (2026-11-25)

Code: `src/RenderGraph/Engine/ImageSpace/NativePost.inl` (snapshot, post state, row writers, the four native forms, observe), the
hooks in `ImageSpace.cpp` (`HdrFamily`, `CompositesFamily`, `WorldPostProcess`, `WorldEndOfFrame`) and `PostChain.cpp`, and
`RenderGraph::Engine::TonemapCallSite` (`TonemapAnchor.cpp`), which Hooks.cpp's tonemap call-site hook and the native end-of-frame
effects share. Switch `CS_ORG_POST_NATIVE` (`org.post_native`): 0 engine, 1 observe, 2 native (default); `:hdr,eof,composites,post`
picks families. Native `eof` implies `hdr`, native `post` implies `composites` and `eof`.

What differs from the design above:

- **AE only.** The inputs are AE ids. SE runs every family on the engine (one log line).
- **Slot effects stay the engine's.** Refraction (slot 1) and the depth of field / distant blur (slot 3) are active in almost every
  river frame. The native end-of-frame effects route the frame themselves and call the engine's `RenderEffect` for each active
  slot, whose steps stage natively per effect as before (`CS_ORG_IMAGESPACE_NATIVE=2`). The world map (slot 0) and FXAA (slot 2)
  keep the whole frame's end-of-frame effects on the engine (decision 7). Porting the slot effects' rows is the next step here.
- **Read point B also holds** the VL composite's colour and VL's activity (VL Render fills the settings block during the frame, and
  an interior turns VL off after A: observe caught both), the slot activity (refraction turns on mid-frame), the screenshot
  request and the world camera's near and far. TAA's state (on, form, water) is read at the end-of-frame entry, after Upscaling's
  call-site hook sets it.
- **Post state** starts each frame from the engine's copy at A. CS writes back the luminance parity, the adapted luminance
  ImageSpaceTexture (target, no texture), the adaptation reset, TAA's parity, row 0, form and reset, the manager's slot count and
  last slot, the screenshot flags, the current camera, the depth of field's near and far, and the shadow state's targets. So a
  frame whose family runs on the engine continues the same state.
- **Jitter** (decision 3): in graph modes on AE, `Main_UpdateJitter` calls `PostChain::UpdateJitter`, CS's copy of the engine's
  sequence (AE 0x140e58a10: eight State table entries, its own index), in place of the engine's. Upscaling then overrides it for
  an upscaler, and `NoteJitter` records the final value the TAA rows read.
- **Post Processing's bloom skip** (its hook on the blur effect's Render) moved into the native HDR family
  (`PostProcessing::SkipsVanillaBloom`).
- **Foreign hooks** (decision 4) are checked at the first frame with the ImageSpace manager: each container's entry must lead into
  CS, CS's detour's original code must not jump to another module, the HDR effect's and bloom blurs' UpdateParams slots must be
  the engine's, and the tonemap call site must lead into CS. A foreign hook keeps its family (and the families around it) on the
  engine.
- **Not replicated:** the shader PreRender/PostRender around each step, the engine's TAA history clear on a reset (AE 107273: CS's
  history pair is its own), and `Renderer::ApplyState` in the body and after the HDR effect (D3D11 work CS's passes do not read).

Answers:

- **The HDR effect's flag at +9** calls vfunc 0x38, which is UpdateParams itself; it returns 1, so UpdateParams runs every frame.
- **Question 6.** A decompile scan of the 118 functions that reference the ImageSpaceManager singleton found no reader of +0x3C,
  +0x40 or +0x60 other than `RenderEndOfFrameEffects`, and no reader of the SAO counter +0x54 other than `RenderSSAO`. CS writes
  +0x3C and +0x40 anyway (two ints).


## Slot effects, as built (2026-11-25, perf/post-slots)

Refraction (1), the depth of field with its compute blur (3) and the radial blur (5) are staged natively by the end-of-frame family
from rows CS computes (`RefractionSteps`, `DofSteps`, `DofBlurStep`, `RadialSteps` in `NativePost.inl`); the engine's RenderEffect
runs only for the full-screen blur (6), get hit (7), the raster DoF blur and a DoF composite whose param UpdateParams leaves stale.
Write-back: refraction's drawn flags, the underwater mask flag and its rt12 clear (the skeleton's pass). Legacy Graphics
Compatibility's selector is written into the rows as its hooks write it. A foreign hook on a slot's Render or UpdateParams keeps that
slot on RenderEffect. Observe compares the slots' rows and the compute steps' CS b0 and thread groups: 0 differences. SE has every
id paired (perf-post-slots.md) but runs on the engine unless `CS_ORG_POST_NATIVE` is set. Details: [perf-post-slots.md](./perf-post-slots.md).


## Round 2, as built (2026-11-25, perf2/post)

- **No switch.** `CS_ORG_POST_NATIVE`, its engine (0) and observe (1) modes and the family list are deleted, with the observe
  helpers (predictions, row and state comparisons). `CS_ORG_IMAGESPACE_NATIVE` and its capture, observe and diagnostic modes are
  deleted too. A family still runs the engine's container where CS does not draw the frame: a foreign hook (decision 4), the world
  map or FXAA (decision 7), SIL or the image-based flares, or no ImageSpace manager yet. A slot another mod hooks still goes
  through the engine's RenderEffect.
- **SE as AE.** SE runs the native chain and CS's jitter by default. Every SE pair was re-checked in Ghidra (perf2-post.md);
  untested at runtime.
- **Slots 6 and 7 native** (`FullBlurSteps`, `GetHitSteps`, `SlotBlurStep` in `NativePost.inl`). The full-screen blur (AE 108563 /
  108564): the downsample into rt22, the blur of radius ceil(manager +0x128) (at least 2 while Main::RenderWorld saw the camera
  move: AE 415441, 415422, 414598) between rt22 and rt26, and the upsample back, which alpha blends below an amount of 1. Get hit
  (AE 108553 through the base Render): the copy into the get-hit buffer, effect 10's blur twice (radius hand-off 2.0), and the
  double vision, whose row 1 is (sin, cos) of the timer (AE 390953) times 0.0017453 times manager +0x12C. The blurs' rows are
  ImageSpaceEffectBlur::UpdateParams (AE 108558) with the parent's radius hand-off. Legacy Graphics Compatibility's full-screen blur
  adapter (eleven entries) is replicated: the blurs take the effect's flag and the upsample's row 0 x is 1. Write-back: the alpha
  blend mode after a blended upsample, and effect 10's flag after get hit (the base Render leaves it there).
  Before the observe mode was deleted it compared these steps against the engine's: 8.7k steps, 0 differed (p2-post-obs1).
- **Reviewed against Ghidra, not observed:** the underwater depth of field (DoF UpdateParams AE 107388 and its override
  0x1414e5610) matches `DofSteps`, except that the engine leaves the masked composite's rows 4-7 as the last override frame wrote
  them when mode 2 draws it, where CS writes zeros. The LDR slot routing matches RenderEndOfFrameEffects (AE 105674) step for step.


## Families 5 and 6, as built (2026-11-25, perf2/native)

Code: `src/RenderGraph/Engine/ImageSpace/NativeWorld.inl`, the hooks in `ImageSpace.cpp` (`RenderReflections`, `RenderSSAO`, the new
`WaterBlend` detour on SE 100550 / AE 107285) and `StageWorldKey` in `WorldSteps.inl`, which `TakeWorldStep` and the native forms
share. Lead notes: [perf2-native.md](./perf2-native.md).

- **Not called.** The reflections pass (AE 107146), both RenderSSAO calls (107245) and the water blend (107285) are no longer called
  in graph modes. Each hook reads what its container reads, at the hook, computes the step rows and stages the world steps' passes.
  The steps, gates and HLSL are unchanged. A container still runs where CS does not draw the frame: a foreign hook on it (decision
  4, checked with the post chain's), or no world steps build yet.
- **Reflections** (`ReflectionSteps`): the mask downsample at divider 2 or 3 (raster, 17 rows: texel size, the int sample count and
  jitter flag, four taps) or 4 (compute, 19 rows, groups `(width * 4 + 1023) / 1024` by height of the mask), the ray march (row 0
  the thickness, radius, alpha weight and 1 / radius settings; row 1 the point (0, 0, 1) through the world camera's view matrix from
  the State's camera data, AE 77507), and the compute blur (the depth of field's helper, `SetBlurCs`, grid sizes from the blur
  shaders' first macro, as the engine reads them). Write-back: the State's camera data for the world camera (`SetCameraData`, flag
  1), the camera at 415058, the normals target at +0x34, the drawn flag +0x11, RT0/RT1 and depth unbound.
- **RenderSSAO's first call** (`SsaoSteps`): camera Z, the minify levels (the first level at most 16 high in the contrast form when
  415069 is set), the raw AO (temporal or not) and the H/V blur, rows from the current camera's near, far and frustum and the SAO
  settings 415060 to 415067. The raw AO's random angle is CS's own generator (the engine's is a static in RenderSSAO; only its
  distribution, `rand * pi / 2^32`, matters). Write-back: the SAO counter (+0x54), kSAO bound, depth unbound. The snow window between
  the calls stays the engine's.
- **RenderSSAO's composite** (`ReadComposite`, `CompositeStep`): the form from the SAO flag (+0x50), the portal graph's fog and
  ToggleFog; eleven rows (fog range and colours, near and far, the sun's direction, the eye with the improved snow flag, the four
  sparkle rows with their colour mode, the reflection row with 415072); form 3 is the copy. Write-back: the destination bound, the
  main depth target.
- **Water blend** (`WaterBlendStep`, `RunWaterBlend`): the clear request (+0x0C bit 0: kWATER_1, kWATER_2 and kRAW_WATER through
  the skeleton's clear pass), then the blend row (near, far, the menu flag, `max(0, 1 - |eye moved| * 0.025)`). Write-back: no
  targets, depth test off, blend mode 0, write mode 1.
- **Parity.** Before the switch to native, a development build ran the containers and compared every step CS predicted with the
  step the engine drew (key, rows, thread groups): 18k steps, 0 differed, 0 key mismatches, over the raster, compute and no mask
  downsample, the fog composite and the copy (ToggleFog). Fields the engine leaves as stack garbage were excluded and CS writes
  zero there: ISMinify's rows 1 and 2 (but its dynamic sampling flag), ISMinifyContrast's unused parts of rows 1 and 2, the SAO-only
  composite's rows 0 to 3, and the composite's row 4 w and row 10 w. The SAO composite forms (SAO on) were not reached in the
  test profile; their rows are the fog form's, read in the decompile.
- **SE.** Every global and function has its SE pair, matched by reference order in each SE/AE function pair (perf2-native.md). SE
  runs the native forms by default; untested at runtime.
