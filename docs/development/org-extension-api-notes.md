# ORG extension API notes

What other SKSE plugins will need from CS once the renderer runs through OpenRenderGraph without D3D11. This is a
collection of requirements, not a design: each port in [org-full-port.md](./org-full-port.md) adds what it finds here,
and the API is designed from this list later.

Until that API exists, the DXVK build stays the default, and plugins that call D3D11 keep working there. In the
DXVK-free build, a plugin that calls D3D11 reaches a tripwire handle, which names the plugin's module in the log
(plan task P4.1). Each one found that way gets an entry below.

For what DCLF needs from mods that write engine objects directly (a different problem: scene state, not GPU calls), see
[external-mod-requirements.md](./external-mod-requirements.md).

## How to add an entry

One entry per need. Give:

- **Need:** what the plugin has to do, in GPU terms.
- **Who:** the plugins known to need it, and how they do it today (the D3D11 calls or hooks).
- **Where it fits:** the frame anchor or ORG concept it would map to.
- **Found by:** the plan task or tripwire report that found it.

## Needs

### An overlay pass after the UI

- **Need:** draw a 2D overlay (usually Dear ImGui) over the finished frame.
- **Who:** ImGui-based SKSE menus and HUDs that hook `IDXGISwapChain::Present` and draw with `imgui_impl_dx11`.
- **Where it fits:** the Overlay anchor (P1.6), with an order key below CS's ImGui pass (1000), so CS's menu stays on top.
  Two ways a plugin could use it:
  - It hands CS its `ImDrawData` each frame, and CS draws it with its ORG ImGui backend (`src/RenderGraph/ImGuiBasicRHI.h`).
    `ImGuiBasicRHI::Commit` and `MakePass` already work this way for CS's own context. A plugin's context has its own
    textures, so the backend would need one state per context. Today its state is a single static.
  - It registers its own raster pass on the overlay's target. That target is whatever D3D11 render target is bound when the
    overlay draws: the framebuffer (`cs.engine.target.<kFRAMEBUFFER>`), or HDR's UI texture (`cs.imgui.target`). The plugin
    would need that resource from an exported call.
- **Also needed:**
  - Its textures as bindless indices: an exported `MakeTexture` (CPU pixels to an ORG texture), and the resolver hook that
    turns a plugin's own `ImTextureID` into a descriptor.
  - Draw callbacks. CS skips `ImDrawList::AddCallback` callbacks on the ORG path, because they are D3D11 code. A plugin that
    switches blend state in a callback (CS's screenshot preview does) needs a declarative replacement: a per-draw blend or
    opaque flag.
  - Input. The overlay's ImGui input still comes from CS's WndProc hook. A plugin's own context needs the same forwarding.
- **Found by:** survey for the plan (2026-11-09); P1.6 (2026-11-09).

### Present notification

- **Need:** run CPU code once per presented frame, and know the back buffer's size and format.
- **Who:** the same Present-hook plugins, plus frame limiters and capture tools.
- **Where it fits:** a callback from the ORG present stage. The swap chain is BasicRHI's, so there is no
  `IDXGISwapChain` to hook in the DXVK-free build.
- **Found by:** survey for the plan (2026-11-09).

### Texture creation and upload

- **Need:** create a texture, fill it from the CPU, and have the engine or a UI sample it.
- **Who:** plugins that make icons, HUD textures or dynamic textures with `ID3D11Device::CreateTexture2D` and
  `UpdateSubresource`.
- **Where it fits:** an ORG texture with an upload, handed to the engine as a tripwire handle that wraps it (P4.1, P4.3).
  Terrain Shadows' port does the graph half today. Its heightmap is a persistent graph texture (`GraphResources::Texture2D`,
  no export). The upload service cannot fill it: async epochs, which the anchors run, record only buffer uploads
  ("Async epochs cannot record these uploads"). So the DDS bytes go into an upload-heap `org::Buffer` at once, laid out by
  `rhi::helpers::PlanTextureUploadSubresources`, and a copy pass (`CopyBufferToTexture`) in the client fills the texture in
  the first epoch. The next frame the client is rebuilt without that pass (`FrameAnchors::Refresh`), and the buffer goes
  with the last build that holds it. Phase 3.5 makes this a framework helper: an `AnchorPasses::Upload` is a one-shot pass
  (`PassDesc::once`) that the anchors drop themselves once a run with it was submitted. BC formats work. An API for plugins needs a texture upload that async epochs record.
- **P4.3 (engine textures, `src/RenderGraph/Engine/Textures`):** `Textures::Create` is CS's one path for a texture
  on an ORG image from any thread, with no wait: BasicRHI creates the image, and while DXVK is the host it wraps the image
  with the contents uploaded in its own stream (`dxvkCreateTextureFromVkImageWithData`), so the contents are there before
  any D3D11 or graph use, including on loading screens, where no anchor runs. A plugin texture API can be this call plus a
  tripwire handle. The DXVK-free build needs the same from the graph: an any-thread texture upload that async epochs
  record and that orders before the first use, which the upload service's mailbox (`PostTextureSubresources`) does not
  give for textures today.
- **Found by:** survey for the plan (2026-11-09); the graph upload by P3 TerrainShadows (2026-11-09); P4.3 (2026-11-10).

### Textures a plugin streams in parts

- **Need:** a texture whose memory and contents change while the engine and other passes read it: a mip window that is
  replaced by a finer or coarser one, made on a worker, filled without the frame waiting, and released once nothing reads it.
- **Who:** CS's texture streaming (P4.4). Any plugin with large textures would want the same.
- **Where it fits:** the image from ORG's allocator with an owner label (`org::memory::CreateAllocatedResource`, added for
  this; ORG's tracked resources cannot be made off the render thread), wrapped for D3D11 in the guest phase, imported into the
  graph in DXVK's resting layout (`RenderGraph::ImportDxvkTexture`), and filled by a one-shot copy pass whose
  `ClientDesc::executed` installs it. Readiness is an artifact's GPU submission set, so whoever waits gates on it. What an API
  would add: a texture-upload job a worker can post (async epochs cannot record texture uploads), a completion it can
  subscribe to, and a release that waits for every reader (D3D11's stream, and every epoch that bound the descriptor) without a
  render-thread queue of its own.
- **Found by:** P4.4 texture streaming on ORG (2026-11-10).

### VRAM by owner

- **Need:** say how much video memory a plugin's resources take, by owner, and how much spilled to system memory.
- **Who:** CS's VRAM attribution and texture streaming's budget (P4.7). DXVK's exports did this before
  (`dxvkPushResourceOwner`, `dxvkGetMemoryHeapReports`, snapshots).
- **Where it fits:** BasicRHI's allocator owner labels (P0.13) through `org::memory::OwnerReport` and `AllocatorBudget`, and
  BasicRHI's `QueryVideoMemoryInfo` (P0.7) with `usageAtBudget` for AMD's clamp. Labels are owner paths ("Game/Textures/x"),
  so they share DXVK's categories. Limits: only allocations made with a label (or under `PushAllocationOwner`) are told
  apart; ORG's tracked resources carry their own owner in ORG's memory metadata instead, so the two must not label the same
  memory, or the report counts it twice.
- **Found by:** P4.7 VRAM tooling (2026-11-10).

### Sharing graph resources with D3D11 code

- **Need:** a plugin's pass writes a texture or buffer that D3D11 code still reads (its own, or an engine draw it feeds), or
  reads a CS feature's graph output from D3D11.
- **Who:** any plugin moved to the frame anchors while part of it stays on D3D11.
- **Where it fits:** CS's graph-backed resources (`src/RenderGraph/GraphResources.h`; render-graph.md, "Graph-backed feature
  resources"). An exported API needs the same rules:
    - an exported resource is never aliased;
    - its D3D11 views go before its memory, and the memory waits for D3D11's last use (CS's `ReleaseAfterD3D11`);
    - a texture rests in DXVK's layout for it between epochs, and its views use its own typed format;
    - a resize rebuilds the client's anchors and republishes the resource (the twins' resize generation, which the anchors
      read; no call from the owner).

  The DXVK exports underneath are `dxvkCreateBufferFromVkBuffer` and `dxvkCreateTextureFromVkImage` (interop version 7). A plugin
  that calls them itself must create images with every usage the D3D11 bind flags imply. In the DXVK-free build there is no
  D3D11 side, so the export becomes a tripwire handle (P4.1).
- **Found by:** P2.1 (2026-11-09).

### Per-frame constants for a pass

- **Need:** give a pass values that change every frame (a camera, a time, the stripe of a rolling update).
- **Who:** every effect a plugin moves to the anchors.
- **Where it fits:** async epochs prepare and record a pass ahead of the frame's values, and kept recordings are submitted
  again, so such values can be neither push constants nor anything a pass reads in `Prepare`. They go in an `org::LatchBlock`
  that the client's `commit` writes at `GraphHost::CurrentFrameSlot()`; the shader reads them through a `LATCH` entry
  (`Common/OrgResources.hlsli`, `ShaderBindings::LatchOf`). What the recording's commands depend on (dispatch counts, group
  counts) goes in the pass's `Revision`; when the commit changes it, the ticket is prepared again. A pass that reads that
  shape in `Prepare` must make sure a shape published between its `Prepare` and the revision check cannot pass as current
  (Terrain Shadows' pass remembers the shape it prepared and adds a fresh value while it differs from the live one). An
  exported API needs the latch block and this rule.
- **Found by:** P3 TerrainShadows (2026-11-09).

### Graph textures read by Drawcall Limit Fix

- **Need:** DCLF's colour passes read a feature's graph texture at a frame-texture register.
- **Who:** CS's ported features whose output DCLF samples (Terrain Shadows' t60 now; IBL, VolumetricShadows and others next).
- **Where it fits:** DCLF has a graph path only for buffers (`Api::FrameGraphBuffers`). A ported texture is still read through
  its D3D11 export, which DCLF imports like any D3D11 view. A frame-graph-texture registration, with the resource registered
  in DCLF's graphs, would drop that import and, once the engine's own reads are ported (Phase 5), the export.
- **Found by:** P3 W1 ports (2026-11-09).
### Reading another client's graph output in another epoch

