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

## Done

- **ImageSpace** (`org-port/primary-imagespace`, p5/imagespace.md "The primary path"): the learning and joining phases, the
  candidate list, cell-change joins and the Building fallback are gone. Shapes are value keys kept in a manifest beside the shader
  store and registered once at Setup; programs are queued at Setup and taken at the first graph build (`ProgramCache::GetNow`). A
  shape outside the manifest is refused for the session and registered from the next one: a graph change in play would need an
  asynchronous anchor build, which FrameAnchors does not have.
