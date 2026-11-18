# Native parity and simplification: status

The plan: `C:\Users\matth\.claude\plans\we-ve-been-working-on-foamy-journal.md`. Each lane's notes are in this folder.

## Done (2026-11-14, `org-native/integrate`)

Phase 1 (the first wave):
- **Hygiene** (hygiene.md): dead switches removed, the epoch switches registered outside DCLF, narrowed ports refused in native.
- **One native model on both backends** (native-context.md, inline-exec.md, and `bd6238b18`):
  - no home states, exclusive imports, graph-owned engine textures, the graph present tail and regions, on Vulkan and D3D12;
  - the native context records BasicRHI commands only, and its work on tracked images goes through ExternalWork;
  - no external epochs in native: at-execution passes record inline into their epoch's list (ORG `SetInlineAtExecution`);
  - deleted: `Native::External`, `CopyExternally`, the raw Vulkan context dialect, `OneShot`, `RouteRaw`, and the switches
    `CS_ORG_NATIVE_HOME_STATES`, `_EXCLUSIVE_IMPORTS`, `_OWNED`, `_GRAPH_PRESENT`, `_IN_STREAM`, `_RHI_CONTEXT` and `CS_ORG_REGIONS`.
- **DCLF's CPU pipeline on BasicRHI** (dclf-cpu.md): sites record `rhi::CommandList`; in native they record inline, with no
  secondaries; the forward and first-person families are back in native.
- **DCLF on D3D12** (dclf-d3d12.md): family vertex stages, shadow views, the Skylighting and precipitation occlusion maps,
  refraction, opaque effects, grass, tree LOD and the CPU pipeline.
- **Coverage** (imagespace.md, scaleform.md, families.md, fx.md): vanilla TAA and most image-space effects, Scaleform render
  targets and filters (its D3D11 parts are deleted), Bink and the families' refusals, the FX runtime's refusals.
- **HDR and tooling** (hdr-tooling.md): the native swap chain's colour space, graph timers, VRAM and idle sources in native,
  Nsight Perf and Aftermath on the native device, D3D12 debug labels.

Phase 2:
- **Upscaling in native** (`f8517c16c`): the feature runs in native; its device hook makes the native device. ImageSpace takes the
  engine's default textures as an input kind, so ISTemporalAA's alpha slot (a default texture when Upscaling turns the vanilla TAA
  setting off) no longer leaves TAA to the engine, which froze the image after a load.
- **Streamline on native Vulkan and D3D12** (`b11512773`, `fc62c40f0`):
  - BasicRHI is built with its Streamline support. Its new `DeviceCreateInfo::streamlineHost` takes CS's own `slInit` (manual
    hooking) and the union of the loaded features' requirements; the device and swap chain come up through Streamline.
  - The evaluates are the native context's external work (`Native::RunExternal`): a list of their own in queue order, each image
    moved into its state by its graph resource's ledger.
  - The fork's sl.fsr and sl.xess run on D3D12 (FidelityFX's DX12 backend, XeSS's D3D12 entry points). The package carries AMD's
    signed `amd_fidelityfx_dx12.dll`.
  - DLSS, FSR and XeSS evaluate on native Vulkan, native D3D12 and guest (`upscalers` configs, the `upscale` test command).
- **DCLF on D3D12 complete**: particles and water reflections left the D3D12 switch table; only the plain-draw rows remain (D3D12 has
  no device-generated commands).
- **Water reflections** (`bb4311e80`): each cube face draws with its own camera (a frame record per face naming its face's b12
  block). Before, every face read one unwritten VS_PerFrame, on both backends.
- **Native fixes found in testing:**
  - GenerateMips' programs load once the shader compiler exists (they never loaded: no engine mips in native).
  - The native context flushes held regions before taking its lock (work they ran could route context work again and threw).
  - D3D12: the CPU pipeline's indirect buffer has no unordered access (an upload buffer cannot; it had no mapping).
  - D3D12: depth formats in shader resource views (the reflection's depth SRV removed the device).
  - D3D12: root constants set again after an ExecuteIndirect whose signature sets one (the spec leaves them undefined).
  - D3D12 (RX 6600, 2026-11-18): the CPU pipeline's refraction draws pushed device addresses where the D3D12 stage reads heap
    indices (the frame blocks and LodFadeFrame). AMD faults on the descriptor load and lost the device at the first world frame.
  - D3D12: texture streaming's feedback buffer is a committed HostCached buffer (an UPLOAD buffer cannot have unordered access).
  - D3D12 (2026-11-18): the shadow caster rows' capacity is the host's, not a raw GetDimensions of a structured view (a quarter of
    the rows on AMD, so most foliage casters were dropped). VisibilityEventsCS drops the same read.
  - D3D12 on AMD (2026-11-18): the tree LOD draw's mesh loads index their buffers as non-uniform. One draw holds every tree type,
    and `DEVICE_REF_ASSUME_UNIFORM` made a wave read all its vertices from its first lane's mesh, so coverage followed the list order.
  - ORG: a barrier between unordered-access (and copy) uses of one resource in consecutive passes.
- **Screenshots** are a gated pass, in the graph for good: a capture needs no build, so it no longer misses its capture point.

## Tested

On the RX 6600 (2026-11-18): native D3D12 quick and river runs pass. The debug layer shows no errors in a quick run, only warnings
(clear values, a pixel stage writing an unbound target).

On the RTX 5080 (background runs):
- Quick runs pass on native Vulkan, native D3D12 and guest.
- The native tours pass on both backends: main menu, Whiterun, Dragonsreach, a save and its load.
- The river configs (`river`): guest, native Vulkan and native D3D12 show the same water reflections.
- The upscalers configs: DLSS, FSR, XeSS and TAA on each of the three modes.

## Performance (2026-11-14, perf-taa configs, RTX 5080, Perf-Fast build with Tracy)

The perf-taa sample: the autoloaded save, TAA, 900 frames. Frame rates:

| Mode | Before | Now |
| --- | --- | --- |
| Guest (DXVK) | 144 | 142 |
| Native Vulkan | 54.5 | 78-83 |
| Native D3D12 | - | 71 |

What changed:
- The graph present's back buffer has one version. Its three versions rotated every frame, and each rotation made every ticket
  prepared ahead stale and every kept recording unusable.
- Passes whose data the commit stores record at execution, with a stable revision: the sky, water, deferred composite, ImGui
  and Scaleform passes. No ticket goes stale in the world now.
- Back-to-back skeleton clears share one region execution.

Where native's time goes now (Tracy, native Vulkan, per frame): native is CPU-bound on the render thread. Guest's render
thread spends about 1.9 ms in instrumented code; native's spends 9.6 ms, about 7.5 ms of it in 47 epoch submissions (guest
has about 7). The graph host thread, which prepares the tickets, is busy about 11 ms a frame. Per epoch:
- render thread, about 160 us: inline recording 2.2 ms a frame, 75 queue submissions (1.3 ms), ticket admission and handoff,
  upload lists, command list resets;
