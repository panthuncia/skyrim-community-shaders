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