- **Need:** a pass reads a texture or buffer that another feature or plugin writes in a different epoch (an anchor epoch, or one
  of DCLF's), ordered after the write, without a copy and without going through D3D11.
- **Who:** DCLF's colour passes reading the ported CS features' outputs (Dynamic Cubemaps, IBL, Skylighting, Terrain Shadows,
  Volumetric Shadows and the others its integrations read), and any plugin pass that consumes CS's outputs.
- **Where it fits:** `PublishedResources` (`src/RenderGraph/PublishedResources.h`; render-graph.md, "Publishing a graph resource
  to other epochs"). An exported table needs:
    - `Publish(id, resource)` and `Withdraw(id)`, with the rule that only non-aliased resources are published;
    - a reader-side lookup of the bindless index (`ShaderResourceIndex`) and of whether the id is published now;
    - a stable id naming convention (`Feature::Name`), because the reader's pass declares the id.
  Open items: a withdrawn resource stays registered (and alive) until the producer publishes again under that id; and DCLF's
  views with frame constants of their own (the reflection's faces) do not declare published textures yet, so they still need
  the producer's D3D11 export.
  Found when every DCLF integration moved to published textures: published means "the graph owns it", not "the graph wrote it
  this frame". A producer that can run a single frame on D3D11 (Exponential Height Fog) stays published, and its readers check
  the producer's own state (`NearSRV()` non-null) beside `ShaderResourceIndex`; an export would want a per-frame "written"
  flag or a producer callback in the table. D3D11 writes to an exported twin (Skylighting's `ResetSkylighting` clears through
  the UAV export) are not ordered against graph reads either.
- **Found by:** P3 DCLF graph textures (2026-11-09); P3 DCLF publish (2026-11-09).

### Depth targets a D3D11 draw still writes

- **Need:** a pass reads a depth texture that D3D11 draws write, or a plugin's depth texture that both D3D11 draws and graph
  passes write (Terrain Blending's terrain depth: the engine's re-issued terrain draws write it through a D3D11 DSV; DCLF's
  depth epoch writes it when it holds the terrain).
- **Who:** Terrain Blending today; any plugin that redraws engine geometry into a depth target of its own.
- **Where it fits:** the texture stays D3D11's and graph passes import it (`FrameAnchors::ImportDepth`). Two gaps keep it there:
  - `GraphResources` exports SRV, UAV and RTV views, but no DSV, so a graph-owned depth texture cannot take the engine's draws.
  - `Api::NearTwinContribution::target` is an `ID3D11Texture2D*` DCLF imports itself. DCLF would need a graph-resource form (the
    target's published id, with DCLF clearing it at the depth commit) before the terrain depth could be a graph texture.
  With both, Terrain Blending's terrain depth becomes a graph texture: DCLF draws its near twin, the engine's re-issue (DCLF
  off, or the terrain toggle off) draws through the export, and the clear becomes a graph pass.
- **Found by:** P3 TerrainBlending (2026-11-09).

### Render to texture

- **Need:** render into a texture the engine later samples on a character.
- **Who:** RaceMenu/SKEE overlay and tint-mask baking.
- **Where it fits:** an ORG render pass the plugin attaches to a frame anchor, with its output imported as an engine
  texture.
- **Found by:** survey for the plan (2026-11-09).

### Reading named engine targets

- **Need:** read the scene colour, depth, normals or motion vectors at a known point in the frame.
- **Who:** post-processing and screenshot plugins that read `BSGraphics::Renderer` render targets.
- **Where it fits:** named graph resources at the frame anchors (P2.2), readable by a plugin's pass.
- **Found by:** survey for the plan (2026-11-09).
- **Cubemaps and other plugins' textures (P3 IBL, 2026-11-09):** the engine's cubemap targets
  (`RENDER_TARGETS_CUBEMAP`, e.g. the sky reflections cube that IBL projects to SH) are a separate table from the 2D
  targets, and a reader wants them as cube views. `FrameAnchors::EngineCubemap` imports them as
  `cs.engine.cubemap.<n>`, with every mip and a cube default SRV. A plugin's own D3D11 texture that a graph pass reads
  (Dynamic Cubemaps' environment cube, not ported yet) goes through `FrameAnchors::D3D11Texture` (`cs.d3d11.<name>`).
  The API needs both: engine cubemaps by name, and "import my D3D11 texture" for plugins that keep a D3D11 producer.
- **Graph textures for Drawcall Limit Fix (P3 IBL, 2026-11-09):** DCLF reads another feature's graph output only for
  buffers (`Api::FrameGraphBuffers`), and anchor clients register their resources only in anchor graphs. A ported
  texture producer (IBL's SH at t76/t77) is read by DCLF through its exported D3D11 SRV, imported like any view. A
  frame graph texture registration (resources shared with DCLF's graphs by name) is a follow-up.
- **Storage image formats (P3 IBL, 2026-11-09):** DXC types a `float4` `RWTexture2D` as `Rgba32f` in SPIR-V unless told
  otherwise, and writing an RGBA16F view through it is invalid. `ProgramBuild` now compiles every ORG program with
  `-fspv-use-unknown-image-format` (as `DxcShaderStore` does for game shaders), so the view's format applies, as in
  D3D11. A plugin shader compiled by the API needs the same.

### Compiling a plugin's shaders

- **Need:** compile the plugin's own HLSL for the ORG path, with the same bindings as CS's passes.
- **Who:** plugins that compile shaders with `D3DCompile` (FXC, `*_5_0`) and bind them with `CSSetShaderResources` and
  friends, or hand CS shader bytecode.
- **Where it fits:** the dual-target convention (`Common/OrgResources.hlsli`; shader-workflow.md, "Dual-target shaders"). A
  plugin can include that header from CS's `Data/Shaders` and keep its D3D11 build. For the ORG build it needs, through an
  exported table: `ComputeProgram::Load` and `RasterProgram::Load` on a source path of its own (today the path is relative to
  `Data/Shaders`), `ComputeProgram::Dispatch`, and `ShaderBindings::CreateSampler`. The push block layout (handles in list
  order, then `PUSH` structs, at most 128 bytes) is part of that ABI, and so is the `CS_ORG_BINDLESS` define. Bytecode a
  plugin compiled itself would need BasicRHI's DXC arguments (`rhi_shader_abi.h`) and the same layout; accepting source is
  simpler, because CS's store then caches and hot-reloads it.
- **Found by:** P2.3 (2026-11-09).

### Attaching passes to the frame

- **Need:** run compute or raster work at a defined point in the frame, with declared inputs and outputs.
- **Who:** any plugin that adds a GPU effect.
- **Where it fits:** the frame anchors from P2.2 (`src/RenderGraph/FrameAnchors.h`; render-graph.md, "Frame anchors"). CS's
  own features use them now. An external plugin would need the same calls through an exported table, as the contributor ABI
  has: a reserved anchor is already a contributor epoch (`cs.anchor.<name>`).
- **Found by:** survey for the plan (2026-11-09).

How the anchors order work:

- An anchor runs at one hook. It runs after the D3D11 work CS's features do at that hook, unless its rule says otherwise.
- Within an anchor, passes run by order key, lowest first. Equal keys keep the registration order.
- The key only decides between passes that do not depend on each other. The graph orders a reader after its writer anyway.
- Leave gaps of 100 between features' keys, so a pass can go between them later. Add each feature's keys to its anchor's rule.
- An anchor nobody reserved has no epoch. A reserved anchor with no active client submits nothing.
- After PostProcessing, Upscale, Tonemap, FrameGeneration, Present and Overlay, the anchor separates its epoch from Vulkan work that may follow. That is the hazard
  rule in render-graph.md.

Each anchor's rule:

- **ReflectionsPrepass.** At `BSCubeMapCamera::RenderCubemap`, before the engine draws the cubemap. After every feature's
  `ReflectionsPrepass`. It runs once per cubemap render, which can be zero or several times a frame. Each run is one
  execution of its epoch.
- **SkyReflections.** At a reflection face's sky batch (`BSShaderAccumulator::FinishAccumulating`'s `RenderBatches` of the sky
  techniques, SE 99938 +0x247 / AE 106583 +0x237), while `BSCubeMapCamera::RenderFaces` draws that face: after the engine cleared
  the face and its depth and drew the face's opaque groups, before its decals. Once per face drawn, so up to six runs per cubemap
  render. Reserved and run only by Drawcall Limit Fix's sky family (`CS_ORG_SKY=1`); the anchor self-test reports it only then.
- **EarlyPrepass.** After `Main::RenderShadowMaps` returns. The shadow maps are drawn, DCLF's shadow epoch included, and the
  shadow light data is copied (t98). After every feature's `EarlyPrepass`. Before `Main::RenderDepth`: the scene depth is
  not drawn yet.
- **DepthDone.** When `Main::RenderDepth` returns, in Terrain Blending's depth hook (`TerrainBlending::BlendPrepassDepths`): the
  z-prepass is drawn, DCLF's depth and first-person depth epochs included, and Terrain Blending's terrain depth is complete.
  Before the features' `Prepass`, which read the blended depth through the depth table. It runs only while Terrain Blending
  is loaded and on: no other hook reaches this point, so the anchor self-test reports it only then.
- **Prepass.** At the main pass's first opaque batch, inside `StartDeferred`. After every feature's `Prepass`, so after Light
  Limit Fix's culling epoch. Before DCLF's colour epoch. The Z-prepass depth is drawn. The GBuffer targets are bound but not
  cleared yet: they still hold the last frame.
- **Sky.** At the main pass's sky batch (the same call, for the main camera's accumulator): after the opaque groups, before the
  decals and Drawcall Limit Fix's colour epoch. The GBuffer targets are bound; the depth is the Z-prepass copy. Reserved and run
  only by Drawcall Limit Fix's sky family (`CS_ORG_SKY=1`); the anchor self-test reports it only then.
- **DeferredPasses.** In `EndDeferred`, after the opaque pass, the blended decals and the final depth copy into
  `kPOST_ZPREPASS_COPY`. Before the D3D11 SSGI, SSS, cubemap update, deferred composite and Effects11's rays. The GBuffer is
  complete. A pass that must follow the composite uses DeferredComposite.
- **DeferredComposite.** In `Deferred::DeferredPasses`, after the D3D11 SSGI, SSS and cubemap update, where the deferred
  composite runs. The ORG composite (`CS_ORG_COMPOSITE=1`, key 0) is a pass here; when it ran, the D3D11 dispatch is skipped.
  A pass with a higher key reads the composited `kMAIN`. Before the D3D11 `DynamicCubemaps::PostDeferred` and Effects11's
  rays. When SSGI and SSS move to DeferredPasses (P3 W2), the composite can join them there. A pass that must follow the
  composite whichever path ran it uses PostComposite: with the ORG composite off, the D3D11 composite runs after this anchor.
- **PostComposite.** At the end of `Deferred::DeferredPasses`, after the deferred composite (D3D11 or ORG), the D3D11
  `DynamicCubemaps::PostDeferred` and Effects11's D3D11 rays. `kMAIN` holds the composited opaque scene. Before the forward
  (blended) pass and DCLF's forward epoch.
- **PostProcessing.** At `Main_PostProcessing`, before Post Processing's `DrawBeforeUpscaling`, the upscaler and the
  ImageSpace chain. The scene is at render resolution.
- **Upscale.** In `Upscaling::PerformUpscaling`, after the upscaler's evaluate (the DXVK interop command ring) and after
  `kMAIN`'s D3D11 copy and depth upscale when those run on D3D11. Before the ImageSpace chain. It runs only on frames where an
  upscaler (FSR, XeSS, DLSS) evaluates, not with TAA. The ring's buffer ends with a full memory barrier and is submitted ahead
  of DXVK's next command list, which the epoch records into, so its passes see the evaluate's output.
- **Tonemap.** At the ImageSpace HDR tonemap's render, after the upscaler. Before Effects11 and Post Processing's `PreProcess`.
  `Point::input` and `Point::output` are the tonemap's targets. Its graph may record ahead of the hook, so a pass that depends
  on them puts them in its revision.
- **FrameGeneration.** In `Upscaling::CopyHudlessColor`, after the ImageSpace chain and HDR's clean composite, before frame
  generation tags or evaluates (DLSS-G, FSR-FG). It runs only on frames where frame generation captures the hudless colour.
- **Present.** In `IDXGISwapChain::Present`, before DCLF's `BeforePresent`, the frame pace signal, CS's overlay, HDR output,
  frame generation and the present. It is before the UI: a pass after the UI uses Overlay.
- **Overlay.** At CS's overlay draw (`OverlayRenderer::FinalizeImGuiFrame`), inside HDRDisplay's present chain: after Present,
  DCLF's `BeforePresent` and the frame pace signal. ImGui has built its frame. Before HDR output and frame generation. It runs
  only on frames where CS's overlay draws (the menu, an editor, notifications, the performance overlay), not every frame. The
  target is the render target bound for the overlay: the framebuffer, or HDR's UI texture when HDR composites the UI.
- **Capture.** At the end of HDRDisplay's present chain (`ScreenshotFeature::ProcessCaptureRequest`), just before the native
  Present or frame generation's present. After Overlay and HDR output: the back buffer (or the owned frame's source) holds
  what the display will show. Reserved only by the Screenshot feature's ORG path (`CS_ORG_SCREENSHOT=1`) and the self-test.
- **HDROutput.** In `HDRDisplay::ApplyHDR`, inside HDRDisplay's present chain: after Overlay and, when HDR composites the UI
  after the present hooks (the deferred composite), after other mods' Present hooks and frame generation's hudless capture.
  Before the real present. It runs only on frames HDR composites (HDR on, the window usable), and only when
  `CS_ORG_HDR_DISPLAY=1`: HDRDisplay calls it. The UI texture is complete here, and the back buffer is not yet presented.
- **EngineShadowClear, EngineCubeClear, EngineFrameClear.** Where the engine clears a target (`Renderer::ApplyState`,
  `SetDirtyStates`, `Renderer::ClearColor`, `ClearDepthStencil`), right before the engine binds it: shadow map slices, the
  reflection faces and their depth, and every other engine clear. Zero or more times a frame. Reserved only by the engine
  skeleton (`CS_ORG_ENGINE_SKELETON=1`, `p5/skeleton.md`) and the self-test.
- **EngineDepthCopy.** Where the main depth is copied into `kPOST_ZPREPASS_COPY`: `Main::RenderDepth`'s end, FinishAccumulating's
  copy, and `Deferred::DeferredPasses` before the DeferredPasses anchor. Reserved only by the engine skeleton and the self-test.
- **Interface.** At each Scaleform display's `EndDisplay` that the graph draws (`RenderGraph/Engine/Scaleform`, P5.10), on the
  main thread, inside `UI::DrawInterface`: after FrameGeneration's hudless capture, before Present. It runs once per display
  (the HUD, each menu, the cursor), so several times a frame, and only with `CS_ORG_SCALEFORM=1`. Its passes draw onto the
  target the display began on (render target 0, the framebuffer or HDR's UI texture) with kMAIN's depth-stencil for masks.
  A late anchor: frame generation or the present may follow it.

Keys used so far:

- The anchor self-test (`CS_ORG_ANCHOR_SELFTEST`): 0, 1 and 2 at every anchor.
- The graph resource self-test (`CS_ORG_GRAPHRES_SELFTEST`): 0 at DeferredPasses.
- Skylighting's probe update (`CS_ORG_SKYLIGHTING`): -5 to -2 (the reset clears, after a reset only) and -1 (the update) at
  Prepass.
- IBL's SH projection (`CS_ORG_IBL`): 0 (environment) and 1 (sky) at Prepass.
- Screen Space Shadows (`CS_ORG_SCREEN_SPACE_SHADOWS`): 100 (clear) and 101 (raymarch) at Prepass.
- Exponential Height Fog (`CS_ORG_EXPONENTIAL_HEIGHT_FOG`): 300 upward at Prepass, one key per pass: the upload, the near
  volume's depth, material, scattering and integration, its history copy (temporal only), then the far volume's passes the
  same way, or its clear. At most 310.
- The deferred composite (`cs.deferred.composite`): 0 at DeferredComposite.
- Effects11's rays (`CS_ORG_E11_RAYS`): 100 to 103 (volumetric rays: raymarch, horizontal and vertical blur, apply) and 200 to
  204 (sun rays: mask, three blurs, composite) at PostComposite.
- The shared constants' upload (`cs.shared.upload`): first at every anchor where a client reads them, outside the keys.
- Terrain Shadows (`CS_ORG_TERRAIN_SHADOWS`): 99 (the heightmap upload, once per heightmap) and 100 (the update) at
  EarlyPrepass.
- Terrain Blending's depth blend (`CS_ORG_TERRAIN_BLENDING`): 0 at DepthDone.
- Volumetric Shadows (`CS_ORG_VOLUMETRIC_SHADOWS`): 100 to 105 at EarlyPrepass (per mip: downsample, horizontal blur, vertical
  blur).
- Cloud Shadows (`CS_ORG_CLOUD_SHADOWS`): 0 (the occlusion copy) at ReflectionsPrepass.
- Light Limit Fix's local shadow cache (`CS_ORG_LIGHT_LIMIT_FIX_SHADOWS`): 100 (slot data) and 101 (shadow map copy) at
  EarlyPrepass.
- Grass Collision (`CS_ORG_GRASS_COLLISION`): 200 (copy the colliders) and 201 (update the texture) at EarlyPrepass.
- CS's ImGui (`CS_ORG_IMGUI`): 1000 at Overlay, last.
- Scaleform (`CS_ORG_SCALEFORM`): 100 upward at Interface, one pass per surface (target and depth pair); one draws per run.
- The Screenshot feature (`CS_ORG_SCREENSHOT`): 100 (the readback copy) at Capture.
- The engine skeleton (`CS_ORG_ENGINE_SKELETON`): its gate entry (0 to 127) at EngineShadowClear, EngineCubeClear and
  EngineFrameClear; 0 (the copy) at EngineDepthCopy.
- HDR Display (`CS_ORG_HDR_DISPLAY`): 100 (the output composite), 101 (the copy into the back buffer, not in the owned frame)
  and 102 (the UI texture's clear) at HDROutput.
- Upscaling (`CS_ORG_UPSCALING`): 100 (the upscaled colour into `kMAIN`), 200 to 204 (the depth and refraction normal copies,
  the depth upscale, the underwater mask copy and its upscale) at Upscale; 100 (the hudless copy) at FrameGeneration.
- Subsurface Scattering (`CS_ORG_SUBSURFACE_SCATTERING`): 199 (Burley clear), 200 (extraction), 201 (horizontal or Burley
  blur) and 202 (vertical blur) at DeferredPasses. SSGI's passes there stay below 199, as the D3D11 order is SSGI first.
- Screen Space GI (`CS_ORG_SCREEN_SPACE_GI`): 100 to 105 (history clears of `cs.ssgi.reset`, after a reset only), 110 to 116
  (the chain's steps) and 117 (the accumulation copy without the blur) at DeferredPasses, below Subsurface Scattering's
  199-202.
- Dynamic Cubemaps (`CS_ORG_DYNAMIC_CUBEMAPS`): 300 to 310 at DeferredPasses (capture, the captures' mips, inference, the
  filtered mip 0 copy, the inferred mips, irradiance, BC6H encoding, the two BC6H copies, publishing), after Subsurface
  Scattering. Not at DeferredComposite, where its D3D11 path runs: the ORG composite reads the cubemaps there through
  descriptors the graph does not track.

### Work whose shape changes every frame

- **Need:** a pass whose dispatch count or group counts follow the frame's data (Screen Space Shadows: Bend's dispatch
  list, up to 8 dispatches with new group counts and wave offsets every frame), while the recording is kept and submitted
  again.
- **Who:** CS's Screen Space Shadows today; any plugin that sizes work on the CPU each frame.
- **Where it fits:** record the capacity, and let the data drive it. The commit writes each dispatch's constants and its
  `Dispatch` arguments into the execution's `LatchBlock` region; the pass records one indirect dispatch per entry
  (`ExecuteIndirect` with a one-argument `Dispatch` signature, read from the latch), and unused entries hold zero groups. A
  plugin API would need the dispatch signature and `LATCH` entries (`ShaderBindings::LatchOf`) exported with the programs.
- **Found by:** P3 W1, Screen Space Shadows (2026-11-09).

### Work that some frames skip

- **Need:** a client whose passes have no work in some frames (Subsurface Scattering: no skin drawn) while other clients
  keep the same anchor's epoch. `FrameAnchors` calls `commit` only for active clients, so an inactive client's passes
  run again with the latch region an older execution in the same frame slot wrote.
- **Who:** CS's Subsurface Scattering; any plugin whose work depends on what the frame drew.
- **Where it fits:** today the client stays always active and its commit writes zero thread groups for the indirect
  dispatches (as Screen Space Shadows does for unused entries). A plugin API would want either a commit for every
  registered client of a running anchor, with an "inactive" flag, or a per-execution skip that `Prepare` can read.
- **Found by:** P3 W2, Subsurface Scattering (2026-11-09).

### Arrays in latch constants

- **Need:** a `LATCH` struct with an array the shader indexes by a run-time value (Subsurface Scattering's 42 kernel
  samples). `LATCH` loads the whole struct into a static copy, which DXC would keep as an indexed private array.
- **Who:** any pass whose per-frame constants hold tables (kernels, cascades, light lists).
- **Where it fits:** the shader reads such an element straight from the latch region
  (`OrgLatch_<name>.Load<T>(OrgOffset_<name> + offset)` under `CS_ORG_BINDLESS`, `SSSKernel` in
  `SubsurfaceScattering/SSSCommon.hlsli`). That reaches into `OrgResources.hlsli`'s internal names; a public
  `ORG_LATCH_LOAD(name, type, offset)` macro would make it part of the dual-target contract.
- **Found by:** P3 W2, Subsurface Scattering (2026-11-09).

### Reading another feature's D3D11 texture

- **Need:** read, in a graph pass, a texture that a D3D11 feature (or plugin) still makes and writes, such as Terrain
  Blending's blended depth, which Screen Space Shadows reads instead of the engine's depth while Terrain Blending is on.
- **Who:** CS ports whose inputs come from features not ported yet; plugins whose textures other passes read.
- **Where it fits:** `FrameAnchors::D3D11Texture(name, texture, srv, uav)`: the DXVK image imported once, as `EngineTarget` does, with
  its views from the texture's bind flags. The owner keeps writing it with D3D11. When the owner is ported, the reader
  switches to the owner's graph resource.
- **Found by:** P3 W1, Screen Space Shadows (2026-11-09).

  blur).
### A per-frame number of dispatches

- **Need:** a pass whose number of dispatches changes every frame (copy the shadow map slices the engine drew this frame),
  without recording again each frame.
- **Who:** Light Limit Fix's local shadow cache (`ORGLocalShadows.cpp`). Any plugin that copies or processes "what changed
  this frame".
- **Where it fits:** a kept recording cannot hold per-frame values, so the pass records the most dispatches a frame can need
  (8), each reading its row of a LatchBlock (`LATCH` entry), and a row with no work returns at once. An indirect dispatch
  whose arguments the commit writes into the latch would save the empty groups. The API needs one of the two.
- **Found by:** P3 W1, LLF (2026-11-09).

### Storage image formats in plugin shaders

- **Need:** write a UAV texture of a format other than the one DXC infers from the element type.
- **Who:** Light Limit Fix's cache copy: `RWTexture2DArray<float>` becomes an `R32f` storage image under SPIR-V, so its ORG
  cache is `R32_FLOAT` even where the D3D11 cache is `R16_UNORM` (twice the memory with 16-bit engine shadow maps).
- **Where it fits:** the dual-target resource list (`OrgResources.hlsli`) would need a format per UAV entry
  (`[[vk::image_format]]`), or the ABI would compile with unknown storage formats (`-fspv-use-unknown-image-format`,
  which needs `shaderStorageImageWriteWithoutFormat`).
- **Found by:** P3 W1, LLF (2026-11-09).
### Per-frame CPU data of varying size

- **Need:** send a list whose length changes every frame (colliders, lights, decals) to a pass whose recording is kept
  from frame to frame.
- **Who:** any plugin that gathers scene objects on the CPU each frame. Grass Collision does it with `Map(WRITE_DISCARD)`
  on two dynamic structured buffers today.
- **Where it fits:** a client's latch block. The commit writes only the bytes in use, but a recorded copy cannot follow
  the length, so Grass Collision's copy into its graph buffers always moves the whole capacity (66 KB). LLF instead
  re-records when the length grows past a power of two. A structured view of a latch slot's region, or a copy whose size
  is read from the latch, would remove both workarounds.
- **Found by:** P3 GrassCollision (2026-11-09).

### Skipping an inactive client's passes

- **Need:** a client with nothing to do this frame records nothing, even when another client keeps the anchor's epoch.
- **Who:** any client whose passes read a latch region.
- **Where it fits:** `FrameAnchors::ClientDesc::active`. Today an inactive client's commit is not called but its kept
  recording still runs, on a latch region written frames ago. Grass Collision stays active every frame for this reason,
  and writes a "clear" frame when the feature is off.
- **Found by:** P3 GrassCollision (2026-11-09).

### Presenting from more than one thread

- **Need:** present a frame from a thread other than the render thread, under the engine's lock.
- **Who:** the engine itself (the loading-screen loop on `JobListManager::ServingThread`, Bink) and plugins that draw
  their own loading or splash screens.
- **Where it fits:** the owned frame's tail (P1.3) already runs on DXVK's submission thread whatever thread called
  Present. An API present call must likewise not tie Vulkan work to the caller's thread.
- **Found by:** P1 ownership inversion (2026-11-09).

### Window and swap chain state

- **Need:** know the real swap chain's size, format, colour space and present mode, and see a resize or colour space
  change before drawing.
- **Who:** HDR and colour-grading plugins that read `IDXGISwapChain4` state, and overlays that size to the back buffer.
- **Where it fits:** in the owned frame the DXGI swap chain is headless: its back buffer and `CheckColorSpaceSupport`
  no longer describe what reaches the display (CS reads `OwnedFrame::GetSurfaceState`). The API needs a query and a
  change notification for the ORG swap chain.
- **Found by:** P1 ownership inversion (2026-11-09).

### CS's shared constants in a plugin's pass

- **Need:** read the camera matrices, screen size, frame counters and CS's feature settings (FrameBuffer, SharedData,
- **Who:** CS's Volumetric Shadows ran at the sun's shadow mask draw (`State::Draw`, RenderShadowmask) and read the engine's
  FeatureData, Permutation) in a graph pass, with the values D3D11 passes see at the same point.
- **Who:** any plugin pass that does screen-space work; today such plugins read `PerFrame` (b12) or CS's b5/b6 bound by CS.
- **Where it fits:** `ClientDesc::sharedConstants` and `SharedConstants::Declare`/`Index` (src/RenderGraph/SharedConstants.h),
  and the shader side's `SHARED` entry with `ORG_SHARED_CONSTANTS` (shader-workflow.md). The struct layouts (cbuffer
  packing, bools as uint) become ABI: FeatureData's layout changes with every feature settings change, so a plugin should
  read only FrameBuffer and SharedData, or CS needs to version FeatureData.
- **Found by:** P2.6 (2026-11-09).

### Reading a D3D11 feature's output in a graph pass

- **Need:** sample a texture a D3D11 pass wrote this frame (SSGI's outputs, cubemaps, a 3D probe volume) without a copy.
- **Who:** CS's own ORG composite today (P2.7); any plugin pass reading another mod's D3D11 output.
- **Where it fits:** `DCLF::GpuTextures::ResolveBinding` gives a read-only descriptor over the DXVK image in its resting
  layout, untracked by the graph. It covers Texture3D, which ORG's external texture imports cannot view yet. It lives in
  DCLF's code; a general API should move it beside `FrameAnchors::EngineTarget`. ORG's `ExternalTextureResource` needs a 3D
  view kind before such imports can be graph-tracked.
- **Found by:** P2.7 (2026-11-09).
### Clears that some runs skip

- **Need:** a clear that runs on some runs of an anchor only, with the recording kept: the engine skeleton clears one shadow
  slice or one cube face per run, out of a fixed set.
- **Who:** CS's engine skeleton (P5.1). Any plugin that clears a target on some frames only.
- **Where it fits:** ORG's clears (`ClearRTV`, `ClearDSV`, a pass's load op) cannot be gated, and a per-run revision would make
  every variant a recording of its own. Today the skeleton draws the clear: a fullscreen triangle through a run gate, depth 1
  from the vertex stage, the stencil zeroed by its ops (`RasterProgram` allows depth-only programs for this). That loses fast
  clears, and every gated pass still begins its render pass. An ORG clear command with an indirect "skip" word (or a gated
  render pass begin) would replace it.
- **Found by:** P5.1 (2026-11-10).

### An anchor that runs many times a frame

- **Need:** run the same anchor's epoch more than `kGameFramesInFlight - 1` times between two Presents (one run per engine clear
  batch).
- **Who:** CS's engine skeleton (P5.1); ReflectionsPrepass when many cubemaps render in one frame.
- **Where it fits:** an epoch's runs take its ring of `framesInFlight` slots in turn, and the host's next preparation waits for
  a slot's GPU work. In-stream work reaches the GPU only at DXVK's flush, so a fifth run in one frame left the host waiting for
  unsubmitted work and the render thread waiting for the host: a deadlock.
- **Done (ORG `org-port/p5-slots`):** `GraphHost::Desc::epochRuns` lists such epochs with their runs a frame. Each has a ring
  of `framesInFlight` times that many slots, counted in `FrameSlots()` (fixed when the host is made, so latch blocks sized from
  it stay right), and starts on `framesInFlight` of them. When the next slot's last run is fewer than
  `max(framesInFlight - 1, 1)` frame boundaries (`SealSubmitted`) old, the ring grows by a slot instead of waiting. A frame with
  more runs than the whole ring makes `SubmitEpoch` throw `EpochRingFullError` for the rest until the next seal: the caller
  falls back for those runs. CS lists its anchors' runs in the anchor table (`FrameAnchors.cpp`, `runsPerFrame`); the
  skeleton's own three-runs cap is gone. A new anchor that runs more than once a frame sets its `runsPerFrame` there.
- **Guest phase only:** This issue exists only because, in the guest phase, each anchor is a separate host epoch spliced into DXVK's stream, which DXVK submits at Present. Once ORG owns the frame (the Phase 6 cutover), these clears and the other per-call engine work are passes inside one frame graph, and the slot-per-run ring no longer applies.
- **Found by:** P5.1, the first skeleton-org1 run (2026-11-10).

### Work at an engine draw that is not an anchor

- **Need:** run a pass where an engine draw happens (a specific Utility pass, a shader bind) instead of at an anchor.
  t4 there. Plugins that hook `SetShader` or a draw to capture inputs do the same.
- **Where it fits:** usually an existing anchor, once the inputs are known to be final there. The shadow mask draw only marks
  where the engine binds the sun cascades; they are drawn in `Main::RenderShadowMaps`, so EarlyPrepass has them. The pass then
  reads the engine target by name (`FrameAnchors::EngineDepth`), not the bound slot. The API should name which engine
  targets are final at each anchor, so a plugin does not need a new anchor per draw. A new anchor costs an epoch at that
  point (entry and exit barriers) each frame.
- **What stays at the draw:** binding the output to a PS slot for the engine's own shaders, until P5 removes those draws.
- **Found by:** P3 Volumetric Shadows (2026-11-09).

### Graph textures read by DCLF's draws

- **Need:** DCLF's colour draws read a feature's graph texture by its graph resource, not through a D3D11 view of it.
- **Who:** CS's Volumetric Shadows (t18), IBL and Terrain Shadows. Today they register a D3D11 SRV as a frame texture
  (`Api::FrameTexture`). On their ORG paths that is the exported SRV, which DCLF imports as it imports any D3D11 view.
- **Where it fits:** `Api::FrameGraphBuffers` takes buffers only, and an anchor client registers its resources only with the
  anchor graphs, not with DCLF's. A graph-texture variant needs both. It is planned as one task for every port.
- **Found by:** P3 Volumetric Shadows (2026-11-09).
### Importing a plugin's own depth textures

- **Need:** a pass reads a depth texture that is not in the engine's render-target tables: a plugin's own map that the
  engine (or DCLF) renders into, or a texture the plugin only sees as a bound SRV.
- **Who:** Skylighting's sky occlusion map, which it swaps in for the precipitation occlusion target; its probe update also
  reads the directional shadow maps through the SRV it captures at the shadow mask draw.
- **Where it fits:** `FrameAnchors::ImportDepth(DepthStencilData, id)`, beside `EngineDepth`: the same import, keyed by the
  id, kept while the texture stays the same, registered with every anchor graph. A capture of an engine texture should
  resolve to `EngineDepth` first (`ORGSkylighting.cpp`, `ImportShadowMap`), so one image has one anchor import.
- **Found by:** P3 Skylighting (2026-11-09).

### Skipping a frame in a kept recording

- **Need:** a client with per-frame work that some frames do not have (Skylighting: interiors, the map menu).
- **Who:** any anchor client whose pass is recorded once and reused.
- **Where it fits:** `ClientDesc::active` false means no commit, but another client can still run the anchor's epoch, and
  the reused recording then reads whatever its frame slot's latch held from frames ago. Skylighting writes a "no update"
  flag into its latch and stays active until every frame slot has been latched idle once, then rests. A framework answer
  would be a per-client "skip" the anchor applies to the client's passes (an empty dispatch, or a predicate the graph
  owns), so clients need not carry the flag into their shaders.
- **Answered in CS (Phase 3.5):** run gates (`RenderGraph::RunGate`, `ClientDesc::gates`). Every run of an anchor closes the
  gates of each client in its graph before any commit, so an inactive client's gated dispatches and draws are empty
  indirect work, never an older slot's values. A plugin API would expose the same gate.
- **Found by:** P3 Skylighting (2026-11-09).

### Per-frame values from CS's shared constant buffers

- **Need:** a ported pass reads values CS uploads in `SharedData`, `FeatureData` or the engine's `PerFrame` buffer.
- **Who:** every ported CS compute shader that includes `SharedData.hlsli` or `FrameBuffer.hlsli`.
- **Where it fits:** until P2.6 makes those buffers bindless, a pass latches the fields it reads (a LATCH entry) and the
  shader reads them through small accessors that return the shared buffer's field under FXC, so the D3D11 bytecode stays
  identical. Matrices go in the latch as `float4` rows (or columns), so no `Load<T>` depends on matrix packing.
  `Deferred`'s directional shadow buffer is a `Map(WRITE_DISCARD)` buffer, which cannot be imported; its CPU copy
  (`Deferred::GetDirectionalShadowLightData`) is latched instead.
- **Found by:** P3 Skylighting (2026-11-09).
### Choosing the image a frame presents

- **Need:** hand the present stage the frame's final image when it is not the back buffer (an HDR or colour-grading
  output), so it is copied once into the swap chain image instead of into the back buffer and again from it.
