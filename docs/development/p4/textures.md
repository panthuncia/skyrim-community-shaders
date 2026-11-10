# P4.3: engine textures

RE results and the design for moving the engine's textures onto ORG ([org-full-port.md](../org-full-port.md) P4.3). The
code is `src/RenderGraph/Engine/Textures/`, behind `CS_ORG_ENGINE_TEXTURES=1`. Addresses were checked in Ghidra on SE 1.5.97
and AE 1.6.1170; the functions below are named there.

## The creators

### File textures (DDS)

| Step | SE ID / addr | AE ID / addr | What it does |
| --- | --- | --- | --- |
| BSTextureDB load | 101532 / 0x141328370 | 108531 / 0x141515810 | Opens a `BSResourceNiBinaryStream`, calls 75509 |
| `CreateTextureFromDDS` | 75509 / 0x140d6dd70 | 77301 / 0x140e484c0 | Calls 75721 with the device; sets block +0x20 = 1 (the refcount) |
| `LoadTextureFromStream` | 75721 / 0x140d7dc50 | 77533 / 0x140e59520 | Reads the whole file into a heap buffer, checks the header (`DDS `, size 0x7C, pixel format size 0x20, DX10 when four-cc says so), copies the 0x7C header out, calls the header function |
| `CreateTextureFromDDSHeader` | 0x140d7e330 (no ID resolved) | 0x140e59c00 | Works out dimension, mips, array size and cube from the header; expands 24-bit RGB to 32-bit; builds the subresource table; calls 75724, a second time with fewer mips if the first fails |
| `CreateD3DResources` | 75724 / 0x140d7dff0 | 77538 / 0x140e598c0 | `CreateTexture1D/2D/3D` (device slots 0x20/0x28/0x30) with the initial data, then `CreateShaderResourceView` (slot 0x38) |

`CreateD3DResources(device, dimension, width, height, depth, mips, arraySize, format, isCube, initialData, out)`:

- `dimension` is the D3D11 resource dimension: 2 (1D), 3 (2D, cubes too), 4 (3D). Anything else returns `E_INVALIDARG`.
- The description is always `USAGE_DEFAULT`, `BIND_SHADER_RESOURCE`, no CPU access, one sample; `MISC_TEXTURECUBE` when
  `isCube`. `arraySize` already counts six faces per cube.
- The view: 2D below 2 slices, 2D array above; cube below 7 slices, cube array (`arraySize / 6`) above; 3D for 3D; 1D or 1D
  array for 1D. Always mip 0 and every mip.
- The block (`NiMemAlloc(0x28)`, zeroed) gets the texture at +0x00 and the view at +0x10. Nothing else; 75509 sets the
  refcount after.

Threads: the loader runs on BSResource's loader threads for queued loads and on the requesting thread for blocking ones
(R6.6). Nothing takes `Renderer::Lock`; the engine relies on the free-threaded device.

### Other textures

| Function | SE ID / addr | AE ID / addr | What it does |
| --- | --- | --- | --- |
| `Renderer::CreateTexture2D` | 75511 / 0x140d6df00 | 77303 | `(renderer, width, height, data, usage, format, uav)`. One mip, row pitch `bits(format) / 8 * width`. Usage 0 DEFAULT (bind SRV, plus UAV when `uav`), 2 DYNAMIC (CPU write), 3 STAGING (no bind, CPU read and write, no view). Block: texture +0x00, view +0x10, height +0x18, width +0x1A, 1 at +0x1C, the format's low byte at +0x1D, refcount 1 at +0x20 |
| `Renderer::CreateTexture1D` | 75513 / 0x140d6e2d0 | not resolved | `(renderer, width, data, usage, format)`. A 0x20-byte block. Its one caller, 0x140d6dd50, passes usage 0 |
| `Renderer::ReleaseTexture` | 75527 / 0x140d6ec70 | 77322 / 0x140e49990 | Atomic decrement of +0x20; at the last one, `Release` of +0x10, +0x00, +0x08, then frees the 0x28 block. 1.7 releases each object twice (`TextureDoubleReleaseFix`) |

Callers of 75511 on SE:

| Caller | Usage | What |
| --- | --- | --- |
| 0x140d7c4e0 | DEFAULT, data | The renderer's default textures (8x8, 16x16, 1x1) |
| 0x140515660, 0x14052efa0 | DEFAULT, data | `BSScaleformExternalTexture` (menu images) |
| 0x1405841d0, 0x140584300 | DEFAULT, data | RGBA8 textures from CPU data |
| 0x1404bd210 | DEFAULT, UAV | An RGBA8 texture a compute shader writes |
| 0x1408a1620, 0x140d6ea30 | as passed | Generic wrappers |
| 0x1410a1870, `BSMovie::CreateTileTextures` (0x1410a1910) | DYNAMIC | Bink's planes, mapped every frame (75514/75515) |
| CS's `Upscaling` FaceGen copy | as passed | A copy target for `CopyResource` |

### Release and lifetime

