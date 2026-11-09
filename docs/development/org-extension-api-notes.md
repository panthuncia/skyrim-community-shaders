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
- **Where it fits:** a pass in the Present anchor, after CS's own overlay (plan P1.6, P2.2). The plugin records its ImGui
  draw data and CS draws it, or it gets a render pass on the back buffer.
- **Found by:** survey for the plan (2026-11-09).

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
  Terrain Shadows' port does the graph half today: its heightmap is a persistent graph texture (`GraphResources::Texture2D`,
  no export), filled once from the DDS by `TEXTURE_UPLOAD_SUBRESOURCES` in its anchor client's `commit`, where the upload
  service is active. The upload copies the bytes at that call, so the source only has to live until then. BC formats work.
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
  frame generation and the present. It is not after the UI yet: an overlay pass after the UI needs P1.6.

Keys used so far:

- The anchor self-test (`CS_ORG_ANCHOR_SELFTEST`): 0, 1 and 2 at every anchor.
- The graph resource self-test (`CS_ORG_GRAPHRES_SELFTEST`): 0 at DeferredPasses.
- Screen Space Shadows (`CS_ORG_SCREEN_SPACE_SHADOWS`): 100 (clear) and 101 (raymarch) at Prepass.
- The deferred composite (`cs.deferred.composite`): 0 at DeferredComposite.
- The shared constants' upload (`cs.shared.upload`): first at every anchor where a client reads them, outside the keys.
- Terrain Shadows' update (`CS_ORG_TERRAIN_SHADOWS`): 100 at EarlyPrepass.
- Volumetric Shadows (`CS_ORG_VOLUMETRIC_SHADOWS`): 100 to 105 at EarlyPrepass (per mip: downsample, horizontal blur, vertical
- Light Limit Fix's local shadow cache (`CS_ORG_LLF_SHADOWS`): 100 (slot data) and 101 (shadow map copy) at EarlyPrepass.

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
