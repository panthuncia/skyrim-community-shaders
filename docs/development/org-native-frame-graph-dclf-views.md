# DCLF shadow and view epochs: resource survey

Phase 1 of the native frame graph plan (to be written up in `org-native-frame-graph.md`) asks for every resource DCLF's shadow and per-view epochs use,
how each is obtained, and whether the passes that use it declare it. This is that survey, taken on `test/refs-sweep` (7805d529a).
Paths are under `src/Features/DrawcallLimitFix/`.

"Declared" means a pass declares the resource. "View-less" means it is declared without a view (`std::span<const org::SrvView>{}`),
which orders it but gives no descriptor: the shader reads it by device address or by a BufferRef (BasicRHI's own view pair from
`Device::GetBufferViewRef`, not an ORG view). A BufferRef cannot come from a declared view token, because ORG has no buffer view ref
view kind.

## Shadow epoch (GPU-driven)

Passes: `Features/Shadows/Engine/ShadowEpochPasses.cpp` (caster rows, build draws, shadow views). Registration:
`Core/Render/Passes/Shadow.cpp`. Resources: `Core/Render/Resources/Resources.cpp`.

| Resource | How it is obtained | Declared |
|---|---|---|
| Shadow and occlusion maps (`cs.dclf.shadow.depth<i>`) | Engine depth targets, imported with `ImportImage` after `DescribeResource` (`ShadowEngine::ImportDepthTexture`); the EngineTargets texture when ORG owns the target | Yes, `DepthReadWrite` per slice |
| `cs.dclf.shadow.constants`, `view-blocks` | DCLF device buffers, filled by latched copies | View-less; shaders read them by the addresses in each view's push words |
| Caster rows, caster firsts, rows used | DCLF buffers (versioned) | Yes, UAV/SRV views; indices from the tokens |
| Draw arguments, draw counts, bucket counts (per slot) | DCLF buffers | Yes, UAV in the build, indirect arguments in the views pass |
| Shadow visibility | DCLF buffer | Yes, UAV |
| Index pool firsts and indices | Scene buffers | Yes, SRV view and index buffer |
| Vertex stream | Scene buffer | View-less, read by address |
| Fade roots, object gates | Cull binding buffers | Yes, SRV views |
| Light contexts, strict gates | Cull binding publications | Yes (`DeclareCullBindingPublications`) |
| Placements, shading, pools | FrameValues | Yes (`FrameValues::DeclareReads`) |
| Root states (last frame's) | Named in the latch | No. The code says so: "undeclared as in the depth segment" |
| Installed payload: scene list, geometry table, shadow rows | Addresses written into the latch | No |
| Latch arenas (`cs.dclf.shadow.latch`, `.zeros`) | ORG `LatchBlock`, written by the CPU per run | No. Read through `at.srvIndex` and used as the `ExecuteIndirect` argument buffer |

## View epochs (reflection faces, first person)

Passes: `Core/Render/Epochs/ViewEpochRecord.cpp`. Commit: `Core/Render/Epochs/ViewEpoch.cpp`. Registration: `ViewExtension` in
`ViewEpochRecord.cpp`.

| Resource | How it is obtained | Declared |
|---|---|---|
| Main visibility, scene buffers | The main resources, registered again under the same ids | Yes |
| View sequences, draw count, bucket counts | DCLF buffers per view | Yes (UAV in the build; indirect arguments and view-less by address in the draw) |
| View blocks | DCLF buffer per view | View-less, by address |
| Frame constants (the view's own, or the main ones for first person) | DCLF buffer | View-less; the frame record holds its BufferRef |
| Frame buffer copies (`frame-buffer.t<n>`) | DCLF buffers per view | View-less, but the frame record names them by `copy->GetSRVInfo(0).slot.index`: a raw index after a view-less declare |
| Frame buffer contents | `BoundPixelBuffer` (the D3D11 PS slot) and `ConstantMirror`, at commit | A D3D11 state read; stale under the flush latch |
| View depth (reflection) | Its own `PixelBuffer` | Yes, DSV |
| View target | The engine's reflection cube, or `kPOST_ZPREPASS_COPY` for first person | Yes, RTV per face or DSV |
| Face rows and lists (tree LOD) | DCLF buffers per face | Yes, by the family's `declare` (`TreeLodFaces.cpp`) |
| Faces' graph textures | Published ids (`FacesGraphTextureIds`) | Yes (`DeclareGraphTextures`) |
| Latch arenas | As in the shadow epoch | No |

## CPU pipeline sites (`Core/Render/Direct/`)

The CPU pipeline records each site into DXVK's command buffer through `EmitExternalCommands`, with a full memory barrier before
it (`Sites.cpp`, `SiteCallback`). The site passes (`SitePasses.cpp`) declare their targets so the graph orders them.

| Resource | Site | How it is obtained | Declared |
|---|---|---|---|
| Main frame constants | Depth, colour, forward, first person | DCLF buffer; BufferRef and address in the push words | Yes since `b4c61a75a` (view-less) |
| Foliage record (owners; ids and colours) | Depth; colour | DCLF buffers; UAV index and BufferRefs in the frame record | Yes since `b4c61a75a` (view-less) |
| Shadow constants, view blocks | Shadow | Addresses in `ShadowFrameView::push` | No |
| Index pool indices | All | The adopted version's raw handle | Only in GPU-driven graphs (`DeclareMainReads`) |
| Vertex stream, frame buffers | Forward, first person | As the main pass | Only in GPU-driven graphs |
| Hosted views' targets and depth (reflection faces, first person depth) | Depth | `ViewSites.cpp`: raw handles, own RTV/DSV heaps | No: the depth site declares only the main depth |
| Hosted views' frame constants and blocks | Depth | The view's push words | No |
| Site RTV/DSV descriptors | All | Own descriptor heaps over the declared targets' raw handles (`Sites.cpp`, `ShadowSite.cpp`, `ViewSites.cpp`) | The targets are declared; the views are not the graph's |
| CPU indirect draws (`cs.dclf.cpu.indirect-draws`) | All (Indirect mode) | Host-visible buffer per frame slot, written by the CPU | No; not a graph resource |

## Shadow features (`Features/Shadows/`)

`Sun/`, `Local/` and `Engine/ShadowCasterPoints.cpp`, `ShadowEpochPoints.cpp` are CPU-side: engine hooks, accumulation, light
culling and exclusion. They own no GPU resources. `Engine/ShadowEpochPasses.cpp` is covered above.

## What this means for native mode

- The GPU-driven shadow and view epochs declare nearly everything. What they leave undeclared is the latch arenas (host-written
  per run) and the payload buffers named by address in the latch. Phase 2 has to decide whether those are exempt or tracked.
- BufferRef reads (frame constants, blocks, foliage ids and colours) are declared but view-less. Taking their indices from a
  declared view token needs an ORG view kind for BasicRHI buffer view refs.
- Raw indices after a view-less declare remain for the view frame buffer copies (`GetSRVInfo`) and the foliage owners
  (`GetUAVShaderVisibleInfo`).
- The view epoch fills its frame buffer copies from D3D11 PS slots (`BoundPixelBuffer`), which goes stale under the flush latch.
- The CPU pipeline's shadow site and the depth site's hosted views rely on the external epoch's full barrier. When Phase 4 records
  these sites inside pass command lists, their shadow constants, view blocks, index pool and hosted targets must be declared.
