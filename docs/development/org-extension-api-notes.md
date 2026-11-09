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
  with the last build that holds it. BC formats work. An API for plugins needs a texture upload that async epochs record.
- **Found by:** survey for the plan (2026-11-09); the graph upload by P3 TerrainShadows (2026-11-09).

### Sharing graph resources with D3D11 code

- **Need:** a plugin's pass writes a texture or buffer that D3D11 code still reads (its own, or an engine draw it feeds), or
  reads a CS feature's graph output from D3D11.
- **Who:** any plugin moved to the frame anchors while part of it stays on D3D11.
- **Where it fits:** CS's graph-backed resources (`src/RenderGraph/GraphResources.h`; render-graph.md, "Graph-backed feature
  resources"). An exported API needs the same rules:
    - an exported resource is never aliased;
    - its D3D11 views go before its memory, and the memory waits for D3D11's last use (CS's `ReleaseAfterD3D11`);
    - a texture rests in DXVK's layout for it between epochs, and its views use its own typed format;
    - a resize rebuilds the client's anchors (`FrameAnchors::Refresh`).

  The DXVK exports underneath are `dxvkCreateBufferFromVkBuffer` and `dxvkCreateTextureFromVkImage` (interop version 6). A plugin
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
- **Found by:** P3 DCLF graph textures (2026-11-09).

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
- After PostProcessing, Tonemap and Present, the anchor separates its epoch from Vulkan work that may follow. That is the hazard
  rule in render-graph.md.

Each anchor's rule:

- **ReflectionsPrepass.** At `BSCubeMapCamera::RenderCubemap`, before the engine draws the cubemap. After every feature's
  `ReflectionsPrepass`. It runs once per cubemap render, which can be zero or several times a frame. Each run is one
  execution of its epoch.
- **EarlyPrepass.** After `Main::RenderShadowMaps` returns. The shadow maps are drawn, DCLF's shadow epoch included, and the
  shadow light data is copied (t98). After every feature's `EarlyPrepass`. Before `Main::RenderDepth`: the scene depth is
  not drawn yet.
- **Prepass.** At the main pass's first opaque batch, inside `StartDeferred`. After every feature's `Prepass`, so after Light
  Limit Fix's culling epoch. Before DCLF's colour epoch. The Z-prepass depth is drawn. The GBuffer targets are bound but not
  cleared yet: they still hold the last frame.
- **DeferredPasses.** In `EndDeferred`, after the opaque pass, the blended decals and the final depth copy into
  `kPOST_ZPREPASS_COPY`. Before the D3D11 SSGI, SSS, cubemap update, deferred composite and Effects11's rays. The GBuffer is
  complete. A pass that must follow the composite uses DeferredComposite.
- **DeferredComposite.** In `Deferred::DeferredPasses`, after the D3D11 SSGI, SSS and cubemap update, where the deferred
  composite runs. The ORG composite (`CS_ORG_COMPOSITE=1`, key 0) is a pass here; when it ran, the D3D11 dispatch is skipped.
  A pass with a higher key reads the composited `kMAIN`. Before the D3D11 `DynamicCubemaps::PostDeferred` and Effects11's
  rays. When SSGI and SSS move to DeferredPasses (P3 W2), the composite can join them there; this anchor stays for passes
  that follow it.
- **PostProcessing.** At `Main_PostProcessing`, before Post Processing's `DrawBeforeUpscaling`, the upscaler and the
  ImageSpace chain. The scene is at render resolution.
- **Tonemap.** At the ImageSpace HDR tonemap's render, after the upscaler. Before Effects11 and Post Processing's `PreProcess`.
  `Point::input` and `Point::output` are the tonemap's targets. Its graph may record ahead of the hook, so a pass that depends
  on them puts them in its revision.
- **Present.** In `IDXGISwapChain::Present`, before DCLF's `BeforePresent`, the frame pace signal, CS's overlay, HDR output,
  frame generation and the present. It is before the UI: a pass after the UI uses Overlay.
- **Overlay.** At CS's overlay draw (`OverlayRenderer::FinalizeImGuiFrame`), inside HDRDisplay's present chain: after Present,
  DCLF's `BeforePresent` and the frame pace signal. ImGui has built its frame. Before HDR output and frame generation. It runs
  only on frames where CS's overlay draws (the menu, an editor, notifications, the performance overlay), not every frame. The
  target is the render target bound for the overlay: the framebuffer, or HDR's UI texture when HDR composites the UI.
- **Capture.** At the end of HDRDisplay's present chain (`ScreenshotFeature::ProcessCaptureRequest`), just before the native
  Present or frame generation's present. After Overlay and HDR output: the back buffer (or the owned frame's source) holds
  what the display will show. Reserved only by the Screenshot feature's ORG path (`CS_ORG_SCREENSHOT=1`) and the self-test.