- **Who:** CS's HDRDisplay (`OwnedFrame::SetFrameSource`); plugins that do their own output transform after the engine.
- **Where it fits:** the Present anchor: a per-frame "present source" a client sets, read by the present pass. A
  screenshot or capture reads the same source.
- **Found by:** P1.5 (2026-11-09).

### One-frame passes after a reset

- **Need:** run a pass in one frame only, such as clearing a feature's history textures after a loading screen, a shader
  reload or new textures, without the pass in every frame's recording.
- **Who:** temporal effects (CS's Screen Space GI clears its History textures; the D3D11 path clears its accumulation
  counts with `ClearUnorderedAccessViewFloat`); any plugin with history it must drop.
- **Where it fits:** today a client sets a flag, calls `FrameAnchors::Refresh` so the next build's `make` includes the clear
  passes, and calls `Refresh` again a frame later to drop them: two graph builds per reset. A framework answer would be a
  per-execution switch on a kept pass (a predicate the graph evaluates at submission), or clears the graph runs once
  on request before a resource's next use.
- **Also:** Skylighting's reset (`ORGSkylighting::QueueReset`) works the same way, and counts the reset done only when the
  clear pass's `Prepare` ran (a flag the client's commit reads), not when `make` returned it: with async epochs a build can
  come too late for the run that asked. A reset flag in a pass's `Revision` cannot replace the rebuilds: with async epochs a
  ticket is prepared before the client's commit, so the revision would lag a frame and a kept recording with the clear
  could be reused.
