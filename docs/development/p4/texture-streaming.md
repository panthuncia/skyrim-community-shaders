# P4.4 and P4.7: texture streaming and VRAM tooling on ORG

Plan rows P4.4 and P4.7 of [org-full-port.md](../org-full-port.md). The streamer's design is in
[texture-streaming.md](../texture-streaming.md); this file covers what changes when its windows are ORG textures, what the
engine side needs, and where the VRAM tooling reads ORG's allocator.

Both are behind switches, and the DXVK paths stay the default:

| Switch | What it does |
| --- | --- |
| `CS_ORG_TEXTURE_STREAMING=1` | Scheduled window builds make ORG textures, uploaded by a copy pass at the Present anchor. |
| `CS_ORG_VRAM_SOURCE=org` | VRAM attribution and the budget controller read BasicRHI's memory budget and ORG's allocator. |

Graph windows imply the ORG reading for the controller even without the second switch, because their memory is in ORG's
allocator.

## What the engine does (RE, SE 1.5.97, confirmed in Ghidra)

-   **The loader the streamer hooks** is ID 75721 (SE `0x140d7dc50`, renamed `BSGraphics::Renderer::LoadTextureFromStream`).
    Its arguments are the device, the `NiBinaryStream`, the `BSGraphics::Texture**` out, a 0x7C-byte header out and two
    flags. It reads the whole stream into a buffer, checks the DDS magic, the header size (0x7C) and the pixel-format size
    (0x20), copies the header out, and creates the texture through `FUN_140d7e330`. Its only caller is
    `BSGraphics::Renderer::CreateTextureFromDDS` (`0x140d6dd70`). That is the call the streamer replaces with a shell.
-   **The release** is ID 75527 (SE `0x140d6ec70`, renamed `BSGraphics::Renderer::ReleaseTexture`). The block is 0x28 bytes:
    the texture at +0x0, an unknown COM object at +0x8, the SRV at +0x10, a refcount at +0x20. The last release releases
    +0x10, +0x0 and +0x8 in that order, then frees the block. A shell's block sets +0x0 and +0x10 and leaves +0x8 null.
-   **Binds.** The engine never reads a shell's storage. It binds the shell's SRV like any texture (the state table, flushed
    by `SetDirtyStates` 75580/77386), and DXVK's `GetImageView` gives the current window. Nothing else in the engine changes
    for graph windows, so streaming needs no new engine hook in the guest phase.

## Design

```
worker (streaming domain)            render thread, after Present        Present anchor (next frame)
  read mips from the file      ->      wrap the image for D3D11      ->     copy staging -> image (one-shot pass)
  allocate image (ORG allocator)       import it into the graph             executed: install, gate opens
  stage mips in an upload buffer       register the batch's copy pass
```

-   **Windows are ORG textures.** The image comes from ORG's allocator (`org::memory::CreateAllocatedResource`, added to ORG
    for this), labelled `Game/Textures/<folder>` like DXVK's windows and spillable like them. DXVK wraps it
    (`dxvkCreateTextureFromVkImage`) so the shell can redirect native binds to it; that is the guest phase. DXVK zeroes the
    image in its stream at the wrap, which is before the copy pass in the same stream.
-   **Bindless.** The image is imported into the graph in the layout DXVK rests it in (`RenderGraph::ImportDxvkTexture`). Its
    default SRV is a descriptor in ORG's resource heap. Drawcall Limit Fix's importer (`GpuTextures::Import`) takes that
    index for a graph window (`TextureStreaming::GraphWindowOf`), with no DXVK import, tracking or second descriptor.
-   **Uploads.** One one-shot copy pass per batch (`PassDesc::once`) at the Present anchor declares each window as its copy
    destination, so the graph moves the image to the copy layout and back to its home. Async epochs cannot record texture
    uploads through the upload service, which is why it is a pass. It runs on the graphics queue, in DXVK's stream: no copy
    queue and no wait, so the shared copy queue's rule never comes up. A batch is at most 256 windows or 256 MiB.
-   **Readiness.** `BuildWindowAsync` returns a `PendingWindow`. DCLF's state graph turns it into the window artifact's GPU
    submission set, and the texture gate now asks for `GpuReady`. A D3D11 window has none and is GPU ready at once, as before.
    So objects wait for their base window as they always did, and frames never wait.
