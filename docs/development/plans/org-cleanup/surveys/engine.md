# Engine-side survey (condensed, agent a4228551, HEAD b1b39842d)

## Ownership today
- CS already makes memory: EngineTargets detours 4 create fns (EngineTargets.cpp:1055-1058); ORG images, fields get DXVK exports (guest) or native handles; versioned slots; tripwire handles (UseHandles :325); TakeOver (:1081) remakes first D3D11 set.
- Not owned: timing (engine BSShaderRenderTargets::Create; 2nd set fatal at BeginSet :1065) and shapes (engine props + PlanTarget overrides).
- Off ORG (CanOwnColour :503): kFRAMEBUFFER (guest = DXVK back buffer via RTV->GetResource, FrameAnchors.cpp:4202); mip-view/mip-gen targets. VL 3D volumes made mid-frame (MakeVolume :894).
- Native: back buffer a CS slot, present copies to swapchain (NativeDevice.cpp:1908-1980); unused targets unbacked from hand table (Frame/EngineTargetUse.cpp). Guest backs all.
- Blockers: (1) fields read right after Create by globals::ReInit, State::Setup (Hooks.cpp:925), engine next draw; (2) guest handles forward to fixed DXVK object, no retarget (native has Rename/CurrentOf); (3) engine sizes outside fields (props, BSGraphics::State screen size, viewports); (4) native wraps 19-75 ms each (Q2); (5) features mutate fields: HDR PlanTarget/Recreate/Restore + kFRAMEBUFFER RTV -> UI texture; Deferred Recreate (Deferred.cpp:90); Skylighting lends slot (Skeleton.cpp:1222); (6) 176 renderTargets[ + 98 depthStencils[ reads, 105 EngineTarget/Depth/Cubemap imports, linear Targets::Find (:1269).
- Engine-owned still imported / worth owning: guest back buffer; HDR UI texture "hdr.ui" (4 files); VL volumes; water flow map raw ptr as variable value; engine pooled CBs (captured per draw, KeptWrites); 64-query occlusion ring. Shadow maps, reflection cube, asset textures, VB/IB already ORG.

## (a) Weaknesses
1. Mid-session target set fatal; window drag kills game [L Q4, item 53]. EngineTargets.cpp:1061; WndProc guard skips size lock during drag (Upscaling::s_windowModifying, Hooks.cpp:1094). Interim S: SWP_NOSIZE during drag in graph modes. Direction L: CS-owned EngineTargetSet: Create thunk records props, raises DisplaySize + shape vars, never makes/releases after first; lane makes versions; at adoption repoint field handles (new tripwire retarget op) and write engine size state; State::Setup not rerun; volumes declared at set from INI; targets no declaration reads never made in both modes (replaces unbacked table).
2. Guest back buffer + HDR UI redirect [N]: kFRAMEBUFFER found via RTV per import; HDR swaps kFRAMEBUFFER RTV; Scaleform (Passes.cpp:921), Bink (BinkGraph.cpp:447), ImGui (ImGuiRenderer.cpp:134) each undo. Dir M: CS scene back buffer slot in both modes, present-time copy into DXVK/ORG swapchain; HDR UI texture a declared target.
3. Feature remakes/slot lending [N]: HDR (HDRDisplay.cpp:1556,1619), Deferred D3D11 field-swap reachable in graph mode -> OutsideAdoption. Dir S: plans only; remove Recreate/Restore in graph modes.
4. Native wraps block render thread [L Q2]: WrapTexture immediate submission + wait.
5. Flush latch only logs withheld engine draw (Flush.cpp:682-716) [N]. Dir S: TapError::EngineDraw fatal in dev.
6. Non-draw residue ungated [N; skeleton L item 66]: copies/clears/maps/updates/queries reach DXVK in guest; native immediate context host-waits on render thread (NativeDevice.cpp:662,1023,3680,3724,3840). Skeleton clears/copies to D3D11 (Skeleton.cpp:1212,1513); Bink Queue false -> engine draw; occlusion queries outside bracket on D3D11; KeptWrites maps DXVK buffers at stage window close. Dir M-L: census -> tap error; engine CB pools become ORG buffers (like EngineBuffers VB/IB), KeptWrites goes.
7. ImageSpace follows observed state [N]: EngineImageSpace polled every Begin incl raw flow-map ptr + effect counts (ImageSpace.cpp:1533-1572,2176); kRequestFrames=300 hold timer (WorldSteps.inl:251, ChainSteps.inl:562,1442); lagging step draws nothing (NoteLaggedWork); draws matched by occurrence counters (lumLevel++, slotDraws[]++). Dir M: steps from description fields read once at Begin; shape facts from creation-hook events; delete requested[]; mismatch fatal.
8. ImageSpace compiles on render thread [N; item 35 analogue]: GetNow at graph build (ImageSpace.cpp:1249), CaptureProgram (ChainSteps.inl:687). Dir S: queue at Setup, GetNow fatal.
9. Setup failures fall back to D3D11 [L C3]: ImageSpace off(...) (ImageSpace.cpp:2079); EngineTargets "made on D3D11 again" (:648); ImGui FallBack (ImGuiRenderer.cpp:281); 10 Skeleton sites; OcclusionQueries, EngineBuffers, Bink. Dir S: fatal.
10. ImGui reads bound target via OMGetRenderTargets (ImGuiRenderer.cpp:96) [L 9b partly]; latch may drop; new target = frame w/o overlay. Dir: overlay draws into declared target.
11. Presentation branches on mode [N]: PresentationRevision.cpp 4 branches; DXVK not reporting replacements -> serial 0 -> adopts before swapchain changes; native Resize HostWait(…,2000) (NativeDevice.cpp:2004); present "copies back buffer by hand" fallback (:1947). Dir S-M: one presenter contract, no fallback.
12. PrimeStage sleeps 1 ms loop, 10 s deadline (FrameAnchors.cpp:4134) [N].
13. D3D12 degrades DCLF via env vars (Native.cpp:78-100) [N].

## (b) Reinvented
- Engine target reverse lookup: ImageSpace InputOfView/TargetOfUav (ImageSpace.cpp:673-705), Skeleton FindColour/FindDepth (Skeleton.cpp:450-480), Scaleform EngineTargetOf/EngineDepthOf (Passes.cpp ~900), Targets::Find/FindImage linear -> Targets::Identify(view|resource) -> optional<TargetRef{kind,index,slice,mip}>, Targets::Slot(TargetRef), hash maps filled in Make*.
- UI target resolution (back buffer, HDR UI, saved RTV) in Scaleform, Bink, ImGui, ORGHDRDisplay, "hdr.ui" repeated -> Engine::UiTargets::Current() {backBuffer,hdrUi,uiTarget,formats}, a variable HDR raises.
- Image->graph resource registries: DxvkImageImport Imported().byImage, made-image registry, duplicate-check ImageResources (DxvkImageImport.cpp:163-330), FrameAnchors imports/loose/FindImage, Native GraphTextureOf/GraphTextureOfImage, Targets::FindImage -> one ImageRegistry Register(image,{resource,slot,views,owner,name})/Find/Retire w/ dup check.
- ORG image exported to D3D11 retired on D3D11 drop: EngineTargets RetireToken + own GUID + pinned (EngineTargets.cpp:49-95,340), EngineTextures AttachGraphResource/Abandon (EngineTextures.cpp:74-90), DynamicRing tokens, GraphResources ReleaseAfterD3D11 -> GraphResources::ExportForEngine(desc,name), one retire.
- Staging pools: ImGui StagingPool/AcquireStaging (ImGuiBasicRHI.cpp:155-235) reimplements FrameStaging; EngineConstants chunk pool + DynamicRing -> ImGui uses FrameStaging; one UploadRing.
- ImageSpace step groups x4 (Key, KeyOf, *Lagging, *DescFor+built cache, Build*, Declare*, row masks, requested[]) WorldSteps.inl:484, ChainSteps.inl:490, StartSteps.inl:90, MenuSteps.inl:60 -> RecipeTable rows w/ predicates, or StepGroup<Key>.
- 10 engine modules + ImGui Requested() all = GraphMode() [L C4] -> Engine::On().
- DXVK exports resolved twice: Upscaling/DXVKInterop.cpp:457, RenderGraphRuntime.cpp:2183 -> one DxvkExports.

## (c) Delete
- Flush: Reason, EngineDraws, VouchPass, RenderPassImmediately detour, marks (no outside callers).
- CS_ORG_ENGINE_FLUSH_LATCH/_KEEP/_UNBIND in tools/cs-test/schema.toml:2458-2473 + switch registry; CS_ORG_TRIPWIRE_GUEST text in Flush.cpp.
- Stale "CS_ORG_X=1 (default off)" in headers EngineTargets, ImageSpace, Skeleton, OcclusionQueries, EngineTextures, EngineBuffers, EngineConstants, LoadingScreen; ImageSpace "set CS_ORG_ENGINE_CONSTANTS=1"; ImGui log CS_ORG_IMGUI; ChainSteps.inl:1 "for native mode"; Withdraw "readers return to D3D11".
- LoadingScreen misnamed (holds Renderer::Begin hook -> Frame::Driver::OnBegin); move to Frame.
- Native PendGraphBackBuffer resize-to-new-size (fatal Create always follows).
- FrameStaging.h include in OcclusionQueries.cpp.
- D3D12SelfTest (504 lines, XSEPlugin.cpp:136) -> tests.

## Device paths
Native::Active: 15 DCLF, 14 Upscaling, 8 FrameAnchors, 1-4 per port. D3D12: 18 OnD3D12 in NativeDevice.cpp + DCLF env forcing (4 switches).
