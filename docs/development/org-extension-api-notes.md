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
- **Found by:** survey for the plan (2026-11-09).

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
- The deferred composite (`cs.deferred.composite`): 0 at DeferredComposite.
- The shared constants' upload (`cs.shared.upload`): first at every anchor where a client reads them, outside the keys.

### CS's shared constants in a plugin's pass

- **Need:** read the camera matrices, screen size, frame counters and CS's feature settings (FrameBuffer, SharedData,
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
