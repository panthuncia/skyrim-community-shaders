# Shader Development Workflow

## Quick Reference

```bash
# Fast shader-only deployment (recommended for dev iteration)
cmake --build build/ALL --target COPY_SHADERS

# Full deployment (DLL + tests + shaders)
cmake --build build/ALL --target DEPLOY_ALL

# Prove an HLSL refactor changed no behavior (compares compiled DXBC vs a git ref)
pwsh tools/verify-shader-refactor.ps1 package/Shaders/Foo.hlsl   # or tools/verify-shader-refactor.sh

# Build a dual-target shader for FXC, DXC SPIR-V and DXC DXIL (see "Dual-target shaders")
python tools/check-dual-target-shaders.py features/IBL/Shaders/IBL/DiffuseIBLCS.hlsl
```

## Verifying refactors

`tools/verify-shader-refactor.ps1` (bash wrapper: `tools/verify-shader-refactor.sh`)
compiles a shader from a base git ref and from the working tree across the
`HDR_OUTPUT` permutations, then compares the compiled bytecode. The base
ref's whole include tree is materialized (via `git archive`), so the base compiles
against base-ref `.hlsli` headers and the working tree against working headers — a
refactor that also edits a shared header is compared correctly, not masked:

- **IDENTICAL** SHA-256 of the `.cso` ⇒ the refactor is a provable no-op (fxc emits
  no timestamps without `/Zi`, so identical source ⇒ identical bytes).
- **DIFFERS** ⇒ it dumps and diffs the `/Fc` assembly so a legitimate-but-non-identical
  change can be reviewed.

Exit codes: `0` all identical, `2` some differ, `1` compile error. Defaults to comparing
the working tree against `merge-base(HEAD, origin/dev)`; pass `-BaseRef <ref>` to override.
Requires `fxc.exe` from the Windows SDK. The permutation sweep is strong evidence, not the
full `shader-validation.yaml` matrix — pass `-Permutations` for exotic define combos.

## Dual-target shaders (D3D11 and ORG)

Until a feature's ORG port replaces its D3D11 path, the same `.hlsl` builds two ways:

| Path | Compiler | Profile | Bindings |
| --- | --- | --- | --- |
| D3D11 | FXC (`Util::CompileShader`) | `*_5_0` | registers: `t#`, `u#`, `s#`, `b#` |
| ORG | DXC (`ComputeProgram`, `RasterProgram`) | `*_6_6`: SPIR-V now, DXIL for D3D12 later | handles in a push block; resources from the descriptor heap |

`package/Shaders/Common/OrgResources.hlsli` lets one resource list serve both. Every ORG compile defines
`CS_ORG_BINDLESS` (`ProgramBuild::MakeRequest`). FXC never does.

**Writing one.** List the resources once, then expand the list twice:

```hlsl
// Before
TextureCube<float4> EnvTexture : register(t0);
RWTexture2D<sh2> IBLTexture : register(u0);
SamplerState LinearSampler : register(s0);

// After
#include "Common/OrgResources.hlsli"

#define DIFFUSE_IBL_RESOURCES(X)               \
	X(SRV, TextureCube<float4>, EnvTexture, 0) \
	X(UAV, RWTexture2D<sh2>, IBLTexture, 0)    \
	X(SAMPLER, SamplerState, LinearSampler, 0)
ORG_PUSH_BLOCK(DIFFUSE_IBL_RESOURCES(ORG_HANDLE))
DIFFUSE_IBL_RESOURCES(ORG_RESOURCE)
```

The shader body does not change. Each entry is `X(kind, type, name, register number)`:

| Kind | D3D11 | ORG |
| --- | --- | --- |
| `SRV` | `T name : register(t#)` | `static T name = ResourceDescriptorHeap[handle]` |
| `UAV` | `T name : register(u#)` | as `SRV` |
| `SAMPLER` | `T name : register(s#)` | `static T name = SamplerDescriptorHeap[handle]` |
| `CBUFFER` | `cbuffer nameBuffer : register(b#) { T name; }` | `static ConstantBuffer<T> name = ResourceDescriptorHeap[handle]` |
| `LATCH` | as `CBUFFER` | `static T name = ((ByteAddressBuffer)ResourceDescriptorHeap[handle]).Load<T>(offset)` |
| `PUSH` | as `CBUFFER` | the struct itself, inside the push block |
| `EXTERN` | nothing: the header that declares it binds the register | its handle; the header reads `OrgHandle_<name>` |
| `EXTERN_LATCH` | nothing: the header that declares it binds the register | its handle and offset; the header declares the `LATCH` |
| `SHARED` | nothing: `FrameBuffer`, `SharedData`, `FeatureData` and `Permutation` stay the headers' cbuffers | four handles, in that order |

`SRV` and `UAV` cover `Texture2D`, `Texture3D`, `TextureCube` and their arrays, the `RW` textures, `(RW)StructuredBuffer`,
`(RW)ByteAddressBuffer` and typed buffers. `SAMPLER` covers `SamplerState` and `SamplerComparisonState`. `CBUFFER`, `LATCH` and
`PUSH` take a struct, and code reads its fields as `name.field` on both paths. Under FXC `LATCH` expands exactly as `CBUFFER`
does, so the D3D11 bytecode is the same whichever of the two a list uses.

**The push block.** Under ORG it is `cbuffer OrgBindings : register(b0)`: the pipeline's push constants on Vulkan, its root
constants on D3D12. It holds one `uint` handle per `SRV`, `UAV`, `SAMPLER`, `CBUFFER` and `EXTERN` entry, two (the handle,
then the byte offset) per `LATCH` and `EXTERN_LATCH` entry and four for `SHARED`, in list order, then each `PUSH` struct, each on a 16-byte
boundary. Put `PUSH` entries last.

**Which constants go where.**

- **`PUSH`**: small values fixed in the recording that change between dispatches of one pass (a mip, a slice, a step).
  They are recorded with the dispatch, so they cost no upload. Never a value that changes by frame: async epochs prepare and
  record ahead of the frame's values, and kept recordings are submitted again.
- **`LATCH`**: values that change every frame (a camera, a time, a stripe of a rolling update). The client's `commit` writes
  them into its `org::LatchBlock` at the host's current frame slot (`GraphHost::CurrentFrameSlot()`), and `Record` names that
  slot's region with `ShaderBindings::LatchOf(block, a_recording.FrameSlot(), offset)`. The recording stays the same from
  frame to frame. The struct must have the same layout packed tightly as in cbuffer packing (no member crosses a 16-byte row,
  no gaps), because DXC loads it from a raw buffer.
- **`CBUFFER`**: constants that change only with the graph's build (settings, sizes), in a graph buffer with a CBV.
- **The limit.** The whole push block is at most 32 words (128 bytes), Vulkan's guaranteed push-constant size. D3D12's root
  signature holds 64 words. Handles take one word each, so a pass with 12 resources has 20 words left for `PUSH` data.
  `ShaderBindings::Words<Block>()` fails to compile above the limit, and `tools/check-dual-target-shaders.py` checks the
  shader side.

**Not covered.** Resource arrays (`Texture2D t[4]`) and append/consume counters.

**Resources that headers declare.** A header used by several shaders declares its own resource with `ORG_RESOURCE` (as
`IBL/IBL.hlsli` and `Skylighting/Skylighting.hlsli` do for the deferred composite). The shader lists it as an `EXTERN`
entry. Its handle must exist before the header is included, so such a shader expands its push block first, then includes
the headers, then expands its own declarations.

Headers that many shaders include declare their resources with `ORG_HEADER_SRV(type, name, register)` instead:
`SharedData::DepthTexture` (t17), Light Limit Fix's `lights`, `lightList`, `lightGrid`, `LocalShadows` and `LocalShadowMaps`
(t35-t37, t102, t103), IBL's `EnvIBLTexture` and `SkyIBLTexture` (t76, t77), Terrain Shadows' `ShadowHeightTexture` (t60) and
Cloud Shadows' `CloudShadowsTexture` (t25). They stay register-bound everywhere, DXC builds included, so no other shader has to
list them. An ORG pass shader that defines `ORG_HEADER_RESOURCES` before including `Common/OrgResources.hlsli` reads them from the
heap, and lists, as `EXTERN` entries, every one of them that the headers it includes declare (the height fog's
`VolumetricFogLightScatteringCS.hlsl` is the example).