- **Answered in CS (Phase 3.5):** one-shot passes (`PassDesc::once`): the pass is in the graph until a run with it was submitted,
  then the anchors build without it; `FrameAnchors::Rearm` makes it pending again. One build per reset instead of two, and no
  flag set in `Prepare`. `ClientDesc::executed` says when the reset's run was submitted.
- **Found by:** P3 W2, Screen Space GI (2026-11-09); Skylighting's reset (2026-11-09).

### Ping-pong history as two lifetimes

- **Need:** a temporal chain whose D3D11 path swaps two textures each frame (function-static indices) needs a fixed
  resource for each binding in a kept recording.
- **Who:** CS's Screen Space GI; any plugin with temporal accumulation.
- **Where it fits:** split each pair by what the next frame reads: one `History` texture that the last writer of the frame
  writes and the next frame's first reader reads, and one `Transient` scratch texture (aliasable) for everything in
  between. A writer that cannot write in place (a pass reading neighbours of the texture it updates) writes the scratch
  texture and a copy pass carries it into the history (Screen Space GI without its blur). A graph-level ping-pong
  resource (two images, the frame's index choosing the views) would avoid the copy.
- **Found by:** P3 W2, Screen Space GI (2026-11-09).

### Constants declared by a shared header

- **Need:** a header shared by several shaders of a feature declares the feature's per-frame constants (a cbuffer whose
  members the shader bodies read by bare name), and each shader's push block must still carry the latch's handle and
  offset.
- **Who:** CS's Screen Space GI (`ScreenSpaceGI/common.hlsli`, `SSGICB`); any plugin with a common header.
- **Where it fits:** the header declares the latch with `ORG_RESOURCE(LATCH, ...)` under ORG and keeps its cbuffer under
  FXC; each shader lists it as an `EXTERN_LATCH` entry (`OrgResources.hlsli`), which puts the handle and offset in the push
  block and declares nothing. Since lane L4 the header declares the latch the same way on both paths
  (`ORG_RESOURCE(LATCH, SSGIConstants, SsgiFrame, 1)`, a `cbuffer` holding one struct under FXC), and the bodies read
  `SsgiFrame.<member>`: one layout, no second cbuffer and no bare-name macros.
- **Found by:** P3 W2, Screen Space GI (2026-11-09).

### Reading other features' graph outputs by name, and knowing whether they are there

- **Need:** a pass reads several other features' outputs (textures and buffers) as graph resources, each only while that
  feature's graph path is active, and otherwise reads the D3D11 resource or does without.
- **Who:** Exponential Height Fog's light scattering reads IBL's SH (`IBL::EnvIBL`, `IBL::SkyIBL`), Skylighting's probes
  (`Skylighting::ProbeArray`), Terrain Shadows' heights (`TerrainShadows::ShadowHeight`), Light Limit Fix's lights and lists
  (`LightLimitFix::Lights`, `LightIndexList`, `LightGrid`), local shadows (`LightLimitFix::LocalShadowData`,
  `LightLimitFix::LocalShadowCache`) and Cloud Shadows' occlusion (`CloudShadows::Occlusion`). Since lane L4 it asks
  `PublishedResources::IsExportOf(view, id)` with each producer's id constant, and imports the D3D11 view under that id
  otherwise. The constants still live in the producers' ORG headers, so the reader includes them.
- **Where it fits:** a registry of named graph outputs (as `PublishedResources`, but for anchor graphs too) with a query
  "is `Feature::Name` registered now", so a reader declares by name without knowing each producer's class.
- **Found by:** P3 ExponentialHeightFog (2026-11-09).

### Per-frame data a D3D11 producer maps with WRITE_DISCARD

- **Need:** a pass reads a small buffer whose D3D11 producer renames it every frame (Map with WRITE_DISCARD), which an import
  cannot follow.
- **Who:** Exponential Height Fog reads Deferred's directional shadow light (t98). It takes the values from
  `Deferred::GetDirectionalShadowLightData`, latches them, and a copy pass fills its own transient structured buffer.
- **Where it fits:** a helper that turns CPU data into a transient graph buffer each frame (a latch region plus the copy), as
  Light Limit Fix's lights upload and this fog's upload both do by hand.
- **Found by:** P3 ExponentialHeightFog (2026-11-09).

### The constants of an engine draw a plugin replaces

