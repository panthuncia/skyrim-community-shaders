# Parity lane `par-fx`: the ORG FX runtime takes what FX11 takes

Plan: WS2 "FX runtime closure" (`we-ve-been-working-on-foamy-journal.md`). Design: [org-fx-runtime.md](../org-fx-runtime.md).
Branch `org-native/par-fx`, based on `org-native/integrate` f34fce25d.

Effects11 runs ENB-style `.fx` presets. In ORG modes the ORG FX runtime (`src/Features/Effects11/ORG/`) runs them as graph
passes at the Tonemap anchor. Native mode never executes FX11's draws, so every preset the runtime refused was lost there. The
user's decision: FX11 execution is retired in ORG modes, and the runtime must take everything FX11 takes.

The work comes in two parts:

1. **Closing the refusals** (one commit each). FX11 still runs a frame the runtime refuses, so guest keeps its net.
2. **The deletion** (the last commit, merged after a test). See "After the deletion" below.

## Refusals

| Refusal (old line in `ORGFxRuntime.cpp`)                     | State      | How                                                                                                                                                       |
| ------------------------------------------------------------ | ---------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `enbdepthoffield.fx` (:1479)                                 | Closed     | DoF planned after colour correction. Aperture, ReadFocus and Focus write kept textures (`DofAperture`, `DofReadFocus`, `DofFocus`) with history copies. The chain goes to `HDRTemp`/`HDRTemp2` and is copied back over `Original`. Later effects read this frame's aperture as `TextureAperture`. |
| `cbuffer` blocks (:688)                                      | Closed     | FX11's cbuffer variables are copied into their region of the values image. `ScreenSize` may be in a cbuffer.                                              |
| Render states in passes (:775)                               | Closed     | Blend and rasterizer states go into the pass's pipeline. A pass that sets none keeps the last state set, as FX11's Apply does. The blend factor is set on the list. Depth-stencil state does nothing, since Effects11 binds no depth view. A sample mask without sample 0 leaves the pass out. The parser also reads the FX10 form (`BlendState = X;`) and `BLEND_FACTOR`. FX9 render states (`ZEnable = ...`) still fail the effect, but FXC's `fx_5_0` does not compile them either. |
| `RenderTarget` naming other textures (:1026)                 | Closed     | Any name FX11 finds maps to its graph texture: TextureManager's common textures (`TextureHDRTemp(2)`, `TextureSDRTemp(2)`, `TextureBloom(Temp)`, `TextureLens`, `TextureAdaptation(Swap)`) and the effect's own (`TextureScratch`, DoF's pairs). FX11's swaps between pairs are fixed textures here: a write lands in the first texture of the pair. |
| Encrypted presets / no preprocessed text (:682)              | Partly     | When only FX11's raw-source fallback compiled an effect, its text is now kept too. Encrypted (`KIEFX`) presets are not closed: see below.                  |
| More than 30 textures or samplers                            | Closed     | Each pass gets only the textures and samplers its shaders reach by name (through the functions, classes and static globals they name). The rest share two words: a black texture and a linear sampler. A pass that reaches more than 28 still fails. |
| `#pragma pack_matrix(row_major)`                             | Closed     | The parser applies the pragma to the declarations after it, struct members included. The layout matches FXC's (`packing.fx`).                            |
| Compute shaders on a pass                                    | Closed     | Ignored: the pass's draw never runs one, on FX11 either.                                                                                                  |
| Hull and domain shaders                                      | Closed     | The pass is left out, logged. The quad is not a patch list, so D3D11 draws nothing.                                                                       |
| Geometry shaders                                             | Not closed | BasicRHI has no geometry stage. Needs a GS stage in BasicRHI pipelines (Vulkan `geometryShader`, a D3D12 PSO GS), a `RasterProgram` geometry source, and a GS wrapper in the lowering. |
| UAVs                                                         | Not closed | Effects11 never binds a UAV, so FX11 reads zero and drops writes. ORG needs an owned null UAV view (`IDescriptorService::CreateOwnedUnorderedAccessView` with an invalid resource; BasicRHI already writes null views). Then the lowering binds each `RW*` global to it. |
| Graph or commit failure returns to FX11 (:1260, :1294, :1316) | Closed in the deletion commit | See "After the deletion".                                                                                                                                 |

### Encrypted presets

FX11 gets an encrypted preset only as the `fx_5_0` binary that KiENBExtender's `ENBExt_Compile` returns
(`ENBExtender::CreateEncryptedEffect`). CS never sees the source, so the parser has nothing to read. FX11's reflection still
works on that binary: variables, annotations, techniques and each pass's DXBC (`GetVertexShaderDesc` / `GetPixelShaderDesc`).
To run it on ORG, the runtime would need:

- DXBC shaders in the graph. On Vulkan that means a DXBC to SPIR-V translation (DXVK's `dxbc` compiler is in `extern/dxvk`). On
  D3D12, DXBC SM5 runs as is.
- A register-bound pipeline layout (`t#`, `s#`, `b#` from the DXBC's reflection) in place of the bindless push block. BasicRHI
  layouts are bindless only.
- The constant buffers as FX11 lays them out, uploaded per pass. FX11's variables already hold the values.

Until then an encrypted effect fails on ORG (`no preprocessed text`).

### FX11 compiled in native mode (the UI seam)

FX11 still compiles every effect: the editor, the ini, weather blending and the ENB SDK globals all read and write FX11's
variables, and the ORG commit copies their bytes. In native mode `globals::d3d::device` is the tripwire device handle. Reading
`Tripwire.cpp` and `NativeDevice.cpp`, `D3DX11CreateEffectFromMemory` gets what it calls:

- `CreateVertexShader`, `CreatePixelShader`, `CreateComputeShader`: native shader handles (bytecode only).
- `CreateBuffer` (one per cbuffer): native buffers.
- `CreateBlendState`, `CreateRasterizerState`, `CreateDepthStencilState`, `CreateSamplerState`: native state handles.
- `GetImmediateContext`: the paired context handle.
- Reflection is `D3DReflect` on the bytecode, with no device. Variable reads and writes (`SetRawValue`, `GetRawValue`) are CPU
  only. FX11 touches the context only in `Apply`, which ORG modes never reach.

Preset texture files (`DirectX::CreateShaderResourceView`) use `CreateTexture2D`, `CreateShaderResourceView` and
`CheckFormatSupport`, which are native services.

The gap: `CreateGeometryShader`, `CreateHullShader` and `CreateDomainShader` are tripwires in native mode (no native handle
kind). FX11 then fails to create the whole effect, so an effect with any of those stages is lost in native mode, including
its passes that ORG could run. The fix belongs to the native-context lane: native handles for those three stages, like the
vertex and pixel ones (`Tripwire::Kind` has no kind for them yet). Replacing FX11's reflection with the parser's (the
`UIVariable` value-handle plan in org-fx-runtime.md "UI layer seam") is not needed for anything else. It was not done.

Confirm in a native run: `[EFFECTS11] Successfully loaded FX file: ...` for each effect, and no tripwire line naming
`CreateVertexShader`, `CreatePixelShader` or `CreateBuffer` from `Effects11`.

## How to test

### The parity test preset

`tools/cs-test/data/fx-parity/` is a small preset that exercises every closed refusal:

| File                   | What it exercises                                                                                                                  |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `enbeffect.fx`         | A `cbuffer` with UI values, a row-major matrix by `#pragma pack_matrix` (asymmetric: a transposed read turns reds green), `RenderTarget = "TextureHDRTemp"` on the chain's first technique, `TextureAperture` from DoF. |
| `enbeffectpostpass.fx` | A copy, then a quarter-strength vignette through `BLEND_FACTOR` with `CullMode = NONE`. Then three red passes that must never show: culled front faces, a pass that inherits that cull, and a sample mask without sample 0. Any red on screen is a bug. |
| `enbbloom.fx`          | 34 textures in one effect. The pass reaches `TextureColor` and one unbound texture (black).                                         |
| `enbdepthoffield.fx`   | Aperture, ReadFocus (16x16 from depth) and Focus with history blending, then a two-technique chain (`HDRTemp` then `HDRTemp2`).      |

Install: make an MO2 mod (say `FX Parity Test`) holding `enbseries.ini` and the `enbseries\` folder at its root, as Picta's
files are. Enable it above the preset in use, and select it if CS's preset manager lists presets. Effects11 must own the
tonemap (`UseEffect=true`, Post Processing not claiming it).

Run before the deletion commit: `tools/cs-ab/scenarios/fx-org0.env` (FX11) against `fx-org1.env` (ORG) in guest, and compare
the two shots. They should match up to numerics: the same tint, the vignette, the blur away from the focused depth, no red.
After the deletion `fx-org0` no longer selects FX11 in guest; compare against a plain D3D11 run (`CS_ORG=0`) instead, and run
native Vulkan and native D3D12.

The offline checks need no game: `tools/fx-parse-check` over the preset and the testdata (`packing.fx`, `many-resources.fx`,
`constructs.fx`) parses, lowers and compiles every pass with DXC, and checks the `$Globals` layout against FXC:

```
fx-parse-check --dxc C:\VulkanSDK\<ver>\Bin\dxc.exe tools\cs-test\data\fx-parity\enbseries tools\fx-parse-check\testdata\packing.fx tools\fx-parse-check\testdata\many-resources.fx
```

Results on this branch: the parity preset, `packing.fx` and `many-resources.fx` parse, lower and compile with no errors and no
layout mismatches. Rudy ENB for Obsidian Weathers is unchanged: 62 of 62 passes lowered, 0 DXC failures.

Rudy's preset (org-fx-runtime.md "Testing") also has an `enbdepthoffield.fx`. Set `EnableDepthOfField=true` in its
`enbseries.ini` to run it.

### Log lines

| Line                                                                                          | Meaning                                                    |
| --------------------------------------------------------------------------------------------- | ---------------------------------------------------------- |
| `[ORG] Effects11: <effect> parsed and lowered for the render graph (...)`                     | The effect built. Expect one per effect, `enbdepthoffield.fx` included. |
| `[ORG] Effects11 runs on the render graph: N passes (M from .fx: <effects>), ...`             | A plan was registered. The list names every effect that runs. |
| `[ORG] Effects11: <effect> technique '<t>' pass <p> has a sample mask without sample 0 and draws nothing` | The test preset's `masked` pass, left out.                  |
| `[ORG] Effects11: <effect> technique '<t>' pass <p> sets tessellation shaders on a quad and draws nothing, as on D3D11` | A hull or domain pass left out.                             |
| `[ORG] Effects11 runs on FX11: <reason>` (before the deletion)                                | A refusal still hit. Should not appear with the test preset or Rudy's. |
| `[ORG] Effects11: <effect> cannot run on the render graph: <reason>`                           | The effect failed to build; the reason names the construct. |

## After the deletion

The deletion commit retires FX11 execution in ORG modes. `EffectManager::ExecuteEffects` runs only when the render graph never
became active (plain D3D11: `CS_ORG=0`, or no DXVK). Details are in the "Deletion" section below, which that commit adds.

## What is left

- Geometry shaders and UAVs (above): BasicRHI and ORG additions.
- Encrypted presets (above): DXBC in the graph.
- Native FX11 creation of effects with GS/HS/DS: native handles for those stages (native-context lane).
- GPU timers per pass and `Effects11::<effect> Pass <n>` profiler zones (org-fx-runtime.md section 5).
