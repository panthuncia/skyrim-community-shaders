# Physical Lens on the ORG post chain

Status, 2026-11-19: phase 1 (focus, image mapping, Composite inputs) is merged. Phases 2 and 3 (diffraction, ghosts) are
written on `org-native/port-lens-p23`, not built or run yet. The chain no longer holds for Physical Lens. Every lens shader
the D3D11 path compiles builds for ORG (SPIR-V and DXIL, checked with dxc and spirv-val), and its FXC bytecode is unchanged.

Upstream: #3171 (`00e99c218`), `src/Features/PostProcessing/PhysicalLens*.{h,cpp}`, shaders under
`features/Post Processing/Shaders/PostProcessing/PhysicalLensFlare/` and `.../PhysicalLensImaging/`.

## 1. What the D3D11 effect does

`PhysicalLens::Draw` writes nothing to the main image (`WritesToMainTexture` is false). Composite reads its side outputs.
Per frame, in order:

| Step | Work | When |
|---|---|---|
| Focus | `focus.cs` `UpdateFocus` (1 thread) into `Focus`, or DoF's focus texture; staging copy, mapped with `DO_NOT_WAIT` a few frames later | Cinematic Camera active |
| Lens model | CPU: `Configure` / `CameraInfinity` + `FocusCamera`, ghost paths (`BuildPath`), uploads `Paths` | preset, f-number, focus, zoom, coating or camera lens changed |
| Image mapping | `trace.cs` `BuildCameraMaps` 33x19x3 into `ChromaticMapping` (RGBA32F array of 3) | Image Effects on, and its inputs changed |
| Diffraction cache | 12 to 18 dispatches: `diffraction_detail.cs` pupil, FFT rows/cols, normalize, prefix, amplitude filter, tail prefix, core kernel, wave max and its 6-mip reduction | Diffraction on, and the pupil or sampling changed |
| Ghost wave cache | `ghost_wave.cs` material, then 11 samples of field, 4 transforms, accumulate; then GenerateMips of the atlas | Intensity and Ghost Edge Diffraction above zero, and the aperture changed |
| Extraction | `flare.cs` `Extract` (64x36), `PrepareRegions` (1), `ExtractLayers` (64x36), `SelectSources` (1) | ghosts or diffraction |
| Diffraction per frame | `diffraction_sources.cs` `BuildWaveSources`, `BuildWaveTiles` (writes indirect args), `diffraction_tail.cs` `BuildWaveTail` (indirect) | Diffraction on |
| Ghost jobs | `BuildJobs` (writes indirect args), plan status clear, `RequestPlans` (indirect), `BakeRequestedPlans` (indirect), `ExpandCachedJobs` (indirect), `TraceBeams`, `ShadeBeams` (indirect), `ReduceGhosts` (indirect) | Intensity above zero |
| Ghost raster | `beam.hlsl` VS + 3 PS, indexed indirect draws, additive, into 2 slices of `Ghosts` (R32F array), `BlueViolet` (RG32F) and 3 area layers x 3 bands | Intensity above zero |
| Resolve | `area.resolve.cs` `ResolveArea` into `Ghosts` (UAV) and the wave tile mask | Intensity above zero |

Composite (`HAS_PHYSICAL_LENS`) reads: t8 ghosts, t9 the mapping, t10-t14 diffraction core, regions, sources, tile mask,
tail radiance, and b4 `DiffractionParameters`. `Composite.HasPhysicalFlare`, `Composite.HasPhysicalLens` and
`DiffractionEnabled` pick its branches per frame.

## 2. Mapping onto the chain

One effect, `ORGPhysicalLens.cpp`, at the PhysicalLens slot (step `physical-lens`). It never writes the chain's current
image. Its outputs are persistent or history graph resources it owns, published by name for Composite.

### Constants

The lens's three large blocks do not fit the chain latch (16 KiB for every effect): `PhysicalLensOptics::Parameters`
(49 392 B), `PhysicalLensImaging::GPUParameters` (49 312 B). They also hold per-frame values (`BeamState`,
`CutoffRelease`, `ExposureDecode`). So the effect owns:

- one `org::LatchBlock` per run with every large block, written by Commit,
- one device-local buffer with a CBV per block,
- an upload pass that copies the run's region into those buffers, as `SharedConstants`' upload pass does.

