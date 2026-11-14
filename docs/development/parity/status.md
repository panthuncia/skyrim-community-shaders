# Native parity and simplification: status

The plan: `C:\Users\matth\.claude\plans\we-ve-been-working-on-foamy-journal.md`. Each lane's notes are in this folder.

## Done (2026-11-14, `org-native/integrate`)

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
  refraction, opaque effects, grass, tree LOD and the CPU pipeline. Still off: particles and water reflections.
- **Coverage** (imagespace.md, scaleform.md, families.md, fx.md): vanilla TAA and most image-space effects, Scaleform render
  targets and filters (its D3D11 parts are deleted), Bink and the families' refusals, the FX runtime's refusals.
- **HDR and tooling** (hdr-tooling.md): the native swap chain's colour space, graph timers, VRAM and idle sources in native,
  Nsight Perf and Aftermath on the native device, D3D12 debug labels.
- **D3D12 fixes found in testing:**
  - BasicRHI put the buffer view refs' region past a 1,000,000-descriptor heap, over D3D12's limit. NVIDIA refused the heap, so
    every view ref read zero and the world drew nothing.
  - ORG sent new graph-owned textures "home" to Undefined in closed executions.
  - BasicRHI named unordered access on non-UAV buffers.
- DCLF's setup programs compile off the render thread (no more watchdog hangs after a shader change).

## Tested

On the RTX 5080 (background runs):
- Quick runs pass on native Vulkan, native D3D12 and guest.
- The native tours pass on both backends: main menu, Whiterun, Dragonsreach, a save and its load.
- The menu tour passes on native Vulkan: inventory and map. Scaleform draws 6,996 of 7,000 displays on the graph; the 4 left out
  are the first displays, before the Interface client is in its epoch.
- D3D12 no longer logs command-list Close failures.

Residue census, native Vulkan tour:
- 294 to 300 of every 300 frames have no engine draw reaching D3D11.
- What is left comes in short bursts:
  - depth draws into the engine's reflection cube face 0 (DistantTree and Lighting), about 2 frames in 300;
  - shadow-map and precipitation-occlusion draws in the frames right after a load;
  - a few ImageSpace draws.

## Open

- **Residue:**
  - the reflection cube face 0 depth draws;
  - load-window shadow draws;
  - the reflection faces' camera: each face's b12 is pushed but the layouts read the frame record's single view (dclf-d3d12.md);
  - DCLF 5.5.6.
- **Map:** native shows flat white patches on the world map's terrain. Compare with guest.
- **D3D12:** DCLF particles and water reflections; shader reload loses the device.
- **Streamline** (DLSS, FSR, XeSS, frame generation) on BasicRHI, both native backends. Until then Upscaling is a startup error in
  native.
- **Held deletion:** FX11 execution in ORG modes (par-fx `ed559b2b7`). It needs a preset A/B (`tools/cs-test/data/fx-parity`).
- **Left over from the lanes:**
  - ImageSpace: `ISTemporalAA_UI`/`_Water`, raster lens flares, Perlin noise, the dynamic-resolution quad.
  - Scaleform: masks inside pushed render targets; engine targets other than the book's.
  - FX: geometry shaders, null UAVs.
- **WS6-WS8, after the parity gate:** guest runs the same ports as native and the port switches go; the declaration and
  registry cleanup; the submission seam and the `NativeDevice.cpp` split.