- host thread, about 200 us: recording, `CompileFrame` (pass preparation, about 1,400 invocations a frame), retirement.

The epochs: 6 of DCLF, 21 anchor epochs, about 20 region executions. ImageSpace's batches still split about 5 times a frame:
the HDR luminance chain swaps its two targets every frame, so no one entry order fits both frames (it would need duplicate
passes).

To match DXVK, native needs what DXVK's CS thread gives it: the GPU work off the render thread. The plan:
1. An ordered submission thread for the graphics queue. The render thread commits each epoch's inputs; the thread admits,
   records inline and submits. The native context's submissions, ExternalWork and the present go through the same queue,
   so the ledger and submission order stay one sequence.
2. Fewer epochs: anchors that run back to back in one region execution beyond gated passes; ImageSpace duplicate passes.
3. Cheaper ticket preparation on the host thread (pass preparation per invocation, retirement), or several preparing threads.

## Open

- **Flicker with upscaling off** (`native-flicker-none`, `native-d3d12-flicker-none`): the whole frame's temporal std is 4.4 to 4.6 on
  both native backends (0.8 with the default method). Not D3D12's. Not looked into (org-native-frame-graph.md, "D3D12 tree LOD read
  the wrong meshes").
- **Tree LOD stage stamps** (`dclf.tree_lod_stages`) read back all zeros on D3D12. Not needed for the tree LOD fix, not looked into.
- **Render thread blocked on compiles**: with a cold shader store the main menu stalls 15 s and more while Lighting compiles (the
  whole compile took 90 s), and after a DCLF compute shader changes the first world frame creates its driver pipelines on the render
  thread (VisibilityEventsCS took the frame past 15 s on guest). Both trip the hang watchdog. 2026-11-18.
- **Screenshots in native**: the UI stage's capture comes out without the HUD; the D3D11 copies taken when its readback buffers are busy
  have it, so the flicker bursts show the compass on every other shot. Not the import (all resolve to `Native::BackBuffer`).
- **D3D12 views**: `CreateWords` buffers read as ByteAddressBuffer have structured views. A raw GetDimensions on them returns the element
  count on AMD. None is left in the shaders, but the views do not match the reads.
- **Frame generation in native**: the native presenter needs the frame-generation controls DXVK exports (swap chain recreate,
  sync present, queue depth, tearing, the torn-down callback). Until then DLSS-G and FSR-FG stay off in native.
- **Residue:** the reflection cube face draws; load-window shadow draws; the refused occlusion-query draw; the sun occlusion
  draw skipped when its depth is neither kMAIN's nor the Z-prepass copy.
- **Map:** native shows flat white patches on the world map's terrain. Compare with guest.
- **D3D12:** shader reload loses the device;
  GPU-based validation shows a Scaleform draw sampling a texture still in COPY_DEST (a streamed texture's upload).
- **Re-recording every frame** (`CS_ORG_REUSE_RECORDINGS=0`) crashes at startup in native D3D12.
- **Held deletion:** FX11 execution in ORG modes (par-fx `ed559b2b7`). It needs a preset A/B (`tools/cs-test/data/fx-parity`).
- **Left over from the lanes:**
  - ImageSpace: `ISTemporalAA_UI`/`_Water`, raster lens flares, Perlin noise, the dynamic-resolution quad.
  - Scaleform: masks inside pushed render targets; engine targets other than the book's.
  - FX: geometry shaders, null UAVs.
- **WS6, first half done (Guest B0, 2026-11-18):** guest runs the same ports as native by default, with the flush latch and the
  guest context (`Native::ApplySwitches`); the dead native branches (fog, screen-space shadows, VL, Sun Rays, the deferred composite,
  IBL) are gone. Left: the port switches themselves (after guest runs on stages), and guest's first-sight ImageSpace shapes, which
  the latch drops (a frozen-world menu's background is black in the session that first sees its shapes; see
  `org-native-frame-graph.md`, "Guest B0").
- **WS7-WS8:** the declaration and registry cleanup; the submission seam and the `NativeDevice.cpp` split.
