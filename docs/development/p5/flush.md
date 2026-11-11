# P5.13 The flush and the shader binds

The engine's state flush and its shader binds on the way to latch-only ([org-full-port.md](../org-full-port.md) P5.13, R2 in
[engine-d3d11-surface.md](../engine-d3d11-surface.md)). Code: `src/RenderGraph/Engine/Flush/`. Switches:
`CS_ORG_RESIDUE_CENSUS=1` (the residual engine-draw census) and `CS_ORG_ENGINE_FLUSH_LATCH=1` (the latch-only flush). Both are
off by default, and both need `CS_ORG_TRIPWIRE_GUEST=1`. RE done in Ghidra on 2026-11-11, SE 1.5.97 and AE 1.6.1170; AE 1.7.104
is not checked.

## The four functions (RE)

The plan listed SE IDs only. The AE IDs were found from the call sites in `BSShader::BeginTechnique` (SE 101341, AE 108328) and
checked by decompiling both: each AE function is the same code as its SE twin.

| Function | SE | AE | CPU side | D3D11 side |
| --- | --- | --- | --- | --- |
| `BSGraphics::SetDirtyStates(bool isCompute)` | 75580 / 0x140d705b0 | 77386 / 0x140e4b5b0 | see below | see below |
| `Renderer::SetShader(Renderer*, VertexShader*)` | 75550 / 0x140d6f9b0 | 77351 / 0x140e4a6b0 | `RSS.flags \|= 0x400`, `RSS.currentVertexShader (+0x348) = vs` | `VSSetShader(vs->shader, 0, 0)`, a tail jump |
| `Renderer::SetPixelShader(Renderer*, PixelShader*)` | 75555 / 0x140d6fd60 | 77356 / 0x140e4aa60 | `RSS.currentPixelShader (+0x350) = ps` | `PSSetShader(ps ? ps->shader : null, 0, 0)`, a tail jump |
| `Renderer::DispatchCSShader(Renderer*, ComputeShader*, x, y, z)` | 75532 / 0x140d6ef90 | 77329 / 0x140e49cb0 | `SetDirtyStates(true)` | `CSSetShader(cs+0x60)`, `CSSetConstantBuffers(0, 1, &computeCB)` (SE 0x143027e80, AE 0x143288780), `Dispatch(x, y, z)`, a tail jump |

Callers (Ghidra cross references, both runtimes):

- `SetShader` and `SetPixelShader` have two code callers each: `BeginTechnique` (SE +0xC3/+0xD7 and a second `SetPixelShader`
  at SE +0xE2; AE +0xD5/+0xEB) and `BSMovie::DrawTiles` (SE 87907 +0xB7/+0xC7, AE 90279 +0xB6/+0xC6, the Bink tiles). CS
  already replaces the `BeginTechnique` calls with its own thunks (Hooks.cpp), which write the same shadow state and bind
  on DXVK's context; those thunks now bind through `Flush::BindVertexShader` / `BindPixelShader`. So the engine function
  is reached from Bink and from SE's second `BeginTechnique` site only. The tripwire's "shader binds (AE 90279)" lines are
  Bink's.
- `DispatchCSShader` has one code caller: `BSComputeShader::Dispatch` (SE 101641 +0x4C, AE 108723 +0x4C), plus a data
  reference. CS detours it twice already (CSShadersSupport in Hooks.cpp, ImageSpace's batch flush).
- R1's "FUN_1410a2370, a tiled texture/screenshot renderer" is `BSMovie::DrawTiles`.

**SetDirtyStates**, both runtimes the same code. What it writes besides D3D11:

| Step | CPU side (kept by the latch) | D3D11 side (dropped by the latch) |
| --- | --- | --- |
| bit 0x1 | each pending target clear (`setRenderTargetMode[i] == 0`) is issued, then the mode is set to 4; the cube face's likewise (+0x6C); depth modes 0..2 and 6 clear `RendererData+0x22` (read-only depth); a depth clear sets the depth mode to 4 | `ClearRenderTargetView` / `ClearDepthStencilView` (kept: the skeleton's call sites take or forward them), `OMSetRenderTargets` |
| bits 0xC | | `OMSetDepthStencilState(table[depth*0x28 + stencil], ref)` |
| bits 0x1070 | with 0x40: the viewport's Min/MaxDepth reset from the saved pair (RSS +0x5C0/+0x5C4), `MaxDepth -= biasOffset[bias]`, bit 0x2 set | `RSSetState(table[...])` |
| bit 0x2 | | `RSSetViewports(1, &RSS.viewPort)` |
| bit 0x80 | | `OMSetBlendState(table[...], factor, 0xffffffff)` |
| bits 0x300 | | `Map(alphaTestCB, WRITE_DISCARD)`, one float, `Unmap` (kept: a buffer upload, which the constants capture records) |
| bit 0x400 (not with isCompute) | the input layout for `vs->vertexDesc & RSS.vertexDesc`: looked up in the cache, or made with `device->CreateInputLayout` and inserted | `IASetInputLayout` |
| bit 0x800 | | `IASetPrimitiveTopology` |
| last | the flag word set to 0 (or `flags & 0x400` with isCompute); the CS UAV, PS sampler, PS resource, CS sampler and CS resource modified words cleared bit by bit | `CSSetUnorderedAccessViews`, `PSSetSamplers`, `PSSetShaderResources`, `CSSetSamplers`, `CSSetShaderResources`, one slot per bit |