Shaders read the blocks as `CBUFFER` entries through `ORG_CB(name)`. A LATCH entry would copy the whole struct into a
static, and `Surfaces[i]` is indexed dynamically, so that is never used for them. `optics.hlsli` copies each value field
of `LensParameters` into a static and reads `Surfaces` in place. The upload is gated with
the passes that read the block, so a frame that rebuilds no cache copies only what its per-frame passes read.

Small per-frame blocks (focus 32 B, diffraction 112 B, the ghost-wave stage 48 B) are chain latch regions.

The bare cbuffer field names stay: each file declares the block as a struct and `#define`s the bare names onto the
`CBUFFER` entry, on both targets, so the shader bodies do not change and the D3D11 layout is the same.

### Caches

Every cache (mapping, diffraction, ghost wave, planning table) is a persistent resource and a set of passes behind one
gate entry. Commit compares the inputs with what the last executed build made, opens the gate and records a pending
key; `Executed` adopts it, as `ORGPhysicalGlare` does. A cache's passes are in the build whenever the feature that needs
them is on, so a settings change never rebuilds the graph.

### Indirect work

Arguments buffers (`IndirectArguments`, `DiffractionDispatch`, `PlanningDispatch`) are persistent graph buffers. The D3D11
path seeds them with `UpdateSubresource` before the compute pass that fills them. On the graph the seed values go in the
effect's latch block and an upload copies them in, ordered before the writer by the graph like any copy.

Indirect dispatches use `ComputeProgram::DispatchIndirect` with the declared buffer (`IndirectArguments` use).

The ghost draws are indexed on D3D11 (`DrawIndexedInstancedIndirect`, 16-bit `BeamIndices`). The ORG build of `beam.hlsl`
pulls its index from a 32-bit structured buffer by `SV_VertexID`, and draws non-indexed through the program's draw
signature on the declared arguments buffer (`RasterProgram::DrawDesc::indirectBuffer`). The compute shaders write D3D11's
five-word indexed form. The non-indexed signature reads the first four at the same offset: count, instances, first index
read as first vertex, base vertex read as first instance. Checked: `BuildJobs` only adds to the instance word
(`InterlockedAdd` at 16, `InterlockedMax` at 68, 88, 108), and the seed's other trailing words are zero.

Kept recordings replay on runs whose commit did not open them. Direct dispatches then read zero counts from the run gate.
Indirect work reads GPU buffers, so one copy pass seeds all three argument buffers from the effect's latch block on every
run, with zeros on runs without that work. The planning status clear is an ORG-only gated dispatch
(`ClearPlanStatus`), not a UAV clear, for the same reason.

The ghost paths (768 paths, about 1.5 MB) are a staged buffer (`StagedBuffer` in the effect): each new lens model is a new
`org::VersionedBuffer` version, uploaded on the uploader's worker path (`PendingVersion`), adopted at the next frame
boundary after its copies complete (`Frame::Driver::AtNextBegin`, then `NoteNewVersions`). Until then the ghosts use the
current version, and the flare constants keep the model fields that version was built for (surfaces, path count, sensor
plane, focal length, pupil, coating). Nothing waits. The beam's index list goes the same way, once.

### Targets

`Ghosts` is an R32F array of 3. The raster pass renders slices 0 and 1 (RTV views with `slice`), `BlueViolet` and the
area layers as whole textures, additively (`RasterProgram::Target` blend One/One/Add), and resolve writes the array
through a full-array UAV (`UAVViewType::Texture2DArrayFull`). The anchor helpers have no array UAV, so the effect has
its own dispatch pass (`LensDispatch`) that declares SRV and UAV variants, CBVs, indirect arguments and buffers.

The ghost-wave atlas has mips made by `GenerateMips` on D3D11. On the graph it is `GraphPasses::MakeGenerateMips`, gated by
group counts in the chain's latch, written only when the cache is remade. Its stage words are push constants per
dispatch, and its aperture and surface a latch region.

## 3. The DoF focus on the graph

The CPU lens model depends on the focus distance, so the focus reaches the CPU. No pass waits for it:

- **Source.** With DoF in the chain, its own focus texture, `PostProcessing::DoF::Focus` (R32F, km, smoothed), which
  `ORGDoF` now publishes to the chain (`Published::kDoFFocus`). DoF runs before Physical Lens in chain order. Without
  DoF the effect runs `focus.cs` on its own 1x1 history texture. On the ORG build that shader reads the previous value
  from its own UAV texel, so no copy pass is needed.