Keys used so far:

- The anchor self-test (`CS_ORG_ANCHOR_SELFTEST`): 0, 1 and 2 at every anchor.
- The graph resource self-test (`CS_ORG_GRAPHRES_SELFTEST`): 0 at DeferredPasses.
- Screen Space Shadows (`CS_ORG_SCREEN_SPACE_SHADOWS`): 100 (clear) and 101 (raymarch) at Prepass.
- The deferred composite (`cs.deferred.composite`): 0 at DeferredComposite.
- The shared constants' upload (`cs.shared.upload`): first at every anchor where a client reads them, outside the keys.
- Terrain Shadows' update (`CS_ORG_TERRAIN_SHADOWS`): 100 at EarlyPrepass.
- Volumetric Shadows (`CS_ORG_VOLUMETRIC_SHADOWS`): 100 to 105 at EarlyPrepass (per mip: downsample, horizontal blur, vertical
- Light Limit Fix's local shadow cache (`CS_ORG_LLF_SHADOWS`): 100 (slot data) and 101 (shadow map copy) at EarlyPrepass.
- Grass Collision (`CS_ORG_GRASS_COLLISION`): 200 (copy the colliders) and 201 (update the texture) at EarlyPrepass.
- CS's ImGui (`CS_ORG_IMGUI`): 1000 at Overlay, last.
- The Screenshot feature (`CS_ORG_SCREENSHOT`): 100 (the readback copy) at Capture.
- The DCLF graph texture test (`CS_DCLF_GRAPH_TEXTURE_TEST`): 0 at Prepass.
- Subsurface Scattering (`CS_ORG_SUBSURFACE_SCATTERING`): 199 (Burley clear), 200 (extraction), 201 (horizontal or Burley
  blur) and 202 (vertical blur) at DeferredPasses. SSGI's passes there stay below 199, as the D3D11 order is SSGI first.
- Screen Space GI (`CS_ORG_SCREEN_SPACE_GI`): 100 (history clears, after a reset only), 110 to 116 (the chain's steps) and 117
  (the accumulation copy without the blur) at DeferredPasses, below Subsurface Scattering's 199-202.
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
- **Found by:** P3 W2, Screen Space GI (2026-11-09).

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
  block and declares nothing. Macros give the members their bare names under ORG, so shader bodies and the FXC bytecode
  stay the same.
- **Found by:** P3 W2, Screen Space GI (2026-11-09).
### Reading other features' graph outputs by name, and knowing whether they are there

- **Need:** a pass reads several other features' outputs (textures and buffers) as graph resources, each only while that
  feature's graph path is active, and otherwise reads the D3D11 resource or does without.
- **Who:** Exponential Height Fog's light scattering reads IBL's SH (`IBL::EnvIBL`, `IBL::SkyIBL`), Skylighting's probes
  (`Skylighting::ProbeArray`), Terrain Shadows' heights (`TerrainShadows::ShadowHeight`), Light Limit Fix's lights and lists
  (`cs.llf.*`), local shadows (`LightLimitFix::LocalShadowData`, `LightLimitFix::LocalShadowCache`) and Cloud Shadows'
  occlusion (`CloudShadows::Occlusion`). Today it asks each
  producer's ORG class whether it is active and compares the feature's D3D11 view with the producer's export to decide.
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
  they go into a `ScratchImage`, and `Util::SaveImageToFile` writes it (`CS_ORG_UNIFIED_WATER=1`). A GPU pass would add an
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
  latched values block plus a handle table (two push words per pass), so recordings stay reusable.
- **Found by:** P3 W5, ORG FX runtime (2026-11-09).

### Textures generated once at startup

- **Need:** fill a persistent lookup texture once (a noise, a LUT), before its first reader in any epoch, and never again
  unless the graph loses it.
- **Who:** CS's TruePBR glints noise (`src/TruePBR/ORGGlintsNoise.cpp`, `CS_ORG_GLINTS`); any plugin with a generated LUT.
- **Where it fits:** today the client registers a fill pass at the earliest anchor its readers follow (EarlyPrepass), is
  active only until the commit of the epoch that carries it, then calls `FrameAnchors::Refresh` so the next build leaves
  the pass out. The texture is a Persistent `GraphResources` twin, published for Drawcall Limit Fix. A framework answer
  would be an "initialize" callback on a persistent resource, which the graph runs once before the resource's first use
  in any epoch and again after a device or graph reset. There is no reset event today: a failed graph returns the
  feature to D3D11, which makes the texture again.
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