- **Need:** a pass that draws engine work on ORG (or a plugin's replacement of an engine draw) reads the constants that one
  engine draw used. The engine's constant buffers are pooled by size and rewritten with WRITE_DISCARD before each use, so
  neither the buffer pointer nor its contents at any later point identify that draw.
- **Who:** Phase 5's sky, ImageSpace, menu 3D and DCLF families; any plugin that hooks an engine draw and re-issues it
  (today they read the bound D3D11 buffer back or mirror its Map, as DCLF's `ConstantMirror` does for b12).
- **Where it fits:** `RenderGraph::Engine::Constants` (P5.7, [p5/constants.md](./p5/constants.md)): each engine draw and
  dispatch is a use with an id; a plugin asks for a stage and slot and gets a host-visible ring location for its shader. A
  plugin that issues its own D3D11 binds is captured too (group Other). A kept recording reads the location through its own
  latch, never baked. Gaps: async-queue readers are not ordered by the ring's retirement, and loading-screen frames on the
  job thread are not captured.
- **Found by:** P5.7 (2026-11-10).

### Resources declared by shared shader headers

- **Need:** a plugin's ORG shader includes CS headers that declare their own resources (SharedData's depth, Light Limit
  Fix's lists, IBL, Terrain and Cloud Shadows), and reads them through its push block.
- **Who:** Exponential Height Fog's light scattering. The headers declare those resources with `ORG_HEADER_SRV`; a shader
  that defines `ORG_HEADER_RESOURCES` lists them as `EXTERN` entries (shader-workflow.md, "Resources that headers declare").
- **Where it fits:** the dual-target shader ABI. A plugin needs the list of header resources and their names kept stable.
  DXC's SPIR-V ignores `row_major` in a templated raw buffer `Load<T>`, so LATCH structs hold matrices as `float4` rows.
- **Found by:** P3 ExponentialHeightFog (2026-11-09).

### Load-time textures written to disk

- **Need:** build a texture once from many source textures and save it as a file the engine loads later (a baked atlas or
- **Who:** Cloud Shadows' occlusion copy at ReflectionsPrepass, whose hook runs at every `BSCubeMapCamera::RenderCubemap`.
  cache), without a device.
- **Who:** Unified Water's flowmap (all of Tamriel's 64x64 flow tiles in one DDS). Its D3D11 path copied the tiles on a
  deferred context and read the texture back with `CaptureTexture` to save it. Plugins that bake textures at load do the same.
- **Where it fits:** no graph pass. When the result only goes to disk, the work is a CPU copy: DirectXTex decodes the tiles,
  they go into a `ScratchImage`, and `Util::SaveImageToFile` writes it (`UnifiedWater/Flowmap.cpp`). A GPU pass would add an
  upload, a readback and a host wait on a graph epoch, and an anchor pass does not run while `DataLoaded` blocks the main
  thread. An API needs no GPU entry for this; it needs the engine side, the texture the engine loads from that file, to come
  from an ORG texture in the DXVK-free build (P5.4).
- **Found by:** P3 UnifiedWater (2026-11-09).
### Copies between engine draws

- **Need:** copy or clear a feature's texture between two engine draws, where the feature adds a render target to those draws
  and each draw reads what the copy wrote.
- **Who:** Cloud Shadows. It adds a 4th render target (one of 32 layer cubemaps) to the engine's cloud draws into the
  reflections cubemap. Before each layer's draw it copies the layer below into it, and when a new face starts it clears the
  face and fills the faces the frame did not draw (`CheckResourcesSide`, `SkyShaderHacks`, `PropagateToCompletion`). None of
  these points is an anchor, so they stay on D3D11; only the copy into the occlusion cubemap moved (the ReflectionsPrepass
  anchor). The EarlyPrepass half of the propagation stays on D3D11 too: its copies change every frame, chain into each other,
  and read and write subresources of the same layer textures, while the graph declares one access per texture per pass.
- **Where it fits:** P5's sky pass, which makes the layers outputs of the graph's own cloud draws. Short of that, two things
  would let the propagation move: a copy pass that declares subresource ranges per frame (or takes a copy list from the
  latch), and a way to copy within one texture between faces in one pass.
- **Found by:** P3 CloudShadows (2026-11-09).

### A once-a-frame pass at an anchor that runs several times a frame

- **Need:** a pass that runs once a frame at an anchor whose hook runs more than once a frame.
  The client's `active` consumes a flag the feature sets once a frame and publishes it in an atomic; the pass's `Revision`
  and `Prepare` read it so a ticket prepared with another value is prepared again (Terrain Shadows' pattern).
- **Where it fits:** `FrameAnchors::PassDesc`: a "first run of the frame only" option, or the run's index in the frame in
  `FrameAnchors::Point`, so the pass decides without its own flag.
- **Found by:** P3 CloudShadows (2026-11-09).
### Work spread over frames by a CPU state machine

- **Need:** a feature that splits a long job over several frames, choosing each frame's part on the CPU (Dynamic Cubemaps:
  capture and inference, then the middle irradiance mips, then the last mip with BC6H encoding and publishing, per
  cubemap), while its recording is kept and may be prepared before the frame's commit.
- **Who:** CS's Dynamic Cubemaps; any plugin that amortizes work (probe updates, rolling texture builds).
- **Where it fits:** a pass may not read the frame's choice in `Prepare`: the ticket is prepared ahead, so a choice
  published between `Prepare` and the revision check would be recorded under the wrong revision. Today every part is in
  the recording and the commit gates it: each dispatch is indirect, with its group counts in the latch region (zero for
  the parts this frame skips), and ORG's mip generation takes its per-mip counts from the latch the same way
  (`GraphPasses::MakeGenerateMips` with a gate). Copies cannot be gated, so they must give the same result every frame
  (copy from a persistent source that only the gated work changes), or join the graph by `FrameAnchors::Refresh` once
  they are needed. A framework answer would be predicated copies and mip generation (a condition word in a buffer the
  commit writes), or variants a kept recording selects by a latched index.
- **Answered in part (Phase 3.5):** the run gate (`RenderGraph::RunGate`) is the one mechanism for gated dispatches and draws
  (`ComputeProgram::DispatchIndirect`, `RasterProgram::DrawDesc::indirect`). Copies and mip generation still have no
  predicate.
- **Found by:** P3 W2, Dynamic Cubemaps (2026-11-09).

### ORG's built-in passes in a host without a pipeline compiler

- **Need:** generate mips (and resolve MSAA) on the graph.
- **Who:** CS's Dynamic Cubemaps (capture and inferred cubemaps); Post Processing's bloom and others next.
- **Where it fits:** `org::GenerateMipsPass` declares its pipeline through `PassBuilder::Pipeline`, which needs an
  `org::PipelineCompiler` in `Runtime::Desc`; CS's runtime has none (its programs are `ComputeProgram`s). CS runs ORG's own
  shader through `ComputeProgram` instead (`GraphPasses::MakeGenerateMips`), and ships ORG's `shaders/` folder in
  `Data/Shaders/OpenRenderGraph` (copied into `package/Shaders` at configure time). A plugin API needs either a
  `PipelineCompiler` in CS's runtime, so ORG's built-in passes work as they are, or these wrappers exported.
- **Found by:** P3 W2, Dynamic Cubemaps (2026-11-09).

### Writing a texture that an untracked reader samples in the same epoch

- **Need:** a pass writes a texture that another pass of the same anchor epoch reads through a descriptor it resolved
  outside the graph (the ORG composite reads Dynamic Cubemaps' and IBL's D3D11 views through `DCLF::GpuTextures`).
- **Who:** CS's Dynamic Cubemaps against the ORG composite.
- **Where it fits:** the graph orders and transitions only declared uses, so the write and the read race, and the texture
  can still be in a copy or storage layout when the read happens. Dynamic Cubemaps runs one epoch earlier
  (DeferredPasses) so the epoch boundary orders them. The answer is for readers to declare published resources by id
  (`PublishedResources`), as Drawcall Limit Fix's frame textures do; the composite and IBL still resolve D3D11 views.
- **Found by:** P3 W2, Dynamic Cubemaps (2026-11-09).
### Running a plugin's .fx effects

- **Need:** a plugin ships ENB-style `.fx` effects (techniques, passes, UI-annotated globals, named render targets) and
  wants them run as passes at a point of the frame, with their variables in CS's menu and ini, without FX11 or D3D11.
- **Who:** CS's Effects11 (ENB presets at the tonemap, through FX11 today); post-processing plugins that embed FX11 or
  write their own D3D11 passes for the same purpose.
- **Where it fits:** the ORG FX runtime (`docs/development/org-fx-runtime.md`). Its parser and lowering
  (`src/Features/Effects11/ORG/`) are std-only for this reason. An export would take the effect text (or a file and an
  include folder), the textures it reads by name (graph resources, or CS's own names such as `TextureDepth`), an anchor
  and order key; and return the UI variables and a handle to set values and choose techniques. Values reach the GPU as a
  latched values region per pass; the texture and sampler handles go in the push block (up to 30 per effect), since the
  commit that writes the latch cannot know heap indices, which ORG gives only at record time.
- **Found by:** P3 W5, ORG FX runtime (2026-11-09).

### Textures generated once at startup

- **Need:** fill a persistent lookup texture once (a noise, a LUT), before its first reader in any epoch, and never again
- **Who:** CS's HDRDisplay (`CS_ORG_HDR_DISPLAY`, the copy at HDROutput); plugins that `CopyResource` into `GetBuffer(0)`.
- **Where it fits:** today the client publishes the area in an atomic at `active`, and the passes put it in their `Revision`
- **Where it fits:** `RecordingContext::Index` is the only way to learn a declared view's heap index, and only at record
- **Where it fits:** a client at the Tonemap anchor that registers again (`FrameAnchors::Register` with the same id) when
  unless the graph loses it.
- **Who:** CS's TruePBR glints noise (`src/TruePBR/ORGGlintsNoise.cpp`, `CS_ORG_GLINTS`); any plugin with a generated LUT.
- **Where it fits:** the client registers the fill as a one-shot pass (`PassDesc::once`) at the earliest anchor its readers
  follow (EarlyPrepass), and binds the texture once `ClientDesc::executed` reports the run that carried it. The texture is a Persistent `GraphResources` twin, published for Drawcall Limit Fix. A framework answer
  would be an "initialize" callback on a persistent resource, which the graph runs once before the resource's first use
  in any epoch and again after a device or graph reset. There is no reset event today: a failed graph returns the
  feature to D3D11, which makes the texture again.
- **Answered in CS (Phase 3.5):** a one-shot pass (`PassDesc::once`) does this without the client's bookkeeping. A reset event
  is still missing.
- **Found by:** P3 W4, TruePBR glints noise (2026-11-09).
### Reading back a frame without waiting

- **Need:** copy the displayed image (or any graph texture) to the CPU at a chosen frame, without stalling that frame, and
  get the bytes on a callback once the GPU copy is done.
- **Who:** CS's Screenshot feature (D3D11 staging texture plus `Map(DO_NOT_WAIT)` polling today); capture, recording and
  photo-mode plugins that hook Present and do the same.
- **Where it fits:** the Capture anchor, after HDR output, and ORG's readback service (`PlanReadbackCapture` and
  `RecordReadbackCapture` in a copy pass, `IReadbackService::ProcessReadbackRequests` for the callbacks). Two things a plugin
  API would hide: the anchor's ticket is prepared ahead of the hook, so a one-off capture must be in the pass's revision
  (the ticket is then prepared again), and an epoch that submits only when it captures must poll the readbacks itself,
  since only submissions poll them. `RangeSpec` names mips and slices, not a rectangle: a crop is cut on the CPU.
- **Found by:** P3 W3, Screenshot (2026-11-09).
### Writing the D3D11 swap chain's back buffer from a pass

- **Need:** a pass writes the frame into the swap chain's back buffer (an output transform's copy), in the DXVK-host frame.
- **Where it fits:** the back buffer imports like any D3D11 texture (`FrameAnchors::D3D11Texture`), but only while the swap
  chain has one image. With a sequential swap effect and several buffers DXVK rotates the images under the D3D11 objects at
  each present (`D3D11SwapChain::RotateBackBuffers`, on its own thread), so an import made once writes the wrong image after
  the first present. HDRDisplay's FLIP_DISCARD swap chain has one, and the pass falls back to D3D11 otherwise. A plugin API
  wants a "back buffer" resource the present stage resolves per frame (in the owned frame, the present source above).
- **Found by:** P3 W3, HDRDisplay (2026-11-09).

### Clearing an imported D3D11 texture

- **Need:** clear a D3D11 texture a feature still owns (a render target the engine draws into) from a graph pass.
- **Who:** CS's HDRDisplay (its UI texture, after the composite read it).
- **Where it fits:** `FrameAnchors::D3D11Texture` imports SRV and UAV only, and no CPU-side UAV, so neither a render target
  clear nor `UnorderedAccessClear` works on it. HDRDisplay imports the texture itself with its RTV and clears it with an
  empty render pass (`LoadOp::Clear`). An import option for an RTV and a clearable UAV would cover both.
- **Found by:** P3 W3, HDRDisplay (2026-11-09).

### A frame anchor only one feature runs

