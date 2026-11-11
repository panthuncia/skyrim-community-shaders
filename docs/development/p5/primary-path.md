# The ports as the primary path

Phase 5's ports were built as optional replacements: each one takes an engine pass when its graph program is built, its
client has joined, its inputs are declared and its constants are captured, and otherwise leaves the pass to the engine.
That made each port safe to switch on before it was proven. It also means every port carries a readiness state machine,
and every readiness miss is an engine draw that reaches D3D11, so the flush latch (P5.13, flush.md) cannot be on.

From here, a port that is proven correct is the primary path while its switch is on. The engine's D3D11 path for that pass
runs only when the switch is off. DCLF's own deferral (objects not yet claimed, flush.md "DCLF's share") is unchanged.

## The rules

1. **Shaders are compiled at startup, into the shader store.** Each port lists its programs' shader permutations when the
   engine loads its shaders, the same way DCLF does (`ShaderCompilationTask` calls `DrawcallLimitFix::PrecompileShader` for
   every descriptor of the engine's shader files). A permutation the port can know only from a draw (a family variant, a
   format) is enumerated from the finite set the engine uses, not discovered.
2. **Pipelines are created before they are needed.** A program key is a value, never a D3D11 object's address: the
   engine's state objects are described by their state-table index or their description (RenderGraph::Engine::Pipeline).
   Keys are enumerated where the set is finite (ImageSpace's effects, Bink's formats, the skeleton's clears, Scaleform's
   draw states). Where it is not (an engine family's descriptor times its states), the keys a session used are written to
   a manifest next to the shader store, and the next session builds them at startup. A key that is neither enumerated nor
   in the manifest is built on the render thread at its first draw, from shaders already in the store, with BasicRHI's
   pipeline cache: one stall the first time it is ever seen, never an engine draw. It is logged, so missing enumerations
   are found.
3. **Producers are trusted.** A port registers its client at setup, with every input it can read declared, and is in the
   anchor's graph before the first frame that needs it. It does not probe, settle, poll `RequestJoin` or wait for a
   published input: the input's producer is a port that has the same rules. Constants come from the capture, which watches
   the buffers from startup.
4. **Inherent refusals stay, and are few.** A case the port does not handle (an unported effect, a blend state with no ORG
   form, a shape outside the port's scope) is listed by name in the port's doc, logged once, and left to the engine. These
   are coverage gaps to close before the latch, not readiness.
5. **A failure is a failure.** A graph run that did not execute, a device loss or a failed build is logged and handled as an
   error (the anchor's existing fail-stop), not retried as "probe again".
6. **Graph shape changes follow the rebuild rule.** Removing readiness must not add rebuilds: a shape that changes during
   play (a resize, a new viewport) is either part of the registered graph from the start or built asynchronously and
   switched in at the next snapshot (render-graph.md).

## What goes

From the 2026-11-11 survey of `org-port/p5`:

- The families' probe state: `clean`, `settled` and `kSettleBatches`, `refusedProbes`, `joining`, `kLastingBatches`,
  `NoteUnclean`, probe batches that keep their owners (shadow mask, water, sky, screen blood).
- The sky's cloud backoff (it exists for programs still building and textures not yet streamed).
- `ProgramState::Building` in `EngineDrawPrograms`, and every `ProgramCache` Building branch that leaves a draw to the engine
  (ImageSpace, Bink, Scaleform).
- ImageSpace's `Learning` and `Joining` phases, `kLearnFrames`, the candidate list, cell-change joins.
- `PublishedInputs::NotDeclared` (every listed id is declared in the first build).
- Per-batch `RequestJoin` polling and the `joined` flags (Bink, Scaleform, the skeleton, the families).
- The constant capture's "first sight draws on the engine".
- Bink's `missed` latch for the rest of a movie.

`RunGate` stays: it keeps recordings stable across runs and is not a readiness device.

## The UI lane (`org-port/primary-ui`)

Scaleform, Bink, the frame skeleton and the loading thread follow the rules above (scaleform.md, bink.md, skeleton.md and
loading-screen.md, each under "The primary path"). Shared pieces:

- **Setup's build point.** `Engine::PostSetup` runs from `State::Setup` after the features' `SetupResources`. It sets Scaleform up,
  then calls `FrameAnchors::BuildPending`, which builds every graph a registered client waits to join, between frames on the
  render owner. That is how rule 3's "in the anchor's graph before the first frame that needs it" is met, without `RequestJoin`.
- **`ProgramCache::Require`.** It returns the key's program, built on the calling thread when the build thread has not finished
  it (logged: `built on the render thread at its first use`). It is rule 2's last resort. `Get` stays for queueing at setup.
- **What is still a rebuild trigger:** a resize, or HDR rebuilding its UI texture. Scaleform's role surface, Bink's framebuffer
  and the skeleton's groups register once again, and the next build point between frames builds them. The work in between stays
  the engine's.

## Proven ports

The ports that matched their D3D11 runs (org-full-port.md, Status): the frame skeleton, engine constants, the sky's main
view and faces, water, screen blood, shadow masks, ImageSpace's taken effects, Bink, Scaleform's graph draws, the
loading thread. Not yet proven, so they keep their current switches and logic until they are: the sky's cloud layers
(`CS_ORG_SKY_CLOUDS`), the menu 3D and map views (a probe only).

## Done

- **ImageSpace** (`org-port/primary-imagespace`, p5/imagespace.md "The primary path"): the learning and joining phases, the
  candidate list, cell-change joins and the Building fallback are gone. Shapes are value keys kept in a manifest beside the shader
  store and registered once at Setup; programs are queued at Setup and taken at the first graph build (`ProgramCache::GetNow`). A
  shape outside the manifest is refused for the session and registered from the next one: a graph change in play would need an
  asynchronous anchor build, which FrameAnchors does not have.

## Phase 3 ports: programs at setup

The Phase 3 feature ports (`src/Features/*/ORG*.cpp`, not Drawcall Limit Fix) keep their whole-feature fallback at setup. Their
programs, audited 2026-11-11 (`org-port/primary-ui`):

- **Built at setup already:** Cloud Shadows, Dynamic Cubemaps (all three reflection variants), Exponential Height Fog (near and far,
  with and without group reduce), Grass Collision, HDR Display, IBL, LLF light culling and local shadows, Skylighting, Subsurface
  Scattering (all five), Terrain Blending, Terrain Shadows, Volumetric Shadows, Screenshot (no program).
- **Screen Space Shadows:** the raymarch's `SAMPLE_COUNT` follows the multiplier (1-4) and the render size. Setup builds every count
  of each multiplier at the full and the current dynamic size (up to 8). A count outside that set (a dynamic scale seen later) is
  built at first sight: `Screen Space Shadows: the N-sample program was not built at setup`. A count change still makes the
  passes again (one graph rebuild, on a settings or render-size change only).
- **Screen Space GI:** the current settings' programs are built at setup; every other configuration's (resolution mode x GI x
  specular x temporal: 24 define sets, 160 programs) is queued on the program build thread (`ProgramCache`). A settings change takes
  them from there, or builds the missing ones on the render thread: `Screen Space GI: ... was not prebuilt yet`. A settings change
  is a new chain registration, as before.
- **Effects11 rays:** the seven programs are built at setup for kMAIN's format (they were built at the first frame). Another
  format (kMAIN made again) is built at first sight: `Effects11 rays: ... was not built at setup`.
- **Upscaling:** kMAIN's copy, the depth upscale programs and the hudless copy for the three formats `CreateHudlessTexture` can
  choose are built at setup. A new engine target format is built at first sight and logged.
- **Post Processing:** its programs are built with the chain, synchronously, at the first Tonemap run and at a settings change that
  makes the chain again (a graph rebuild in itself). The chain's shape (kMAIN's size and formats, the effects' settings) is known
  only there, and enumerating every effect's permutations (Color Grading's tonemap function, DoF's ring count, Lens Flare's and
  Physical Glare's FFT sizes) is not done. A program first needed after the first chain is logged: `Post Processing: ... is new to
  this session`. The frame never runs on D3D11 because a program is building.

Shader reloads from the menu (`ClearShaderCache`, `recompileFlag`) build programs again on the render thread, as their D3D11 paths
do: a user action, not a readiness state.

## The engine draw families (sky, water, screen blood, shadow masks: `org-port/primary-families`)

Done on 2026-11-11 (code in `src/Features/DrawcallLimitFix/Core/Engine/Family/` and `Features/{Sky,Water,BloodSplatter,ShadowMask}/`;
each family doc has a "Primary path" section with its own "What stays the engine's").

- **Stages at shader load (rule 1).** `BSShader::LoadShaders` (Hooks.cpp, every descriptor the engine loaded, in CS's lookup form,
  whatever CS's own precompile does) -> `DrawcallLimitFix::PrecompileFamilyStages`, and `ShaderCompilationTask` ->
  `DrawcallLimitFix::PrecompileShader`, both -> `Family::PrecompileStages` (each stage once, compiled on CS's compile pool): every
  descriptor for BSSkyShader (pixel; plain for the faces' variant, `| 256` for the main view's), BSWaterShader
  (vertex), BSBloodSplatterShader (pixel) and BSUtilityShader (pixel, RenderShadowmask* bits only) compiles the family's two ORG stages
  into the shader store (`RasterProgram::CompileStages`, the same defines `Load` resolves). Requests before the render graph has a
  compiler wait for the families' setup (`Family::StartStagePrecompile`). The first run of 2026-11-11 had CS's background compilation
  on, so its `ShaderCompilationTask`s for the sky and the water came only at their first draws: no shader object was known at setup and
  the manifest's sky and water keys were all built on the render thread (`no descriptor of its shader was loaded`). The LoadShaders
  enumeration fixes that; the shader object also comes from it.
- **Program keys are values (rule 2).** `Family::ProgramKey` holds the descriptor CS looks the shader up by (`State::modified*`), the
  variant, the target formats and the D3D11 descriptions of the blend, depth-stencil and rasterizer states field by field
  (`Family::DescribeStates` over `Pipeline::FindBlend/FindDepthStencil/FindRaster`). A program is built from its key alone. The
  manifest is `Data/ShaderCache/v2/engine-draw-programs.txt` (one line a key, family tag then the key's words; at most 512 keys a
  family; written on ProgramCache's thread when a key is first drawn). At setup each family asks for the manifest's keys
  (`EngineDrawPrograms::Prebuild`), which are queued on ProgramCache's build thread as soon as the family's shader object is known
  (setup, the LoadShaders enumeration, or at the latest the first draw). The `not prebuilt` line carries the key's hash: one descriptor
  has several keys when its draws differ in state (a face's sky with and without its depth, say).
- **The gate.** The families' setup runs at CS's setup (`Family::RunSetups` from `DrawcallLimitFix::PostSetupResources`, after every
  feature's SetupResources and before `RenderGraph::Engine::PostSetup`'s `FrameAnchors::BuildPending`), so their clients are in the
  setup's graph build and the prebuilds have the whole main menu and the load. (The first version registered at the first MainTick,
  after that build: the first world batches logged `its anchor cannot run now`.) MainTick only retries a setup that found no host. A draw whose prebuilt key is still building waits for it on the
  render thread (logged once a family: `the first draw waited N ms for its prebuilt program`), never the engine. A key in neither the
  enumeration nor the manifest is built on the render thread at its first draw (`was not prebuilt: built on the render thread at its
  first draw in N ms`, a warning, once per key). `ProgramState::Building` is gone; `EngineDrawPrograms` no longer uses ProgramCache's
  map (it keeps its own, with the wait), only its build thread and `Load`.
- **Producers trusted (rule 3).** Every client registers at CS's setup with its fixed published inputs and is in the setup's graph build. The engine targets a family lists are imported at setup (`PublishedInputs::AddEngine`),
  which publishes them before that build. A batch is taken when `FrameAnchors::CanRun` says the run will draw (else the engine draws it,
  logged as an error once); there is no per-batch `RequestJoin`, probe, settle, clean flag or cloud backoff. A listed id first published
  during play is `InputResult::PublishedLate`: its producer's Publish rebuilds every graph (not the family), and until then that
  batch is the engine's, logged as an error. Constants come from the engine constants capture (`Family::CaptureConstants`; the
  families need `CS_ORG_ENGINE_CONSTANTS=1`), which tracks every buffer from startup; ConstantMirror is no longer used by them.
- **Refusals keep the engine's order.** A draw a family cannot take runs the latched part at that point (`FlushLatched`) and leaves
  the rest of the batch to the engine, vouched for under the flush latch (`Flush::EngineDraws`, reasons `sky: left to the engine`,
  `sky: cloud layers`, `sky: sun occlusion query`, `water: left to the engine`, `shadow mask: left to the engine`, `blood splatter: left to the engine`). A pass the
  geometry-type dispatcher draws through another function (`Family::DrawsThroughTriShape`: types 3, 5, 8 and 9, and 6 and 7 unless
  the pass's accumulation hint is 12, from Ghidra of AE 107637) is refused at its SetupGeometry, before its draw. (The first version
  took TriShape alone and refused the water's sub-index land shapes, type 9 at the dispatcher's +0x430/+0x4CE: 12 water draws a frame.) A run that does not submit is an error, logged
  once; nothing is retried.

### D3D11 state reads

The families read the engine's slots (t0-t15) from the shadow state (`RSS.PSTexture`). `Family::ResolveHeader` still reads
`PSGetShaderResources` for its header registers, which are CS's (t16 and up: IBL, Dynamic Cubemaps, the fog, Light Limit Fix...): CS
binds them on DXVK's own context, which the latch never filters, so the read stays true under it (flush.md's readers table rates
`FrameRecords.cpp`'s identical read "fine" for the same reason). It refuses a register below 16. The water's directional shadow
record (t98) is now Deferred's CPU data (`Deferred::GetDirectionalShadowLightData`), not a buffer read back.

### What can trigger a graph rebuild

The families' client registrations at CS's setup (in its one graph build). Nothing during play: no client registers,
refreshes or imports again; a batch of other targets is an inherent refusal. A producer publishing an input for the first time during
play rebuilds every graph through its own Publish.

### Stage filters and the first world frame (2026-11-11, `org-port/primary-families-3`)

- The LoadShaders enumeration compiles only what a family draws. Shadow masks: Utility pixel descriptors with a RenderShadowmask*
  bit and RenderDepth, as Utility.hlsl's ORG build requires (legacy layouts set bits 21-24 for other techniques). Sky: every
  technique but the sun glare (raw 1, pass `0x5C006072`, outside the batch), whose ORG vertex stage reads SharedData next to the
  vertex pull and makes DXC 1.9 emit invalid SPIR-V (a `ConstantBuffer<T>` in Function storage, VUID-10684; reproduced with
  `check-dual-target-shaders.py Sky.hlsl --stage vs -D VSHADER -D TEX -D DITHER`; without the pull it builds). A precompile's
  failure is a warning (`RasterProgram::CompileStages`); the family's line names the descriptor.
- The first world batches found their anchors unable to run (`its anchor cannot run now`) once in run
  `1111-153238-p35-p5-all-census-misc-fix2`, although the clients were in the setup's build. At the first world frame the engine makes
  targets for the first time (the volumetric lighting's volumes, Create3DRenderTarget), and EngineTargets bumped the global resize
  generation for each; `FrameAnchors::Service` then marks every client current and asks every anchor to build again (`graph
  resources resized (x2)`), so every client was outside its built graph until the frame's end. Fixed on `org-port/primary-families-4`
  (EngineTargets.cpp): an engine texture notes when a reader is handed its image (`Serve`: `Find`, `FindImage`), and only the
  replacement of such an image bumps the generation (`Move`, logged as `[ORG] engine targets: <name> made again while a reader held
  its old image; the anchors build again`). A first creation, or one no reader asked for yet, only publishes (a registered id again).
  Sets (EndSet), TakeOver, Recreate and Restore follow the same rule; a target the engine makes alone outside a set now moves too
  (before, replacing a held one there bumped nothing and left graphs on the old image).
- Not narrowed: when a held image is replaced, every anchor still builds again and every client waits for it. The image is
  published under its id, and PublishedResources registers every id in every graph, so a replaced id rebuilds every graph anyway;
  and readers declare engine imports by pointer (`FrameAnchors::EngineTarget`), which ORG does not track by graph
  (`GraphHost::RequestRebuildFor` covers names looked up). Scoping it needs ORG to record pointer-declared resources per graph.

### Streamed textures and the latch's vouch (2026-11-11, `org-port/primary-families-5`)

- Run `1111-161326-p35-latch-census-vouch`: a water surface draw (Water 0x35e into kRAW_WATER) was refused once for `a streamed texture
  without its full window yet`, then reached D3D11 every frame. Two causes:
  - The texture never got its full window. `TextureStreaming::PinFull` asked for it once; the controller's later requests (a smaller
    step, a release under pressure) are later revisions that replace that ask, and a pinned texture's upgrade back to full counted as
    off screen, so the allowance skipped it. PinFull now asks again every second while the window is missing, and the controller counts a
    pinned texture as on screen (Streamer.cpp). The families ask on every batch (`GpuTextures::ResolveBinding` calls PinFull), so the
    draw returns to the graph once the window is resident.
  - The refused batch's rest was not vouched for under the latch. It was an `EngineDraws` scope opened at the refusal and closed after
    the batch, but the latch's `RenderPassImmediately` detour, for a pass DCLF vouched for (`dclf-vouch`), restores the reason it saw at
    the pass's start when the pass ends, which ended the family's scope for every later pass. `Family::EngineRest` now uses
    `Flush::BeginEngineRest` / `EndEngineRest`, a thread-local reason outside the scopes' nesting (an open scope's reason counts first).
