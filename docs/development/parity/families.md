# Parity wave: the families lane (`org-native/par-families`)

Goal (plan WS2, "Bink", "Families", "Occlusion queries"): no refusal left in Bink, the DCLF engine draw families, the menu scenes and
the occlusion queries. In native mode, and in guest once the flush latch is on everywhere, a draw a subsystem refuses is lost. An item
is done when its draws leave the residue census (`CS_ORG_RESIDUE_CENSUS=1`) and its refusal is gone from the source.

Nothing here was built or run in this lane. Scenario: `tools/cs-ab/scenarios/families-native.env` (native Vulkan, residue census, a
Bink movie in the world, Whiterun at noon, the inventory, the map, Dragonsreach). Run it with guest (`CS_ORG_NATIVE` removed) too.

## Summary

| Item | State |
| --- | --- |
| 1. Bink | Done except an RE invariant, an allocation failure and one rebuild window (below) |
| 2. Sky | Draw count and published slots done; sun glare and the rest of the per-draw refusals open |
| 3. Water | Draw count done; group 10's Lighting draws and the batch refusals open |
| 4. Shadow mask, blood, precipitation | Target changes and draw counts done; precipitation outside the main view open |
| 5. Menu scenes | Topology and PerGeometry refusals done; tree LOD and streamed textures open |
| 6. Occlusion queries | The sun's early refusals fixed; unbracketed queries with no client open |
| 7. DCLF residue 5.5.6 | Not taken; what is left is written down below |

## 1. Bink

Commits: `fix(bink): close the movie refusals`, `refactor(bink): turn the graph path off cleanly`.

- **The logo's first frames.** `Bink::Setup` moved from `Engine::Setup` to `Engine::PostSetup`, after the features'
  `SetupResources`. HDR Display makes its UI texture there. Before this, with HDR loaded, the client registered without its
  `cs.bink.tiles.hdr` pass, and the logo's first tick found a new target and refreshed the client. It still runs before
  `BuildPending("CS's setup")`, so the client is in the Present graph before the first frame.
- **A target made again.** `Update` now refreshes only when a target becomes another graph resource. A resize or a format change is a
  new version of the same slot. The tile program is chosen per frame at the commit (`ProgramFor(target.format)`), so a new format
  needs no rebuild. HDR's UI texture appearing, going, or being made again (`ORGHDRDisplay::ReleaseTextures` drops the twin) is still
  a new resource. A blocking tick between frames builds that at once before the first world frame. After it, the ticks until the build
  manager attaches the graph are refused, with the reason `the Present graph with HDR's new UI texture is not attached yet`.
- **Movies larger than 4096x2304.** The limit is gone. A movie of any size is drawn in chunks of the 2048x1152 movie texture
  (`MakeChunks`, as before).
- **The Present ring full.** `FrameAnchors::CanRun` has no ring check, so that reason was never true. It is deleted.
- **No staging block.** The pool has no cap now (`maxBlocks` 0, `runsPerFrame` 2 spares per frame in flight). `Lock` returns null only
  when a block cannot be allocated.
- **HDR's UI texture not importable.** HDR Display's UI texture is a graph twin (`ORGHDRDisplay::GraphOf`), with a `D3D11Texture`
  import as the fallback. The report stays for the fallback's failure. It was not seen.
- **An unexpected tile layout.** `MakeLayout` builds the tiles from the set's own fields (columns, rows, edge sizes). The count
  check can fail only if the engine's set disagrees with its own fields. The engine passes 4 bytes per pixel. This is an RE invariant,
  logged as a warning, never seen.
- **Mode 2 (`TickInFrame`).** Deleted, with the `MovieTiles` reservation, `DrawInFrame` and the in-frame passes. The executables were
  scanned with capstone and the address libraries (Ghidra was down): `MoviePlayer::TickInFrame` (SE 87920 0x1410a2f10, AE 90293
  0x141197a60) has no call, jump, RIP-relative reference or pointer in SE 1.5.97 or AE 1.6.1170. Only `.pdata` and EH IP-to-state
  entries refer to it. `BSMoviePlayer::Tick` has two callers on each runtime: `PlayBlocking` (SE 0x1410a0edf, AE 0x14119599f) and
  `TickInFrame` (SE 0x1410a2f5e, AE 0x141197aae). So no mode-2 tick ever runs. The `MovieTiles` anchor is still in
  `FrameAnchors::Anchor`, `kInfo`, `kTimeline` and `RenderGraphRuntime::Segment`, which this lane does not own: delete it there, it has
  no reserver left.
- **A graph failure.** `Fail` turns the path off for the session: one error line naming the reason, the client unregistered, and no
  per-tick report. The old text claimed the engine draws Bink from then on, which is false in native mode and under the latch.

