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
| `PUSH` | as `CBUFFER` | the struct itself, inside the push block |

`SRV` and `UAV` cover `Texture2D`, `Texture3D`, `TextureCube` and their arrays, the `RW` textures, `(RW)StructuredBuffer`,
`(RW)ByteAddressBuffer` and typed buffers. `SAMPLER` covers `SamplerState` and `SamplerComparisonState`. `CBUFFER` and `PUSH`
take a struct, and code reads its fields as `name.field` on both paths.

**The push block.** Under ORG it is `cbuffer OrgBindings : register(b0)`: the pipeline's push constants on Vulkan, its root
constants on D3D12. It holds one `uint` handle per `SRV`, `UAV`, `SAMPLER` and `CBUFFER` entry, in list order, then each
`PUSH` struct, each on a 16-byte boundary. Put `PUSH` entries last.

**Which constants go where.**

- **`PUSH`**: small values that change between dispatches of one pass (a mip, a slice, a step). They are recorded with the
  dispatch, so they cost no upload.
- **`CBUFFER`**: everything else. The pass uploads it (a `LatchBlock` region or a graph upload) and passes its CBV handle.
- **The limit.** The whole push block is at most 32 words (128 bytes), Vulkan's guaranteed push-constant size. D3D12's root
  signature holds 64 words. Handles take one word each, so a pass with 12 resources has 20 words left for `PUSH` data.
  `ShaderBindings::Words<Block>()` fails to compile above the limit, and `tools/check-dual-target-shaders.py` checks the
  shader side.

**Not covered.** Resource arrays (`Texture2D t[4]`), append/consume counters, and the shared constant buffers (`SharedData`,
`FrameBuffer`, `PermutationCB`). Those keep their own declarations until P2.6 makes the shared buffers bindless. A shader
that includes them still builds under DXC as long as it does not read them, because DXC drops what is unused.

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

A `PUSH` struct is an `alignas(16)` member after the handles. Raster passes give `ShaderBindings::Data(block)` as
`RasterProgram::DrawDesc::constants`.

**Validating.**

```bash
# The D3D11 output is unchanged by the conversion: IDENTICAL, exit 0.
pwsh tools/verify-shader-refactor.ps1 features/IBL/Shaders/IBL/DiffuseIBLCS.hlsl

# FXC cs_5_0, DXC SPIR-V and DXIL with CS_ORG_BINDLESS, spirv-val, and the push block's size.
python tools/check-dual-target-shaders.py features/IBL/Shaders/IBL/DiffuseIBLCS.hlsl
```

Without arguments the script checks `tests/Shaders/OrgResourcesSampleCS.hlsl`, which uses every kind and resource type, and
`DiffuseIBLCS`. It finds `dxc.exe` in the Vulkan SDK (`VULKAN_SDK` or `C:\VulkanSDK\*`) and `fxc.exe` in the Windows SDK.

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
