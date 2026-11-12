# P5.5: the screen blood on the render graph

Plan task P5.5 ([org-full-port.md](../org-full-port.md), Phase 5): the BSBloodSplatterShader family (the blood splattered on the
screen when the player is hit, and its light flare) drawn by ORG, in the sky's and the water's pattern ([sky.md](sky.md),
[water.md](water.md)). Reverse engineered with GhidraMCP on SE 1.5.97 and AE 1.6.1170 (2026-11-10). The code is
`src/Features/DrawcallLimitFix/Features/BloodSplatter/` and the switch is `CS_ORG_BLOOD_SPLATTER=1` (default off).

## RE

| Function | SE ID / addr | AE ID / addr |
| --- | --- | --- |
| BSBloodSplatterShader::SetupTechnique (slot 2) | 100265 / `0x1412d8bf0` | 106975 / `0x1414c18b0` |
| RestoreTechnique (slot 3) | 100263 / `0x1412d8b60` | |
| SetupGeometry (slot 6) | 100266 / `0x1412d8da0` | 106976 / `0x1414c1a60` |
| RestoreGeometry (slot 7) | 100267 / `0x1412d90d0` | |
| `Main::RenderScreenSplatter` (renamed in SE) | 100429 / `0x1412e4950` | 107147 / `0x1414ce740` |
| `BSShaderAccumulator::FinishAccumulatingScreenSplatter` (renamed in SE) | 99956 / `0x1412cca30` | 106601 / `0x1414b4b00` |

SE vtable `0x14185eea8`; slots 4 and 5 are the base (empty) SetupMaterial and RestoreMaterial. The AE renames did not take.