Trigger: `bink-org1.env`, or `families-native.env` (`dclf:bink=BGS_Logo.bik` at frame 1150). For the logo, `loading-org1.env`.
Look for: `[ORG] Bink: on (CS_ORG_BINK); registered at the Present anchor ...` after `drawing onto HDR's UI texture` when HDR is on,
`a 1920x1080 movie ended: 166 frames drawn by the render graph, 0 by the engine`, no `the engine draws this frame` line, and no
Bink class in the census during the movie.

## 2. Sky family

Commits: `fix(dclf): draw large sky and water batches in runs`, `fix(dclf): read a sky slot's published texture by id`.

- **More than 128 draws.** At the count the latched part runs and the batch goes on in a new run (`Continue`), with the same view,
  face, t17 and headers. Their imports went to `kept` with the submitted run, which holds them for `KeptFrames`. A face run that holds
  cloud layers is still refused at 128: its layer copies draw every earlier layer of the run, so it cannot be split. A face draws about
  40.
- **A published texture at t0-t2.** Read by its id when the view's client lists it, as the water's slots are.
- **Topology.** Fixed for every family (item 5).
- **Open, per draw:** a state with no ORG form (dual source blending needs RHI support), a texture that cannot be imported (a
  streamed texture without its full window, item 5), sampler modes with no graph sampler, a face's cloud layers out of order. Each is
  logged once as `[DCLF] sky: the engine draws: ...`.
- **Open, per batch:** a t17 that is an engine depth other than the Z-prepass copy or the depth target, a header that is a D3D11
  buffer, a cloud group holding a sky-object effect (`BSEffectShader`) or another geometry type. "A refusal leaves the rest of the
  batch to the engine" still holds for these: the batch would need the effect drawn by the effect family at that point.
- **The sun glare (`0x5C006072`)** is not drawn on the graph. It is outside the batch's technique range, and its ORG vertex stage does
  not build: `GetSunGlareVisibility` reads `SharedData` in the vertex stage (SkyDraws.cpp, sky.md:219). It needs a change in
  `Sky.hlsl`, which another lane is editing now. The plan lists it under ImageSpace too.
- **Cloud Shadows on D3D11** (no client): left, as asked. It goes away when ports are always on.

Census: the `Sky, descriptor ...` classes into `kMAIN` and the reflection cube. Trigger: `sky-org1.env`, `sky-river-org1.env`.

## 3. Water family

Commit: `fix(dclf): draw large sky and water batches in runs`.

- **More than 64 draws.** At the count `Interrupt` runs the latched part and a new run goes on.
- **Open: group 10's other-shader draws.** Group 10's batch (AE 106584 +0x143, SE 99939 +0x146) holds Lighting cross-fade copies
  beside the water's fade passes. The water's taker interrupts at them and lets the engine draw them, where DCLF's pipeline draws or
  vouches for them. Drawing them needs DCLF's Lighting pipeline (Core/Gpu/Pipelines, the handoff), which this lane does not own.
- **Open:** a non-main accumulator or cube face (the water has no face client), a header that is a D3D11 buffer (Light Limit Fix on
  D3D11: gone when its ORG path is always on).

Census: Water into `kRAW_WATER` and `kMAIN`. Trigger: Riverwood's river (`sky-river-org1.env`).

## 4. Shadow mask, blood splatter, precipitation

Commits: `fix(dclf): follow a new back buffer or shadow mask`, `fix(dclf): draw large mask and blood batches in runs`.

