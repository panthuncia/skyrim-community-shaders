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

## Proven ports

The ports that matched their D3D11 runs (org-full-port.md, Status): the frame skeleton, engine constants, the sky's main
view and faces, water, screen blood, shadow masks, ImageSpace's taken effects, Bink, Scaleform's graph draws, the
loading thread. Not yet proven, so they keep their current switches and logic until they are: the sky's cloud layers
(`CS_ORG_SKY_CLOUDS`), the menu 3D and map views (a probe only).

## The engine draw families (sky, water, screen blood, shadow masks: `org-port/primary-families`)

Done on 2026-11-11 (code in `src/Features/DrawcallLimitFix/Core/Engine/Family/` and `Features/{Sky,Water,BloodSplatter,ShadowMask}/`;
each family doc has a "Primary path" section with its own "What stays the engine's").

- **Stages at shader load (rule 1).** `ShaderCompilationTask` -> `DrawcallLimitFix::PrecompileShader` -> `Family::PrecompileStages`:
  every descriptor CS loads for BSSkyShader (pixel; plain for the faces' variant, `| 256` for the main view's), BSWaterShader
  (vertex), BSBloodSplatterShader (pixel) and BSUtilityShader (pixel, RenderShadowmask* bits only) compiles the family's two ORG stages
  into the shader store (`RasterProgram::CompileStages`, the same defines `Load` resolves). Requests before the render graph has a
  compiler wait for the families' setup (`Family::StartStagePrecompile`). This runs where CS precompiles its own shaders: with the disk
  cache on (the default); without it, CS compiles at first draw and so do the families.
- **Program keys are values (rule 2).** `Family::ProgramKey` holds the descriptor CS looks the shader up by (`State::modified*`), the
  variant, the target formats and the D3D11 descriptions of the blend, depth-stencil and rasterizer states field by field
  (`Family::DescribeStates` over `Pipeline::FindBlend/FindDepthStencil/FindRaster`). A program is built from its key alone. The
  manifest is `Data/ShaderCache/v2/engine-draw-programs.txt` (one line a key, family tag then the key's words; at most 512 keys a
  family; written on ProgramCache's thread when a key is first drawn). At setup each family queues the manifest's keys on
  ProgramCache's build thread (`EngineDrawPrograms::Prebuild`).
- **The gate.** The families' setup runs at the first main-thread tick the render graph runs (`Family::AddSetup` over `MainTick`: the
  main menu), so the prebuilds have the whole main menu and the load. A draw whose prebuilt key is still building waits for it on the
  render thread (logged once a family: `the first draw waited N ms for its prebuilt program`), never the engine. A key in neither the
  enumeration nor the manifest is built on the render thread at its first draw (`was not prebuilt: built on the render thread at its
  first draw in N ms`, a warning, once per key). `ProgramState::Building` is gone; `EngineDrawPrograms` no longer uses ProgramCache's
  map (it keeps its own, with the wait), only its build thread and `Load`.
- **Producers trusted (rule 3).** Every client registers at setup with its fixed published inputs, asks for its build once, and is
  in the graph from the menu frame's end on. The engine targets a family lists are imported at setup (`PublishedInputs::AddEngine`),
  which publishes them before that build. A batch is taken when `FrameAnchors::CanRun` says the run will draw (else the engine draws it,
  logged as an error once); there is no per-batch `RequestJoin`, probe, settle, clean flag or cloud backoff. A listed id first published
  during play is `InputResult::PublishedLate`: its producer's Publish rebuilds every graph (not the family), and until then that
  batch is the engine's, logged as an error. Constants come from the engine constants capture (`Family::CaptureConstants`; the
  families need `CS_ORG_ENGINE_CONSTANTS=1`), which tracks every buffer from startup; ConstantMirror is no longer used by them.
- **Refusals keep the engine's order.** A draw a family cannot take runs the latched part at that point (`FlushLatched`) and leaves
  the rest of the batch to the engine, vouched for under the flush latch (`Flush::EngineDraws`, reasons `sky: left to the engine`,
  `sky: cloud layers`, `water: left to the engine`, `shadow mask: left to the engine`, `blood splatter: left to the engine`). A pass
  whose geometry is not a TriShape is refused at its SetupGeometry, before its draw. A run that does not submit is an error, logged
  once; nothing is retried.

### D3D11 state reads

The families read the engine's slots (t0-t15) from the shadow state (`RSS.PSTexture`). `Family::ResolveHeader` still reads
`PSGetShaderResources` for its header registers, which are CS's (t16 and up: IBL, Dynamic Cubemaps, the fog, Light Limit Fix...): CS
binds them on DXVK's own context, which the latch never filters, so the read stays true under it (flush.md's readers table rates
`FrameRecords.cpp`'s identical read "fine" for the same reason). It refuses a register below 16. The water's directional shadow
record (t98) is now Deferred's CPU data (`Deferred::GetDirectionalShadowLightData`), not a buffer read back.

### What can trigger a graph rebuild

The families' client registrations at setup (one build at the main menu frame's end). Nothing during play: no client registers,
refreshes or imports again; a batch of other targets is an inherent refusal. A producer publishing an input for the first time during
play rebuilds every graph through its own Publish.