- **Need:** a point of the frame that only exists inside one feature's code path (HDRDisplay's present chain, after the
  overlay and other mods' Present hooks), where its passes and any plugin's passes that read the finished UI go.
- **Who:** CS's HDRDisplay (`FrameAnchors::Anchor::HDROutput`, run from `ApplyHDR`).
- **Where it fits:** a frame anchor whose hook is the feature's own call (`FrameAnchors::Run`), reserved by that feature. It
  runs only on frames the feature reaches it (HDR on, the window usable). A plugin that needs a pass after the UI but on
  every frame uses Overlay or Present instead.
- **Found by:** P3 W3, HDRDisplay (2026-11-09).

### One helper for an ORG build's D3D11 defines

- **Need:** an ORG program built from a D3D11 shader needs the defines `Util::CompileShader` adds (the common defines, then
  `COMPUTESHADER`, `WINPC`, `DX11`) as wide strings.
- **Who:** every CS port writes the same function (`ProgramDefines` in Subsurface Scattering, SSGI, Dynamic Cubemaps, the height
  fog, the deferred composite and HDRDisplay).
- **Where it fits:** one helper beside `Util::GetCommonShaderDefines`, or `ComputeProgram::Desc` taking narrow defines and
  adding the stage's defines itself. A plugin API would expose the same.
- **Found by:** P3 W3, HDRDisplay (2026-11-09).
### A pass after the deferred composite, whichever path ran it

- **Need:** add to the composited opaque scene (`kMAIN`) before the forward pass, as screen-space light shafts do.
- **Who:** CS's Effects11 volumetric rays and sun rays; any plugin that adds a deferred effect (fog, shafts, decals on the
  lit scene) before the blended geometry.
- **Where it fits:** the PostComposite anchor. DeferredComposite is not enough on its own: with the ORG composite off, the
  D3D11 composite runs after that anchor, so a higher key there sees the scene before the composite.
- **Found by:** P3 W4, Effects11 rays (2026-11-09).

### Raster passes whose area follows the dynamic resolution

- **Need:** fullscreen raster passes whose viewport (and compute passes whose group counts) follow the dynamic resolution,
  which can change from frame to frame, while the recording is kept.
- **Who:** CS's Effects11 rays (half-res and full-res areas of `kMAIN`); any plugin pass sized to the render area.
  and read it in `Prepare`, so a change records them again (as the live flags do). The shaders read the same sizes from
  the latch. A plugin API would want the render area as a graph value the viewport and dispatch can take at execution
  (a viewport from a buffer, or indirect draws and dispatches sized from the latch), so the recording never changes.
- **Found by:** P3 W4, Effects11 rays (2026-11-09).
### Graph passes around a vendor SDK's Vulkan work

- **Need:** a plugin runs a vendor SDK (an upscaler, frame generation, a denoiser) on its own Vulkan command buffer, and wants
  the passes that feed it and consume its output on the graph: a copy of its output into an engine target, a resolve, a format
  conversion of its input.
- **Who:** CS's Upscaling (`CS_ORG_UPSCALING`): Streamline's FSR, XeSS and DLSS run in the DXVK interop command ring
  (`Upscaling/DXVKInterop.cpp`); the copy into kMAIN, the depth upscale and the hudless copy are graph passes now.
- **Where it fits:** the SDK's own images are graph textures exported to D3D11 (`GraphResources`, `Persistent`, SRV and UAV
  or RTV), because the SDK reaches them through their D3D11 images (`IDXGIVkInteropSurface`). Its passes go at an anchor the
  plugin runs right after the SDK's submission (`Upscale`, `FrameGeneration`): the ring's buffer is submitted after DXVK's
  command list in flight and ends with a full memory barrier, so the next list's epoch sees its writes, and the exported
  textures rest in DXVK's layout on both sides. Two things a plugin API would need: an anchor the plugin itself runs at its own
  point of the frame (today only CS code calls `FrameAnchors::Run`), and the hazard separator before the SDK's submission
  (`Upscaling::SeparateFromGraphEpochs`), which Upscaling installs as the anchors' late-epoch hook. Per-run work in a kept
  recording is a bit each pass reads at `Prepare` and adds to its revision ("Skipping a frame in a kept recording"); Upscaling
  and Cloud Shadows both carry that pattern, which the run gate replaces (lane L4 moves them).
- **Found by:** P3 W3, Upscaling's own passes (2026-11-09).
### Heap indices outside the recording

- **Need:** a pass whose shader reads more resource handles than the push block holds (an effect with 40 textures, a
  material table) wants the handles in a buffer, written once per build or per frame.