A block's refcount is the engine's: the texture database, `NiSourceTexture` and the shader property each take one. The last
release frees the D3D11 objects with plain `Release` calls, from whatever thread drops it (cell unload on the main thread,
failed loads on loader threads). Nothing synchronizes it with the GPU; D3D11's deferred destruction covers it.

## How CS reads them today

- **DCLF** (`Core/Gpu/GpuTextures.cpp`): materials, grass, tree LOD, landscape seams and frame textures resolve the view a
  block holds (`rendererTexture->resourceView`) through `GpuTextures::Request` or `ResolveBinding`. The import marks the
  image stable in DXVK (`dxvkTrackInteropResources` or `dxvkGetInteropResourceInfo`), wraps it in BasicRHI and writes an
  ORG descriptor. Pinless imports follow DXVK's relocations.
- **Texture streaming** (`src/TextureStreaming`): hooks 75721. For a streamable file it returns a block of its own (a DXVK
  shell with no storage), so 75724 never runs for it. Windows are ordinary D3D11 textures made on its worker threads.
- **Census** (`TextureStreaming/Census.cpp`) also hooks 75721, only to count.
- **VRAM tagging** (`Vram/Debug.cpp`) names each file texture through a DXVK export.
- **Feature binds:** TruePBR, Terrain Helper and Skin keep engine views for their extended PS slots and bind them on the raw
  context; the landscape seam fix does the same. Most other features bind engine textures through the shadow state, so the
  engine's own context binds them.
- **Upscaling** calls 75511 for its FaceGen copy and `CopyResource`s into it on the raw context.
- **EngineFixes** `TextureDoubleReleaseFix` patches 77322 on 1.7.

## The design

One path, `Textures::Create`, for every texture this takes:

1. BasicRHI creates a device-local image (`CreateCommittedResource`). ORG owns it; the record keeps it.
2. DXVK wraps it as the D3D11 texture (`RenderGraphRuntime::WrapTexture`, `dxvkCreateTextureFromVkImageWithData`), with
   the contents uploaded by DXVK's initializer in its stream, where the plain wrap would zero the image. That is the same
   upload the engine's `CreateTexture2D` gets, so the texture is complete before any D3D11 draw or graph epoch can read it,
   from any thread, with no wait and no pass.
3. The graph sees it as an external texture in the layout DXVK rests it in (`ImportDxvkTexture`).
4. The D3D11 view is made as the engine would make it.
5. Once the tripwire's guest context is installed (`Tripwire::GuestContextInstalled`, P4.1), the engine's block gets
   tripwire handles wrapping the texture and view; before that, the D3D11 objects themselves.
6. `Tripwire::AttachGraphResource` (P4.1) ties the record to the D3D11 texture and view. When DXVK destroys them (on any
   thread) the record goes through `Tripwire::ServiceRetired` to `ReleaseAfterD3D11`, so the image outlives every D3D11
   command issued before. `Find` maps a view to its record through `Tripwire::GraphResourceOf` and a registry keyed by the
   graph texture, which the record leaves when it is freed.

Hooks: 75724/77538 (2D, cube, arrays and 3D; 1D stays the engine's) and 75511/77303 (DEFAULT only). 75721/77533 is hooked
only to name the texture after its file ("Engine::textures\\...").

Why not the graph's upload service: async epochs, which the anchors run, record only buffer uploads, a texture's first
D3D11 use can come before the next epoch, and loading screens run no anchors at all. An upload in DXVK's stream has none of
these gaps. The DXVK-free build will need the graph equivalent (`org-extension-api-notes.md`, "Texture creation and upload").

DCLF: `GpuTextures::Import` first asks `Textures::Find(view)`. For an engine texture it writes the descriptor from the
graph texture's image, with DXVK's swizzle, and keeps the record in the import's backing. Nothing goes to DXVK: no track, no
pin, no relocation, and a tripwire handle never reaches DXVK's import.

Texture streaming: unchanged. A streamed file still becomes a shell in 75721 and never reaches 75724. P4.4 can make its
windows through `Textures::Create`, which takes any 2D description with initial data.

## What stays the engine's

- 1D textures (DXVK's wrap takes 2D and 3D only).
- DYNAMIC and STAGING textures from 75511 (Bink's planes, readbacks). They need `Map`, which P5's Bink work replaces.
- Typeless formats, and formats BasicRHI has no equivalent of.
- Textures made before the render graph is active (the renderer's default textures at init are the likely ones), and all
  of them with a DXVK build that lacks `dxvkCreateTextureFromVkImageWithData`.

## Open points

- AE address of 75513 (1D) was not resolved; it is not hooked.
- Plugins and CS code that pass an engine view to the raw context or a DXVK export crash on a tripwire handle rather than
  log. TruePBR, Terrain Helper, Skin, the landscape seam fix, VRAM tagging and Upscaling's FaceGen copy unwrap now. A full
  audit of CS's raw-context binds of engine textures is still owed before handles are on by default.
- A thread-ID log on 75724 (R6.6's open question) would confirm which loads run on loader threads; the design does not
  depend on it.
