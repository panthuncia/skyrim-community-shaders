# P4.2: the engine's render targets on ORG

Task P4.2 of [org-full-port.md](../org-full-port.md). The code is `src/RenderGraph/Engine/Targets/EngineTargets.{h,cpp}`, behind
`CS_ORG_ENGINE_TARGETS=1`. This file holds the RE it rests on and how the port works. Addresses are image addresses, IDs are
Address Library IDs. SE is 1.5.97, AE is 1.6.1170. Functions marked **renamed** were renamed in both Ghidra databases.

## Creation functions

`BSShaderRenderTargets::Create` (SE 100458 `0x1412e5570`, AE 107175 `0x1414cf210`) makes every render target, depth-stencil target
and cubemap through three small wrappers. Each wrapper copies the properties into `RenderTargetManager`'s table (singleton SE
`0x14302bb20`), then calls the Renderer function with a constant name string. CS's call-site hooks in `Hooks.cpp` patch the calls
to these wrappers (that is why their thunks take three arguments), so they change the properties before the Renderer function sees
them.

| Function                                     | SE ID / addr        | AE ID / addr        | Signature                                                                                    |
| -------------------------------------------- | ------------------- | ------------------- | -------------------------------------------------------------------------------------------- |
| `BSShaderRenderTargets::CreateRenderTarget`  | 75638 / 0x140d74b60 | 77445 / 0x140e4fbb0 | `(manager, target, RenderTargetProperties*)`, table stride 0x1C                              |
| `RenderTargetManager::CreateDepthStencilTarget` | 75639 / 0x140d74bb0 | 77446 / 0x140e4fc00 | `(manager, target, DepthStencilTargetProperties*)`, table at +0xC78 (SE), stride 0x10     |
| `RenderTargetManager::CreateCubeMapRenderTarget` | 75640 / 0x140d74be0 | 77447 / 0x140e4fc30 | `(manager, target, CubeMapRenderTargetProperties*)`, stride 0xC                           |
| `Renderer::CreateRenderTarget` **renamed**   | 75467 / 0x140d6a870 | 77253 / 0x140e44d90 | `(Renderer*, target, "RenderTarget", RenderTargetProperties*)`                               |
| `Renderer::CreateDepthStencilTarget` **renamed** | 75469 / 0x140d6aca0 | 77255 / 0x140e45180 | `(Renderer*, target, "Depth/stencil target", DepthStencilTargetProperties*)`            |
| `Renderer::Create3DRenderTarget` **renamed** | 75470 / 0x140d6aef0 | 77256 / 0x140e453b0 | `(Renderer*, target, name, width, height, depth, int usage, DXGI_FORMAT)`                    |
| `Renderer::CreateCubeMapRenderTarget` **renamed** | 75471 / 0x140d6b0c0 | 77257 / 0x140e45540 | `(Renderer*, target, "Cubemap target", CubeMapRenderTargetProperties*)`               |
| `VolumetricLighting::CreateAccumulationTargets` **renamed (SE)** | 0x1412dcdd0 | 0x1414c4f40 _(caller list)_ | 3D targets 0 and 1, R16_FLOAT, usage DEFAULT                          |
| `VolumetricLighting::CreateNoiseTarget` **renamed (SE)** | 0x1412dce60 |                     | 3D target 2, R8_UNORM                                                                        |
| `Renderer::CreateRenderTexture`              | 75507 / 0x140d6dbc0 | 77299 / 0x140e48190 | `(Renderer*, width, height)` returns a `BSGraphics::Texture`: not a render target (below)    |