**The shared constant buffers.** `FrameBuffer` (b12), `SharedData` (b5), `FeatureData` (b6) and `Permutation` (b4) are
declared with the `ORG_SHARED_*` macros (`Common/NamespacedCBuffer.hlsli`). Under FXC, and under DXC by default, they are
the cbuffers they always were. An ORG pass shader that defines `ORG_SHARED_CONSTANTS` before including
`Common/OrgResources.hlsli`, and puts `X(SHARED, ConstantBuffer, Shared, 0)` first in its list, reads them from the heap
instead: each becomes a struct read as a `ConstantBuffer` through its handle, and aliases give the members back their names.
Shader bodies read `SharedData::BufferDim` and the rest unchanged on both paths. A `bool` member is declared
`ORG_SHARED_BOOL`: a `uint` in the struct, since DXC 1.9 emits invalid SPIR-V for a `bool` read from a `ConstantBuffer<T>`.
The C++ side is `src/RenderGraph/SharedConstants.h`: a frame anchor client sets `ClientDesc::sharedConstants`, its passes
declare the buffers with `SharedConstants::Declare` and fill the four handles with `SharedConstants::Index`.

```hlsl
#define ORG_SHARED_CONSTANTS
#include "Common/OrgResources.hlsli"
#define MY_RESOURCES(X)                  \
	X(SHARED, ConstantBuffer, Shared, 0) \
	X(SRV, Texture2D<float4>, InputTexture, 0)
ORG_PUSH_BLOCK(MY_RESOURCES(ORG_HANDLE))
#include "Common/SharedData.hlsli"
MY_RESOURCES(ORG_RESOURCE)
```

A shader without `ORG_SHARED_CONSTANTS` still builds under DXC with the cbuffers, as long as it does not read them, because
DXC drops what is unused.

**Two FXC limits shaped the macros.** FXC cannot pass a list's name as a macro argument, so the shader invokes the list
itself (`DIFFUSE_IBL_RESOURCES(ORG_RESOURCE)`). It also rejects an empty macro argument, so under FXC `ORG_HANDLE` expands to
the entry's kind, which `ORG_PUSH_BLOCK` drops.

**The C++ side** (`src/RenderGraph/ShaderBindings.h`). Mirror the push block in a struct, fill the handles from the pass's
declared views at record time, and push it:

```cpp
struct DiffuseIBLBlock
{
	ShaderBindings::Handle envTexture, iblTexture, linearSampler;  // list order
};

// Setup: the program, and a sampler kept while the program lives.
program = ComputeProgram::Load(device, { .source = "IBL/DiffuseIBLCS.hlsl", .constantWords = ShaderBindings::Words<DiffuseIBLBlock>() });
sampler = ShaderBindings::CreateSampler(linearClampDesc);

// Record.
const DiffuseIBLBlock block{ a_recording.Index(a_bindings.env), a_recording.Index(a_bindings.ibl), sampler.Index() };
program->Dispatch(a_recording, ShaderBindings::Data(block), 1, 1, 1);
```

A `LATCH` entry is a `ShaderBindings::Latch` member (two words) in list order. A `PUSH` struct is an `alignas(16)` member
after the handles. Raster passes give `ShaderBindings::Data(block)` as
`RasterProgram::DrawDesc::constants`.

**Validating.**

```bash
# The D3D11 output is unchanged by the conversion: IDENTICAL, exit 0.
pwsh tools/verify-shader-refactor.ps1 features/IBL/Shaders/IBL/DiffuseIBLCS.hlsl

# FXC cs_5_0, DXC SPIR-V and DXIL with CS_ORG_BINDLESS, spirv-val, and the push block's size.
python tools/check-dual-target-shaders.py features/IBL/Shaders/IBL/DiffuseIBLCS.hlsl
```