- **Blood after a swap chain recreate, and the shadow mask after kSHADOW_MASK is made again.** Both refused every batch for the rest
  of the session. Their imports are versioned slots: a new texture of the same format is the next version and needs nothing. A new
  format is a new slot. The client now takes it under a mutex (`Target()`, which the pass's `Declare` reads) and asks for a refresh.
  The import already asked every graph holding the old slot to be built again. The batches wait for that attach and log
  `the back buffer is another graph resource (a format change)` or `kSHADOW_MASK is another graph resource`.
- **More than 32 draws.** These now go on in a new run.
- **Open: precipitation outside the main view.** A cube face's draw and a depth target other than kMAIN's are refused
  (`PrecipitationCapture.cpp`). Rain in the water's reflection faces needs a face client: the reflection cubemap per face with
  `kCUBEMAP_REFLECTIONS`, as the sky's faces have.
- **Open (both):** a state with no ORG form, a texture that cannot be imported.

Trigger: blood with `blood-org1.env` (a hit), plus a resolution change in the menu. Shadow mask with `shadow-mask-org1.env` and the
inventory. Census: Utility into `kSHADOW_MASK`, BloodSplatter into the back buffer.

## 5. Menu scenes

Commits: `fix(dclf): set a draw's topology before the takers`, `fix(dclf): take menu draws with no PerGeometry`.

- **"its topology is not a triangle list, topology 0".** `DrawTriShape` (SE 75477 0x140d6bfe0, AE 77263 0x140e464d0, disassembled)
  sets the shape's VertexDesc (dirty bit 0x400) and topology 4 (dirty bit 0x800) in the shadow state before `SetDirtyStates`. The
  families' takers ran before that, so they read what the previous draw left: 0 before the first draw. The family detour now sets both
  first (`SetDrawState`), as its DrawData-copy detour already did. This fixes every family's static draws.
- **"a menu-depth draw: no vertex PerGeometry".** The check refused draws whose vertex shader declares no PerGeometry: a skinned depth
  draw takes its transforms from the bones. Now only a declared group (`VertexShader::constantBuffers[2].buffer`) that is not bound is
  refused.
- **Particles** were already latched as two-stream draws. The header comment saying otherwise is fixed.
- **Open: tree LOD (`DistantTree`) in the local map.** It has no ORG build in this family. The tree LOD family's program would have to
  be reused.
- **Open: "a texture it samples could not be imported ... a streamed texture without its full window yet".** `GpuTextures` refuses
  a streaming texture until its window covers every mip (Core/Gpu, texture streaming). The fix is there: import the window the
  engine samples now.

Trigger: the inventory and the map (`menus-org1.env`, `families-native.env`).

## 6. Occlusion queries

Commit: `fix(org): make occlusion query pools at setup`.

- **"cs.sky.sun: its anchor cannot run now" early in the session.** `QueryRing` made its first pool (lease indices 0-63) at setup and
  each later one at a commit. Making one returned true and the client called `FrameAnchors::Refresh`: a graph rebuild during play,
  with the sun's runs refused until it attached. The host's lease indices pass 64 early in a session. `Create` now makes
  `kSetupChunks` = 4 chunks (256 lease indices), which the client's first build declares. A chunk past them is still made at a commit
  and refreshes the client, but the host does not reach it in play. The decal occlusion family's ring gets the same.
- **The sun's client** is made at setup already (`SkyDraws::Setup`, from `Family::RunSetups` before setup's build point).
- **Open:** a query begun outside any bracket (the sun with no sky client: `CS_ORG_SKY` off or refused at setup) is D3D11's. In
  native mode it answers "hidden" (`NativeDevice.cpp`). Both engine users (the sun, AE 106604, and the decal test, AE 36562) are
  bracketed whenever their family has a client.

Look for: `[ORG] occlusion queries: cs.sky.sun: 1 queries a run, in pools of 64 runs; 256 lease indices made at setup`, no
`a sun occlusion query answers its last count` line, and no graph rebuild naming `cs.sky.sun` during play.

## 7. DCLF residue (5.5.6)

Not taken. None of these is in this lane's files.

- **Refraction normals** (Utility RENDER_NORMAL into `kREFRACTION_NORMALS`, 1-3 a frame): Lighting objects' refraction pass, which
  DCLF's refraction class (`CS_DCLF_REFRACTION`) does not claim. DCLF's census (`p5-all-census-dclf.env`) names the objects.
- **First-person parts in main depth and the sun's shadows behind a menu** (skinned Utility, vs `0x2004`/`0xc004`, through AE 70776):
  DCLF calls them Hidden, but the engine still draws them. RenderDepth's first-person block is not read yet. `Features/FirstPerson`
  covers only its own accumulator.
- **Passes through `FUN_1414b2810` (AE 106574) and `FUN_1414b2b90` (AE 106577, mode 0x19).** Neither function has a direct call.
  Both are stored in a table of registration callbacks at startup, in AE 105624 (0x14147e2c0). 106574 goes at 0x14332b0b0 and
  0x14332b0b8, from 0x14147ec0a. 106577 goes at 0x14332b0e8 and 0x14332b0f0, from 0x14147ec5e. The registration code picks the
  callback by render mode. The passes they insert land in a group renderer that no DCLF set holds. The step left: read which modes
  index those slots, then hold their group renderers in `PassCapture`.

## Rebuild triggers left in this lane

- Bink: HDR's UI texture becoming another graph resource (HDR turned on, or its textures made again).
- Blood and shadow mask: their target changing format.
- Query rings: a fifth chunk (more than 256 lease indices).

Everything else (a resize, a swap chain made again, a format change of Bink's target) is a new version of a slot, with no rebuild.

## Needs from other lanes

- FrameAnchors owner: delete the `MovieTiles` anchor (no reserver left).
- Sky.hlsl owner: a vertex-stage `SharedData` read for the sun glare's ORG build, so the sky family can latch technique 1.
- GpuTextures and texture streaming: import a streaming texture's current window, so families stop refusing streamed textures.
- DCLF pipelines: draw group 10's Lighting cross-fade copies, so the water family can latch through them.
