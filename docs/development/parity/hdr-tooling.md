# Parity wave: HDR and tooling in native mode (WS3)

Lane `par-hdr-tooling`, branch `org-native/par-hdr-tooling`, based on `ababbdf09`. BasicRHI branch `org-native/par-hdr-tooling`.

## HDR

### What changed

- **Swap chain creation.** Native mode configures BasicRHI's swap chain through `Engine::ConfigureSwapchain` (SDR, the frame
  latency). Before this, only the D3D12 self-test called it. Line: `[ORG] Vulkan swap chain: SDR, frame latency 2` (or `D3D12`,
  with ` (waitable object)`).
- **The colour space.** `HDRDisplay::UpdateSwapChainColorSpace` asks the swap chain for `IDXGISwapChain4`. The native swap chain
  handle refuses it, so the colour space never changed. In native mode it now calls `Native::SetSwapChainColorSpace(hdr)`. That
  function takes the present lock, waits for the queued presents' copies, and calls `ConfigureSwapchain` with HDR on or off.
  BasicRHI rebuilds the swap chain images in the new colour space. The present reads the image again at each present, so nothing
  else holds the old images.
  I chose this over answering `IDXGISwapChain3/4` on the handle. Answering would need a new tripwire interface chain (about 40
  methods) for three calls, and HDR Display is the only caller.
- **Resize.** BasicRHI keeps the colour space across `ResizeBuffers` on both backends. Native mode's resize passes the same
  swap chain format, so no change was needed there.
- **The output reaching the present.** In guest mode with the owned frame, `OwnedFrame::SetFrameSource` lets the owned frame
  present HDR Display's output texture directly, which saves the copy into the back buffer. Native mode already has a path:
  `ApplyHDR` finds no owned frame and gives `ORGHDRDisplay::Compose` the native back buffer. The composite's copy pass then writes
  the output into the back buffer on the graph, and `Native::Present` copies the back buffer into the swap chain image. So HDR
  output reaches the present without a native `SetFrameSource`. The only gain from one would be a full-screen copy fewer. It
  would also mean editing `Native::Present`, which another lane owns. Line:
  `[ORG] HDR Display: the output goes into the back buffer on the graph` (once, and again after a resize).

- **The scene in graph modes (2026-11-18).** In D3D11, `RedirectFramebuffer` swaps kFRAMEBUFFER for HDR's float scene texture
  around `RenderEndOfFrameEffects`, so ISHDR and the effects after it write the scene the composite reads. The post chain's CS passes
  import their final target once, at registration, so they wrote the back buffer and the composite read a scene nothing wrote. The
  chain now ends in the scene texture while HDR is on (ImageSpace's `kHdrSceneOutput`, `HDRDisplay::PostChainTarget`) and registers
  again when HDR is switched. The Tonemap anchor's clients get the same target (`ChainTonemapTargets`).

### How to test

HDR shots need an HDR display with Windows HDR on. Without one:

1. Force HDR on in HDR Display's settings: `enableHDR: true` and `hdrAutoDetected: true` in its user settings (otherwise auto-detect
   turns it off on an SDR display).
2. Run `native-quick` (Vulkan) and `native-d3d12` quick runs.
3. Look for:
   - `[ORG native] swap chain colour space: HDR10 (PQ, Rec. 2020) asked, HDR10 now (...)` and `[HDR] Set swap chain color space
     to HDR10 (PQ/BT.2020) (native)`. NVIDIA's Vulkan driver lists the HDR10 surface format even with Windows HDR off (the
     stage 4 line says `HDR10 true`), so on Vulkan the switch goes through on an SDR display. The picture is washed out then,
     which is expected.
   - On D3D12, DXGI accepts HDR10 only while the output has HDR on. On an SDR display expect
     `[ORG] HDR is on, but the display path does not take HDR10 (ST.2084, Rec. 2020). Presenting SDR.`, then
     `... HDR10 (PQ, Rec. 2020) asked, SDR now ...` and `[HDR] Failed to set HDR10 color space (native)`. That proves the call
     reaches DXGI.
   - Turning HDR off in the menu logs `SDR asked, SDR now`.