The Renderer functions have no callers but the wrappers, and the wrappers none but `BSShaderRenderTargets::Create`. The 3D function is
called only by the volumetric lighting, when it renders, so 3D targets are made later than the rest and are not part of the set.
Every call runs on the render thread (InitD3D, or `Renderer::Begin`'s resize).

### `Renderer::CreateRenderTarget` (75467 / 77253)

`RenderTargetProperties` (0x1C): width +0, height +4, format +8 (a DXGI_FORMAT), copyable +0xC, supportUnorderedAccess +0xD,
allowMipGeneration +0xE, mipLevel +0x10, textureTarget +0x14.

Own texture (`mipLevel == -1`):

- `CreateTexture2D`: width, height, `MipLevels = allowMipGeneration ? 0 : 1`, ArraySize 1, the format, one sample, DEFAULT, bind
  SRV | RT (0x28), plus UAV (0xA8) with supportUnorderedAccess, `MiscFlags = allowMipGeneration ? GENERATE_MIPS : 0`.
- `CreateRenderTargetView(texture, NULL)` and `CreateShaderResourceView(texture, NULL)`: whole-texture views in its format.
- copyable: the same description with bind SRV only (0x8), its own texture and default SRV (`textureCopy`, `SRVCopy`).
- supportUnorderedAccess: `CreateUnorderedAccessView` with {format, TEXTURE2D, mip 0}.

Mip view (`mipLevel != -1`): no texture of its own. RTV {format, TEXTURE2D, MipSlice = mipLevel}, SRV {format, TEXTURE2D,
MostDetailedMip = mipLevel, MipLevels 1} and with supportUnorderedAccess a UAV of that mip, all of `renderTargets[textureTarget].texture`.
Its `texture` field stays null.

Fields: `RenderTargetData` at `Renderer + 0xA58 + target * 0x30` (`RendererData + 0xA48`): texture +0, textureCopy +8, RTV +0x10,
SRV +0x18, SRVCopy +0x20, UAV +0x28 (CommonLib's `RenderTargetData`). AE has 116 targets against SE's 114, at the same start.

### `Renderer::CreateDepthStencilTarget` (75469 / 77255)

`DepthStencilTargetProperties` (0x10): width +0, height +4, arraySize +8, stencil +0xD, use16BitsDepth +0xE.

| Case                      | Texture         | DSV               | Depth SRV              |
| ------------------------- | --------------- | ----------------- | ---------------------- |
| default                   | R32_TYPELESS    | D32_FLOAT         | R32_FLOAT              |
| use16BitsDepth            | R16_TYPELESS    | D16_UNORM         | R16_UNORM              |
| stencil (wins over 16-bit) | R24G8_TYPELESS | D24_UNORM_S8_UINT | R24_UNORM_X8_TYPELESS  |

- The texture is made through `Renderer + 0x48` (the device), MipLevels 1, ArraySize = arraySize, bind DS | SRV (0x48), no misc flags.
- Per slice: a DSV (flags 0) and a read-only DSV (READ_ONLY_DEPTH, plus READ_ONLY_STENCIL with a stencil). TEXTURE2D for one slice,
  TEXTURE2DARRAY {mip 0, FirstArraySlice = slice, ArraySize 1} for more.
- One depth SRV: TEXTURE2D {0, 1}, or TEXTURE2DARRAY {0, 1, 0, arraySize}. With a stencil, a second SRV in X24_TYPELESS_G8_UINT
  with the same dimensions.
- Fields: `DepthStencilData` at SE `Renderer + 0x1FB8 + target * 0x98` (AE `+0x2018`): texture, views[8], readOnlyViews[8], depthSRV,
  stencilSRV. Eight views per table entry, so arraySize is at most 8.

### `Renderer::CreateCubeMapRenderTarget` (75471 / 77257)

`CubeMapRenderTargetProperties` (0xC): width, height, format. Texture: six slices, MipLevels 1, bind SRV | RT, misc TEXTURECUBE.
An RTV per face {format, TEXTURE2DARRAY, mip 0, FirstArraySlice = face, ArraySize 1}, one SRV {format, TEXTURECUBE, 0, 1}. Fields:
`CubemapRenderTargetData` at SE `Renderer + 0x26D8 + target * 0x40` (AE `+0x2738`): texture, cubeSideRTV[6], SRV. One cubemap
(kREFLECTIONS). CS's hook makes it 256x256.

### `Renderer::Create3DRenderTarget` (75470 / 77256)

Texture3D {width, height, depth, MipLevels 1, format, usage}. Bind SRV | UAV (0x88), none for STAGING; CPU write for DYNAMIC, read
and write for STAGING. On success: the dword at +0x18 is set to 1; without STAGING an SRV {TEXTURE3D, 0, 1} at +0x10 and a UAV
{TEXTURE3D, mip 0, FirstWSlice 0, WSize = depth} at +8. Texture at +0. `Texture3DTargetData` at SE `Renderer + 0x2718 + target * 0x20`
(AE `+0x2778`). It sets a TLS memory context (+0x768 = 0x28) around the creation. The volumetric lighting makes all three (accumulation,
its copy, noise) with usage DEFAULT.

### `Renderer::CreateRenderTexture` (75507 / 77299)

Not a render target: `BSFaceGenManager::PrepareHeadPartForShaders` makes an R8G8B8A8_UNORM, SRV-only, one-mip texture and wraps it
in a new `BSGraphics::Texture` (SRV at +0x10). It goes the way of every texture (released through 75527 / 77322), may run on loader
threads, and nothing in `RendererData` points at it. It belongs with P4.3's texture path and is not hooked here.

## Readers of the fields

- The engine: `SetDirtyStates` and `ApplyState` bind RTVs (`RendererData + 0xA58 + i*0x30 + 0x10`), DSVs and read-only DSVs, cube face
  RTVs, and SRVs for every shader; `CopyResource` into kPOST_ZPREPASS_COPY and between targets and their copies; ImageSpace effects
  read SRVs and write UAVs. All through the immediate context, so DXVK's own objects must sit in the fields unless that context is a
  wrapping tripwire guest.
- CS: `FrameAnchors::EngineTarget`/`EngineDepth`/`EngineCubemap`/`D3D11Texture`/`ImportDepth` (about 30 call sites in the ports),
  DCLF's `Core/Render/Resources` (main targets, depth, refraction normals, shadow mask, view targets, shadow maps), `Deferred.cpp`
  (replaces kINDIRECT, kINDIRECT_DOWNSCALED, kRAWINDIRECT, kRAWINDIRECT_DOWNSCALED, kRAWINDIRECT_PREVIOUS,
  kRAWINDIRECT_PREVIOUS_DOWNSCALED, kWATER_1, kWATER_2 with its own textures), HDR Display (upgrades seven LDR targets to RGBA16F, and
  points kFRAMEBUFFER at its own texture each frame), Upscaling (kMAIN, the depth and their copies), `Vram::TagGameRenderTargets`.

## Resize

`Renderer::WindowSizeChanged` (75453 / 77238) at `Renderer::Begin`, when the window size changed:

1. `ClearState`, `Flush`, the shadow state's flags reset, window views and kFRAMEBUFFER's views released.
2. For each of the 114 (AE 116) `RenderTargetData` whose RTV is set: Release RTV, SRV, texture; then, if textureCopy is set, Release it
   and SRVCopy. Every one of those fields is nulled. **The UAV is never released**: each resize leaks every target's UAV (and CS adds
   UAVs to five targets).
3. `ZeroDepthStencilData` on each depth-stencil entry in order, **stopping at the first one whose texture is null** (12 on SE).
4. `Flush`, `ResizeBuffers(0, 0, 0, UNKNOWN, ALLOW_MODE_SWITCH)`, `CreateSwapChainViews`, then the callback at `0x14302c868`, which is
   `BSShaderRenderTargets::Create`: the whole set is made again, CS's hooks included.

Cubemap and 3D targets are not released there. CS keeps the window from resizing after its menu initialized (`WndProcHandler_Hook`), so
in practice the set is made at startup only, but the path works.

## How the port works

- **Detours** on the four Renderer functions (`Install`, from `Hooks.cpp`). The wrappers and CS's call-site hooks above them run as
  before, so CS's upgrades (`ModifyRenderTarget`'s UAVs, the snow targets' RGBA16F, copyable refraction normals and underwater mask,
  the precipitation mask's 16-bit depth without stencil, the reflections' 256x256) arrive in the properties and become the ORG
  description. Nothing is duplicated, and with the switch off nothing changes.