- **To the CPU.** A readback pass (the `IReadbackService`, as Histogram Auto Exposure's adaptation) copies the texel on
  frames the camera is active. The callback stores it; the next Update takes it. The lens follows the focus a few frames
  late, as the D3D11 path does ("Lens focus follows by a few frames").
- **Reset.** As D3D11: a frame gap or a history reset starts the focus over at the camera's manual distance. The ORG
  shader gets the reset flag in its latch.

The CPU inputs (focus point, target, race menu) are computed by one function on both paths (`PhysicalLens::FocusInputs`).

## 4. Composite on ORG

The `#error` is gone. Under `HAS_PHYSICAL_LENS` the resource list adds, after the existing entries:
`TexPhysicalFlare`, `TexPhysicalLens`, `TexWaveKernel`, `TexWaveRegions`, `TexWaveSources`, `TexWaveTiles`,
`TexWaveTail` and the `Diffraction` latch (`EXTERN_LATCH`, declared by `diffraction_parameters.hlsli` with the
`ORG_GLOBAL_*` macros). The push block grows from 21 to 30 words. The SRVs moved from `diffraction_composite.hlsli` into
Composite's list, at the same registers, since only Composite includes that header.

`ORGComposite` builds the `HAS_PHYSICAL_LENS` permutation whenever Physical Lens published its outputs, binds the
published resources (unused ones as `Bind::Unused`; their branches are off), and reads the lens's diffraction region.
The per-frame flags come from the lens's Commit (`PhysicalLens::OrgComposite`), which runs before Composite's.

## 5. Order of work

1. Focus, lens model on the CPU, image mapping, Composite inputs. (Merged.)
2. Extraction front end (`flare.cs` Extract to SelectSources, with exposure inputs from the published auto exposure
   and local exposure), the diffraction cache, the per-frame diffraction passes, Composite's wave inputs. Dual-target
   `flare.cs.hlsl`, `exposure.hlsli`, `optics.hlsli`, `diffraction*.hlsl(i)`. `optics.hlsli`'s `LensParameters` becomes a
   `CBUFFER`. (Written, not run.)
3. Ghosts: jobs, planning (its table and status are persistent; the status clear is a gated dispatch), tracing, the
   ghost-wave cache, the raster pass, resolve. (Written, not run.)

The diffraction and ghost passes are in a build only while their setting is on, so toggling Diffraction Starburst or
Ghost Intensity between zero and above rebuilds the chain. Other settings never do.

## 6. Open questions

- **Push words.** `flare.cs.hlsl`, `planning.cs.hlsl` and `beam.trace.cs.hlsl` share one header that declares about 20
  resources, several at the same register under different types (u1 is `Selected` and `OutRays`). Counted: 15 views,
  `LensParameters`, the exposure latch (2 words), and 5 more in `planning.cs` make 23 words, under the 32-word limit,
  so one list per file works without splitting by entry point.
- **CBUFFER entries with indexed arrays.** Fixed in `Common/OrgResources.hlsli`. DXC's SPIR-V copied a ConstantBuffer held
  in a variable (static or heap-read value) into function memory once an array in it was indexed dynamically, which fails
  validation; even the cast form fails with arrays of structs. On Vulkan a `CBUFFER` is now an element of an array
  declared at the resource heap's binding, indexed in place; reads go through `ORG_CB(name)`. The binding numbers are
  copied from `rhi_shader_abi.h` and must stay in step with it.
- **The 16-bit index pull.** Settled: the effect uploads a 32-bit copy.
- **Ghost model lag.** A new lens model's ghosts show one or more frames after its constants, once the paths' upload
  completes. Not measured.
- **Push words.** The flare list is 23 words, beam.hlsl 16, ghost_wave.cs.hlsl 12, the diffraction tree 16,
  diffraction_sources.cs.hlsl 18.
- **The focus readback service.** Histogram Auto Exposure drops a readback when the service has no buffer. The lens
  then keeps its last focus; no fallback is needed, but a long run of drops would freeze the lens focus. Not seen yet.
- **Exposure inputs.** The flare reads Auto Exposure's adaptation (t5) and constants (b1). On the graph both are
  published (`kAdaptation`, `kAutoExposureConstants`); the constants are a latch region, so `exposure.hlsli`'s
  `ExposureParameters` becomes a LATCH entry. No new mechanism is needed.