Without arguments the script checks `tests/Shaders/OrgResourcesSampleCS.hlsl`, which uses every kind and resource type,
`tests/Shaders/OrgSharedConstantsSampleCS.hlsl` (the shared constant buffers and an `EXTERN` entry), and `DiffuseIBLCS`.
`-D NAME[=VALUE]` adds a define to every build, for a shader's permutations (the composite:
`-D COMPUTESHADER -D INTERIOR -D SSGI`). It includes `package/Shaders` and every `features/*/Shaders`, as `Data/Shaders` merges
them. `verify-shader-refactor.ps1` takes several `-IncludeDir` roots for the same reason (call it in-process, `& ./tools/verify-shader-refactor.ps1`,
to pass an array). It finds `dxc.exe` in the Vulkan SDK (`VULKAN_SDK` or `C:\VulkanSDK\*`) and `fxc.exe` in the Windows SDK.

A conversion that moves `cbuffer` members into a `CBUFFER` struct changes the DXBC's reflection names, so
`verify-shader-refactor` reports DIFFERS even though the instructions are the same. Check that its assembly diff shows only
the reflection comments (the `//` buffer definitions) and no instruction lines.

## Overview

Two deployment targets for different workflows:

- **`COPY_SHADERS`** - Fast shader-only deployment (seconds)
- **`DEPLOY_ALL`** - Full build + tests + deployment (minutes)

With `AUTO_PLUGIN_DEPLOYMENT=ON`, building the plugin alone (`--target CommunityShaders`) deploys too:
-   its DLL and PDB, on every link;
-   the changed shaders (`DEPLOY_SHADERS`), copied straight from `package/Shaders` and every feature's `Shaders` folder,
    whenever an `.hlsl` or `.hlsli` changed.

So the deployed DLL and shaders match after any plugin build. `DEPLOY_SHADERS` does not go through the AIO staging,
whose rules depend on the plugin.

### Requirements

- Must have `AUTO_PLUGIN_DEPLOYMENT=ON` in your CMake preset
- Must have `CommunityShadersOutputDir` environment variable set to your Skyrim directory

### Usage

#### Manual

```bash
# Fast iteration: Only copy changed shaders to game directory
cmake --build build/ALL --target COPY_SHADERS

# Or in Visual Studio: Right-click "COPY_SHADERS" target -> Build

# Full deployment (same as running cmake --build with no target):
cmake --build build/ALL --target DEPLOY_ALL
```

#### Automatic (VSCode)

You can configure VSCode to automatically deploy shaders when you save `.hlsl` or `.hlsli` files using the [RunOnSave](https://marketplace.visualstudio.com/items?itemName=emeraldwalk.RunOnSave) extension.

**See [VSCode Setup](../development/vscode-setup.md) for complete configuration instructions.**

### Prerequisites

1. Run `cmake --preset ALL-WITH-AUTO-DEPLOYMENT` at least once to create build directory
2. Set `CommunityShadersOutputDir` environment variable to your Skyrim `Data` directory
3. Ensure `AUTO_PLUGIN_DEPLOYMENT=ON` in your CMake preset

### What COPY_SHADERS does now

1. ✅ Transforms shaders from source layout → game layout (via AIO staging)
2. ✅ Copies only changed shader files (incremental robocopy)
3. ✅ Deploys to `$CommunityShadersOutputDir/Shaders`
4. ❌ Does NOT build the DLL
5. ❌ Does NOT run shader tests
6. ❌ Does NOT deploy non-shader files

### Target Comparison

| Target            | Builds DLL | Runs Tests | Copies Shaders | Use Case              |
| ----------------- | ---------- | ---------- | -------------- | --------------------- |
| `COPY_SHADERS`    | ❌         | ❌         | ✅             | Fast shader iteration |
| `DEPLOY_ALL`      | ✅         | ✅         | ✅             | Full deployment       |
| `prepare_shaders` | ❌         | ✅         | ✅ (AIO only)  | CI validation         |
