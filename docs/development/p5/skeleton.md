# P5.1 The frame skeleton around DCLF

The engine's own clears and its main depth copy as graph passes ([org-full-port.md](../org-full-port.md) P5.1, R8 in
[engine-d3d11-surface.md](../engine-d3d11-surface.md)). Code: `src/RenderGraph/Engine/Skeleton/`. Switch:
`CS_ORG_ENGINE_SKELETON=1` (default off: nothing is installed). RE done in Ghidra on 2026-11-10, SE 1.5.97 and AE 1.6.1170;
AE 1.7.104 is not checked.

## What the engine does (RE)

Clears are deferred: `SetRenderTarget`, `SetCurrentDepthStencilTarget` and `SetCurrentCubeMapRenderTarget` with a clear
mode only record the request. Two functions issue every deferred clear, then bind:

| Function | SE | AE | Calls (offset in the function, both runtimes unless noted) |
| --- | --- | --- | --- |
| `Renderer::ApplyState` | 75462 / 0x140d6a330 | 77247 / 0x140e44850 | cube face `ClearRenderTargetView` +0x85, each RT's +0xE1, `ClearDepthStencilView` +0x184, `OMSetRenderTargets` +0x1A8 |
| `BSGraphics::SetDirtyStates` | 75580 / 0x140d705b0 | 77386 / 0x140e4b5b0 | cube face +0x8F, each RT's +0xF1, depth SE +0x1B5 / AE +0x1BC, bind SE +0x1D9 / AE +0x1E0 |

All eight are `call [rax+disp32]` (6 bytes). The colour is `RendererData2::clearColor`; the depth clear flags are 3 (mode 0
or 6), 1 or 2, depth `1.0f` (SE 0x1415232d8, AE 0x141ad2870), stencil 0. Immediate clears:

| Function | SE | AE | What |
| --- | --- | --- | --- |
| `Renderer::ClearColor` (renamed) | 75465 / 0x140d6a7a0 | 77251 / 0x140e44cc0 | clears RT slot 0 of the shadow state (`RendererShadowState::GetRenderTarget0`, AE 0x140e4fcf0) with `clearColor`; the vtable call is a tail jump, so the tripwire names the caller |
| `Renderer::ClearDepthStencil` | 75466 / 0x140d6a810 | 77252 / 0x140e44d30 | `ClearDepthStencilView(depthStencils[ds].views[slice], flags, 1.0, clearStencil)` at +0x48 (`call [r9+0x1a8]`, 7 bytes); clears read-only depth first |
| `Renderer::SetClearColors` / `RestoreClearColors` (renamed) | 75463 / 75464 | 77249 / 77250 | push and pop of `clearColor` |

Where they come from in a frame:

- **Shadow maps.** `BSShadowLight::RenderShadowmap` (SE 100820, AE 107604 / 0x1414f0cf0, renamed from
  `BSShadowParabolicLight::sub`): only when `descriptor.clearRenderTarget` (+0xE8), `SetClearColors(1,1,1,1)`,
  `SetCurrentDepthStencilTarget(target +0x54, CLEAR_DEPTH, slice +0x58)`, RT0 none, `ApplyState(false)`: a depth-only clear
  of one slice (flags 1) and the DSV bind. Then `SubmitAccumulator` draws the engine's residue casters into it; DCLF's shadow
  epoch draws later into the same slice.
- **Reflection faces.** `BSCubeMapCamera::RenderFaces` (100780 / 107564): per face `SetCurrentCubeMapRenderTarget(kREFLECTIONS,
  CLEAR, face)`, `SetCurrentDepthStencilTarget(kCUBEMAP_REFLECTIONS, CLEAR)`, `ApplyState(false)`: the face RTV and depth 6
  (flags 3), then the bind.
