# Native context: one model on both backends (WS1.1-1.3)

Lane `par-native-context`, branch `org-native/par-native-context`, base `f34fce25d`. BasicRHI branch `org-native/par-native-context`
(b6142d9). Plan: WS1 items 1-3 of the native parity plan.

The goal is one native model on Vulkan and D3D12:

- ORG's ledgers carry image state, with no home states;
- imports are exclusive;
- the native context records BasicRHI commands only;
- context work reaches the queue on one of two routes: a static graph pass where one exists (the skeleton's `TakeContext*`), else
  ORG's external client (`ExternalWork`) with BasicRHI commands recorded inside it.

## Commits

First part. Guest and the current defaults are unchanged: native Vulkan still uses the raw Vulkan context and home states.

1. BasicRHI `feat: plane-aware buffer-texture copies`, and the CS pointer bump.
2. `feat(native): one BasicRHI route for context work`.
3. `feat(native): close the BasicRHI context's gaps`.
4. `feat(native): D3D12 onto the ledgers`.
5. `feat(native): creation uploads on the two routes`.
6. This document.

Deletion commits. Merge them after testing the first part.

7. `refactor(native): delete CopyExternally`.
8. `refactor(native): BasicRHI-only context`. This deletes the raw Vulkan dialect and `CS_ORG_NATIVE_RHI_CONTEXT`.

## What changed

### One route for the context's work (`RunOp`, `Immediate::Route`)

This route is what D3D12 always uses. On Vulkan it needs `CS_ORG_NATIVE_RHI_CONTEXT=1` until commit 8.

Every context operation names its uses. A use is a resource, a subresource range, and the state its command needs. The operations
covered are copies, unmaps, updates, clears, mip generation, the present probe and the zeroing of wrapped textures.

Once the render graph is up, the operation goes through ORG's external client:

- **A tracked image** (`LedgerResourceOf`) is declared in that state. Its commands name the graph resource's own handle. The ledger
  moves the image from wherever it is, so the same code is right with home states on or off.
- **Every other resource** rests in Common. An exempt texture rests in the shader-read state and leaves it around the work. These
  resources move into their state and back inside the recorded commands.

Before the graph is up, the same commands run on the context's own lists.

The work signals the context's timeline, so `Map`, `Wait` and the queries cover both routes. Each slot's descriptors (RTV, DSV,
UAVs) serve both routes. A UAV clear's descriptor therefore stays valid until the client's work is done.

If the client refuses an operation once the graph is up, the operation is lost. `ExternalWork` logs it once per label.

### Gaps closed on the BasicRHI path (both backends)

- **Staging regions anywhere.** A placed footprint must start at an aligned offset. A region inside a staging subresource goes through
  a bounce buffer: the image region is copied to the bounce at offset 0, and the rows are copied between the bounce and the staging
  texture. Staging-to-staging copies of a region go row by row.
- **Depth staging copies.**
  - A depth-only image copies its one plane straight to and from the CPU's texels.
  - A depth-stencil staging texture has room for both planes after its texels (`SubresourceLayout::depthOffset/stencilOffset`).
    `RepackDepthStencil` packs D3D11's interleaved texels into the planes before a copy to an image. After a copy from an image,
    `Map` rebuilds the texels from the planes (`planesNewer`).
  - D24S8 that BasicRHI stores as D32S8 is converted between 24-bit UNORM and float. This happens on Vulkan devices without D24S8
    attachments (`DepthPlaneFloat`).
  - BasicRHI's buffer copies name the plane (`BufferTextureCopyFootprint::plane`).
- **UpdateSubresource of a depth-stencil image.** D3D12 used to refuse it. The data is now split into the two planes, and each plane is
  copied.
- **UAV clears past `kUavsPerSlot` mips.** A D3D11 UAV names one mip, and a texture has at most 15 mips (`D3D11_REQ_MIP_LEVELS`). A
  `static_assert` holds that, so the old warning path is gone.
- **GenerateMips.** BasicRHI has no blit, and the engine's textures have no UAVs, so the mip chain runs as a compute pass:
  - ORG's `GenerateMips.hlsl` builds the chain (`GraphPasses::LoadGenerateMips`, loaded in `Native::OnGraphReady`).
  - The chain is built in a per-texture scratch texture with UAVs. The scratch is UNORM for an sRGB texture, and the shader averages
    in linear space.
  - Mip 0 is copied into the scratch, the chain is built there, and mips 1 to N-1 are copied back.
  - The descriptors are in the slot's shader-visible heap.
  - When the graph tracks the texture, mip 0 is declared as a copy source and the other mips as copy destinations.
  - Refused, with one log line per reason: 1D and 3D textures, block-compressed formats, and formats without typed UAVs (B8G8R8X8).

### D3D12 on the ledgers

- `NativeHomeStatesOff()` and `NativeGraphMovesImages()` no longer exclude D3D12. `CS_ORG_NATIVE_HOME_STATES=0` and
  `CS_ORG_NATIVE_EXCLUSIVE_IMPORTS=1` mean the same on both backends:
  - no external home states;
  - an open external client;
  - no host home on graph-owned textures;
  - host-made images found through their graph import;
  - DCLF views made on the graph resource.
- D3D12's imports of images resting in Common were already exclusive, because its Common layout cannot be a render target, depth or
  UAV. That backend check stays in `ImportDxvkTexture`.
- `GraphResources::ExportTexture` takes one path for images resting in Common on both backends (`ImportDxvkTexture`).
- In-stream epochs stay off on D3D12. The in-stream block in `RenderGraphRuntime.cpp` belongs to `par-inline-exec`. It already turns
  in-stream off on D3D12, so this lane needs no change there.

### Creation uploads

These used to go through the one-shot lists.

- **A graph-owned texture's first contents** go through the client. Its `PixelBuffer` is declared as a copy destination, or in Common
  when there is nothing to copy. The ledger takes it out of Undefined, and no tracker is set by hand.
- **Any other new image** goes through the context and is waited for.
- Commit 8 deletes `OneShot`.

### Deletions

- **`CopyExternally` (commit 7).** Copies between tracked images are ordinary `RunOp` uses. In the raw dialect, until commit 8, they
  take `RouteRaw`.
- **The raw dialect (commit 8).** Commit 8 deletes:
  - `RhiContext()`;
  - `Immediate::Run`, `RunTracked` and the Vulkan entry points;
  - `RecordCopy`, `StagingRegion` and `ClearColor`;
  - the Vulkan clears, fill and blit chain;
  - `RouteRaw`;
  - the inline texture-update submission;
  - `CheckRawAccess`, which was dead;
  - `OneShot`;
  - the switch registry and cs-test schema rows of `CS_ORG_NATIVE_RHI_CONTEXT`.

  After commit 8 the context's own lists run only before the graph is up, so they no longer flush held regions. The regions invariant
  (`FlushRegions` before a submission) then lives in `ExternalWork::Submit` alone. The present and `Native::External` still submit on
  their own; other lanes replace them.

## What to test

Run each configuration with the quick scenario, and with `native-validation` or `native-d3d12-validation`.

| # | Backend | Switches | Expect |
|---|---|---|---|
| 1 | guest | defaults | unchanged |
| 2 | native Vulkan | defaults (first part only) | unchanged: raw context |
| 3 | native Vulkan | `CS_ORG_NATIVE_RHI_CONTEXT=1` | shots match run 2; mip chains built |
| 4 | native Vulkan | `CS_ORG_NATIVE_RHI_CONTEXT=1 CS_ORG_NATIVE_HOME_STATES=0 CS_ORG_NATIVE_EXCLUSIVE_IMPORTS=1 CS_ORG_NATIVE_IN_STREAM=1` | shots match; no layout errors in validation |
| 5 | native D3D12 | defaults | shots as before; GenerateMips no longer refused |
| 6 | native D3D12 | `CS_ORG_NATIVE_HOME_STATES=0 CS_ORG_NATIVE_EXCLUSIVE_IMPORTS=1` | home-states line says off; shots match run 5; no barrier errors from the debug layer or GPU-based validation |
| 7 | both | after commits 7-8, the settings of runs 4 and 6 | as above; native Vulkan defaults now use the BasicRHI route |

Also run:

- `CS_ORG_NATIVE_PRESENT_PROBE=1` with each setting. The probe's `mean RGB` lines should match: the probe reads back through a
  staging copy.
- `CS_ORG_NATIVE_OP_CENSUS=1`. On the BasicRHI route, work on tracked images counts as ExternalWork routes. Untracked work is
  counted and logged as before (see the log lines below).
- A loading screen and a save load in runs 4 and 6. Loading screens carry most of the uploads and copies. On Vulkan, home states off
  needed in-stream epochs to finish the first load. D3D12 has no in-stream epochs. If run 6 stalls on the first load, it waits on
  `par-inline-exec`'s primitive.

## Log lines

- `[ORG native] GenerateMips: the context builds mip chains with a compute shader (2D textures, arrays and cubes)`: at startup. It says
  `not for ...` when a program failed to load.
- `[ORG native] GenerateMips on "<name>": <reason>; its mips stay as they are (logged once)`: a refusal (see above).
- `[ORG native] <operation> on "<name>" runs outside the ledgers (...)`: work on a resource no graph tracks. Logged once per operation
  and resource, up to 64 lines. The wording changed from "the immediate context's own path".
- `[ORG] native home states: off (CS_ORG_NATIVE_HOME_STATES=0): ...`: now on D3D12 too. The old "D3D12 keeps them" text is gone.
- `[ORG] external work: ORG's external client is ready (open submissions ...)`: with home states off.
- `[ORG] external work: '<label>' could not be submitted: ...`: an operation was lost. This is a bug to report.
- `[ORG native] CopySubresourceRegion|CopyResource from "<a>" to "<b>" is not a copy native mode makes (logged once)`: a pair the
  route cannot copy. Examples: a buffer to an image, or staging textures with different texel sizes.

## Not verified (no build was made)

- That it compiles. That includes BasicRHI's D3D12 change, which uses `D3D12GetFormatPlaneCount` and `GetCopyableFootprints` in
  `DxBufferCopyLocation`.
- ORG's external client with an empty access list (untracked work), and with two ranges of one resource (a copy within one image).
  `BuildExternalPublication` supports both, but ORG has no test for either.
- GenerateMips on D3D12 and Vulkan when the context's own UAV heap is bound (`SetDescriptorHeaps` with no sampler heap) under
  `ComputeProgram`'s layout.
- The D3D12 footprint formats that `GetCopyableFootprints` gives for the D24S8 and D32S8 planes, and the D24-as-D32S8 conversion on
  AMD (RX 6600).
- Throughput: every context operation now goes through the client, which allows 64 submissions in flight. Loading screens make many
  uploads; watch for `ORG.ExternalClient.WaitForCapacity`.

## Left

- **`D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON` for render-target and depth textures: skipped.** Not every cross-queue use goes through
  a ledger in a form that layout allows:
  - ORG's own handoffs of an import whose home is Common record the next queue's barrier from layout Common. If BasicRHI mapped
    Common to `DIRECT_QUEUE_COMMON`, a compute-queue list would record a barrier out of `DIRECT_QUEUE_COMMON`, which only the direct
    queue may do. ORG would need to release such textures on the direct queue first.
  - BasicRHI cannot tell, at creation or at a barrier, that a texture is used only on direct lists.
  - The hand-copied present (`NativeDevice.cpp` `Present`) and the context's own lists before the graph is up name Common directly.

  It becomes safe when ORG emits direct-queue releases for those textures, and when the present goes through the graph tail (WS1.4).
- The present paths and `Native::External` still submit on their own (WS1.4 and WS1.5, other lanes).
- `LeaveShaderRead` for exempt textures, the `RawAccess` counters, and the switches themselves (WS1.6).
- GenerateMips of 3D and 1D textures, and of formats without typed UAVs.
