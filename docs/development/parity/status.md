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
  - ORG: a barrier between unordered-access (and copy) uses of one resource in consecutive passes.
- **Screenshots** are a gated pass, in the graph for good: a capture needs no build, so it no longer misses its capture point.

## Tested

On the RTX 5080 (background runs):
- Quick runs pass on native Vulkan, native D3D12 and guest.
- The native tours pass on both backends: main menu, Whiterun, Dragonsreach, a save and its load.
- The river configs (`river`): guest, native Vulkan and native D3D12 show the same water reflections.
- The upscalers configs: DLSS, FSR, XeSS and TAA on each of the three modes.

## Open

- **D3D12 shadow flicker** (the `flicker` configs, scored by tile spikes over 60 consecutive frames): D3D12 only, GPU-driven shadow
  path only (`CS_DCLF_PIPELINE=cpu` and `CS_DCLF_SHADOWS=0` show none; Vulkan shows none). Ruled out: barrier sync scopes
  (`BASICRHI_DX12_FULL_BARRIERS`), the compute and copy queues, occlusion culling, scene value copies, root constants after
  ExecuteIndirect, UAV hazards between passes, the upscaler.
- **Frame generation in native**: the native presenter needs the frame-generation controls DXVK exports (swap chain recreate,
  sync present, queue depth, tearing, the torn-down callback). Until then DLSS-G and FSR-FG stay off in native.
- **Residue:** the reflection cube face draws; load-window shadow draws; the refused occlusion-query draw; the sun occlusion
  draw skipped when its depth is neither kMAIN's nor the Z-prepass copy.
- **Map:** native shows flat white patches on the world map's terrain. Compare with guest.
- **D3D12:** texture streaming's feedback buffer (unordered access on a host-visible heap); shader reload loses the device;
  GPU-based validation shows a Scaleform draw sampling a texture still in COPY_DEST (a streamed texture's upload).
- **Re-recording every frame** (`CS_ORG_REUSE_RECORDINGS=0`) crashes at startup in native D3D12.
- **Held deletion:** FX11 execution in ORG modes (par-fx `ed559b2b7`). It needs a preset A/B (`tools/cs-test/data/fx-parity`).
- **Left over from the lanes:**
  - ImageSpace: `ISTemporalAA_UI`/`_Water`, raster lens flares, Perlin noise, the dynamic-resolution quad.
  - Scaleform: masks inside pushed render targets; engine targets other than the book's.
  - FX: geometry shaders, null UAVs.
- **Dead native branches** (the `Native::Active()` "empty input" cases in the fog, SSS, VL, Sun Rays, the deferred composite and
  IBL): they go with WS6, since guest still imports producers on D3D11 there.
- **WS6-WS8, after the parity gate:** guest runs the same ports as native and the port switches go; the declaration and
  registry cleanup; the submission seam and the `NativeDevice.cpp` split.