- **Main depth.** `Main::RenderDepth` (100421 / 107139) sets `kMAIN` with CLEAR; the clear happens at the first
  `ApplyState`/`SetDirtyStates` after it (DCLF's `CaptureDepthPass` applies it too).
- **Main::Draw** (35560 / 36559): after the depth pass, `ClearColor` on the normals target (`Main::CurrentNormalTaaMaskTarget`,
  AE 107177 / 0x1414d0490: 4 or 5 by frame parity when TAA's effect is on) with (0.5, 0.5, 0, 0), then on `kMAIN` with the
  restored colour; optionally target 0x70. These are the tripwire's `36559+0x6df` and `+0x712` lines.
- **G-buffer.** Deferred's `SRTM_CLEAR` on targets 2-7 is cleared by the next `SetDirtyStates`.
- **FinishAccumulating** (99938 / 106583): the stencil clear of `kMAIN` through `ClearDepthStencil(STENCIL)` for passes
  0x5C00006D-70.

Copies of the main depth into `kPOST_ZPREPASS_COPY` (`CopyResource(depthStencils[7].texture, depthStencils[current].texture)`,
`call [r9+0x178]`, 7 bytes):

| Site | SE | AE |
| --- | --- | --- |
| `Main::RenderDepth`'s end | 100421 +0x523 (0x1412e3a43) | 107139 +0xA3F (0x1414cd56f) |
| `FinishAccumulating`, flags bit 7 with passes 0x5C000071-6071 | 99938 +0x3AE (0x1412cb07e) | 106583 +0x39D (0x1414b312d) |
| CS's own, `Deferred::DeferredPasses` | `src/Deferred.cpp` | |

`Renderer::Begin` (75460 / 77245): `ClearState` (slot 110) at SE +0xF4 / AE +0xF1, then `VSSetConstantBuffers` /
`PSSetConstantBuffers(12)` with the per-frame buffer.

Not in this task: `CopyResource` at AE 107193+0x466 (0x1414d1af0, the ImageSpace volumetric lighting's depth copy, P5.6) and at
`Renderer::SetRenderTarget` (75646 / 77453) +0x54, which copies a target into its copy texture for mode 5.

## What replaces each

| Engine work | On the graph | Anchor |
| --- | --- | --- |
| Shadow slice clears (depth targets 2, 3, 4, 10) | a pass per slice, its gated depth-clear draw | `EngineShadowClear` |
| Reflection face clears (cube faces, depth 6) | a pass per face (gated colour draw), one for the depth | `EngineCubeClear` |
| kMAIN, targets 2-7, kSHADOW_MASK and the main depth | a pass per view: a gated colour draw, or a gated draw per depth flag set | `EngineFrameClear` |
| The three `kPOST_ZPREPASS_COPY` copies | one copy pass `kMAIN` to `kPOST_ZPREPASS_COPY` | `EngineDepthCopy` |

- **The thunks.** The 8 ApplyState/SetDirtyStates calls, the ClearDepthStencil call and the two engine copies are patched with
  `call [rip+x]` (a NOP after the 7-byte ones); `ClearColor` is detoured whole. Each site's bytes are checked first, and a
  function whose sites are not all the RE's is left alone.
- **Registered once.** At Setup every view of the fixed list above that the engine's tables hold is made a pass (its program
  built then, one per format and depth flags), and each group registers once. A clear never registers anything: a view not
  in the list keeps the engine's call (logged once per view). A group registers again only when the engine made one of its
  targets again (its texture or view in the table changed: a resize), with a new gate and colour latch, so a recording of the
  old registration (its gate closed by the anchors) cannot clear the wrong view. The depth copy does the same.
- **Clears.** A clear thunk finds the view in the engine's tables (`renderTargets[i].RTV`, `depthStencils[i].views[s]`,
  `cubemapRenderTargets[i].cubeSideRTV[f]`; `kFRAMEBUFFER` is left out) and takes it when its group's registration has a pass
  for that view and flags, the registration is in the anchor's built graph (`FrameAnchors::RequestJoin`) and the anchor can run
  now (`FrameAnchors::CanRun`). The bind thunk (and ClearColor/ClearDepthStencil, which have no bind) flushes: each group with
  clears runs its anchor, whose commit writes each colour clear's value into the group's colour latch and opens the draws'
  gate entries. The engine's `OMSetRenderTargets` follows. A run that did not submit (no `executed` signal) gets the engine's
  calls, in order.
- **Why draws.** An anchor's passes are fixed and its recordings are kept, and an ORG clear cannot be switched off per run. A
  fullscreen triangle through a `RunGate` can: the clear programs (`package/Shaders/RenderGraph/EngineClear.hlsl`) write depth 1
  from the vertex stage (test Always) and zero the stencil with `StencilOp::Zero`, or write the colour latched for the run (a
  LATCH entry: one pass serves every clear colour). This
  needed depth-only raster programs (`RasterProgram`, no colour target with a depth format).
- **Copy.** `AnchorPasses::Copy` (both aspects of a depth-stencil format). It has no run gate: a copy has no indirect form,
  and a raster copy cannot write the stencil the engine's `CopyResource` copies. It needs none: `EngineDepthCopy` is the
  skeleton's own anchor, nothing else registers there, and every run of it is `RunCopy`'s, which wants the copy.
- **Kept on D3D11 on purpose.** The binds (`OMSetRenderTargets`): the engine's residue casters, the face residue and the other
  D3D11 draws still read them (P5.2 finishes DCLF's coverage). `ClearState` and the b12 binds in `Renderer::Begin`: no handle
  passes through them, and the D3D11 readers that remain rely on both; the per-frame buffer's Map belongs to P5.7.

## Runs per frame (the deadlock found in the first run)

Each run of an anchor is an epoch run, and an epoch's runs take the slots of its ring of `kGameFramesInFlight` (4) in turn.
The host prepares a slot's next ticket only after the slot's last run finished on the GPU, and it prepares the next ticket
right after each submission. A skeleton run's GPU work is in DXVK's stream, which reaches the GPU only at DXVK's next flush,
the Present. With every shadow clear batch a run of its own, `EngineShadowClear` ran more than four times in one frame: the host
waited in `WaitSlot` for a run of the same frame that was still unsubmitted, and the render thread waited for that host in
`WaitTicket` (the first skeleton-org1 run: black screen, watchdog).

The fix is in ORG's host: an epoch that runs many times a frame has a ring that grows (`GraphHost::Desc::epochRuns`). The
skeleton's anchors are listed with their runs a frame in the anchor table (`FrameAnchors.cpp`: `EngineShadowClear` 8,
`EngineFrameClear` 8, `EngineCubeClear` 4, `EngineDepthCopy` 3; `ReflectionsPrepass` 2 too). Each such ring holds
`kGameFramesInFlight` (4) times that many slots and starts with 4. When the slot a run would take next ran less than three
Presents ago, the ring takes one slot more instead of waiting, so the host's thread waits only for runs earlier Presents
flushed. A frame with more runs of an anchor than its whole ring (32 shadow clears) is the only limit left: the runs past it get
the engine's D3D11 calls (logged once: `... ran more than its 32 frame slots in one frame; runs past them fall back until the
next Present`). The skeleton has no cap of its own; `FrameAnchors::CanRun` sees the full ring before a clear is withheld,
so those clears are the engine's from the start.

This issue exists only because, in the guest phase, each anchor is a separate host epoch spliced into DXVK's stream, which DXVK submits at Present. Once ORG owns the frame (the Phase 6 cutover), these clears and the other per-call engine work are passes inside one frame graph, and the slot-per-run ring no longer applies.

## Fallback

Per call, the engine's D3D11 call is made (in the original order, at the flush, for taken clears) when:

- the view is not in the skeleton's list or not in the engine's tables, is not one mip-0 slice, has an integer format, its
  value is not depth 1 / stencil 0, or its depth flags have no draw (a depth-only clear of a stencil format);
- the caller is not the render owner, the registration is not in the anchor's built graph yet, or the anchor's ring is full
  this frame (`CanRun`);
- the run did not submit with the pass (no `executed` signal: a failed epoch);
- the engine made the view's target again: the group registers again, and clears stay the engine's until it has joined;
- the group failed (anchor failure).

Any clear value is drawn by the view's one pass (the colour is latched), so there is no per-value limit, and the old limits (4
values a view, 128 gate entries, 16 rebuilds, 32 clears) are gone.

## Test plan

Scenarios `tools/cs-ab/scenarios/skeleton-org0.env` (the P4 switches with the guest tripwire) and `skeleton-org1.env` (the same
with `CS_ORG_ENGINE_SKELETON=1`).

- Log: `engine skeleton: 11 of 11 engine call sites patched, Renderer::ClearColor detoured`, the `joins the ... anchor` lines,
  `the first shadow/cube/frame clears ran on the graph`, `the main depth is copied into kPOST_ZPREPASS_COPY on the graph`. No
  `[E]`, no `the ... anchor failed`, no `stays on D3D11` for the targets below.
- Tripwire lines that should disappear: `ClearDepthStencilView` from 77247+0x18a, 77386+0x1c2 and 77252+0x4f;
  `ClearRenderTargetView` from 77247+0x8b, 77247+0xe7, 77386+0xf7, 36559+0x6df and 36559+0x712; `CopyResource` from
  107139+0xa46. The binds' `OMSetRenderTargets` lines now name `CommunityShaders.dll` (the bind thunk makes the call); so does
  any fallback clear. 107193+0x466 and 77453+0x54 stay.
- Shots `whiterun` and `dragonsreach` against org0: shadows (a slice not cleared shows last frame's casters), water reflections
  (faces), the sky behind objects (kMAIN's clear), normals and SSR, soft particles and decals (kPOST_ZPREPASS_COPY).
- Frame time against org0 (Dev-Fast, GPU-bound): the clears are draws (no fast clear) and each run begins a pass on every view
  of its group.
- Then a resize (window mode change) and a loading screen (clears off the main thread stay on D3D11).

## Open

- SE and AE 1.7.104 are not run.
- The GPU cost of clears as draws, and of every gated pass beginning its render pass on each run, is not measured. If it
  shows, the groups split further, or ORG gets a clear that a gate can skip.
- Main::Draw's optional clear of target 0x70 (`LAB_142033480`) was not traced further.
