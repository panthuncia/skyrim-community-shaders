# Native post chain

Status: 2026-11-24, design only. Nothing here is built. Owner: the ImageSpace shim (`src/RenderGraph/Engine/ImageSpace/`) and the
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

## Switch and parity

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