- **ORG makes the image** with `GraphResources::ExportTexture` (new: the twins' export path with a typeless texture format and a
  format per view). A depth image is made in its DSV's format; BasicRHI and DXVK both turn D24S8 into D32S8 where the device has no
  D24S8 attachment, so the image DXVK expects and the one BasicRHI makes agree. `dxvkCreateTextureFromVkImage` gives the D3D11
  texture; CS makes the views exactly as the engine would, on the raw DXVK device.
- **Kept on D3D11:** kFRAMEBUFFER; `allowMipGeneration` (DXVK refuses `GENERATE_MIPS` on a client image); mip views (their views
  are made by the engine's function on the owner's texture, ORG's when it is); non-DEFAULT 3D targets; any failure.
- **Startup:** the first `Create` runs inside InitD3D, before `RenderGraphRuntime::Initialize` (State::Setup). The detours record
  the properties and let D3D11 make the targets. `TakeOver`, right after Initialize and before any feature's SetupResources, makes them
  on ORG, releases the D3D11 objects and fills the fields, then has the engine's function remake the mip views on the new textures.
  The D3D11 set lives for those few milliseconds only.
- **Resize as a set:** `BeginSet`/`EndSet` around `Create`; each detour makes its target on ORG directly; `EndSet` republishes every
  published id and moves the resize generation once, so the anchors build again once and `PublishedResources::Refresh` sees the set.
  The UAV the engine leaks is released when ORG made it.
- **Lifetime:** every exported D3D11 texture carries a COM token in its private data (`SetPrivateDataInterface`). DXVK releases it when
  it destroys the texture, after every view and reference is gone, from any thread. The token queues the image; the frame pace point
  hands it to `ReleaseAfterD3D11`. So whoever releases last (the engine, a cached reference in a feature), the memory outlives it.
- **Publishing:** ids `Engine::<RENDER_TARGET>`, `Engine::<RENDER_TARGET> Copy`, `Engine::Depth::<name>`, `Engine::Cubemap::<name>`,
  `Engine::3D::<name>`, on first request (`Find`, `FindImage`): publishing every target would make each graph register about 150 more
  resources. `FrameAnchors`' imports (`EngineTarget`, `EngineDepth`, `EngineCubemap`, `D3D11Texture`, `ImportDepth`) and DCLF's
  `ImportImage` return the published texture for an image ORG owns. Anchors and DCLF then share one resource per image, so ORG orders
  their uses; before, they imported the image twice and relied on never sharing an epoch.
- **Repurposed targets:** `Deferred::SetupRenderTarget` and HDR Display's LDR upgrade call `Targets::Recreate` with their description
  (HDR keeps the copy); HDR's restore calls `Targets::Restore`, which remakes the target from the engine's properties.
- **Tripwire handles:** `CS_ORG_ENGINE_TARGET_HANDLES=1` puts handles that wrap the DXVK objects in the fields, only when the engine's
  device and context are tripwire guests (P4.1). The startup set is made and taken over before the guest exists. So
  `Targets::InstallHandles`, from the hook at the end of `Renderer::Init`, wraps the fields of every target ORG made: each
  handle wraps the object the field holds and takes the field's reference. Nothing is made again. Mip views and targets kept on
  D3D11 stay plain, as at creation. Targets made later (a resize, `Recreate`, the 3D targets) get handles at creation.
  `RenderGraphRuntime::DescribeResource` describes the wrapped object. The order is in
  [p4.1-tripwire.md](./p4.1-tripwire.md) ("Startup order").

## Open points

- Mip-generation targets stay D3D11 until the DXVK export accepts `GENERATE_MIPS` (or an ORG GenerateMips pass replaces the engine's
  call). The startup log counts how many targets were taken over.
- With handles on, every CS site that hands an engine field to `globals::d3d::context` or a DXVK export must unwrap it first.
  The readers were audited and fixed (p4.1-tripwire.md, "CS code that reads engine fields"). New code must do the same.
- `Vram::TagGameRenderTargets` tags the startup D3D11 set, which `TakeOver` releases at once. The ORG images are named `Engine::...`,
  which the VRAM ledger reads as a CS owner prefix (P4.7).
- HDR Display's per-frame redirect of kFRAMEBUFFER is untouched: kFRAMEBUFFER is the swap chain's.
- AE render target indices 114 and 115 are named `Engine::Target114`/`115` (CommonLib's enum stops at 114).
