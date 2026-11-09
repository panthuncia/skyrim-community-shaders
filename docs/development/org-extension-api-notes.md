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
  complete. Until the composite is an ORG pass (P2.7), a pass that must follow the composite cannot use this anchor.
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