- **Who:** the ORG FX runtime (its handle table had to move into the push block, which caps an effect at 30 textures and
  samplers); any port with a long resource list.
  time, where nothing can write a buffer the same recording reads without a barrier. An ORG service that resolves a pass's
  declared views into a small host-visible table at prepare time (with the table's lifetime tied to the build) would let
  the commit, or the graph itself, fill it.
- **Found by:** P3 W5, ORG FX runtime execution (2026-11-09).

### Clearing a render target without a draw

- **Need:** clear a kept graph texture once (to match D3D11's zero-filled textures) without a UAV, whose storage format
  support varies (R11G11B10, R10G10B10A2).
- **Who:** the ORG FX runtime's History textures; any port that keeps render targets across frames.
- **Where it fits:** `GraphPasses::MakeUavClear` needs a UAV. The FX runtime records a render pass that only clears
  (`LoadOp::Clear`, no draw). A shared `MakeRenderTargetClear` beside `MakeUavClear` would serve every port.
- **Found by:** P3 W5, ORG FX runtime execution (2026-11-09).
### A chain of optional effects built from settings

- **Need:** run a sequence of effects, each one optional and each reading the previous one's image, whose members, order
  and passes follow user settings (Post Processing's 13 sub-effects at the tonemap), with the intermediate images aliased.
- **Who:** CS's Post Processing (`ORGPostEffect.h`); post-processing plugins with their own effect lists.
  a key of the chain's shape changes: the effects in it, the targets' size and formats, program variants. Each effect adds
  its passes through a builder that hands it the current image, makes its outputs as transient `GraphResources` (which the
  graph aliases, so no ping-pong is written by hand) and reserves latch regions. A plugin API would export that builder, so
  a plugin's effect can join the chain at a named position instead of registering a client of its own.
- **Found by:** P3 W3, Post Processing (2026-11-09).

### Keeping a client's passes out of an epoch another client runs

- **Need:** a client whose passes write engine targets must record nothing in a frame it is inactive, even when another
  client of the anchor runs the epoch (Post Processing's output pass writes kMAIN and kMAIN_COPY; run on a stale latch it
  would overwrite the frame the D3D11 fallback then reads).
- **Who:** CS's Post Processing chain; the ORG FX runtime will share the Tonemap anchor with it.
- **Where it fits:** Post Processing gates every pass on one flag the hook sets before the anchor runs. `Prepare` returns
  no frame data while it is off, and `Revision` follows the value the pass was prepared with (the copy pass of
  `ORGCloudShadows.cpp`), so a kept recording is prepared again when the flag changes. A per-client skip in
  `FrameAnchors` (see "Skipping an inactive client's passes") would replace this.
- **Found by:** P3 W3, Post Processing (2026-11-09).

### An anchor input that is one of two targets

- **Need:** read the image an engine pass hands to the anchor when it is one of several targets, chosen per frame (the
  tonemap's input is kMAIN or kMAIN_COPY, depending on refraction and the ImageSpace chain).
- **Who:** CS's Post Processing; any plugin pass at the Tonemap anchor.
- **Where it fits:** the pass declares both targets and a latch value picks one (`copy.ps.hlsl`'s `Copy.source` under
  ORG), so the recording does not change with the choice. An API would resolve `FrameAnchors::Point::input` to the graph
  resource itself, with a per-execution binding.
- **Found by:** P3 W3, Post Processing (2026-11-09).

### 16-bit types in shared shader headers

- **Need:** compile shader headers written for SM 6.2+ with 16-bit types (RenoDX's `math/sign.hlsl` declares `uint16_t`
  whenever the target is 6.2 or later) on the ORG path, which DXC runs without `-enable-16bit-types`.
- **Who:** CS's Color Grading (RenoDX tonemappers); plugins that bring RenoDX or other SM6 shader libraries.
- **Where it fits:** CS guards those sections with `__HLSL_ENABLE_16_BIT` as well. A plugin API's program build would
  need a per-program option for 16-bit types (which changes `half` everywhere in that program) rather than a global flag.
- **Found by:** P3 W3, Post Processing (2026-11-09).

### One constant block, changed between a pass sequence's dispatches

- **Need:** a shader whose one cbuffer the D3D11 path rewrites between dispatches (per-pass sizes, a kernel group's scale
  and ghost mask, the halo only in the first group), on a recording that never changes. The block is 304 bytes, too big for
  push constants.
- **Who:** CS's Lens Flare (up to 12 dispatch shapes); any multi-pass effect ported from D3D11 with a shared cbuffer.
- **Where it fits:** each pass gets its own latch copy of the block, written in full by the commit (`ORGLensFlare.cpp`).
  That costs latch space (about 2.7 KB in Ultra mode, of the chain's 16 KB). An API could take one base block plus small
  per-pass overrides in push constants, if the shader could read the base from the latch and patch it from the push block.
- **Found by:** P3 W3, Post Processing Lens Flare (2026-11-09).

### Knowing that a commit's build is the epoch's

- **Need:** run work only on some frames (a bokeh kernel's spectrum, baked when the aperture changes) and count it done
  only when it ran. After a rebuild, the chain's commit writes the new build's latch while the epoch may still run the old
  build's passes, so marking the work done in the commit can lose it.
- **Who:** CS's Lens Flare (kernel bake), Color Grading (LUT bake, which marks it done in the commit), TruePBR's glint
  noise (counts prepared passes).
- **Where it fits:** Lens Flare bakes in three frames after a change, which is deterministic and harmless. An API would
  tell the commit which build the epoch was made from (or pass a per-build epoch counter), so one frame is enough.
- **Found by:** P3 W3, Post Processing Lens Flare (2026-11-09).

### Chain images of other sizes

- **Need:** transient chain images at half, quarter or FFT (N x N) resolution, aliased with the rest.
- **Who:** CS's Lens Flare, and likely Bloom, Physical Glare and Depth of Field.
- **Where it fits:** `Builder::CreateImage` takes an optional width and height. RG32F images are sampled with linear
  filtering (the FFT compose), which Vulkan does not guarantee for 32-bit float formats; current desktop GPUs support it.
- **Found by:** P3 W3, Post Processing Lens Flare (2026-11-09).
### A blend factor that changes without a new recording

- **Need:** a blend whose factor follows a setting (COD Bloom scales each mip by its blend weight with
  `D3D11_BLEND_BLEND_FACTOR`), in a recording the graph keeps across frames.
- **Who:** CS's Post Processing bloom; any port whose D3D11 path calls `OMSetBlendState` with a changing factor.
- **Where it fits:** the blend constants are dynamic state recorded with the draw, so a kept recording fixes them, and a
  setting in the build key rebuilds the whole chain on every slider step. The bloom upsample returns the weight in alpha
  under ORG and blends the destination by `SrcAlpha`, with alpha masked out of the write. That works when the pixel
  stage's alpha is free. A framework answer would be blend constants read from a latch at submission, or a
  `RasterProgram` option that sets them from a buffer the commit writes.
- **Found by:** P3 W3, Post Processing COD Bloom (2026-11-09).

### Per-mip views in a chain helper

- **Need:** passes down and up one texture's mip chain, each reading one mip and writing another of the same texture.
- **Who:** CS's Post Processing bloom (9 mips); any other mip pyramid of the chain still to port.
- **Where it fits:** ORG declares a subresource range per use (`Subresources(resource, Mip(m, 1))` with
  `SrvView`/`RtvView{ .mip }`), so one pass may read mip i and render to mip i + 1. The chain's `Image::Mip(m)` carries the
  mip through `Bind::Srv` and `FullscreenDraw::target`, and a draw's default viewport is then that mip's size. A published
  `Image` keeps its mip, so its reader binds that mip's view without knowing the chain.
- **Found by:** P3 W3, Post Processing COD Bloom (2026-11-09).
### Knowing the epoch ran this build's passes

- **Need:** count one-shot GPU work done (a PSF remade on a settings change, a baked LUT) only once it reached the GPU. A
  client's commit cannot tell whether the epoch it writes runs the client's newest build: right after a rebuild the
  graph may still run the previous one, whose passes read the previous latch, so work marked done there never happens.
- **Who:** CS's Physical Glare (`ORG/ORGPhysicalGlare.cpp`); Color Grading's LUT bake has the same shape; TruePBR's
  glints noise fill met it first.
- **Where it fits:** answered by `FrameAnchors::ClientDesc::executed` (lane L3). The chain forwards it to each effect's
  `Effect::Executed`: the effect keeps what this frame's commit asked for and counts it made there; a frame whose epoch ran
  an older build gets no call, so the next commit asks again.
- **Found by:** P3 W3, Physical Glare (2026-11-09).

### Side outputs at the work size, not the screen size

- **Need:** transient work images whose size follows a setting rather than the screen (FFT squares of 128-1024), aliased
  with the rest of the chain, plus persistent spectra of the same size kept across graph builds.
- **Who:** CS's Physical Glare and Lens Flare.
- **Where it fits:** `Builder::CreateImage` takes a size; the persistent textures are `GraphResources` the effect owns and
  `Builder::Register`s at each build, made again only when the size changes (which is part of the chain's key).
- **Found by:** P3 W3, Physical Glare (2026-11-09).
### Small per-frame tables in a latch

- **Need:** a table a few KB large that the CPU rebuilds whenever a setting moves (DoF's 120 gather samples, procedural
  or from a custom aperture), read by many passes, without a graph upload pass or a rebuild per change.
- **Who:** CS's Depth of Field; any pass whose kernel weights or sample sets come from the CPU.
- **Where it fits:** the table is a latch region the client writes every frame, and the shader reads it as a raw buffer
  at the latch offset (`EXTERN_LATCH` handle and offset, `LoadBokehSample` in `dof.cs.hlsl`). An API would give a
  structured view of a latch region, so the D3D11 and ORG declarations could be one `StructuredBuffer`.
- **Found by:** P3 W3, Post Processing DoF (2026-11-09).

### History initialized by the pass that first uses it

- **Need:** a history texture whose first frame must not read uninitialized memory (DoF's 1x1 focus distance; a NaN
  there would stick for the session), without a one-shot clear that a commit cannot tell was executed.
- **Who:** CS's Depth of Field; temporal passes generally.
- **Where it fits:** the shader takes a reset word from the latch and the client sets it for a few commits after each
  build (`ORGDoF.cpp`, `kFocusResetCommits`). An API would tell a client which build an epoch runs, or give history
  resources a "contents undefined" flag the pass can read.
- **Found by:** P3 W3, Post Processing DoF (2026-11-09).
### Side outputs and constants shared between effects of a chain

- **Need:** an effect of a chain reads another effect's side outputs (images, buffers) and its per-frame constants, and
  must run without them when that effect is off.
- **Who:** CS's Post Processing Composite, which reads COD Bloom, Lens Flare, Physical Glare, Histogram Auto Exposure and
  Local Exposure (`ORG/ORGComposite.cpp`); post-processing plugins whose effects combine others' results.
- **Where it fits:** each effect publishes its images by name (`Builder::Publish`) and its constant regions of the chain's
  latch (`Builder::PublishLatch`); the reader `Find`s them at the build and picks the shader permutation of those present,
  so binding another effect's constants costs no copy. The names are string ids agreed between the effects
  (`ORGPostChain::Published`), and the reader checks a latch region's size against the struct it expects. A plugin API would
  want these as typed, versioned outputs of an effect (an image with its view and size, a constants struct with its layout)
  rather than strings, so a contributor's change of format or layout is caught when the chain is built.
- **Found by:** P3 W3, Post Processing Composite (2026-11-09).
### Reading back small GPU values only on the frames that want them

- **Need:** read a few bytes (a histogram, one adapted luminance) back to the CPU on the frames a settings panel or a log
  asks for them, without a host wait and without a recording that replays an old readback plan.
- **Who:** CS's Histogram Auto Exposure (histogram panel, Auto ISO meter); any plugin with a GPU-side statistic in its UI.
- **Where it fits:** ORG's `ReadbackCapturePass` captures every frame. CS's pass (ORG/ORGHistogramAutoExposure.cpp) is a
  copy of it that plans the capture only while an atomic "wanted" flag is set, and has no `Revision`, so it is prepared
  every frame. An API would take a "capture this frame" predicate, or a revision-safe one-shot request.
- **Found by:** P3 W3, Post Processing (2026-11-09).

### Pyramids without per-mip views in a shared pass helper

- **Need:** compute passes that read mip N-1 and write mip N of one texture.
- **Who:** CS's Local Exposure (log-luminance pyramid); bloom chains; any plugin downsampling chain.
- **Where it fits:** the chain's Bind entries take whole-resource views. Local Exposure gives each level its own transient
  texture the size of the D3D11 mip (rounded down, as mips are), which reads the same as single-mip views and keeps every
  level aliasable. An API would let a binding name a mip range (`SrvView`/`UavView` with mips).
- **Found by:** P3 W3, Post Processing (2026-11-09).

### First contents of a history buffer

- **Need:** a history buffer that starts at zero, as a new D3D11 buffer does, before its first frame reads it.
- **Who:** CS's Histogram Auto Exposure (histogram, adaptation); any plugin with an accumulating buffer.
- **Where it fits:** UAV clears in CS (`GraphPasses::MakeUavClear`) cover textures only, and a one-shot clear must count
  as done only when its pass ran. The exposure port adds a `CS_Reset` entry, dispatched indirectly with one group over
  its first few frames and zero after. An API would take an initial value (zero fill) for a History resource.
- **Found by:** P3 W3, Post Processing (2026-11-09).
### Calling D3D11 in the DXVK-free build

- **Need:** any GPU work a plugin does today through the game's `ID3D11Device`, immediate context or swap chain
  (`BSGraphics::Renderer` +0x48/+0x50, the device global, `RendererData` +0x38/+0x40/+0x60), or through the views and
  textures in the render-target tables.
- **Who:** every SKSE plugin that renders with D3D11: ImGui menus and HUDs, post-processing and screenshot plugins,
  texture and overlay bakers.
- **Where it fits:** in the native build those fields hold tripwire handles (`src/RenderGraph/Engine/Tripwire/`). Each
  call logs once per call site as `[ORG] tripwire: <Interface>::<Method> from <Module>+0x<offset> (another plugin calls
  D3D11: see org-extension-api-notes.md)` and fails (E_FAIL, null outputs). `CS_ORG_TRIPWIRE_LOG=1` lists every site
  with its count. Each module found that way gets its own entry here with the calls it made, and maps to one of the
  other needs (overlay pass, texture upload, render to texture, named targets).
- **Found by:** P4.1 (2026-11-09).

### Passing engine resources to a plugin's own D3D11 code

- **Need:** bind an engine texture or view (a render target's SRV, the depth SRV) in a plugin's own D3D11 draw.
- **Who:** post-processing and screenshot plugins that read `renderTargets[i].SRV` and bind it on the game's context or
  their own.
- **Where it fits:** while DXVK is loaded (guest mode) a handle wraps the DXVK object, and a call through a wrapping
  device or context unwraps it. A handle passed straight to DXVK's own device or context is cast to DXVK's class and
  crashes. Until the engine's fields go native, a plugin can pass `Tripwire::Unwrap(object)` (no reference added; a
  native handle unwraps to null). The API replacement is "Reading named engine targets": the graph resource by name,
  not the D3D11 view.
- **Found by:** P4.1 (2026-11-09).

### Interfaces beyond the ones the engine holds

- **Need:** what plugins reach through `QueryInterface` on the device and swap chain: `IDXGIDevice` (adapter, frame
  latency), `ID3D11Device1-5` and `ID3D11DeviceContext1-4` (constant-buffer offsets, fences), `IDXGISwapChain3/4` (HDR
  colour space and metadata, the waitable object).
- **Who:** HDR, frame-pacing and upscaler/frame-generation plugins; ImGui backends that read the swap chain's buffer.
- **Where it fits:** a tripwire handle answers only its own interface chain (IUnknown up to the interface the engine
  stores). Any other IID is logged with its GUID and refused in native mode (forwarded in guest mode). Each IID that
  shows up in the log names a capability the API must offer directly: present-time colour space and metadata (P0.9),
  frame latency (P0.10), a GPU completion signal.
- **Found by:** P4.1 (2026-11-09).

### Objects a forwarded getter returns

- **Need:** compare what a getter returns with the engine's fields (`OMGetRenderTargets` against
  `renderTargets[kMAIN].RTV`, `GetResource` against the texture field).
- **Who:** plugins that detect the current pass from the bound targets.
- **Where it fits:** in guest mode a forwarded getter returns the DXVK object, not the handle the engine stored, so the
  pointers differ. Views made with their resource answer `GetResource` with that handle. Identity checks should go
  through `Tripwire::Describe`/`Unwrap`, or better through the frame anchor the plugin attaches to, which says which
  pass runs.
- **Found by:** P4.1 (2026-11-09).

### Waiting for GPU work through D3D11 queries

- **Need:** know when the GPU has finished work the plugin issued (readbacks, ring buffers).
- **Who:** plugins that spin on `GetData` of an event query until it returns S_OK; the engine's own dynamic ring does
  the same (75484, R6.4), which P4.5 replaces.
- **Where it fits:** a native tripwire context returns E_FAIL from `GetData`, so such a loop spins forever. The API needs
  a completion signal per frame or per submission (ORG's frame fences, or a callback after the epoch that used the
  resource retires).
- **Found by:** P4.1 (2026-11-09).
### Reading a ported feature's output that DCLF never named

- **Need:** a draw or pass reads another feature's output at a register it only ever captured (the view bound there when the
  colour commit ran), or through a row of its own (a descriptor index it writes into a GPU table), and the producer moved to
  the graph.
- **Who:** DCLF's colour draws at t45 (Screen Space Shadows) and t102/t103 (Light Limit Fix's local shadow cache); DCLF's grass
  draw row (Grass Collision's texture at t100). On the producers' ORG paths these were the graph twins' D3D11 exports,
  imported by DCLF as resources of their own, unordered after the graph's writes.
- **Where it fits:** each integration registers the register as an `Api::FrameTexture` without a view, so only its graph path
  is new, and grass declares the published id in its family's `MainDeclare`. A further framework answer: DCLF maps any captured view that is a
  published twin's export to that id by itself (`PublishedResources::ExportedId` already finds it), declaring and indexing it
  at the main resources' check, so a producer's port needs no integration change. Until then, a colour commit logs
  `[DCLF] t<n> reads the D3D11 export of the published '<id>'` once per register and id.
- **Also:** the third pass registered t17 (Terrain Blending's 16-bit blended depth, bound by `VanillaBinding::BindSceneDepth`) and
  t26 (Cloud Shadows' occlusion cubemap, bound for cloud self shadowing) the same way, and gave Effects' soft depth (t118, the depth
  table's `kPOST_ZPREPASS_COPY`, which names the blended depth during the opaque pass) a graph path.
- **Done (2026-11-10), one rule:** a commit reads the graph resource when the view it resolved (the registration's, or the
  captured one) is the export of a published id its passes declared (`Extensions::GraphTextureIndex`,
  `PublishedResources::IsExportOf`). The per-register `graph` callbacks are gone: some read producer state at resolve time, after
  the capture. A registration lists its ids, more than one when a register can hold either texture, and the passes declare every
  id that was ever published. A register that may hold an id no registration lists (t17's R32 depth) still imports it.
- **Found by:** P3 DCLF published resources, second pass (2026-11-09); third pass (2026-11-10).

### One image imported under two names

- **Need:** two features' graph passes use the same D3D11 texture (one writes it, the other reads it).
- **Who:** Screenshot's capture read HDR Display's output and back buffer through imports of its own while HDR Display's
  passes wrote them through theirs (fixed: `ORGHDRDisplay::ImportOf`); Effects11's rays imported Terrain Blending's blended
  depth export (fixed: by name); ORGIBL imported Dynamic Cubemaps' environment export (fixed: by name). Open: DCLF's near
  twin writes Terrain Blending's terrain depth through DCLF's import (`cs.dclf.terrain-blending-depth`) while Terrain
  Blending's graph blend reads it through `FrameAnchors::ImportDepth`.
- **Where it fits:** `FrameAnchors` keys its imports by id, so a second id makes a second graph resource of one image, and
  nothing orders their uses. It now logs `[ORG] frame anchors: <id> imports the image <id> already imports` when that
  happens. A framework answer: key imports by the image (one graph resource per VkImage, whatever the name), and let DCLF's
  imports and the anchors' share that table.
- **Found by:** P3 DCLF published resources, second pass (2026-11-09).

### Graph textures read by views drawn before their producers

- **Need:** a view drawn early in the frame (DCLF's reflection faces) reads features' outputs that the graph writes later in
  the frame (last frame's contents).
- **Who:** DCLF's views with frame constants of their own read every frame texture's `view`: on the producers' ORG paths
  the D3D11 exports of Volumetric Shadows (t18), Exponential Height Fog (t19, t22), Cloud Shadows (t25), Dynamic Cubemaps
  (t30, t31), Skylighting (t50, t53), Terrain Shadows (t60) and IBL (t76-t79), imported as resources of their own. The ORG
  deferred composite reads its inputs through descriptors the graph does not track (`GpuTextures::ResolveBinding`) as well.
- **Where it fits:** the faces' passes would declare the published ids as the colour passes do (`DeclareFrameBuffers`); a
  read in an earlier epoch is then ordered after the previous frame's write (PublishedResources' rule). The composite needs
  the same declarations once its inputs are graph resources.
- **Done for the faces (2026-11-10):** their passes declare the ids of the frame textures with a view
  (`Extensions::FacesGraphTextureIds`), and their commit follows the same rule as the colour commit. The composite is still
  open.
- **Found by:** P3 DCLF published resources, second pass (2026-11-09).

### Knowing which build of a client an anchor's epoch runs

- **Need:** a client that registers new passes at its anchor's hook (a new shape, a readback plan) must know whether this
  frame's epoch runs them, so its commit writes the latch, readbacks and once-only bookkeeping of the passes that run.
- **Who:** Effects11's ORG FX runtime. With DCLF on, the host builds the graph only at DCLF's build point at the frame's
  start (`GraphHost::SetExplicitBuilds`), so a client registered at the Tonemap hook runs from the next frame, while
  `FrameAnchors::Run` already calls its commit. The FX readback read snapshot textures in the frame that registered them,
  before any build held their copies, and logged zeros for every step (fixed: each shape's passes set a flag in Prepare,
  and the commit writes the newest prepared shape). Post Processing's chain writes the newest build's latch the same way
  (one frame of stale values after each new chain).
- **Where it fits:** `FrameAnchors` could pass the commit the client registration its epoch's build holds (the snapshot the
  extension built from), or run a client's commit only once a build holds it.
- **Found by:** P3 FX execution, Tonemap anchor readback (2026-11-09).

### Boundary states of an in-stream epoch's imports

- **Need:** an in-stream epoch (`GraphHost::SetExternalEpochs`) enters and leaves every DXVK import in the layout DXVK rests
  it in (`DXVK_ORG_INTEROP_RESTING_LAYOUTS`, external home states). That is the whole contract on both sides: ORG's handoff
  lists name the resting layout as the entry state, and its exit barriers return to it. DXVK must have the image in that
  layout when the external command runs, and must not assume anything else about it afterwards.
- **What broke it:** DXVK, not the graph. `DxvkContext::acquireResources` promoted a render target's layout change to the
  init barriers whenever DXVK had not yet written the image in the command list, so the change ran before the whole list,
  ahead of an earlier external command that used the image. The validation layer showed it as an ORG-imported depth or
  colour target (DCLF's native depth, refraction normals, native targets) whose list expected the resting layout while the
  image was in the attachment layout, followed by DXVK's own `vkCmdBeginRendering` and `vkCmdPipelineBarrier2` with a stale
  layout. Every ORG barrier (`vkCmdPipelineBarrier`, BasicRHI's) agreed with the layer. Fixed in DXVK
  (`fix(dxvk): keep interop layout changes in order`).
- **For clients:** a kept or revision recording needs nothing beyond the resting layout at its boundary: its body depends on
  the compiled graph alone, and its handoff list comes from the ledger at submission. A client that records into DXVK's own
  command buffer (a late pass) gets the same boundary. Imports resting in GENERAL (`CS_ORG_RESTING_LAYOUTS=0`) never
  change layout and hide this kind of bug.
- **Copies of tracked images:** a DXVK relocation of an ORG-tracked image copied the old storage in its resting layout
  (often `SHADER_READ_ONLY_OPTIMAL`, not a legal copy source). It now copies from the transfer source layout and puts the old
  storage back to rest in the same command buffer (`fix(dxvk): copy relocated interop images legally`).
- **Found by:** P3 kept layouts, RX 6600 validation runs (2026-11-10).

### Pipelines built from a D3D11 plugin's shaders and states

- **Need:** a plugin that hands the engine its own shaders or state objects (ShaderTools' replacement Lighting pixel shaders,
  anything that writes `BSGraphics::*Shader::shader` or a state table entry) gets a pipeline on ORG built from them. ORG has to
  know each object's bytecode, its source when there is one, and its state description.
- **Who:** CS itself (ShaderCache's replacements, `Deferred`'s blend table swap, `ShadowmapRasterizerFix`'s rasterizer
  clones) and any mod that creates D3D11 shaders or states through the engine's device.
- **Where it fits:** P4.6's description registry (`src/RenderGraph/Engine/Pipeline/Descriptions.h`): the device hooks record
  every shader and input layout, state objects are described from `GetDesc` on first lookup, and `PipelineKey` /
  `PipelineStates` feed `org::PipelinePermutations`. A plugin's FXC-only shader has bytecode but no source, so an ORG build
  needs its HLSL (or DXC) from the plugin.
- **Gaps found:** `org::GraphicsPipelineDesc` has one define list for all stages, but an engine vertex and pixel shader are
  compiled with different defines, so either ORG takes per-stage defines or engine stages are compiled one at a time (as
  DCLF's `ShaderPrograms` does). rhi has no scissor-enable flag (a scissor-off draw needs a covering scissor), and input
  layouts need their strides at pipeline creation, so the key carries the geometry's strides unless the pipeline takes
  dynamic strides.
- **Found by:** P4.6 (2026-11-10).

### Textures the CPU rewrites every frame

- **Need:** a texture whose whole contents the CPU writes each frame (a decoded video frame, a software-rendered overlay)
  reaches a pass without a D3D11 `Map(WRITE_DISCARD)` and without the producer waiting for the GPU.
- **Who:** the engine's Bink player (256x256 BGRA tiles through `Renderer::LockTexture`, SE 75514); ImGui's font pages
  (updated by rectangle). Plugins that play their own video or draw their own CPU canvas do the same with dynamic textures.
- **Where it fits:** both ports keep a small pool of host-visible `LatchBlock` staging buffers, hand the producer a pointer
  into one, and have the pass record the copies and the textures' barriers itself (`src/RenderGraph/Engine/Bink/BinkGraph.cpp`,
  `src/RenderGraph/ImGuiBasicRHI.cpp`); the frame keeps its buffer until its GPU work retires. The pool and the copy recording
  are written twice now: one upload-ring helper (a buffer per frame from a retire-aware pool, plus "copy these regions into
  this untracked texture" for a pass) would serve both and a plugin API.
- **Found by:** P5.11 Bink (2026-11-10).

### Anchors on the thread that holds the renderer's lock

- **Need:** run anchor epochs on whichever thread is rendering a frame under the engine's `Renderer::Lock`, not only the
  main thread.
- **Who:** the engine's startup sequence (the Bethesda logo decodes and presents on the "MoviePlayer" thread, which holds
  the lock for the whole sequence) and its loading screen (P5.12). Today `FrameAnchors::Run` returns false there and those
  frames stay on D3D11.
- **Where it fits:** `FrameAnchors::OnMainThread` widened to "the main thread, or the owner of `Renderer::Lock` while the
  main thread is not in a frame". The engine already serializes frames by that lock, so anchor runs never overlap and the
  lock orders the anchors' state between threads. The design is in `docs/development/p5/bink.md`, "Threads, and what P5.12
  needs".
- **Found by:** P5.11 Bink (2026-11-10).
- **Built (P5.12, `CS_ORG_LOADING_THREAD=1`):** `FrameAnchors::OnRenderThread`, a render owner claimed by the thread that holds
  the lock (`docs/development/p5/loading-screen.md`). ORG needed no change: `GraphHost::Uploads()->SetOwnerThread()` moves the
  frame upload instance, and the host has no thread id of its own. What a plugin API would want: an explicit
  `GraphHost::SetOwnerThread()` that moves every owner-thread structure at once (today only the upload instance keeps one), and
  a debug check in the host that its owner-thread calls come from one thread between two such moves.

### Builds that wait for an explicit point while no world frame runs

- **Need:** a client that joins an anchor's graph while no world frame runs (a load, a blocking movie) is built only at
  Drawcall Limit Fix's build point, the next world frame; a synchronous `BuildIfRequested` on the frame's path is what the
  rebuild rule forbids.
- **Who:** clients registering during a load (texture streaming's per-batch copy pass, a new Scaleform surface); Bink's logo
  builds once by itself.
- **Where it fits:** the host building a requested graph on its own thread and switching to it at the next submission that
  allows it (as DCLF's revisions do), or features that never register during play (one kept pass with gated work in place of
  one-shot passes per batch).
- **Found by:** P5.12 loading screen (2026-11-10).

### A pass at an engine draw inside another frame

- **Need:** draw where an engine subsystem draws inside a frame it does not own, under what is drawn after it.
- **Who:** Bink's in-frame movies (player mode 2, `MoviePlayer::TickInFrame`, SE 87920 / AE 90293), which draw their tiles
  into the caller's frame and do not present; the Present anchor is too late for them (it would draw over the UI).
- **Where it fits:** an anchor at the draw, or the draw joining the epoch of whatever frame the caller is in. Until then
  those movies stay on D3D11.
- **Found by:** P5.11 Bink (2026-11-10).

### Register-bound shaders on a pass

- **Need:** a pass draws or dispatches a shader written for D3D11 registers (textures t0-t15, samplers s0-s15, UAVs,
  constants at b0, b2 and b12) that cannot move to a bindless push block, because the D3D11 path still finds its
  constants by cbuffer member name. The ORG pass binds each register from a table filled per draw.
- **Who:** every ImageSpace shader (P5.6: 124 slots), and any mod shader replacing one of them.
- **Where it fits:** a descriptor-table binding model beside `ShaderBindings`' push block: the shader builds for SPIR-V
  with register shifts (`-fvk-t-shift 0 0 -fvk-s-shift 100 0 -fvk-u-shift 200 0 -fvk-b-shift 300 0` builds and validates
  for all of them), and the pass maps each binding to an engine target, texture or latch. See
  `docs/development/p5/imagespace-shaders.md`, "What the ORG ImageSpace pass still needs".
- **Found by:** P5.6 shader sources (2026-11-10).

### Drawing an engine family at its own call with the engine's state latched

- **Need:** replace a family of engine draws (the sky) at the exact point in the engine's pass where it draws, with the engine's
  own CPU setup, constants and state, and draw it on the graph there. The point is inside the engine's render pass and can run
  several times a frame (the main pass, each reflection face).
- **Who:** Phase 5 families that the engine draws in a batch of its own (sky, water, blood splatter); a plugin that replaces an
  engine shader's draws.
- **Where it fits:** a frame anchor at the call (CS added `Sky` and `SkyReflections` for this, P5.3), whose client's pass is
  recorded per run from a snapshot the render thread publishes before `FrameAnchors::Run` (revision = the snapshot's draws without
  their constants, constants in a latch). The engine's batch runs with its draw function latched (a detour that records instead of
  drawing), so every engine side effect of the setup stays. `ConstantMirror` (`Watch` from the shader's SetupGeometry, `Contents` at
  the draw) gives the constants without a decode.
- **Gaps found:** an anchor is a fixed enum, so every such point is a framework edit; a generic "run an epoch here" for families
  would avoid that. `RasterProgram` has no vertex input or index buffer: a family pulls its vertices by device address
  (`DCLFVertexPull.hlsli`, SPIR-V only, so its DXIL build draws nothing yet), and draws one render pass per draw. It has no
  stencil reference either. ORG makes per-slice views of a cube array as cube-array views, so a pass cannot bind one cube of an
  array as a `TextureCube` (Cloud Shadows' per-layer occlusion stays out of the ORG sky for that reason).
- **Found by:** P5.3 (2026-11-10).
### Drawing a UI library's batches on the graph

- **Need:** a 2D renderer (Scaleform, another UI library, a plugin's HUD) draws many small batches per frame, each with its
  own blend mode, stencil mask state, scissor and textures, interleaved with D3D11 work, and wants them on the graph without a
  pipeline per batch made at record time.
- **Who:** the engine's Scaleform (P5.10); HUD plugins that draw through Scaleform's renderer or their own D3D11 code.
- **Where it fits:** the Interface anchor and the pattern P5.10 uses: a CPU capture of the batches, one staging buffer per
  display holding the geometry and a constants record per draw (read by `SV_VertexID`), and a `RasterProgram` per (shader
  pair, blend, stencil, formats) made on first use. What would make it cheaper:
  - **Topology in `RasterProgram`.** Its pipelines are triangle lists only, so Scaleform's line strips stay on D3D11.
  - **Pipelines keyed at record time** for raster programs (P0.6 has them for ORG's own pipelines): a blend or stencil state
    per batch without a program object each.
  - **Several runs of an anchor a frame are slots.** Each display is an epoch, and each uses a frame slot. A per-frame batch
    (one epoch at the end of `UI::DrawInterface`) needs the D3D11 work between displays (menu 3D, Bink, other mods) on the
    graph first.
  - **Render ownership on another thread** (the loading screen, P5.12): anchors run on the main thread only, so a UI drawn
    from the loading-screen thread stays on D3D11.
- **Found by:** P5.10 (2026-11-10).