4. With HDR off (the default on an SDR display) nothing changes except the `[ORG] ... swap chain: SDR` line at startup.

On an HDR display (Windows HDR on), the `hdr` config switches HDR on and off mid-session and takes a shot in each:
`native-d3d12+hdr`, `native-vulkan+hdr`, `guest+hdr`, `d3d11+hdr`. HDR shots are HDR10 PNGs (PQ, Rec. 2020), which look washed out
in an SDR viewer. Judge brightness on the display.

## Tooling

### GPU timers, VRAM and the idle trace

Native mode has no D3D11 timestamp queries (the context's queries answer 0), no DXVK memory snapshot and no DXVK submission trace.
So in native mode:

- `GpuTimingSource::SelectedTimers()` is always `Graph`. A `CS_GPU_TIMERS` set to anything else is logged once
  (`[GpuEvents] native mode: GPU timers come from the render graph ...`) and ignored.
- `GpuTimingSource::IdleFromGraph()` is true: `CS_GPU_IDLE_TRACE` reads the graph's ranges
  (`[GpuIdle] Tracing the render graph's graphics queue (CS_GPU_IDLE_SOURCE=graph)` appears without the switch).
- `Vram::Org::Selected()` is true: VRAM attribution and texture streaming's budget read BasicRHI's heaps.

Guest and plain D3D11 keep their defaults.

### Nsight Perf and GPUPerfAPI

- **Native Vulkan.** `RenderGraphRuntime::Initialize` registers the Vulkan counters backend on the native device
  (`Native::DescribeVulkanDevice`: the instance, physical and logical device, graphics queue and the loader's
  `vkGetInstanceProcAddr`). Nsight Perf's frame boundaries run through `Native::RunOnGraphicsQueue`, which calls them right away
  under the lock every native queue access takes, in place of DXVK's `dxvkEnqueueQueueCallback`. The graph's passes are the
  ranges, as on DXVK. Engine perf events record no GPU work in native mode, so they push no ranges. Line:
  `[NvPerf] Vulkan backend registered on the native device's graphics queue`.
  The native device lists no extensions, so the `VK_KHR_external_semaphore_win32` warning is DXVK-only now.
- **GPUPerfAPI** (AMD) refuses native mode by name: `[GPA] native mode: GPUPerfAPI's Vulkan backend needs DXVK's device ...`. It
  must be set up before the device exists and needs `VK_AMD_gpa_interface` on it, which only the DXVK path arranges
  (`GpaBeforeDeviceCreation`).
- **Native D3D12.** Not registered: `[GpuCounters] native mode on D3D12: no hardware counters ...`. ORG's NvPerfCapture already
  takes D3D12 queues. What is missing is a D3D12 command-list accessor in CS that knows whether a range was pushed
  (org-extension-api-notes.md, "Push results from `nvperf::BeginPassRange`").

To test: a native Vulkan run on an NVIDIA card with `CS_GPU_COUNTERS_RANGES=*` (or a capture from the Remote Control panel).
Expect the registration line, then the usual `[NvPerf]` capture lines and a CSV. Untested here.

### Aftermath

The checkpoint registration was adoption-only (`AdoptedVulkanDeviceInfo::diagnostics`). BasicRHI's `DeviceCreateInfo` now takes
the same callback (`vulkanRegisterCheckpoint`, `vulkanCheckpointUser`, `vulkanDetailedCheckpoints`). Native device creation sets
it to `Aftermath::RegisterOrgCheckpoint` when Aftermath is armed (`CS_GPU_CRASH_ANALYSIS=1` and the runtime present). With crash
markers on (that switch or `DXVK_DEBUG=hang`), every debug label then sets an NV checkpoint that the dump resolves to the pass. The
NV checkpoints then belong to the host, and BasicRHI's own crash markers keep the other sources (buffer markers, device fault).
Line: `CreateVulkanDevice: the host's checkpoint markers record VK_NV_device_diagnostic_checkpoints at every debug label`.

Not done: the `VkDeviceDiagnosticsConfigCreateInfoNV` chain DXVK adds under hang mode (shader debug info, resource tracking).
Aftermath on D3D12 would need the DX12 watched-API flag in `Aftermath::Enable` and DRED-style markers. D3D12 has DRED with
breadcrumb context, and the debug labels below now name the passes there.

### D3D12 debug labels (BasicRHI)

`SupportsDebugLabels` is true on D3D12 lists. Begin, end and insert are `BeginEvent`, `EndEvent` and `SetMarker` with metadata 0 and
a UTF-16 name, which is the encoding pix3's `PIXBeginEvent` falls back to and which PIX, RenderDoc and Nsight read. The colour is
not carried. ORG's `rhi::debug::Scope` (one per pass in `RhiExecution.h`) now labels passes in PIX captures and in DRED's
breadcrumb context.

## D3D12 Close failure (E_INVALIDARG on 'ORG Graphics CommandList #5')

Every native D3D12 run since 1113-201200 shows it once, on the first recording of DCLF's Z-prepass epoch after the scene
buffers grow. Passes: `cs.dclf.z.latched-copies`, `texture-transforms`, `pixel-rows`, `member-portal-queries`,
`member-portal-evaluate`, `build-draws`, `index-pool`, `tree-lod-cull`, `grass-cull`, `grass-args`. No validation run has the
debug layer's message for it. An older one (1112-081353, global barriers with layout-bound access) was a different cause, fixed
since.

I could not pin it from the code. The most likely causes are commands D3D12's core runtime checks at Close: a buffer copy out of
bounds (a latched copy sized for a grown buffer while the old one is resolved), an indirect count outside its buffer, or a query
index or resolve out of range. BasicRHI's D3D12 lists now:

- skip those commands before D3D12 sees them, and log each with the list, the open label (the pass) and the buffers' names:
  `DX12 CopyBufferRegion skipped: ...`, `DX12 ExecuteIndirect skipped: the count buffer ...`, `DX12 timestamp skipped`,
  `DX12 BeginQuery skipped`, `DX12 EndQuery skipped`, `DX12 ResolveQueryData skipped` (the first 16 per site). ExecuteIndirect's
  undersized argument buffers were already skipped this way.
- log footprint copies D3D12 may refuse (an offset not on 512 bytes, a row pitch not on 256, a start past the buffer) without
  skipping them: `DX12 CopyBufferToTexture(...): footprint offset ...`.
- keep a ring of the last 32 commands of each list, each with its label, and log it when Close fails:
  `D3D12 command-list Close failed: '<list>' recorded N commands since its reset; the last 32 follow` and
  `D3D12   #i <command>(a, b, c) a=.. b=.. c=.. in '<pass>'`.

What to look for in the next native D3D12 quick run: a `skipped` line naming `cs.dclf.z.*` (the cause, and the Close failure
should be gone), or the ring after the Close failure (the last command before it is the suspect).

## Commits

CS: `e42dfdeed` HDR colour space, `ba7043f6f` sources, `53c1993bf` BasicRHI bump, `45444234e` Nsight Perf and Aftermath, and the
docs commit. BasicRHI: `a605e0d` D3D12 labels and Close diagnostics, `9887544` host checkpoints.

## What is left

- An HDR display check by eye: shots and the colour space lines pass on both backends (2026-11-18), brightness is not judged.
- A native `SetFrameSource` (present HDR Display's output directly, one copy fewer). It is optional: the copy works.
- Nsight Perf on native D3D12 (the ORG item above). GPUPerfAPI in native mode (early initialization and `VK_AMD_gpa_interface` on
  the native device).
- Aftermath's diagnostics config on the native Vulkan device, and Aftermath on D3D12.
- The Close failure's cause, from the next run's lines.
