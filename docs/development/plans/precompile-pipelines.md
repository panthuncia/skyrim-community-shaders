# Precompile: shader stages and pipelines

Status: plan, 2026-11-23. Builds on `shader-precompile-proposal.md` (phases 2, 4 and 5 there).

## Goal

No DCLF shader stage or pipeline is built during play. The startup precompile, which already holds the main menu
(`XSEPlugin.cpp`, `kDataLoaded`, while `ShaderCache::IsCompiling()`), gets two more steps:

1. **Stages.** Every SPIR-V or DXIL stage DCLF can ask for is compiled by DXC, or found in the store or a shipped pack.
2. **Pipelines.** Every pipeline those stages are used in is built and saved in the on-disk pipeline cache
   (`RenderGraph/PipelineCache`, `Data/ShaderCache/v3/pipelines-*.bin`).

Decided with the user:

- The pipeline keys come from a local manifest and a shipped one, expanded over likely feature combinations.
- The pipeline step blocks at the startup precompile, as the shader step does.
- Lowered SPIR-V is not persisted. Each boot lowers again during the pipeline step, behind the precompile screen.
- Shader bytecode ships as a read-only pack in the build, without debug info.
- A game-form source of keys comes later, ported from SARP. The key sources are an interface so it plugs in.

## Why

On the GTX 980 Ti's first launch each DCLF pipeline took about 2 s to build in the driver, and a load waited up to 32 s
for about 400 of them. On the 1070 Ti a warm launch still builds 340 to 500 pipelines per run, queued up to 9 s, and
objects wait for them (`[DCLF] admission latency`, "frames on: pipeline").

## What a pipeline needs

A DCLF pipeline is built on the build pump from:

- its key: `PipelineKey` (Lighting and program families: VS, PS, pass and runtime descriptors, raster flags, vertex
  layout), `ShadowPipelineKey`, `ForwardPipelineKey`, the draw families, refraction, tree LOD, grass;
- its stages, from `ShaderPrograms`;
- the engine's blend and raster state for the key's raster bits (`EngineState`, asked of the render thread today:
  `completions->stateRequests`);
- the target formats (`TargetFormats`), the front face (`frontCCW`) and the pipeline layouts (device objects).

Layouts, formats and the engine's states all exist once the renderer is up, which is before `kDataLoaded`. So the
pipeline step can run inside the startup gate.

## Design

### Key sources

```
struct PipelineKeySource { virtual void Collect(PipelineKeySet&) = 0; };
```

| Source | What it gives | When |
|---|---|---|
| Local manifest | Every key a session built, with its resolved inputs | Now |
| Shipped manifest | Keys gathered by cs-test tours per backend (`shader-gather.toml`) | Now |
| Combination expander | Likely neighbours of the keys above (see below) | Now |
| Engine technique lists | Not built: the engine loads vertex and pixel descriptors as separate lists, not pairs, and a key also needs the pass and runtime descriptors and raster flags, which only a drawn key has | - |
| Game forms | Pipelines the user's load order needs, from parsing its forms at startup (SARP's parser) | Later |

The set is deduplicated by key and capped per family, like the family manifest (`kMaxManifestKeys`).

Water keys (the deferred range family) are not listed: each range's engine depth-stencil state and target formats are read
on the render thread from the deferred pass and the range's registration, and no line records them yet.

### The manifest

- One text file per backend in the store's folder: `Data/ShaderCache/v3/dclf-pipelines-<backend>.txt`. A shipped copy sits
  in the read-only pack folder.
- A line is a family tag, then the key's words, then the resolved inputs: the engine state words and a target-format
  id. Then a pipeline can be built with no render thread involvement.
- Written when a key is first built, like `engine-draw-programs.txt` (`EngineDrawProgram.cpp`).
- Keys only, so it is driver independent and ships.

### The combination expander

Keys that commonly appear together, so a tour that missed one does not leave it to play:

- each raster variant the engine uses with the technique (two-sided, decal nudge, alpha-tested write mode);
- each shadow mode's canonical technique for every caster technique (`CanonicalShadowTechnique`);
- the program-family variants of a Lighting key (forward, decal, terrain groups);
- for each observed vertex layout of a technique, the technique's other observed pairs.

Every rule is a small function over a key, listed in one file, and each logs how many keys it added.

As built: rules read only listed keys and take every input of a new key from listed keys (state words by state bits,
targets, shadow rasterizer states by mode). They add at most 1.5 times the listed keys, each rule under its own share.
An expanded key joins the local manifest only when play asks for it. One that fails to build goes to
`dclf-pipelines-<backend>-unbuildable.txt` and is not offered again. `CS_DCLF_PIPELINE_EXPAND=0` turns the expander off.

### The startup step

After the CS precompile's tasks:

1. **Stages.** DCLF's own stage batch: the derived shadow techniques in closed form (proposal phase 2), then every stage
   the pipeline set needs. All through the store and the pack.
2. **Pipelines.** Every key in the set is built on the build pump at full width (nothing else runs yet). Each result
   goes into the lane's catalog, so play starts with it, and into the on-disk pipeline cache. The cache is saved at the
   end of the step.
3. The gate holds until both are done. The overlay shows "pipelines n / m". The skip key (background compilation) lets
   the rest finish in the background, with the load hold waiting as now.

A key whose build fails is logged and dropped from the local manifest.

### The shipped pack

- `Data/ShaderCache/pack/cs-<version>-<backend>-<vendor>.pack`: read-only zstd pack in the store's format, looked up after
  the user's store and checked by the same file hashes (proposal "Shipped pack and local store").
- `Data/ShaderCache/pack/dclf-pipelines-<backend>.txt` and `engine-draw-programs.txt`: shipped manifests.
- Release builds compile ORG and DCLF stages without debug info (proposal phase 3), or no pack matches.
- Built by `tools/build-shader-pack` from a cs-test gather run per backend.

## Phases

| Phase | Work | Check |
|---|---|---|
| 1 | Pipeline manifest: record keys with resolved inputs, read at startup. Key source interface with the local source | Manifest lists the run's keys; a second run reads them |
| 2 | Startup pipeline step in the gate: build every manifest key before the main menu, with progress and skip | Warm second run: 0 `on-demand pipeline build` lines after the first loading screen |
| 3 | DCLF stage closure: derived shadow techniques, stages for every manifest key | 0 `on-demand SPIR-V compile` lines |
| 4 | Combination expander (`PipelineExpander.cpp`); forward, refraction and draw family lines in the manifest | A tour after a gather of a different route still builds nothing in play |
| 5 | Shipped pack and manifests: store pack lookup, `build-shader-pack`, `shader-gather.toml`, no debug info in Release, packaging | Cold store with the pack: startup precompile under 30 s, nothing compiled |
| Later | Game-form key source from SARP | |

## Risks

- **Boot time.** Thousands of pipelines at about 2 s each in a cold driver is minutes on Maxwell. The skip key and the
  driver's own cache bound it to first boots. Report the step's time and count.
- **Memory.** Every prebuilt pipeline lives for the session. Count them and report the driver memory.
- **Stale keys.** A key from another build or a removed mod builds for nothing. Keys unused for N sessions leave the
  local manifest.