**Techniques.** Pass `0x5C006073` is the flare (descriptor 1, CS's `FLARE`), `0x5C006074` the splatter (descriptor 0, `SPLATTER`).
SetupTechnique: the splatter blends with mode 4 (DEST_COLOR, ZERO: the blood multiplies the frame); the flare with mode 5 (ONE, ONE)
and binds t3 = the HDR source (`DAT_143028f00[DAT_143232d58]`, a render target's SRV, or the default texture `DAT_14302c900`), CLAMP,
LINEAR. Both set depth mode 0 (no test, no write) and no stencil.

**SetupGeometry** maps VS and PS b2: `WorldViewProj` (c0-c3, the geometry's world times the camera's view-projection, transposed),
`LightLoc` (c4, `DAT_143232d60`), `Ctrl` (c5, `fBloodSplatterFlareOffsetScale` `DAT_143232d54`); PS `Alpha` (c0, the property's alpha
times its fade times `DAT_141e0e374`, and for the flare `DAT_141e0e370 * DAT_143232d50`). Textures: the splatter's t0 (colour,
property +0x88) and t1 (alpha, +0x90), CLAMP, LINEAR; the flare's t2 (+0x98).

**The draw.** `Main::Draw` (35560 `+0xA6A` / 36559 `+0xA73`) calls `Main::RenderScreenSplatter` after
`ImageSpaceManager::RenderWorldPostProcess` (the tonemap and the rest of the ImageSpace chain), when the ScreenSplatter singleton's
node has children. It sets the camera data to ScreenSplatter's camera, RT0 = **kFRAMEBUFFER** (the back buffer), and calls
FinishAccumulatingScreenSplatter on the splatter accumulator (SE `DAT_143233420`), which is `RenderBatches(0x5C006073 .. 0x5C006074,
flags 0)` at **SE 99956 `+0x1D` / AE 106601 `+0x21`**, then the first-person view's end. The draws reach `DrawTriShape` (75477 / 77263).

## The family

The pieces every engine draw family shares are `Core/Engine/Family` (render-graph.md, "Shared helpers for engine families").

- **The batch** (`BloodSplatterCapture.cpp`): a thunk on the RenderBatches call runs the batch with its draws latched; the family's
  taker at the shared DrawTriShape detour records each draw: VS b2 c0-c5 and PS b2 c0 through ConstantMirror (watched from the shared
  SetupGeometry watch), t0-t3 and their samplers (GpuTextures), the geometry (index range and stride checked against the buffers, a
  triangle list only) and the viewport. A draw that tests depth or stencil, or whose targets are not the back buffer alone, is refused.
- **Setup, withholding, running** (primary path, primary-path.md). The client is made at CS's setup (`Family::RunSetups`,
  before the setup's graph build), so it is in its anchor's graph before any world frame. A batch is taken when the client's targets match and `FrameAnchors::CanRun` says
  the anchor's run will draw it; otherwise the engine draws the batch, vouched for (`Flush::EngineDraws`), logged once. After the
  batch (or at a refusal) `Family::RunWithheld` runs the anchor; any status but `Ran` is an error, logged once, and nothing is
  retried. The client's `executed` callback counts the runs that drew.
- **The pass** (`BloodSplatterDraws.cpp`): the `cs.blood-splatter` client at the EngineDraws anchor (shared with the water), one
  RasterProgram draw per latched draw into the back buffer (FrameAnchors::EngineTarget(0)), no depth attachment, the engine's blend and
  rasterizer states (their descriptions), `BloodSplatter.hlsl` built for ORG off the render thread (`Family::EngineDrawPrograms`; a
  draw whose program is still building stays the engine's), a 176-byte latch region per draw; the push block is the latch's two words.
- **Refusals end the latched part.** There is no probe: every batch is latched from the first. A draw the family cannot take (its
  inherent refusals, "What stays the engine's") runs the draws latched so far at that point and leaves the rest of the batch to the
  engine, vouched for (`Flush::EngineDraws` until the batch returns), so the engine's draws come after the graph's in the engine's
  order. A pass the dispatcher draws through another function than DrawTriShape
  (`Family::DrawsThroughTriShape`) is refused at its SetupGeometry, before the engine draws it. A batch that ran nothing
  keeps its imports (`Keep`).

### Graph rebuilds

The client is made once, at the first main-thread tick (`RenderGraph::Engine::MainTick`, after the frame-job wait) the render graph
runs, before any blood is drawn, and asks for its build there (`RequestJoin`): the first hit does not rebuild the EngineDraws graph
mid-fight. The first batch after the client is in the built graph probes. Nothing registers again: a new back buffer (a swap chain made again) keeps the
screen blood the engine's for the session, logged once.

### Accepted differences

- **A withheld batch whose run then fails** (an epoch failure, after `CanRun` passed) is not drawn that frame.
- **The flare's HDR source** (t3) is a render target read through GpuTextures; if a later port moves its writer onto the graph, it must
  be read as a published resource instead.

## The switch

`CS_ORG_BLOOD_SPLATTER=1`; default off. It needs DCLF running. Registered in DCLF's list (`Features/FeatureList.cpp`,
`BloodSplatter::Feature()`, after Water); its status line is DCLF's `blood splatter` entry.

## Test plan

1. `blood-org0.env` and `blood-org1.env`: Riverwood, first person (`dclf:pov=first`), health raised, one melee bandit
   (`player.placeatme 1bcd8 1`: EncBandit01Melee1HNordM in Skyrim.esm), the splatters made to last (`setgs fBloodSplatterDuration`
   and `FadeStart` 1000). After about 8 s every actor's AI is turned off (`tai`) and the camera looks down at the ground, then two
   shots and a third-person one. No console command applies screen blood directly; hits taken in first person do, and their
   placement and count are the engine's random draws: compare the splatters' look, not their pixels.
2. The blood textures (`sBloodSplatterColor01OPTFilename`, `Alpha01`, `Flare01`) are streamed: GpuTextures pins every view it imports
   at its full window, so a probe that sees one before its full window is loaded refuses the draw (`a streamed texture without its
   full window yet`, with the slot, format and size) and a later batch takes it. Expect that line once per texture, then
   `every screen blood draw can be drawn by the graph` and `the first screen blood run drawn by the graph`.
3. Log: `[DCLF] blood splatter: on`, `the cs.blood-splatter client registered` (at startup, not at the first hit), `[DCLF] blood
   splatter: built the program for descriptor 0x0` (and 0x1 when a light flare shows), `every screen blood draw can be drawn by the
   graph`. No `the engine draws` after the first batches, no `a withheld batch was not drawn this frame`.
4. Shots: red splatters darkening the frame (multiply) in org1 as in org0; their placement varies by run.
5. Not verified without a build: everything above. The risk to look at first is the back buffer import under HDR output or frame
   generation (another swap chain image layout).

## Primary path (2026-11-11, `org-port/primary-families`)

Programs (all four families, `Core/Engine/Family/EngineDrawProgram.{h,cpp}`): the family's ORG stages are compiled into the shader
store when CS loads each of its shader's descriptors (`ShaderCompilationTask` -> `DrawcallLimitFix::PrecompileShader` ->
`Family::PrecompileStages`; before the render graph has a compiler they wait for `Family::StartStagePrecompile` at setup). A program
key is a value: the descriptor CS looks the shader up by (`State::modified*Descriptor`), the variant, the D3D11 descriptions of the
blend, depth-stencil and rasterizer states field by field (`Family::DescribeStates`), the target formats. Every key drawn is written
to `Data/ShaderCache/v2/engine-draw-programs.txt`, and the next start queues the listed keys on ProgramCache's build thread at setup
(`EngineDrawPrograms::Prebuild`). A draw whose prebuilt key is still building waits for it (`the first draw waited N ms for its
prebuilt program`); a key never seen is built on the render thread at its first draw and logged once (`was not prebuilt: built on the
render thread at its first draw`). Constants come from the engine constants capture (`Family::CaptureConstants`,
`CS_ORG_ENGINE_CONSTANTS=1`, which the families now require), so a buffer's first use has its bytes.

### What stays the engine's

- A back buffer other than the one the client imported (a swap chain made again): every batch, logged once.
- Per draw: not BSBloodSplatterShader, a depth or stencil test, targets other than the back buffer alone, a state with no
  description or no ORG form, geometry that is not a static triangle-list TriShape within its buffers, a slot holding a published
  texture, a texture that cannot be imported, more than 32 draws, an inactive engine constants capture.

### What can trigger a graph rebuild (primary path)

The client's registration at setup. Nothing in play registers again.

### What to look for in the log

- `[DCLF] <family>: the <id> client registered at setup (...)` once per client, at CS's setup, before `graphs built at CS's setup`, and no `frame anchors: cs.<family>.*
  registered` line after it; `; not published yet: <ids>` names inputs no producer had published by then.
- `[DCLF] engine draw programs: the manifest lists N keys`, `[DCLF] <family>: N programs from the manifest queued`, and on a second
  run no `was not prebuilt` line. Each such line names a key the enumeration or the manifest missed.
- `[DCLF] <family>: the engine draws: ...` lines name inherent refusals (once each); `[E]` `latched draws were not drawn this frame`
  and `the engine draws a batch: ... its anchor cannot run now` are errors.
- With the latch on, a batch the family did not take, or the rest of one, reaches D3D11 unvouched: an `[E] [ORG] flush latch: engine
  ... nobody vouched for it` line, counted by the residue census. The `<family>: left to the engine` reasons are deleted.