Every draw function (R1) latches `RSS.vertexDesc` and topology, calls `SetDirtyStates(false)`, then binds its index and vertex
buffers and draws with a plain `call` (checked in both runtimes, so the draw's return address is inside the draw function). AE
77275 is SE 75488 with its call to 75489 inlined, so AE has one more draw site.

## The census (`CS_ORG_RESIDUE_CENSUS=1`)

The guest context asks a filter (`Tripwire::SetContextFilter`) before it forwards any engine call. The census counts there every
`Draw*` and `Dispatch*` the engine (or another plugin through the engine's context) issues: what reaches it has passed DCLF's
withholding and every family's taker, so a withheld draw is never counted. CS's own draws on `globals::d3d::context` do not go
through the guest and are not counted.

Each draw is classed by:

- the context method and the draw function it came from (the Address Library ID of its return address; DispatchCSShader for a
  dispatch, whose tail jump hides it);
- the three game frames above it, and the first CS frame between them (DCLF's native draws call the engine's draw function from
  CS code);
- the last BSShader technique (`State::currentShader`: type, file name or ImageSpace shader name) and the descriptors of the
  bound shaders (`RSS.currentVertexShader->id`, `currentPixelShader->id`);
- the view: RSS render target 0 (+0x18), the depth stencil (+0x38) and the cube target (+0x40).

It also counts every `SetDirtyStates` call by caller (empty ones, ones from CS code) and every vertex and pixel shader bind, and
which of them no draw followed on the same thread before the next one of the same kind.

Every `CS_ORG_RESIDUE_CENSUS_FRAMES` main-thread frames (default 600; the scenario uses 300) it logs, per frame of that window:

```text
[ORG] residue census: <n> frames, <z> with no engine draw reaching D3D11: <d> engine draws and dispatches a frame reach D3D11 (DrawIndexed .., Dispatch ..); since the start <total> over <frames> frames, <zero> frames with none
[ORG] residue census: by draw function: DrawTriShape (75477/77263) .., ...
[ORG] residue census: by last technique: Lighting .., ImageSpace .., ...
[ORG] residue census: by view: RT kMAIN DS kMAIN .., ...
[ORG] residue census: flushes .. a frame (.. with nothing to apply, .. from CS code, .. followed by no draw); vertex shader binds .. (..); pixel shader binds .. (..)
[ORG] residue census: draw #1: .. a frame, DrawIndexed in DrawTriShape (75477/77263) at SkyrimSE.exe+0x.. (ID 77263+0x..) <- ... (via CS ...); Lighting "Lighting" vs 0x.. ps 0x..; RT .. DS ..
[ORG] residue census: flush with no draw #1: .. a frame of .. at ...
[ORG] residue census: vertex shader bind with no draw #1: ...
```

`CS_ORG_RESIDUE_CENSUS_TOP` sets how many entries each list shows (default 25). The census walks the stack once per draw
(`RtlCaptureStackBackTrace`), so frame times with it on mean nothing.

## The latch (`CS_ORG_ENGINE_FLUSH_LATCH=1`)

The four functions are detoured (`Flush.cpp`; SetDirtyStates outside Hooks.cpp's detour, DispatchCSShader inside ImageSpace's
and Hooks.cpp's). With the latch on:

- **The engine's code still runs.** Each detour calls the original inside a latch scope (thread-local). In the scope the guest
  context drops every `IASet*`, `VSSet*`, `PSSet*`, `GSSet*`, `HSSet*`, `DSSet*`, `CSSet*`, `OMSet*`, `RSSet*` and `SOSet*`
  call and the dispatch. Everything else still goes through: `Map`/`Unmap` (the alpha-test constant), clears (the skeleton's
  call sites take or forward them), queries. So the shadow state, the dirty and modified bits, the deferred clear modes, the
  viewport depth tweak and the input-layout cache come out exactly as without the latch, and captures that read the shadow
  state keep working.
- **What a latched flush cleared is owed.** The detour keeps the six words it consumed (flags, PS resources, PS samplers, CS
  resources, CS samplers, CS UAVs) and the shaders the latched binds would have bound.
- **A flush CS asks for is real.** A `SetDirtyStates` call whose return address is in CS's own image (DCLF's
  `ApplyPendingState`, Grass Optimizations' grass draws, ImageSpace's withholding) first puts the owed bits back and binds the
  owed vertex and pixel shaders on DXVK's context, then runs as usual. These callers read the D3D11 state back or draw with it.
- **An engine draw that still reaches D3D11 is skipped.** A `Draw*` from one of the BSGraphics draw functions (the R1 list,
  SE 75477-75500 and AE 77263-77290, with AE 77275) or a dispatch inside `DispatchCSShader` would run with state that was never
  bound, so the guest context drops it and logs the site once, as an `[E]` line:
  `[ORG] flush latch: engine DrawIndexed in DrawTriShape (75477/77263) at SkyrimSE.exe+0x.. (ID ..) <- ... reached D3D11 with the engine's state latched: skipped`.
  A draw from elsewhere in the game (Scaleform's renderer, which binds its own state) is forwarded; the census shows it.
- Every `CS_ORG_RESIDUE_CENSUS_FRAMES` frames: `[ORG] flush latch: N frames: .. flushes and .. shader binds a frame latched,
  .. D3D11 binds a frame dropped, .. flushes a frame from CS code applied the owed state, .. engine draws and dispatches skipped`.
- `CS_ORG_ENGINE_FLUSH_LATCH_KEEP=PSSetShaderResources,OMSetRenderTargets,...` names binds that still reach D3D11 inside the
  latched functions, to find which reader below still needs which bind.
- The compute shader and `CSSetConstantBuffers(0)` of a latched dispatch are not owed: nothing in CS dispatches with the
  engine's compute state.

Nothing in this lane registers a graph client, so it causes no graph rebuild.

### Readers of the D3D11 state

CS code that asks D3D11 what the engine bound, outside a flush it asked for, reads stale state under the latch. These have to
read the shadow state (`RSS.PSTexture[]` +0x140, `RSS.renderTargets[]` with the renderer's view tables, `RSS.viewPort` +0x70)
or a published graph resource before the latch can be the default. From `grep` on 2026-11-11:

| Reader | Reads | When |
| --- | --- | --- |
| DCLF `Core/Engine/Family/EngineDrawFamily.cpp` `ResolveHeader` | `PSGetShaderResources(register)` | every family's texture headers |
| DCLF `Core/Render/Resources/FrameRecords.cpp` | `PSGetShaderResources` | frame records |
| DCLF `Core/Render/Resources/FrameSlots.cpp` `CaptureBindings` | PS t0.., `OMGetRenderTargets`, `RSGetViewports` | after `ApplyPendingState` (a real flush, so correct) and wherever else it is called |
| DCLF `Features/MainView/MainViewPoints.cpp` | `OMGetRenderTargets` | the main view's points |
| DCLF `Features/Water/WaterCapture.cpp` | PS directional shadow register | water capture |
| `Skylighting::CaptureShadowCascadeSRV`, `VolumetricShadows`, `ExponentialHeightFog::CaptureDirectionalShadowMap` | PS t4 (the shadow cascades) | from `State::Draw`, right after the engine's flush |
| `CloudShadows.cpp` | `OMGetRenderTargets` | the reflection faces |
| `RenderGraph/Engine/Scaleform/Renderer.cpp` | `OMGetRenderTargets` | the Scaleform renderer's target |

The state backups around CS's own D3D11 passes (Effects11 `D3D11StateBackup`, `Globals.cpp`, HDRDisplay, PostProcessing's
raster pass, the screenshot, ImGui, GPU timers) save and restore whatever is bound and need no engine state.

## What reaching zero needs

The census is the measure. The latch becomes the default only when, over a full tour (`p5-all-census.env`):

1. the census reads zero: every window's `<z> with no engine draw reaching D3D11` equals its frame count, and "flushes from CS
   code" is zero or only callers that read the shadow state;
2. a `p5-all-latch.env` run logs no `[ORG] flush latch: engine ... skipped` line, and its screenshots match `p5-all.env`;
3. the readers above read the shadow state or a graph resource.

Then `SetShader` and `SetPixelShader` can drop their D3D11 call for good (Bink's DrawTiles is their only engine caller once
Bink is on the graph), `SetDirtyStates` keeps only its CPU side, and the guest context's binds become tripwires.

**Census results.** None yet: no run with the census switch has been made on this branch. The tripwire run of
`p5-all.env` (2026-11-11) showed, before the census existed, engine draws still reaching D3D11 from SetDirtyStates (AE 77386,
77247), shader binds from AE 90279 (Bink's DrawTiles), the draw functions AE 77263/77264/77265/77275/77276, `CopyResource`
77453/107193, compute dispatch setup 77329/77386 and queries 106604/77370.
