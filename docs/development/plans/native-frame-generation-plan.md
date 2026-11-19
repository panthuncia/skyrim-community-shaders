# Frame generation in native D3D12 and native Vulkan: plan

Status: plan, 2026-11-18. Nothing here is implemented. Read against CS `org-native/phases` in the `test-runner` worktree.
Code citations are `file:function`. "(inferred)" marks a claim I did not prove from code or headers.
`frame-revision-plan.md` did not exist when this was written. The change-system terms used here come from
`central-resize-events.md` (sections 4.0, 5.1). Re-read that plan when it exists and adjust steps 5 and 9.

Updated 2026-11-19, read-only, against `integ/hard-ports` at `5668ec493`: the post chain gained passes and inputs (pre-exposure
through the ORG chain and the upscalers, two-pass Camera FX, Physical Lens), the scene gained Physical Sky and ViewAtmosphere
passes, and the screenshot's clean scene became a graph copy on the FrameGeneration ticket. `frame-revision-plan.md` now exists;
step 9 follows it. The additions are in 1.2, 2.4, 3.1, 3.6, steps 1, 3, 6 and 9, and open decisions 8 to 10 (section 6).

Short answer to "what is still needed":
1. A per-backend way to replace the swap chain at a frame boundary without the render thread waiting (native has only `ResizeBuffers` today).
2. BasicRHI swap chains that a presentation layer (FidelityFX's or DLSS-G's swap chain, through Streamline) may wrap.
3. A D3D12 backend for CS's `sl.fsr_g` Streamline plugin (it is Vulkan-only).
4. The frame generation inputs (hudless colour, depth, motion vectors, the FSR-FG prepare, DLSS-G's tags) as passes of a stage, not DXVK interop ring work.
5. One presenter interface so `FrameGen::Controller` drives DXVK's presenter and the native swap chain the same way.
6. CS loading the frame generation features in native (`Streamline::Initialize`, `Streamline::SetNativeDevice` turn them off today).

---

## 1. How guest frame generation works today

### 1.1 Who owns what

| Piece | FSR-FG | DLSS-G |
|---|---|---|
| Frame generation runtime | CS's own Streamline plugin `sl.fsr_g` (`extern/Streamline/source/plugins/sl.fsr_g/fsrGEntry.cpp`), driving FidelityFX's prebuilt `amd_fidelityfx_vk.dll` (FFX API, SDK v1.1.4 plus two Vulkan luminance fixes: `external/fidelityfx-bin/README.md`) | NVIDIA's `sl.dlss_g` (binary) |
| Backends | Vulkan only. `fsr_g.json` lists `"rhi": ["vk"]`; `fsrGEntry.cpp:slOnPluginStartup` returns false on any other device | D3D12 and Vulkan (SL standard) |
| Swap chain | FFX's `FrameInterpolationSwapChainVK`, made inside DXVK's `vkCreateSwapchainKHR` by the plugin's before-hook `fsrGEntry.cpp:slHookVkCreateSwapchainKHR` → `createFgSwapchain` (`FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK`) | Streamline's own hook on DXVK's swap chain |
| How it sees DXVK's swap chain | DXVK's Vulkan loader resolves to `sl.interposer.dll` (`Streamline.cpp:PreloadInterposer`, before DXVK's `VkInstance`). The interposer dispatches the plugin's before-hooks on CreateSwapchain, DestroySwapchain, GetSwapchainImages, AcquireNextImage, QueuePresent (`fsr_g.json` "hooks") | Same interposer |
| Queues | Game queue = DXVK's present queue, captured at the first present (`slHookVkQueuePresentKHR`). Present and acquire queues = two extra graphics queues the interposer adds; the plugin asks for three and skips the first, which `sl.dlss_g`'s pacer takes (`fsrGEntry.cpp:updateEmbeddedJSON`, `acquireFgQueues`) | One extra graphics queue for its pacer; optical flow |
| DXVK's part | Presenter becomes a thin submit and hand-off when the swap chain is wrapped (`extern/dxvk/src/dxvk/dxvk_presenter.cpp:Presenter::createSwapChain`, comment at line 1006). Exports used by CS: `dxvkRequestSwapchainRecreate`, `dxvkSetSyncPresent`, `dxvkSetPresentQueueDepth`, `dxvkSetTearingPreference`, `dxvkGetPresenterSurfaceState`, `dxvkSetSwapchainTornDownCallback` (`Streamline.cpp`, anonymous `g_dxvk::Resolve`) | Same exports |

### 1.2 What CS delivers each frame, from where

Per frame, on the render thread:

1. `Upscaling.cpp:Main_UpdateJitter::thunk` → `Upscaling::BeginRenderFrame` (HDR colour-space commit) and `Streamline.cpp:Streamline::BeginRenderFrame`.
   The latter discards any FSR-FG frame still prepared and advances `renderFrameId` (the Streamline frame token).
2. `Upscaling.cpp:Main_PostProcessing::thunk`, after the engine's `RenderWorldPostProcess` returns:
   - `PostProcessing::ClearBorderMotionVectorsForFrameGen` zeroes the letterbox band's motion vectors.
   - PCL markers SimulationEnd and RenderSubmitStart.
   - `Upscaling::CaptureHudlessColor`: `CreateHudlessTexture` (format from DXVK's presenter: `DXVKInterop::GetPresenterFormatForFrame`;
     R10G10B10A2 under HDR), then `CopyHudlessColor` draws kFRAMEBUFFER, or HDR's clean composite (`HDRDisplay::ComposeCleanCapture`),
     into it. In graph modes that draw is a pass at the FrameGeneration anchor with its own ticket (`ORGUpscaling.cpp:RegisterHudless`,
     `RecipeTable.h` post stage `Ticket(Anchor::FrameGeneration, { "cs.upscaling.hudless" })`).
   - `Upscaling::PrepareFrameGeneration(hudless)`, only in gameplay (not paused, no main or loading menu):
     - FSR-FG: `Streamline::EvaluateFSRFrameGen` → `cs_EvaluateFeatureCore(kFeatureFSR_G, viewport 1, depth, motion vectors, hudless)`.
       Constants (`cs_BuildConstants`: camera, jitter, near/far, FOV) go to `slSetConstants`; the three images are wrapped as Vulkan
       views and tagged; `slEvaluateFeature` records into a DXVK interop ring command buffer (`DXVKInterop::BeginFrameCommandBuffer`,
       `SubmitFrameCommandBuffer`). In the plugin, `fsrEndEvaluation` runs FFX's `FRAMEGENERATION_PREPARE` dispatch (depth, motion
       vectors, render size, jitter, frame time from QPC, camera), and stores the hudless `VkImage` and its current layout in
       `pendingFrame`. Then `Streamline::CaptureFSRFrameGenState` reads the presented-frame count.
     - DLSS-G: `FrameGen::Controller::EngageDLSSG` (`slDLSSGSetOptions`: mode, multiplier, dynamic target, `eRetainResourcesWhenOff`,
       `eBlockPresentingClientQueue`). Without CS upscaling it first runs an FSR evaluate to make a display-size colour. Then
       `Streamline::TagDLSSGResources`: depth, motion vectors, hudless tagged `eValidUntilPresent` through `cs_SubmitPresentTags`.
     - Depth is kMAIN's depth, or kMAIN_COPY when upscaling is on. Motion vectors are kMOTION_VECTOR.
   - What the hudless holds by then (2026-11-19). Everything before the post stage is scene colour: Physical Sky's LUT passes
     (EarlyPrepass), its aerial perspective pass after `cs.deferred` (phase 2, not ported yet), and ViewAtmosphere's post-water
     composite (EngineDraws, after the water's ImageSpace blend). The post chain then adds, in chain order: DoF, Physical Lens's
     side outputs (ghosts, starburst, the image mapping that distorts and splits colour), read by Composite, which also divides
     the pre-exposure out under linear lighting (or the output copy scales by 1 / pre-exposure without Composite, `eb71d2e7f`),
     then colour grading, LUT, Camera FX's two passes (a lens pass with distortion into a transient image, a finish pass with
     grain) and Border (`PostProcessing.h:FeaturePipelineIndex`). So the hudless is display-referred, with no pre-exposure
     left in it, and it includes the lens and camera effects. In graph modes the same FrameGeneration ticket also copies kFRAMEBUFFER into the screenshot's clean scene
     (`cs.screenshot.clean`, `027887780`) when an SDR capture is pending.
3. Present hook `Hooks.cpp:IDXGISwapChain_Present::thunk`:
   - DLSS-G forces sync interval 0 when it reports vsync unsupported; `Streamline::QueryDLSSGCapabilities` on the present thread.
   - PCL markers RenderSubmitEnd, TriggerFlash, PresentStart; `FrameAnchors::Run(Anchor::Present)`.
   - `HDRDisplay::HandleSwapChainPresent` → `Upscaling::PresentWithFrameGeneration`:
     FSR-FG calls `Streamline::EnsureFSRFGPresentState` (discard when nothing was prepared this frame); DLSS-G calls
     `Streamline::EnsureDLSSGPresentTag` (null tags force passthrough). Then DXVK presents.
   - In DXVK's present, the plugin's `slHookVkQueuePresentKHR` takes `pendingFrame` if its frameID matches, configures FFX
     (`ffxConfigureDescFrameGeneration`: enabled only for a prepared frame, `HUDLessColor`, debug flags, `onlyPresentGenerated`,
     generation rect), then calls FFX's replacement present. FFX's interpolation dispatch runs through `FfxFrameGenDispatchCallback`
     on that thread. The UI composition mode is **hudless difference**: FFX extracts the UI as back buffer minus hudless. No UI texture,
     no present callback.
   - After the present: `Streamline::CaptureDLSSGPresentState`, `RenderGraphRuntime::SignalFramePace(pacedByCS)` with
     `pacedByCS = !IsFrameGenerationActive()`.

### 1.3 On/off transitions

`FrameGen::Controller` (`FrameGenController.cpp`) runs from `Upscaling::CheckResources` every frame (`Controller::Reconcile`).

- Gates: Upscaling loaded, Streamline feature support resolved, window usable (`Upscaling::IsWindowUnusable`: minimized, unfocused unless
  `CS_UPSCALE_UNFOCUSED=1`, size/move), display settled.
- Phases: `kIdle` and `kTransitioning`. `StepModeTeardown` turns DLSS-G mode off and waits for device idle, or drains the interop ring
  and sends FSR-FG `enabled=false`. `StepLoadState` sets the desired loaded state of each plugin, pushes sync present and the tearing
  preference, starts DXVK's colour-space transition and calls `RequestDxvkSwapchainRecreate`. DXVK tears the swap chain down and calls
  `Streamline.cpp:DxvkSwapchainTornDownCallback`, which tears FFX's swap chain down and calls `slSetFeatureLoaded` for both plugins
  (SL allows load changes only while no swap chain exists). `StepPhaseCompletion` adopts the new owner and relaxes DXVK's present queue
  to depth 2 (`CS_FSRFG_QDEPTH`). `StepFSRDelivery` sends FSR-FG's options; the enable edge asks for a second recreate, because FFX
  wraps only inside `vkCreateSwapchainKHR`.
- The plugin also forces recreation itself: on a wrap mismatch its present hook presents and returns `VK_SUBOPTIMAL_KHR`
  (`slHookVkQueuePresentKHR`, `needRecreate`).
- Vsync change with FSR-FG: FFX bakes the present mode, so the controller recreates one frame later (`fsrVsyncRebakePending`).
- HDR change with FSR-FG: new `colorBuffersHDR`, recreate ("FSR-FG HDR transfer change"). The FG context is rebuilt when its HDR flag
  differs (`fsrGEntry.cpp:ensureFgContext`).
- Faults: `Upscaling::PresentWithFrameGeneration` (`requestFaultTeardown`) and `Controller::NotifyFaultTeardownRequested` turn frame
  generation off and recreate. These are retry and deferral loops (counters, "deferred" logs), which the standing rules forbid.

### 1.4 Menus, pause, loading screens, focus

- CS menu: not a gate. Frame generation stays on (`docs/development/parity/status.md`, "Frame generation in guest").
- Pause, main menu, loading screen: `PrepareFrameGeneration` delivers nothing. FSR-FG discards (1:1 passthrough); DLSS-G gets null
  tags (passthrough). The comments in `Streamline::EnsureFSRFGPresentState` explain why discard beats turning it off (measured).
- Focus loss: no transitions (`Reconcile` returns early) and no inputs delivered (`Main_PostProcessing`'s `windowUsable`).

### 1.5 Pacing, vsync, tearing, Reflex, HDR

| Topic | FSR-FG | DLSS-G |
|---|---|---|
| Present mode | Follows vsync only (`Upscaling::GetPresentModePreference`), pushed before each recreate | Same |
| Vsync | Baked into FFX's swap chain: change = recreate | Sync interval 0 when unsupported (`Hooks.cpp`) |
| Reflex low latency | Off (`Upscaling::GetEffectiveReflex`, measured reasons in the comment) | Forced on (DLSS-G requires it) |
| Frame limiter | `slReflexSleep` still runs when a limit is set (`Streamline::UpdateReflex`); limit divided by the presented multiplier (`Upscaling::GetRenderedFrameRateLimit`) | Limit given undivided (lands on the presented cadence) |
| Present queue | DXVK sync present during the switch, depth 2 after | Blocks in its own present |
| FFX pacing | Safety 0.01 ms, variance 0 (`Streamline::SetFSRFrameGen`); hybrid spin on (`fsrGEntry.cpp:applyFramePacingTuning`) | n/a |
| HDR | Hudless is 10-bit; FFX gets a default 1000-nit mastering range through its own `SetHdrMetadataEXT` (`createFgSwapchain`) | `hdrFormatNotSupported` status is reported (`CaptureDLSSGPresentState`) |

---

## 2. What native has

### 2.1 Device and Streamline

- `NativeDevice.cpp:NativeStreamline` initializes CS's Streamline for the native backend before the device exists
  (`Streamline::InitializeForNative`, `eUseManualHooking`), and hands BasicRHI the interposer and the loaded features
  (`rhi::DeviceCreateInfo::StreamlineHost`, `rhi.h`).
- `Streamline.cpp:Streamline::Initialize` leaves `kFeatureFSR_G` and `kFeatureDLSS_G` out when `native`.
  `Streamline.cpp:Streamline::SetNativeDevice` sets `featureDLSSG = false; featureFSRFG = false;`.
- Vulkan (`rhi_vulkan.cpp:VkInitHostStreamline`): the union of the features' extensions and features, the largest queue count of each
  kind, `slSetVulkanInfo` with the reserved queue ranges, and the swap chain through Streamline's proxies
  (`VkResolveStreamlineDeviceProxies`: CreateSwapchain, DestroySwapchain, GetSwapchainImages, AcquireNextImage, QueuePresent). In
  manual hooking the interposer publishes `kVulkanTable` from `slSetVulkanInfo` (`sl.interposer/vulkan/wrapper.cpp:processVulkanInterface`),
  which is what `sl.fsr_g:acquireFgQueues` reads. So the plugin's queue lookup should work in native (inferred; not run).
  Present ID is off under Streamline (`CreateVulkanDevice`, `allowPresentId`).
- D3D12 (`rhi_dx12.cpp:CreateD3D12Device`): `slSetD3DDevice`, `slUpgradeInterface` on the factory and the device, queues made on the proxy
  device with native queues from `slGetNativeInterface` (`d_createQueue`).

### 2.2 Swap chain

| | D3D12 (`rhi_dx12.cpp`) | Vulkan (`rhi_vulkan.cpp`) |
|---|---|---|
| Create | `d_createSwapchain`: `CreateSwapChainForHwnd` on the **SL proxy factory** with the proxy graphics queue; flip sequential; waitable object and mode-switch flags, tearing flag when allowed. Images from the **native** swap chain's `GetBuffer` | `d_createSwapchain` → `VkCreateOrResizeSwapchain` through the SL proxy |
| Present | `sc_present`: waits the graph's timeline on the graphics queue, then `pSlProxySC->Present` (tearing flag when vsync off) | `sc_present`: relay submit (timeline wait → binary present semaphore, progress timeline), `vkQueuePresentKHR` through the proxy, latency wait (`VkWaitPresentedFrames`), then acquire. `VK_SUBOPTIMAL_KHR` and out-of-date return `ModeChanged` |
| Resize | `sc_resizeBuffers`: device idle, release images, proxy `ResizeBuffers`, images from native again | `sc_resizeBuffers` → `VkCreateOrResizeSwapchain`: **creates the new swap chain with `oldSwapchain` and destroys the old one after** |
| Vsync switch | Per present | Per present with `VK_EXT_swapchain_maintenance1` (`VkSwapchainPresentModesCreateInfoEXT` at create, `VkSwapchainPresentModeInfoEXT` at present); otherwise recreate and `ModeChanged` |
| Colour space | `SetColorSpace` (`SetColorSpace1` on the proxy) | `SetColorSpace` rebuilds the swap chain |
| Pacing | `WaitForFrameLatency` on the waitable object; `d_destroySwapchain` closes the handle | Present blocks on `SetMaximumFrameLatency` |
| Recreate from scratch | None (only `ResizeBuffers`) | None (only resize) |

### 2.3 CS's native present

- Creation: `NativeDevice.cpp:CreateDeviceAndSwapChain` (one swap chain on the game's window, tearing allowed, SDR, frame latency 2 via
  `D3D12Device.cpp:ConfigureSwapchain`).
- The back buffer is a graph-owned versioned slot (`NativeState::MakeGraphBackBuffer`, `NextGraphVersion`). The swap chain image is not a
  graph resource.
- Present (`NativeState::Present` → `QueuePresent` when `RenderGraphRuntime::QueuesPresent`): the render thread queues the present on
  the submission thread after the frame's epochs. There `PresentLocked` waits the D3D12 waitable object, takes `CurrentImageIndex`,
  and `PresentThroughLedger` copies the back buffer version into the swap chain image through ORG's external client
  (`RenderGraph::ExternalWork::Submit`), then `FinishPresent` calls `Swapchain::Present` with the copy's timeline point.
  `ModeChanged` or a non-loss failure sets `resizeRequested`; the next present calls `RecreateSwapchain` (`ResizeBuffers`).
  Device loss sets `lost` (nothing more is shown; the run reports device-lost).
- A loading screen's thread presents on its own thread and copies the current version (`PresentLocked`, the not-render-owner branch).
- HDR: `NativeDevice.cpp:SetSwapChainColorSpace`, called from `HDRDisplay::UpdateSwapChainColorSpace` on the render thread, **drains
  the submission thread** and waits for presents (a render-thread wait today).
- `Hooks.cpp:IDXGISwapChain_Present::thunk` turns the queued present off whenever frame generation is active (`queuedPresent` requires
  `!IsFrameGenerationActive()`), and then drains on the render thread.

### 2.4 Where native's frame generation inputs would come from

- The UI stage (`RecipeTable.h`, `StageId::Ui`) draws the UI over kFRAMEBUFFER; HDR output keeps its own ticket after it.
- The hudless copy is ORGUpscaling's FrameGeneration client with its own ticket after the post stage. It never runs in native
  (frame generation is off). Moving it and HDR output into the UI stage is plan piece A7 (`docs/development/org-native-frame-graph.md`,
  "Stayed").
- The screenshot's clean scene (`ORGScreenshot.cpp`) is the second client on that ticket. It already runs in native: a pending
  SDR capture copies kFRAMEBUFFER into a Persistent graph texture there, and the capture reads it from the graph. So native has
  a working HUD-free copy at the hudless point today. It registers inside the frame when kFRAMEBUFFER's size or format changes
  and skips frames until its build is scheduled (`FrameAnchors::Scheduled`).
- Pre-exposure reaches the upscalers through their SDK options (`Streamline.cpp`: `DLSSOptions::preExposure`, FSR through
  `FSROptions` version 2, XeSS through `sl.xess`'s exposure multiplier, Streamline `01dcbb1`). The options are set on the render
  thread from `globals::state->preExposure`, while the native evaluate records later on the submission thread (inferred:
  `EvaluateXeSS` calls `slXeSSSetOptions` before `cs_EvaluateFeatureCore` queues the pass).
- The upscalers' evaluate is already a pass recorded at execution, on the recording thread (native: the submission thread), with the
  images described natively (`Streamline.cpp:cs_EvaluateFeatureAsPass`, `ORGUpscaling::QueueEvaluate`, `NativeDevice.cpp:DescribeImages`).
  It refuses a hudless image and takes exactly four images. `cs_EvaluateFeatureCore` sends every native evaluate there, FSR-FG included.

### 2.5 SDKs in the tree

- Streamline: fork `community-shaders/Streamline`, branch `extra-plugins` (2.12.0 base: `changelog.txt`). CS's plugins `sl.fsr`,
  `sl.fsr_g`, `sl.xess`. Recent fork commits are by Panthuncia (`7555408 feat(plugins): FSR and XeSS on D3D12`), so the fork is
  changeable from this effort (inferred from authorship; confirm).
- FidelityFX: `extern/Streamline/external/fidelityfx-sdk` (FFX API: `ffxCreateContext` / `ffxConfigure` / `ffxDispatch` / `ffxQuery`,
  loaded at runtime with `ffx_api_loader.h`). Prebuilt signed `amd_fidelityfx_dx12.dll` and `amd_fidelityfx_vk.dll` in
  `PrebuiltSignedDLL`; the shipped VK DLL is rebuilt with the luminance fixes (`external/fidelityfx-bin`). `sl.fsr` already drives the
  DX12 DLL for upscaling (`sl.fsr/fsrDX12.cpp`). Not the older `ffxReplaceSwapchainForFrameinterpolation`.
- FFX DX12 swap chain (`ffx_api/dx12/ffx_api_dx12.h`): create kinds `WRAP_DX12` (wrap an existing `IDXGISwapChain4` + game queue),
  `NEW_DX12`, `FOR_HWND_DX12` (factory, hwnd, `DXGI_SWAP_CHAIN_DESC1`, game queue); `REGISTERUIRESOURCE_DX12` (UI texture mode);
  `WAIT_FOR_PRESENTS_DX12`; key-value pacing tuning. Its implementation
  (`sdk/src/backends/dx12/FrameInterpolationSwapchain/FrameInterpolationSwapchainDX12.cpp`) hands out **replacement back buffers**
  from `GetBuffer`, gives a **replacement waitable object** from `GetFrameLatencyWaitableObject` (it does not close it), forwards
  `SetColorSpace1` and `SetHDRMetaData` (and sets its transfer function from the colour space), forces flip-discard with 3 buffers and
  the waitable flag, supports `ResizeBuffers` (destroys its replacement resources first) and not `ResizeBuffers1`.
- FFX VK swap chain (`ffx_api/vk/ffx_api_vk.h`): `FGSWAPCHAIN_VK` create with three queues; replacement WSI functions from
  `FGSWAPCHAIN_FUNCTIONS_VK`. The plugin drops the create info's `pNext` (`createFgSwapchain`: "FFX rejects DXVK's extension chain").
- Streamline D3D12 swap chain hooks: the proxy factory runs plugin before-hooks on `CreateSwapChainForHwnd` and lets a plugin create the
  swap chain itself (`skip`), then wraps the result in its proxy (`sl.interposer/dxgi/dxgiFactory.cpp:DXGIFactory::CreateSwapChainForHwnd`).
  So a D3D12 `sl.fsr_g` can install FFX's swap chain the way the Vulkan one does.
- DLSS-G and manual hooking (`docs/ProgrammingGuideDLSS_G.md` sections 4.0, 16.0, 17.0, 18.0; `ProgrammingGuideManualHooking.md` 2.0):
  attaches only to the swap chain the host makes through the proxy; back buffers must come from the **proxy** swap chain (off-screen
  rendering); recreate the swap chain on every load/unload (`slSetFeatureLoaded` while none exists); mode off before any window change;
  `presentCommon` must run once per frame (proxy Present); Vulkan: present binary semaphore signalled by the host, acquire semaphore
  waited before the frame.

---

## 3. The gap

### 3.1 Common to all four

| Need | Today | Gap |
|---|---|---|
| Hudless colour, depth, motion vectors, FSR-FG prepare, DLSS-G tags as stage work | Guest: interop ring and own-epoch ticket. Native: nothing | A7 (hudless copy as a UI-stage step) plus a "frame generation inputs" pass recorded at execution after it |
| Hudless format = swap chain format | From DXVK's presenter (`Upscaling::CreateHudlessTexture`) | From the active presenter |
| "Is frame generation possible this frame" | DXVK checks in `Upscaling::IsFrameGenerationActive`, `PrepareFrameGeneration`, `Main_PostProcessing` | Presenter interface |
| Replace the swap chain at a boundary, with "torn down" in between | DXVK export + callback | Native: nothing (only `ResizeBuffers`) |
| Render thread never waits | DXVK's recreate runs in DXVK's presenter | Native `SetSwapChainColorSpace` drains; `Hooks.cpp` drains whenever FG is on |
| Errors fatal | Guest retries and defers | Native must not inherit the retry loops |
| Frame IDs | One prepared frame slot, matched at present by frameID (`fsrGEntry.cpp`) | Native records stages on the submission thread, which also presents, so frame N's prepare comes before present N and frame N+1's after (inferred from `ORGUpscaling.h:QueueEvaluate` and `NativeState::QueuePresent`). Holds as long as recording stays on that thread |
| Resources the SDK reads after the frame | Hudless read at present; depth and motion vectors read in the prepare | All are persistent textures (engine targets, `GraphResources::Texture2D` for the hudless), never frame-arena transients, and written only on the graphics queue. FFX (game queue, `allowAsyncWorkloads=false`) and DLSS-G (`eBlockPresentingClientQueue`) read them on that queue, so queue order protects them. Keep them out of the alias arena |
| Hudless layout at present | The plugin records the hudless layout at evaluate and declares it to FFX at present | The inputs pass must declare the hudless in the state it keeps until present (shader read), and nothing may move it in between |
| Exposure of the inputs | Hudless is taken after the chain's output, so the pre-exposure is already divided out (Composite under linear lighting, or the output copy's 1 / pre-exposure). FSR-FG's prepare and DLSS-G take no exposure. The upscalers take it per frame through their options | Keep the hudless point after the chain's output in A7. Give FG no exposure value. The upscalers' pre-exposure must be the value of the frame whose colour they evaluate: set the options with the frame's value at the evaluate pass, or carry it in the frame's token, not read live on the render thread (inferred race in native, 2.4) |
| Depth and motion vector extent | Render-size region of kMAIN's depth (or kMAIN_COPY) and kMOTION_VECTOR | With `frame-revision-plan.md` decision 6a, render-size textures are display-size and only the viewport changes. The inputs pass takes the render rect and the dynamic ratio from the adopted revision, not from the textures' size |
| Screen-space lens effects | The hudless includes Physical Lens's image mapping (distortion, chromatic split), ghosts and starburst, and Camera FX's distortion and grain. The engine's motion vectors describe the undistorted scene, and ghosts move against the light sources, not with the geometry. Guest has the same mismatch today | Decision 8 |
| Screenshot clean scene at the hudless point | Its own client on the FrameGeneration ticket, an SDR capture only, registered inside the frame on a size or format change | Moves into the UI stage with the hudless copy in A7. One copy or two: decision 9 |
| Replayed recordings | The FrameGeneration ticket is its own epoch, recorded each run | After A7 the hudless copy, the clean scene copy and the inputs pass are steps of the UI stage, a CS stage whose recordings are replayed (`frame-revision-plan.md` rule R9). The inputs pass is recorded at execution, so its prepared pass must be replayable: no submission or completion callback, no external signal. The frame token and the prepared-frame bookkeeping go through the commit and the latch, not through lifecycle effects. The open post-stage error ("A revision-driven epoch's recording is not replayable ...", being fixed now) is this rule failing in the post stage |

### 3.2 FSR-FG on native Vulkan

- SDK needs: wrap at `vkCreateSwapchainKHR`, three distinct queues (game, present, acquire), replacement WSI functions, a prepare on a
  command buffer, a configure at each present on the present thread, HDR metadata (plugin does it), hudless in the swap chain format.
- BasicRHI must:
  - Create a swap chain that a layer may wrap: no `pNext` chain at create (FFX drops it, so present-time `VkSwapchainPresentModeInfoEXT`
    would name modes the swap chain never declared), no present-mode switching (vsync change = recreate), destroy the old swap chain
    **before** making the new one. `VkCreateOrResizeSwapchain` overlaps them; `sl.fsr_g:createFgSwapchain` refuses while a wrapped swap
    chain still exists and its create hook then returns `VK_NOT_READY` (`slHookVkCreateSwapchainKHR`, `unsafeFallback`).
  - Keep `ModeChanged` on `VK_SUBOPTIMAL_KHR` (it does).
- ORG: nothing new (the graph present's copy into the swap chain image works on FFX's replacement images).
- CS must: load `kFeatureFSR_G` in native Vulkan; resolve its functions in `SetNativeDevice`; check the BasicRHI log line
  "the host's Streamline: ... queues: 3 graphics" (the union takes the maximum, and the plugin asks for 3); drive replacement from the
  controller; keep the queued present with frame generation on.
- Plugin (`sl.fsr_g`) must: let the host drive recreation (today it forces one by returning `VK_SUBOPTIMAL_KHR`). Proposed option
  `FSRFrameGenOptions::hostRecreatesSwapchain`.
- Tension: queue access. FFX submits its composition on the game queue from inside the present call through `FfxGameQueueSubmit`
  (no BasicRHI lock). In native, BasicRHI presents inside `VkQueueAccessGuard`, and CS installs one queue lock for every access
  (`CreateDeviceAndSwapChain`, `ShareVulkanDevice` with `LockQueue`). So FFX's submits run with that lock held by the same thread
  (inferred). FFX's own present and acquire threads use the reserved queues.

### 3.3 FSR-FG on native D3D12

- SDK needs: FFX's DX12 frame-interpolation swap chain (`FOR_HWND_DX12` or `WRAP_DX12`) on the game queue; app renders into FFX's
  replacement back buffers (`GetBuffer`), paces on FFX's replacement waitable object, presents through it; prepare and configure as on
  Vulkan with D3D12 resource states.
- Missing entirely: `sl.fsr_g` has no D3D12 backend. It needs a before-hook on `IDXGIFactory::CreateSwapChainForHwnd` that makes FFX's
  swap chain with the **base** factory and the native queue (so FFX's inner presents stay outside the interposer, while the host's one
  Present per frame still goes through the proxy and runs `presentCommon`), a Present before-hook that configures FFX, unlink on
  destroy and resize, and the prepare through `amd_fidelityfx_dx12.dll` (as `sl.fsr/fsrDX12.cpp`, in its own file because
  `ffx_api_dx12.h` cannot sit beside `ffx_api_vk.h`).
- BasicRHI must: make a new swap chain after destroying the old one (DXGI allows one flip-model swap chain per window; FFX wraps only at
  creation), and read images after every create. Native `GetBuffer` on the swap chain FFX made returns its replacement buffers, which
  is right; for DLSS-G it must be the proxy (3.4), so take images from the proxy in both cases.
- Waitable object: BasicRHI takes it from the proxy (`d_createSwapchain`), which forwards to FFX (inferred); FFX never closes its handle,
  so BasicRHI's `CloseHandle` in `d_destroySwapchain` is the only close. Fine.
- Tension: FFX DX12 creates its own async compute queue and present thread, and waits on its fences inside `Present` (inferred from
  `FrameInterpolationSwapchainDX12.cpp` fence setup). That runs on the submission thread in native, not the render thread.

### 3.4 DLSS-G on native D3D12

- SDK needs (SL guide): swap chain made through the proxy factory while `sl.dlss_g` is loaded; back buffers from the proxy; present
  through the proxy; `slSetFeatureLoaded` only with no swap chain; mode off before window changes and before teardown; Reflex markers;
  tags `eValidUntilPresent`; constants once per frame.
- BasicRHI must: images from the proxy swap chain (it uses the native one), recreate from scratch. The rest is in place.
- CS must: load `kFeatureDLSS_G` in native when the hardware probe passes; tags as a pass with D3D12 states; the torn-down callback
  between destroy and create calls `slSetFeatureLoaded`.
- Needs the RTX 5080. NVIDIA D3D12 had "no world" from the view-reference heap limit (memory note `d3d12-heap-limit-viewrefs`); check
  that it is fixed before testing DLSS-G there.

### 3.5 DLSS-G on native Vulkan

- As 3.4, plus: `VK_NV_optical_flow` device extension and queue (BasicRHI reserves optical-flow queues from the requirements already),
  binary semaphore rules (BasicRHI's relay signals the present semaphore; its acquire batch waits the acquire semaphore), present ID off
  (already). The swap chain needs the same "layer may wrap" rules as 3.2 (inferred: `sl.dlss_g` also replaces presentation).
- Needs the RTX 5080. `CS_STREAMLINE_DLSS=0` already exists because DLSS-G's plugin adds optical flow to every device on a machine with
  an NVIDIA GPU (`Streamline::Initialize` comment).

### 3.6 Tensions with the current design, summarized

| Design point | Tension | Resolution in this plan |
|---|---|---|
| The graph present step | None: the present copies the back buffer version into whatever image the swap chain hands out, FFX's replacement images included | Keep it |
| Back buffer as a graph-owned versioned slot | Lets the swap chain be replaced without any graph rebuild (size unchanged) | Swap chain replacement is not a graph event |
| Render thread never waits | Native colour-space change drains; FG presents on the render thread (`Hooks.cpp`) | Replacement runs on the submission thread after a present; keep the queued present with FG on |
| SDK-side waits | FFX waits inside its present and on its waitable object; DLSS-G blocks the presenting queue | All on the submission thread |
| Async submission | Recording and present share the submission thread in native, so the plugin's single prepared-frame slot holds | Document it as a contract; fatal check if a present finds a prepared frame from a frame other than its own |
| Frame alias arena | SDK reads inputs after the stage ends | Inputs are persistent textures |
| HDR10 switching | Vulkan FFX bakes HDR into its context and swap chain | Colour space is part of the presentation config; a change is a replacement |
| Tearing | FFX bakes the present mode | Vsync/tearing is part of the presentation config |
| D3D12 waitable object | FFX supplies a replacement | BasicRHI reads it again after every create |
| Device loss on the RX 6600 | Plugin latches `dispatchFaulted` and returns device lost | Native: fatal (report and end), no teardown loop |
| Replayed UI stage (A7) | The inputs pass carries per-frame state (frame token, prepared frame) into the plugin | Per-frame values through the commit and latch; the pass's packet stays replayable (3.1) |
| Lens and camera effects in the generated frame | Motion vectors do not describe them | Decision 8 |
| Upscaler options and frames in flight | Options, pre-exposure included, set on the render thread for an evaluate recorded later | The frame's values travel with the frame (3.1); `frame-revision-plan.md` section 11, 4.3 for the SDK contexts |
| Fog defects in the graph modes | Guest kMAIN goes non-finite with the fog's volumetric fog on, and native's fog volumes are empty (`dclf-open-defects.md`, "Graph-mode port gaps") | Not frame generation's, but a non-finite hudless corrupts the generated frames. Test FG with the fog's volumetric fog off until fixed, and say so in the run notes |

---

## 4. Options and recommendation

### 4.1 The options

**(a) Frame generation as a BasicRHI swap chain capability.** BasicRHI links FFX (and SL), offers "a swap chain that interpolates" at
creation, and a per-frame "describe this frame" call. CS supplies inputs.
- For: native is self-contained; BasicRenderer could use it.
- Against: guest cannot use it (DXVK owns the swap chain), so FSR-FG would have two implementations: `sl.fsr_g` in guest, BasicRHI in
  native. DLSS-G can only be done through Streamline anyway, which BasicRHI already routes the swap chain through. BasicRHI would carry
  product logic (menus, hudless policy) or a large input struct.
- Deletes nothing in guest. Generalises nothing.

**(b) CS on BasicRHI's native handles.** CS calls FFX directly with the native device, queues and swap chain in native.
- For: no SDK work in BasicRHI.
- Against: again two FSR-FG implementations; CS then owns DXGI/WSI interception that Streamline's interposer already does; native and
  guest diverge in the controller.

**(c) One presenter interface, two backends.** `FrameGen::Presenter` in CS with `DxvkPresenter` and `NativePresenter`.
`FrameGen::Controller`, the inputs pass and the present path have one code path.
- For: matches "one interface with two backends". The frame generation runtime stays where it is: Streamline plugins, which see either
  presenter through the interposer.
- Against: some DXVK-only knobs (sync present, queue depth) must hide inside the DXVK backend.

### 4.2 Recommendation

**(c), with Streamline as the frame generation runtime on every backend, and BasicRHI gaining only generic swap chain capabilities.**
1. `sl.fsr_g` gets a D3D12 backend. FSR-FG then has one implementation for guest Vulkan, native Vulkan and native D3D12.
2. BasicRHI learns "a presentation layer may wrap this swap chain" and "replace" (destroy, then create). Nothing FFX-specific.
3. CS gets `FrameGen::Presenter` (DXVK backend, native backend). The controller is written once against it.
4. The inputs (hudless copy, FSR-FG prepare, DLSS-G tags) are stage passes in every graph mode. Plain D3D11 keeps the interop ring.
5. A presentation change (frame generation method, HDR, vsync/tearing) is a variable whose change replaces the swap chain on the
   submission thread at a present boundary. The render thread adopts the result at a later Begin.

Why not (a): it duplicates `sl.fsr_g` for one mode and cannot serve guest. Why not (b): same duplication, in CS.

What it deletes or generalises:

| Code | Fate |
|---|---|
| `Streamline::SetNativeDevice` hard-off lines, `Initialize`'s `!native` gates | Deleted |
| `FrameGen::Controller` DXVK calls (`RequestDxvkSwapchainRecreate`, `PushDxvk*`, `DXVKInterop::WaitDeviceIdle`, `DrainCommandRing`) | Behind `Presenter`; controller generalised |
| `Upscaling::IsFrameGenerationActive`, `CreateHudlessTexture`, `PrepareFrameGeneration`, `Main_PostProcessing` DXVK checks | `Presenter::Surface()` / `Ready()` |
| FG's interop-ring evaluate (`cs_EvaluateFeatureCore` FSR_G branch), DLSS-G view wrapping (`TagDLSSGResources`), `cs_CanReleaseFailedFSRFrame`, FG deferred releases | Plain D3D11 only |
| FrameGeneration own-epoch ticket, the own-epoch tail in `FrameAnchors::Run`, `EpochOf`, `Placed` epochs (with HDR output) | Deleted by A7 |
| `Hooks.cpp` FG exceptions to the queued present | Deleted |
| `NativeDevice.cpp:SetSwapChainColorSpace`'s render-thread drain | Replaced by the presentation replacement |
| Guest fault-teardown retry loops | Fatal (open question 6) |
| `sl.fsr_g`'s SUBOPTIMAL recreate trigger | Off when the host drives recreation |

---

## 5. Steps

Every step leaves the tree building and `guest-quick`, `native-quick`, `native-d3d12-quick`, `d3d11`, `hdr`, `menu` passing.
Order: my recommendation differs from the prior (FSR-FG native D3D12 first). **Native Vulkan first**: the plugin exists, runs the same
FFX Vulkan backend that already works in guest on the RX 6600, and BasicRHI's Vulkan path already presents through the plugin's hooks.
D3D12 needs new plugin code. Steps 6 and 7 are independent after step 5; swap them if native D3D12 is the backend that ships first
(open question 3). A7 stays early.

### Step 1: A7, the hudless copy and HDR output as UI-stage steps
- Purpose: every path needs the hudless copy as stage work; removes the last own-epoch tickets.
- Scope: `RecipeTable.h`: UI stage gets `{ .anchor = Anchor::FrameGeneration, .clients = { "cs.upscaling.hudless" } }` as its first step
  (the hudless is taken where `Main_PostProcessing` takes it today: after the post chain, before the screen blood) and an HDROutput step
  after Overlay; drop post's `Ticket(Anchor::FrameGeneration, ...)` and the UI stage's `commitBefore` on `AnchorHDROutput`.
  `ORGUpscaling.cpp:RegisterHudless` stops opening its own epoch; `ORGHDRDisplay` likewise. Delete `FrameAnchors::Run`'s own-epoch tail,
  `EpochOf`, the late-epoch hook and the `Placed` epochs once unused (`org-native-frame-graph.md`, "Stayed").
  The FrameGeneration ticket now holds two clients: the step names `"cs.upscaling.hudless"` and `"cs.screenshot.clean"`, and
  `ORGScreenshot.cpp`'s clean copy stops opening its own epoch too. Both copies read kFRAMEBUFFER after the chain's output, where
  the pre-exposure is already divided out; keep that point. The UI stage replays its recordings, so both copies are gated per run
  from the commit (as the clean copy's `RunGate` already is) and have no lifecycle effects (`frame-revision-plan.md` rule R9).
- Tests: `guest-quick`, `native-quick`, `native-d3d12-quick`, `hdr` (all four modes), `menu`, guest `fg` (FSR-FG still engages; hudless
  without HUD). A screenshot in native and in guest, checking the log for the clean scene being used (`HasFreshCleanScene`), not
  the D3D11 fallback. Census: no own-epoch mismatch, no replay error.
- Acceptance: no anchor runs an epoch of its own in either graph mode; `fg` shots match the last guest run; screenshots are
  HUD-free in both graph modes.
- Risks: HDR output's order against the overlay and capture (the HDR black-frame note in `parity/status.md`). The UI stage's
  replay (R9): a copy gated per run through a callback would fail it, as the post stage fails today.
- Size: M. NVIDIA: no.

### Step 2: `FrameGen::Presenter`, DXVK backend only
- Purpose: one interface before a second backend exists. No behaviour change.
- Scope: new `src/Features/Upscaling/FramePresenter.h`:
  ```cpp
  namespace FrameGen {
      struct PresentationConfig { Method method; bool hdr; bool vsync; bool tearing; };
      struct SurfaceState { rhi::Format format; bool hdr10; uint64_t serial; bool ready; };
      class Presenter {
      public:
          virtual bool Available() const = 0;                       // frame generation can run on this presenter
          virtual SurfaceState Surface() const = 0;                 // what the present writes into, for this frame
          virtual void RequestConfig(const PresentationConfig&) = 0; // replace at the next present boundary
          virtual uint64_t AdoptedSerial() const = 0;               // last config the swap chain has
          virtual void SetTornDown(std::function<bool()>) = 0;      // runs between destroy and create
      };
      Presenter& ActivePresenter();
  }
  ```
  `DxvkPresenter.{h,cpp}` wraps today's calls: `RequestDxvkSwapchainRecreate`, `PushDxvkSyncPresent`/`QueueDepth`/`TearingPreference`
  (inside its transition), `DXVKInterop` surface state, `RegisterDxvkSwapchainCallbacks`. Edit `FrameGenController.cpp`, `Upscaling.cpp`
  (`IsFrameGenerationActive`, `CreateHudlessTexture`, `PrepareFrameGeneration`, `Main_PostProcessing`, `PresentWithFrameGeneration`),
  `Streamline.cpp`, `HDRDisplay.cpp` (`UpdateSwapChainColorSpace`'s DXVK part). The controller compares desired and adopted configs
  instead of tracking DXVK phases by hand.
- Tests: `guest-quick`, `fg` (guest and d3d11), `hdr`, `menu`.
- Acceptance: same `[FrameGen]` sequence and presented multiplier as before; no new DXVK call outside `DxvkPresenter`.
- Risks: the DXVK knobs' order is measured and fragile (`StepPhaseCompletion` comments). Move them as a block.
- Size: M. NVIDIA: no (DLSS-G guest regression check on the 5080 when convenient).

### Step 3: the frame generation inputs pass (FSR-FG), shared by guest and native
- Purpose: one delivery path in graph modes.
- Scope: `ORGUpscaling::QueueFrameGenInputs(std::string label, InputsRecord)` with three images (depth, motion vectors, hudless), recorded
  at execution as the step after the hudless copy, declaring the hudless in shader-read state. `Streamline.cpp`: `cs_RecordFrameGenInputs`
  (constants on viewport 1, tags, `slEvaluateFeature(kFeatureFSR_G)` on the native list; D3D12 states and Vulkan views as in
  `cs_EvaluateFeatureAsPass`). `Streamline::EvaluateFSRFrameGen` uses it in graph modes; the ring path stays for plain D3D11.
  Plugin contract written in `fsrGEntry.cpp`: a present that finds a prepared frame for another frame ID is a host error, logged as an
  error and fatal in native (inferred safe given 3.1).
  Inputs: depth and motion vectors with the render rect of the frame (from the adopted revision once `frame-revision-plan.md`
  step 8 lands; the textures are display-size then); no exposure value. The pass is recorded at execution inside a replayed UI
  stage, so its prepared pass must be replayable: the frame token and the constants come from the commit (latch), and the plugin
  learns the frame from the token, not from a submission callback.
- Tests: guest `fg` (background), `d3d11`+`fg` (ring path unchanged), `guest-quick`.
- Acceptance: guest FSR-FG presents about 2x as before (`[Streamline] FSR-FG presenting 2 frame(s)`, driver presents from
  `CS_VK_DRIVER_TRACE`); no interop ring command buffer for FG in guest.
- Risks: the "external Vulkan work after a graph epoch" hazard in guest (`Upscaling::SeparateFromGraphEpochs`); the pass path restores
  the graph's descriptor heaps, as the upscale evaluate does. A pass recorded at execution that is not replayable makes the UI
  stage's recording unkeepable and stops the game in a revision (the post stage's open error). Check with a `fg` run that the
  census shows no "not replayable" count for the UI stage.
- Size: M. NVIDIA: no.

### Step 4: BasicRHI, swap chains a presentation layer may wrap
- Purpose: the generic capability both FFX and DLSS-G need.
- Scope (BasicRHI `dev`, submodule first): `rhi::SwapchainDesc { hwnd, width, height, format, bufferCount, allowTearing,
  SwapchainColorSpace colorSpace, bool presentationLayer; }` and `Device::CreateSwapchain(const SwapchainDesc&, SwapchainPtr&)` (old
  overload kept). With `presentationLayer`:
  Vulkan: no `pNext` on create, no maintenance1 present-mode switching (vsync change → `ModeChanged` after recreate), destroy before
  create in `VkCreateOrResizeSwapchain`, colour space at create. D3D12: images through the proxy `GetBuffer`, waitable object and
  current index read again after every create and resize. `tests/` with `StreamlineStubs.cpp`: destroy-before-create order, images re-read.
- CS: `CreateDeviceAndSwapChain` moves to the desc overload, flag off.
- Tests: BasicRHI unit tests; `native-quick`, `native-d3d12-quick`, `hdr`, `resize`.
- Acceptance: no behaviour change with the flag off; unit tests cover the flag.
- Size: M. NVIDIA: no.
- Done 2026-11-19 (branch `org-native/fg-presentation`, BasicRHI `org-native/presentation-layer`). The device takes the desc
  at abi 17 (`createSwapchainFromDesc`); `Caps().presentationLayer` reports the flag. With the flag, Vulkan also leaves
  `VkPresentIdKHR` out of the present and reports no present wait. `BasicRHIPresentationLayerSmoke` covers both backends:
  on Vulkan the test executable is the host's Streamline interposer and records every swapchain create and destroy; on
  D3D12 `StreamlineStubs.cpp` gained a pass-through mode whose native swapchain counts `GetBuffer`. Writing it found a
  D3D12 bug: swapchain destroy and resize waited for the GPU through `Dx12Device::self`, which is never set, so they never
  waited (a resize after presents raised the debug layer's corruption exception). Fixed in the same commit.

### Step 5: `NativePresenter` and presentation replacement on the submission thread
- Purpose: the native backend of step 2; the tactical form of the change-system rule.
- Scope: `NativeDevice.cpp`: `Native::RequestPresentation(PresentationConfig, std::function<bool()> tornDown)` posts one entry with
  `RenderGraphRuntime::QueueOnSubmitThread` after the current queued present. It waits presents (`WaitPresents`), destroys the swap
  chain, runs `tornDown`, creates the new one with the step 4 desc (`presentationLayer` when a method is set), configures latency and
  HDR metadata, and publishes the adopted serial (atomic). Latest wins: an entry superseded before it runs does nothing. Failure is
  fatal (`stl::report_and_fail` naming the config). `SetSwapChainColorSpace` becomes a `RequestPresentation` with the new colour space
  (removes its render-thread drain). `Hooks.cpp`: keep the queued present with frame generation on; `pacedByCS` asks the presenter.
  `src/Features/Upscaling/NativePresenter.{h,cpp}` implements the interface; `Surface()` reads the adopted config.
  The render thread reads the adopted serial at `Frame::Driver::OnBegin` (through `AtNextBegin` or a direct read) and only then turns
  the inputs pass on or off. Frames recorded for the old config present on the new one harmlessly: FFX passes an unprepared frame
  through, and a prepare without a wrapped swap chain is rejected by the plugin.
- Tests: `native-quick`, `native-d3d12-quick`, `hdr` (native colour space now through replacement), `resize`, `menu`.
- Acceptance: HDR toggles in native with no render-thread drain (Tracy: no `DrainSubmissions` from HDR); `[ORG native]` logs one
  replacement per change.
- Risks: the loading-screen thread presenting during a replacement (it takes `presentLock`; the entry must take it too).
- Size: M. NVIDIA: no.
- Done 2026-11-19 (branch `org-native/fg-presentation`). `Native::RequestPresentation` and `Native::Presentation()` /
  `FramePresentation()` (the snapshot `Native::OnFrameBegin` takes in `Frame::Driver::OnBegin`); the entry runs under
  `presentLock` and logs `[ORG native] presentation replaced (request N): ...`. `SetSwapChainColorSpace` is a request.
  `FrameGen::NativePresenter` implements the step 2 interface; `FramePresenter.h` is written here (see the open question
  below). `Hooks.cpp` keeps the queued present with frame generation on in native, and `pacedByCS` reads the frame's
  presentation. The engine's `SetMaximumFrameLatency` now takes `presentLock`, since a replacement swaps the swap chain.

### Step 6: FSR-FG on native Vulkan
- Scope:
  - Streamline fork: `FSRFrameGenOptions::hostRecreatesSwapchain` (no SUBOPTIMAL trigger, no bootstrap recreate); keep everything else.
  - CS: `Streamline::Initialize` loads `kFeatureFSR_G` in native Vulkan; `SetNativeDevice` resolves its functions;
    `IsFSRFGSupported` true there. `Controller` uses `RequestPresentation` with a torn-down callback that calls
    `slFSRFrameGenerationCompleteSwapchainTeardown` and `slSetFeatureLoaded`.
  - Pacing: the limiter via `slReflexSleep` and `GetRenderedFrameRateLimit` as in guest; measure whether BasicRHI's latency wait
    (progress timeline) plus FFX's present give a bounded queue (inferred to be enough; DXVK needed depth 2).
- Tests: new `native-fg.toml` = `extends = ["native-vulkan", "fg"]` (fix `fg.toml`'s comment), background (FSR-FG generated without focus
  in guest); `native-fg` + `hdr` steps; `--set` PresentMon count (below). Foreground eye check, with the user's say-so: HUD not ghosting,
  pacing, menu and loading-screen passthrough. Also a `native-fg` run with Physical Lens's ghosts and starburst and Camera FX's
  distortion and grain on, then off, for decision 8. Keep the fog's volumetric fog off in FG runs until the fog defects are fixed
  (3.6).
- Acceptance: `[Streamline] FSR-FG presenting 2 frame(s)`; plugin `totalPresentedFrames` about 2x CS's `[Perf] rendered` rate; PresentMon
  presents about 2x; no device loss in a `native-fg` run on the RX 6600 with `DXVK_DEBUG=hang`.
- Risks: `VK_NOT_READY` from the plugin if anything still overlaps swap chains; queue reservation (check the BasicRHI line); AMD
  driver quirks (memory notes on RX 6600 miscompiles).
- Size: L. NVIDIA: no.

### Step 7: FSR-FG on native D3D12
- Scope:
  - Streamline fork: `sl.fsr_g` D3D12. `fsr_g.json` lists `d3d12` and DXGI hooks (`eIDXGIFactory_CreateSwapChainForHwnd` before,
    `eIDXGISwapChain_Present` before, `eIDXGISwapChain_ResizeBuffers`, `eIDXGISwapChain_Destroyed`). New `fsrGDX12.cpp`:
    `FOR_HWND_DX12` with the base factory and the native queue, `FRAMEGENERATION_PREPARE` on the D3D12 list, configure per present with
    the hudless's D3D12 state, `WAIT_FOR_PRESENTS_DX12` and unlink at destroy. `slOnPluginStartup` stops refusing D3D12.
  - CS: load `kFeatureFSR_G` on D3D12; the inputs pass already gives D3D12 states (step 3).
  - BasicRHI: nothing beyond step 4 (check the replacement waitable object is read again after create).
- Tests: `native-d3d12-fg.toml` = `extends = ["native-d3d12", "fg"]`; with `hdr`; foreground eye check.
- Acceptance: as step 6 on D3D12; D3D12 InfoQueue clean with `--set org.validation=true` for a short run.
- Risks: FFX DX12 creates its own compute queue and present thread; DRED on hangs (`DXVK_DEBUG=hang` turns on crash markers in native).
- Size: L. NVIDIA: no.

### Step 8: DLSS-G in native (D3D12, then Vulkan)
- Scope: load `kFeatureDLSS_G` in native when `ProbeDLSSGHardware` passes; DLSS-G tags in the inputs pass (move `TagDLSSGResources`'s
  graph-mode path there); mode off before `RequestPresentation`; `slSetFeatureLoaded` in the torn-down callback; images through the
  proxy (step 4). Vulkan: optical-flow queue and semaphores (already in BasicRHI), check `presentCommon` runs every frame.
- Tests: `native-d3d12-fg` and `native-fg` with `do = "fg", arg = "dlss"`, foreground on the RTX 5080 only.
- Acceptance: `[Streamline] DLSS-G status OK (presenting 2 frames...)`, no `getCurrentBackBufferIndexNotCalled` or
  `reflexNotDetectedAtRuntime` status.
- Risks: untestable on the daily machine; the NVIDIA D3D12 heap limit.
- Size: L. NVIDIA: yes.

### Step 9: the frame-revision system
- When that plan's variables land (its step 5): `PresentationConfig` becomes a variable written from the settings event and from HDR's
  display check. Its member in the prepared set is "the swap chain has this config" (produced on the submission thread by step 5's
  entry) plus the inputs pass's presence. Adoption at Begin replaces step 5's serial read.
- Mapped onto `frame-revision-plan.md` (written 2026-11-18, extended 2026-11-19):
  - `PresentationConfig` is a row of its step 5b table, next to `HdrEnabled`, `HdrOutputTarget` and `UpscalerOptions` (its
    section 11, 4.3). The hudless texture is its migration 6.14 (on `HdrEnabled` and the presenter format). The swap chain
    colour space switches inside adoption for the revision whose content matches it (its section 11, 4.1). Step 5's replacement
    is that change for native: the submission thread's entry is the fragment, and the revision seals when it reports the
    adopted serial.
  - The hudless copy, the clean scene copy and HDR output are UI-stage steps after this plan's step 1, so they are ordinary
    revision members, and that plan's 7b shrinks to nothing (its Q10).
  - Upscaler options (mode, output size, pre-exposure) are read from the adopted revision and the frame's own values, not from
    live settings and `globals::state` at the render thread's call (3.1).
  - Its rule R9 and open question Q11 (readbacks against replay) apply to the UI stage and the post stage. The inputs pass must
    not add a readback.
- Size: S. NVIDIA: no.

### Testing generated frames in native
- Count, backend-neutral: PresentMon. `tools/pacing/capture-pacing.ps1` and `pmstats.awk` exist. Add a cs-test option (`--set
  fg.presentmon=true`) that runs PresentMon on the game's process for the step's window and writes presents per second and the
  display-change cadence (`MsBetweenDisplayChange`, as the pacing README says) next to `[Perf] rendered`. FFX's and DLSS-G's generated
  frames are real presents, so the ratio is the multiplier.
- Count, in process: `slFSRGetFrameGenState.totalPresentedFrames` (FSR-FG) and `DLSSGState.numFramesActuallyPresented` (DLSS-G), logged
  every second with the `[FGTEST]` line (`Upscaling::BeginRenderFrame`, marked TEMP-FG-TEST; make it permanent and terse).
- See: FFX's `onlyPresentGenerated` (`fgShowOnlyGenerated`) shows only generated frames. A cs-test step can grab the window's monitor
  region with DXGI desktop duplication while it is set. This needs the window unoccluded (inferred), so a foreground run.

---

## 6. Open questions

**Questions 1 to 7 were decided by the user on 2026-11-18: option (a) for each.** The options are kept below for the record. Questions 8 to 10 are open.

| # | Decision |
|---|---|
| 1 | DLSS-G in native is in scope. It comes last, in a session on the RTX 5080. |
| 2 | FSR frame generation on D3D12 goes through Streamline. `sl.fsr_g` gets a D3D12 backend. |
| 3 | Native Vulkan comes first, native D3D12 second. |
| 4 | Frame generation with HDR is required in native for the first version. |
| 5 | One long frame when frame generation is toggled is acceptable. |
| 6 | Guest moves onto the shared interface in the same effort and loses its fault-teardown retries. Faults are fatal in every mode. |
| 7 | The tactical swap chain replacement (step 5) lands before the frame-revision system and is migrated in step 9. |

**Steps 4 and 5, decided by the agent, to confirm (2026-11-19).**

| # | Decision | Why |
|---|---|---|
| 11 | `FramePresenter.h` is written in step 5 as step 2 gives it (plus default member values and a virtual destructor). Only the native backend exists; `ActivePresenter()` is declared, not defined, and the controller does not use the presenter yet. Step 2 adds `DxvkPresenter`, defines `ActivePresenter()` and moves the controller. | Step 2 was not done when step 5 ran; this keeps step 2's scope whole and avoids a stub definition it would have to delete |
| 12 | Every native colour space change is a full replacement (destroy, create), with or without frame generation. | One path, the one frame generation needs; the cost is one long frame per HDR toggle |
| 13 | `Native::SetSwapChainColorSpace` returns whether the output takes HDR10 (cached at the last replacement), not whether the change applied. The `[ORG native] presentation replaced` line reports what was made. | The change is asynchronous now |
| 14 | No HDR mastering metadata is set at a replacement. | As before in native and in guest (HDR Display: "Do NOT set HDR10 static metadata") |
| 15 | `PresentationConfig::vsync` does not drive the replacement yet; presents still choose vsync per frame. With a presentation layer on Vulkan a vsync change recreates inside `Present` (`ModeChanged`) without the torn-down step. Step 6 must make a vsync change a replacement while frame generation is on. | Nothing wraps the swap chain before step 6 |
| 16 | The render thread takes the presentation with a direct read at `Frame::Driver::OnBegin` (`Native::OnFrameBegin`), not `AtNextBegin`. | A read of one published value, no work to defer |

**Added 2026-11-19 from the hard ports.** Decided the same day.

| # | Question | Options | Recommended |
|---|---|---|---|
| 8 | **Decided 2026-11-19: (a), accept.** Documented in `dclf-validation.md`, "Bugs found by parity". Physical Lens's image mapping, ghosts and starburst, and Camera FX's distortion and grain are in the hudless. The motion vectors describe none of them. How should generated frames treat them? | (a) accept, as guest does today; (b) the inputs pass warps the motion vectors by the lens's image mapping (`ChromaticMapping`), so distortion matches, ghosts and grain stay as they are; (c) draw the lens overlays and grain after the hudless point, so the hudless-difference mode treats them as UI (changes the chain's order and the look) | (a) for the first version, with the step 6 A/B run to see how bad it is; (b) if distortion shows |
| 9 | **Decided 2026-11-19: (a), keep two copies.** The hudless copy and the screenshot's clean scene copy read the same image at the same step. One copy or two? | (a) two copies, as now (the clean copy runs only on frames with a pending SDR capture); (b) one copy when SDR and the formats match, two otherwise | (a): the clean copy is rare and its texture follows kFRAMEBUFFER's format, the hudless follows the presenter's |
| 10 | **Decided 2026-11-19: (a), set the options when the evaluate pass records.** The event-driven work (frame-revision-plan.md) will change this path again. Upscaler options (pre-exposure, mode, output size) are set on the render thread while native records the evaluate later (inferred). Fix it in this effort? | (a) yes: the evaluate pass sets the options with the frame's own values when it records; (b) leave it to `frame-revision-plan.md` section 11, 4.3 | (a), after a run confirms the mismatch (log the pre-exposure set and the one recorded, per frame) |


1. **Is DLSS-G in native in scope?**
   (a) yes, last, in a session on the RTX 5080; (b) not now: build the interface so it fits, implement later; (c) D3D12 only.
   *Recommended: (a). The architecture is the same; only step 8 needs the card.*

2. **FSR-FG on D3D12 through Streamline or directly?**
   (a) give `sl.fsr_g` a D3D12 backend (one FSR-FG implementation everywhere; needs the Streamline fork to be ours to change);
   (b) call FFX's DX12 swap chain from CS or BasicRHI in native D3D12 only.
   *Recommended: (a).*

3. **Which native backend first?**
   (a) Vulkan (plugin exists, least new code); (b) D3D12 (if it is the backend that ships).
   *Recommended: (a), unless D3D12 is the shipping native backend.*

4. **Is frame generation with HDR required in native for the first version?**
   (a) yes; (b) SDR first, HDR in a later step.
   *Recommended: (a). Colour space is part of the same replacement, and guest already supports it.*

5. **Is a hitch acceptable when frame generation is toggled?** The swap chain is destroyed and created on the submission thread between
   two presents. The window keeps the last image (inferred), so no black frame is expected, but one long frame is.
   (a) yes, one long frame; (b) no, it must be seamless (would need two swap chains on one window, which DXGI flip model forbids).
   *Recommended: (a).*

6. **Should guest move onto the shared interface in the same effort, and lose its fault-teardown retries?**
   (a) yes: steps 2 and 3 move guest; frame generation faults become fatal in every mode; (b) move guest, keep its retries for now;
   (c) leave guest as it is and build native beside it.
   *Recommended: (a).*

7. **Land the tactical replacement before the frame-revision system?**
   (a) yes, step 5 as written, migrated in step 9; (b) wait for the frame-revision variables first.
   *Recommended: (a).*

---

## 7. How to work in this repo

- Worktree: `C:\Users\matth\source\repos\SARP\external_refs\org-port-worktrees\test-runner`. CS branch `org-native/phases`, remote
  `panthuncia/skyrim-community-shaders`.
- Submodules on their `dev` branches of the `panthuncia` remotes: `extern/OpenRenderGraph`, `extern/BasicRHI`,
  `extern/SceneWriterFramework`. Commit in the submodule first, push, then bump it in CS. `extern/Streamline` is
  `community-shaders/Streamline`, branch `extra-plugins` (confirm you may push there, open question 2). `extern/dxvk` is read-only, always.
- Build: in a vcvars64 shell, `cmake --build --preset=Perf-Fast`. `BuildRelease.bat` fails at DXVK setup. Streamline plugins build with the
  fork's own scripts (`extern/Streamline/build.bat`); check how CS stages the plugin DLLs before changing them.
- Stage: `python tools/cs-test/cs_test.py stage . --name nfg --preset Perf-Fast`.
- Run: only through `tools/cs-test`. Background by default (`CS_TEST_BACKGROUND=1`). One launch per shell call. Never raise a timeout to
  make a run pass; find the cause. `DXVK_DEBUG=hang` on the RX 6600 (it also turns on native crash markers). Never arm RGD crash
  capture around a run. No synthetic OS input.
- Frame generation and focus: FSR-FG generated without focus in guest runs; DLSS-G needs the foreground. Foreground runs need the user's
  say-so. Generated frames cannot be captured by the harness today (section 5, "Testing generated frames").
- MO2 instance `C:\Modding\MO2`: keep `+CS AB nfg`, `+Start On Save`, `*TerrainHelper.esp` in the modlist.
- Commits: conventional, `type(scope): description`, title at most 50 characters, body wrapped at 72, no attribution lines.
- Comments: one line, two at most, plain language, only the why. No `:` `;` or chained commas in comments. No TODO/FIXME.
- Docs of record: `docs/development/org-native-frame-graph.md` (design and change log) and `docs/development/parity/status.md` (the
  "Frame generation in native" entry). Update both in the step that changes them.
- Existing configs to reuse: `guest-quick`, `native-quick`, `native-d3d12-quick`, `d3d11`, `hdr`, `fg`, `menu`, `resize`. New ones
  proposed: `native-fg` (step 6), `native-d3d12-fg` (step 7).