-   **Install.** After the run that submitted the batch (`ClientDesc::executed`), the render thread sets each shell's window
    and tells DCLF of the swap. Native binds after that point in the stream read the new window.
-   **Release.** A window is held by its texture's record and by each DCLF import. When the last one lets go, its D3D11 view
    and wrap are released on the render thread, then its descriptor and memory go through `ReleaseAfterD3D11`.
-   **Fallback.** Synchronous builds (the safety net, an import that finds no window, the full-chain test mode) stay on D3D11.
    If the graph cannot wrap, import or register, the backend turns off for the session and every window it holds is posted
    again as a D3D11 build.
-   **Controller.** `Policy::StepPullback` and the rest of the policy are unchanged. Only the reading under it moves:
    `ReadOrgMemory` adds ORG's allocator to DXVK's memory, uses BasicRHI's `usageAtBudget` flag for AMD's clamp (keeping the
    0.995 share as a second test), and counts spilled allocations as overflow. The clamp rule (hold what is outside while
    clamped) is the same.

## VRAM tooling (P4.7)

-   `Vram/OrgMemory.{h,cpp}` reads BasicRHI's `QueryVideoMemoryInfo` for video and system memory (with `usageAtBudget`), the
    allocator's capacity and budgets, and its owner report (`org::memory::OwnerReport`).
-   With `CS_ORG_VRAM_SOURCE=org`, `Vram::Current` uses those as heaps 0 (video) and 1 (system), sums DXVK's totals into them,
    and moves DXVK's rows to the group of their heap. The reconciliation notes AMD's clamp when it is on.
-   The allocator's owner rows are listed. A labelled row counts toward the totals: only host code that allocates outside
    ORG's tracked resources sets a label (today, the streamer's windows and staging). The unlabelled row is ORG's own memory,
    which the tracked rows and the "not tracked" residual already cover, so it is listed only.
-   Labels are owner paths, so streamed windows land in `Game/Textures/<folder>` as DXVK's do.
-   The memory window's report line adds the allocator by owner and the spilled total.
-   Not moved: `Vram/SpillLab` (DXVK's placement exports, a test tool) and the VRAM Budget feature's DXGI reading.

## What to test first

1.  `tools/cs-ab/scenarios/tex-org.env` against `tex-org-off.env`: the log line `[TextureStreaming] windows are made on the
    render graph`, `dclf:tex-stats`'s new line (`render graph windows: N uploaded in M copy passes, 0 built on D3D11
    instead`), no `graph windows are off` error, screenshots equal to the baseline (distant land included).
2.  `CS_DCLF_TEX_STREAM_DELAY_MS=100` with `tex-org.env`: late windows still hold objects back (no flat silhouettes).
3.  `vram-org.env`: `dclf:vram-json` has `"heapSource": "org"`, and its device-local totals match the DXVK run within a few
    percent. On the RX 6600, check the clamp note appears when the use reaches the budget.

## Risks

-   The window's descriptor index assumes ORG's resource heap is the heap DCLF's shaders index, as `IDescriptorService`'s
    descriptors are. If DCLF draws read garbage for graph windows only, this is the first thing to check.
-   BC tail mips smaller than a block (2x2, 1x1) are copied with their texel size. BasicRHI's Vulkan copy takes the row
    length in texels from the 256-byte pitch (`VkBuildBufferImageCopy`), and Vulkan accepts an extent equal to the mip's. The
    D3D12 path has not been looked at.
-   The Present anchor runs in menus and on the map, and on loading screens' own present loop only with
    `CS_ORG_LOADING_THREAD=1` (P5.12). Without it, windows staged during a loading screen wait for the first presented frame.
    With it, a batch is registered there, but with Drawcall Limit Fix's explicit builds its graph is built only at the first
    world frame's build point, so the upload still lands then (p5/loading-screen.md, "Rebuilds in this lane").
-   Each batch rebuilds the Present anchor's graph twice (with and without the one-shot pass). At most one batch is in flight,
    so this is at most two builds a frame while streaming is busy.

## Native mode (later)

Without DXVK there is no shell: the engine's field holds a tripwire SRV handle (P4.1) whose window is the graph texture, and
native draws bind it through the ORG draw path (Phase 5). The streamer then needs the shell's identity to be the handle
instead of the DXVK view, and `dxvkStreamGetBinds`'s native-bind count from the ORG bind path.
