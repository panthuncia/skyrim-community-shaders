# ORG FX runtime: ENB-style `.fx` presets without FX11

Plan task P3 W5 ([org-full-port.md](./org-full-port.md)). Status (2026-11-09): design, parser and lowering are written and
checked offline against the corpus (section 10); execution on ORG is not started.

Effects11 runs ENB-style `.fx` presets at the Tonemap position through FX11 (`D3DX11CreateEffectFromMemory`,
`src/Features/Effects11/Effects/Effect.cpp`). FX11 is a D3D11 library: it owns the shaders, constant buffers, state
objects and binds them with `ID3DX11EffectPass::Apply`. This document designs its replacement: an `.fx` front end that
CS owns, DXC compiles per pass, and ORG raster passes in the Tonemap anchor. FX11 stays as the default; `CS_ORG_FX=1`
selects the new runtime.

## 1. What Effects11 uses from FX11 today

### 1.1 The effects and their order

`EffectManager::ExecuteEffects` (called from the Tonemap hook, `Effects11.cpp`) runs, all on the D3D11 immediate context:

| Step | Code                                                     | Input                                                                                   | Output                                                                 |
| ---- | -------------------------------------------------------- | --------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| 1    | `ApplyColorCorrection` (CS-owned CS)                     | `kMAIN` UAV                                                                             | `kMAIN` in place                                                       |
| 2    | `enbdepthoffield.fx`                                     | techniques `Aperture`, `ReadFocus`, `Focus` by name, then the selected chain on `kMAIN` | aperture/focus history pairs, then `kMAIN` (copy back)                 |
| 3    | `TextureManager::UpdateDownsampledTexture` (CS-owned PS) | `kMAIN`                                                                                 | 1024² mip chain (`TextureDownsampled`, blurry mip 2)                   |
| 4    | `enbbloom.fx`                                            | `TextureDownsampled`                                                                    | `TextureBloom` (1024², ping-pong with `TextureBloomTemp`)              |
| 5    | `enblens.fx`                                             | `TextureDownsampled`                                                                    | `TextureLens` (ping-pong with `TextureHDRTemp`)                        |
| 6    | `enbadaptation.fx`                                       | techniques `Downsample` (16², from the sun-masked blurry mip) and `Draw`                | `TextureAdaptation` / `TextureAdaptationSwap` (1x1 R32F, history pair) |
| 7    | `enbeffect.fx` (required)                                | `kMAIN` (HDR)                                                                           | `TextureSDRTemp` (R10G10B10A2, ping-pong with `TextureSDRTemp2`)       |
| 8    | `enbeffectpostpass.fx`                                   | `TextureSDRTemp`                                                                        | `TextureSDRTemp2`/scratch, copied back                                 |
| 9    | `CopyTexture`                                            | `TextureSDRTemp`                                                                        | the hook's output target                                               |

`enbeffectprepass.fx`, `enbunderwater.fx`, `enbsunsprite.fx` and `enbsmaa.fx` exist in presets but Effects11 does not run
them (`enbsmaa.fx` is only included by postpass). The volumetric/sun rays passes (W4) and the adaptation sun mask are
CS-owned shaders, not FX, and are not part of this runtime.

### 1.2 Loading (`Effect::LoadFXFile`)

1. `Util::ShaderPatches::Apply` (preset-specific text patches, `ShaderPatches.json`).
2. `ENBExtender::ConvertExtenderSyntax`: KiENBExtender defines, `#pragma exists(...)`, `#pragma uidefine(...)` (UI values
   that are preprocessor defines; changing one recompiles).
3. `D3DPreprocess` with `ENBExtender::PresetInclude` (includes resolved inside the preset folder, missing includes become
   empty), stringify macros expanded by hand (`D3DPreprocess` has no `#x`), `ParseSourceGroupScopes` (UI group order from
   the source text), `StripLineDirectives`.
4. `D3DCompile(..., "fx_5_0")` then `D3DX11CreateEffectFromMemory`. Two fallbacks re-try with inlined includes and with
   the raw source.
5. Encrypted presets (`KIEFX` header, e.g. Silent Horizons) never reach CS as source: KiENBExtender's `ENBExt_Compile`
   returns an `fx_5_0` binary.

Steps 1-3 are text processing and `d3dcompiler`'s preprocessor; they do not need a D3D11 device and are reused as is.

### 1.3 Reflection (`Effect::ReflectCompiledEffect`)

- **Variables.** Every global is enumerated (`EnumerateAllVariables`). Globals with a `UIName` annotation become
  `Effect::UIVariable`s (`ENBExtender::CreateUIVariable`): `float`, `float2-4`, `int`, `bool`. Annotations read:
  `UIName`, `UIMin`, `UIMax`, `UIWidget` (`Spinner`, `Color`, `Dropdown`, `Quality`, `Vector`), `UIList`, `UIGroup`,
  `UIOrdering`, `UIReadOnly`, `UIHidden`, `UIVisible`, `UITopLevel`, `UniqueName`, `Separation`, `UIBinding*`,
  `UIIgnorePerfMode`, `UIWeatherString`, `UIWeatherOnlyString`. `string` globals are labels, separators and group scopes
  (`UIGroupBegin`, `UIGroupEnd`, `UISeparator`). FX11 reorders globals by type, so the source order is recovered from
  the text (`sourceOrderMap`).
- **Extern bindings.** Globals with `ExternBinding` (camera matrices as `float4` columns) are set per frame.
- **Textures.** Globals with `ResourceName` are loaded from the preset folder (DDS, else WIC without sRGB).
- **Techniques.** Every technique, grouped into chains by the ENB numbering rule (`Name`, `Name1`, `Name2`, ... follow
  the base when the base exists; a named `fxgroup` is one chain). Technique annotations: `UIName` (makes it a selectable
  preset technique), `UIDefault`, `RenderTarget`, `UIBinding<n>`/`UIInvBinding<n>` (a technique runs only while a bool
  variable is on/off).

### 1.4 Values and the menu

`Effect::Load` reads `<effect>.ini` (`[ENBEFFECT.FX]` style sections, key `Group.UIName` or `UniqueName`, vectors as
`X/Y/Z/W` keys unless a colour) and writes each value into the FX11 variable. `ExtendedEffect` blends weather files and
time-of-day variants every frame (`ApplyWeatherBlending`, `ApplyTimeOfDayInterpolation`), writing into the variables.
The editor (`Effects11Editor`, `ExtendedEffect::RenderMergedUI`) edits `uiVariables` and `UpdateUIVariables` pushes them.
The only FX11 handle in that layer is `UIVariable::effectVariable` (`ID3DX11EffectVariable`), used by `Effect.cpp`,
`ExtendedEffect.cpp` and `SettingsPatches.cpp`.

Per frame `EffectManager::UpdateCommonVariablesForEffect` sets the ENB SDK globals by name: `Timer`, `Weather`,
`WeatherAndTime`, `TimeOfDay1/2`, `ENightDayFactor`, `EInteriorFactor`, `FieldOfView`, `tempF1-3`, `tempInfo1/2`,
`LightParameters`, `AdaptiveQuality`, `BloomSize`, and the textures `TextureDepth`, `TextureAperture` and every
`RenderTarget*`. Each effect adds its own (`Params01`, `ENBParams01`, `DofParameters`, `AdaptationParameters`,
`TextureBloom`, `TextureLens`, `TextureAdaptation`, `TextureOriginal`, `TextureCurrent`, `TexturePrevious`,
`TextureFocus`, `TextureDownsampled`). `RenderPasses` sets `ScreenSize` per technique from the output size.

### 1.5 Render targets and lifetime

`TextureManager` creates the shared textures lazily by name and keeps them for the session: screen-size temporaries
(`TextureHDRTemp(2)`, `TextureSDRTemp(2)`, `RenderTargetRGBA32/64/64F/R16F/R32F/RGB32F`), 1024² bloom textures,
square `RenderTarget1024` ... `RenderTarget16`, and the 1x1 adaptation pair. Effects own a few more (DoF aperture and
focus pairs, postpass scratch, adaptation 16² current). Ping-pong is done by swapping map entries
(`SwapTextures`) and by the frame parity (`GetTextureSwap`).

### 1.6 How passes chain (`ExecuteTechniqueSequence`)

For each technique of the chain, skipping those whose `UIBinding` is off: `TextureColor` is bound to the chain input;
the output is the next ping-pong texture, unless the technique has a `RenderTarget` annotation, which sends it to that
named texture and leaves the chain where it was. Every pass of a technique draws a 4-vertex strip quad
(`POSITION` float3, `TEXCOORD0` float2) into the same target with the pass's shaders; no preset in the corpus sets a
blend, depth or rasterizer state, so the defaults (opaque, no depth, no cull) apply. The result is in one of the two
ping-pong textures and the caller swaps or copies.

### 1.7 The FX language the corpus uses

The corpus is every unencrypted preset on the development machines: NAT (HexRim, 7 files), Rudy ENB for Obsidian
Weathers (10 files plus `Modular Shaders/*.fxh` and `SMAA.h`), and Silent Horizons (encrypted, not parseable). Used:

- `technique11 Name <annotations> { pass p0 { SetVertexShader(CompileShader(vs_5_0, VS())); SetPixelShader(CompileShader(ps_5_0, PS(args))); } }`.
  The arguments bind to the entry's `uniform` parameters in order and are literals or global textures
  (`PS_Resize(TextureDownsampled, 1024.0, 512.0)`).
- `SamplerState Name { Filter = ...; AddressU = ...; AddressV = ...; };` blocks.
- `Texture2D` globals (no template argument), with `ResourceName` annotations for files.
- Numeric globals with annotations and brace initializers (`float x <string UIName="..."; float UIMin=0;> = {1.0};`),
  arrays (`float4 Params01[7]`), `string` globals for labels and groups.
- Structs, plain functions, `static const` tables, and HLSL classes with static member functions (NAT's `Texture2DAtlas`).
- Annotation values given by macros (`<string RenderTarget = SMAA_STRING(SMAA_EDGE_TEX);>`), after preprocessing.

Not used anywhere in the corpus: `fxgroup`, `BlendState`, `DepthStencilState`, `RasterizerState`, `SetBlendState` and
friends, geometry/hull/domain/compute shaders, UAVs, `cbuffer` blocks, DX9-style `sampler_state`/`compile` syntax. The
parser still recognises them (section 3), so a preset that uses them gets a clear message, not a silent miscompile.

## 2. Architecture

```
.fx file ──(1.2 steps 1-3, unchanged)──> preprocessed text
  ──OrgFx::Parse──> OrgFx::EffectDesc (techniques, passes, globals, resources, samplers, functions, structs, annotations)
  ──OrgFx::Lower──> one HLSL translation unit per pass stage (bindless prologue + original body + entry wrapper)
  ──DXC (RasterProgram)──> SPIR-V pipelines
  ──OrgFx::Runtime──> values image, latches, graph passes in the Tonemap anchor
```

Module layout, `src/Features/Effects11/ORG/`:

| File                   | What                                                                             | Depends on         |
| ---------------------- | -------------------------------------------------------------------------------- | ------------------ |
| `FxModel.h`            | The data model.                                                                  | std only           |
| `FxParser.{h,cpp}`     | Tokenizer, parser, constant evaluator, ENB technique chains, `$Globals` packing. | std only           |
| `FxSourceText.{h,cpp}` | Text passes shared with FX11's path: stringify macros, `#line` stripping.        | std only           |
| `FxLower.{h,cpp}`      | Model + text to DXC sources, one per pass.                                       | std only           |
| `ORGFxRuntime.{h,cpp}` | Programs, values, latches, Tonemap anchor passes (step 3).                       | ORG, RasterProgram |

The parser and lowering have no D3D11 or ORG dependency so they can be tested offline
(`tools/fx-parse-check`, section 6) and reused by an extension API (section 8).

## 3. Parser (`OrgFx::Parse`)

Input: the preprocessed text (any leftover `#` line is skipped; comments are handled, so raw source also parses when it
has no macros). Output: `OrgFx::EffectDesc` plus diagnostics with line numbers. It never throws; an unknown construct at
global scope is skipped to the next `;` or balanced `}` with a warning.

Recognised at global scope:

- `technique`, `technique10`, `technique11`, `fxgroup` with annotations; passes with annotations and their state
  calls (`SetVertexShader`, `SetPixelShader`, `SetGeometryShader`, `SetHullShader`, `SetDomainShader`,
  `SetComputeShader`, `SetBlendState(name, factor, mask)`, `SetDepthStencilState(name, ref)`, `SetRasterizerState`) and
  the FX9/10 assignment form (`VertexShader = compile vs_3_0 VS();`, render-state keys kept raw).
- Shader bindings: `CompileShader(profile, Entry(args))`, `NULL`, or the name of a global shader variable
  (`VertexShader vs = CompileShader(...);`). Arguments are kept as text and classified (literal, global resource,
  expression).
- State objects: `SamplerState`, `SamplerComparisonState`, `BlendState`, `DepthStencilState`, `RasterizerState`, with
  indexed keys (`BlendEnable[0]`). Samplers are decoded into `OrgFx::SamplerDesc` (filter, address modes, LOD, anisotropy,
  comparison, border colour); the other three into typed descs for the keys D3D11 defines; unknown keys are kept raw and
  reported.
- Global variables: storage modifiers, type (scalar/vector/matrix, struct, `string`, resource or sampler with optional
  template argument), array size, semantic or register, annotations, initializer. The initializer is evaluated by a small
  constant evaluator (literals, `true`/`false`, brace lists, constructors such as `float3(...)`, unary/binary arithmetic,
  references to earlier `static const` scalars); its text is kept when it cannot be evaluated.
- Functions (return type and semantic, parameters with `in`/`out`/`inout`/`uniform`, interpolation modifiers, semantics
  and defaults, body range), structs and classes (members with semantics), `cbuffer`/`tbuffer`, `typedef`.

Derived views computed after parsing:

- **Technique chains** with the exact rule of `Effect::LoadTechniques` (numbered suffix joins an existing base; named
  groups are one chain).
- **Entry resolution**: each pass stage resolves its entry by name and argument count among overloads and maps the
  arguments onto the `uniform` parameters; mismatches are diagnostics.
- **`$Globals` layout**: FXC's cbuffer packing (16-byte rows, no straddling, arrays start rows, structs start rows) for
  every non-static numeric global, so the CPU value image matches what FX11 had and the generated loads read it back.
- **Default values** (`BuildDefaultValues`): the evaluated initializers written at their offsets, as FXC writes them.
  Two FXC habits found by comparing with its reflection: `true` is stored as `0xFFFFFFFF`, and a global matrix's
  initializer list is read column by column whatever the constructor says. FX11 starts from those bytes, so the image
  does the same.
- **ENB view**: UI variables in source order (the `UIName` rule of `CreateUIVariable`), labels/groups from `string`
  globals, `ResourceName` textures, `ExternBinding` globals.

## 4. Lowering to DXC (`OrgFx::LowerEffect`, `OrgFx::LowerPass`)

`LowerEffect` makes the effect's body once; `LowerPass` appends one pass's entry wrappers, giving one source per pass with
entries `OrgFxVS` and `OrgFxPS` (passes with the same shaders produce the same text, so the shader store dedupes them):

1. **Replace in place.** Each uniform global's statement becomes a `static` of the same type initialised from the values
   buffer; each texture and sampler becomes a `static` taken from the descriptor heaps. In place, not in a prologue, so a
   uniform of a struct type still follows its struct. Techniques, groups, state objects, `string` and shader globals,
   and annotations on kept statics are blanked. Newlines are kept and a `#line 1` follows the prologue, so DXC's messages
   give lines of the preprocessed text. Function bodies, structs, classes and `static` globals stay byte for byte.
2. **Prologue (bindless ABI).** The push block (`cbuffer OrgFxBindings : register(b0)`, as `OrgResources.hlsli`) is two
   words: the heap index of a raw buffer (a `LatchBlock` region in practice) and this pass's byte offset in it. The
   region holds the constant buffers (`$Globals`, then each `cbuffer`, `LoweredEffect::regions`) and, from
   `LoweredEffect::valuesSize`, one heap index per `LoweredEffect::resources` entry. So:
    - `static float4 Timer = OrgFxValues.Load<float4>(OrgFxValuesOffset + 16u);` at the variable's offset; bools compare
      with zero; arrays, matrices and structs load element by element into initializer lists (DXC's SPIR-V ignores
      `row_major` in templated loads, so matrices are built from scalars with FXC's register layout);
    - `static Texture2D TextureColor = ResourceDescriptorHeap[OrgFxHandle(3)];`, samplers from `SamplerDescriptorHeap`.
      This is the `OrgResources.hlsli` LATCH/SRV expansion, generated, because presets have 13-18 resources, more than
      the 32-word push block holds as handles.
3. **Entry wrappers.** The vertex wrapper takes `SV_VertexID` and builds the user's input (struct members by semantic:
   `POSITION`/`SV_Position` from the quad corner, `TEXCOORD0` from its UV, anything else zero with a warning) for a
   6-vertex two-triangle quad with the same corners and UVs as Effects11's strip, then calls the user's entry with the
   pass's uniform arguments. Its outputs, and the pixel wrapper's inputs, are flattened with `[[vk::location(n)]]`:
   locations are given to the vertex outputs in order (arrays take one per element, as `TEXCOORD1[10]` in Rudy's DoF),
   and each pixel input takes the location of the output whose semantic name and index range covers it, because Vulkan
   links stages by location while FX matched them by semantic. A pixel input no output writes reads zero (with a
   warning; D3D11 would have refused the draw). The pixel wrapper rebuilds the user's input struct and calls the entry
   with its uniform arguments (`PS_Resize(OrgFxArg0, OrgFxArg1, TextureDownsampled, 1024.0, 512.0)`); `COLORn` outputs
   become `SV_Targetn`.
4. **DXC flags**: `-HV 2018` (FXC-era rules: `&&`, `||` and `?:` on vectors are allowed and evaluate both sides),
   warnings logged and not fatal, `vs_6_6`/`ps_6_6`. Because the prologue is generated, the same unit can be emitted for FXC
   (register-bound declarations instead of heap loads, as `OrgResources.hlsli` does without `CS_ORG_BINDLESS`). That
   "dual target" is a validation tool: the FXC unit can replace FX11's pass shaders on the D3D11 path to prove the
   lowering on its own before ORG execution is involved.

`RasterProgram` loads its stages from files under `Data/Shaders`. The runtime needs the in-memory form: a
`RasterProgram::Desc` that carries source text and a virtual name (the hash), compiled through the same `ProgramBuild`
request and shader store.

## 5. Execution on ORG (step 3)

- **Anchor.** One client `cs.effects11` on `FrameAnchors::Anchor::Tonemap`; the D3D11 hook calls `FrameAnchors::Run` with
  the tonemap's input/output and skips `EffectManager::ExecuteEffects` when the ORG client ran.
- **Passes.** One graph raster pass per FX pass (`RasterProgram::Draw`, 6 vertices), plus the CS-owned colour correction
  (ComputeProgram), the 1024² downsample (RasterProgram over its existing PS, or the P0 GenerateMips pass), the
  adaptation sun mask, and the final copy. Pass order keys follow section 1.1.
- **Chain resources.** The ping-pong swaps become single-assignment graph textures: each chain step that writes the chain
  gets its own transient `Effects11::<effect>::Chain<i>` (aliasing on), sized and formatted like the texture it replaces.
  The graph orders them by reads and writes; no `SwapTextures`.
- **Named targets.** `RenderTarget*`, `TextureBloom`, `TextureLens` and friends are graph textures by the same names,
  transient (aliased) unless read the next frame.
- **History.** `TextureAdaptation`, DoF aperture and focus pairs are `GraphResources` History textures (this frame's write,
  last frame's read). The first frame clears the previous value, as `historyValid` does today.
- **Engine images.** `kMAIN` (`TextureOriginal`, chain input of `enbeffect.fx`) and the tonemap output through
  `FrameAnchors::EngineTarget`; `TextureDepth` through `EngineDepth`. Inputs of disabled effects (bloom off) bind a 1x1
  black texture: FX11 with a null SRV reads zero.
- **Files.** `ResourceName` textures are decoded on the CPU (DirectXTex, as today) and uploaded to persistent ORG textures
  once per load.
- **Values.** Each effect keeps a CPU `$Globals` image (the parser's layout). `UIVariable` writes, ini loads, weather and
  time-of-day blending, the ENB SDK globals and extern bindings write into it. The client's `commit` copies the image
  into a `LatchBlock` region per pass (that pass's `ScreenSize` patched in) together with its handle table. Values never
  go in the push block, so recordings stay reusable.
- **Shape changes.** The graph's shape is the set of passes that run: effect enable settings, the selected technique,
  `UIBinding` toggles, and loaded effects. A hash of those is checked at the hook; a change calls
  `FrameAnchors::Refresh("cs.effects11")`. Value edits never rebuild.
- **Compilation.** At `Effect::Apply` (preset load, reload, `#pragma uidefine` change): parse, lower, compile every pass
  of every technique. Any failure falls the effect back to FX11 with the reason in the log and in the effect's errors.
- **Switch.** `CS_ORG_FX=1` enables the runtime; default 0 keeps FX11 unchanged. Encrypted presets always use FX11.

### UI layer seam

`UIVariable::effectVariable` becomes a small value handle: on FX11 the `ID3DX11EffectVariable`, on ORG an index into the
effect's value image. `Effect`'s `Set*Variable` helpers dispatch on the backend. The UI tree, ini, weather and editor
code keep working on `uiVariables`. UI variables are built from the model on the ORG path (same annotation rules as
`CreateUIVariable`, source order directly from the parser).

## 6. Offline test

`tools/fx-parse-check/` is a console tool built straight with `cl` (its header has the command). It walks the given
folders, runs the same text steps as the runtime (`D3DPreprocess` with a preset-folder include handler, stringify
expansion, `#line` stripping; ShaderPatches and `#pragma uidefine` are not applied), parses each `.fx`, and prints per
file: techniques, chains, passes, entry resolution, globals with their `$Globals` offsets and evaluated defaults,
textures, samplers, UI variables, and every diagnostic. It then compiles pixel entries with FXC (`ps_5_0`) until one
reads `$Globals` and compares every variable's offset, size and default bytes with FXC's reflection. With
`--dxc <dxc.exe>` it also lowers every pass and compiles both wrappers with the runtime's DXC flags, keeping the sources
in `--out`. `testdata/constructs.fx` exercises the syntax the presets do not use. Exit code 0 only when everything
passed.

```
fx-parse-check [-v] [--dxc C:\VulkanSDK\<ver>\Bin\dxc.exe] [--out <folder>] <enbseries folder>...
```

## 7. Out of scope

- Encrypted (`KIEFX`) presets: CS only sees the `fx_5_0` binary. They stay on FX11.
- Geometry, hull, domain and compute shaders in `.fx`, stream output, UAVs, `shared` variables and effect pools, dynamic
  shader linkage (`interface`), MSAA textures. Parsed and reported; an effect using them stays on FX11.
- DX9-style render states (`ZEnable`, `AlphaBlendEnable`) and `sampler_state` initializers: parsed and reported.
- The CS-owned Effects11 shaders (volumetric rays, sun rays, colour correction): W4 ports them as ordinary dual-target
  shaders; this runtime only schedules them in the same anchor.
- `enbeffectprepass.fx`, `enbunderwater.fx`, `enbsunsprite.fx`: Effects11 does not run them today.

## 8. Risks

- **FXC-only HLSL.** DXC rejects some code FXC accepts (implicit conversions that become errors, `sampler2D`/`tex2D`,
  uninitialised `out`, literal-int overload ambiguity, `half`). Mitigation: `-HV 2018`, a compile-error log per pass,
  fallback to FX11. Measure with the corpus once lowering exists.
- **Numerics.** DXC/SPIR-V and FXC/DXBC differ slightly; screenshot A/B (FX11 vs `CS_ORG_FX=1`) is the acceptance test,
  not bit equality.
- **Stage linking.** Location assignment from semantics must match on both sides; a preset whose PS reads a semantic the
  VS does not write gets a zero-filled input on FX11 and must get the same here (the wrapper initialises it).
- **Defaults the evaluator cannot compute.** Rare (`= 1.0/2.2` is handled; function calls are not). Such variables
  report a diagnostic; the runtime then compiles a one-off probe or falls back.
- **Compile time.** About 50-100 pass stages per preset. The shader store caches by source hash; first load is slower
  than FX11's single `fx_5_0` compile.
- **Shape rebuilds** when the user toggles bound techniques in the editor: one graph build per toggle.
- **Preset text patches** (`ShaderPatches.json`) were written against FXC; they apply unchanged because they patch text.

## 9. Next steps (execution)

1. `RasterProgram` from in-memory source: a `Desc` with source text and a virtual name, through `ProgramBuild` and the
   shader store; flags `-HV 2018`.
2. The UI seam (section 5): `UIVariable` value handle, and UI variables built from the model on the ORG path.
3. `ORGFxRuntime`: per effect, `LowerEffect` at `Apply`, programs per pass, the value image from
   `BuildDefaultValues` plus ini/weather/UI writes, the resource table (graph textures, files, samplers from
   `SamplerDesc`), a latch region per pass, the Tonemap client, shape hash and `Refresh`.
4. Scenarios under `tools/cs-ab/scenarios/` with `CS_ORG_FX=1` and FX11 for screenshot A/B on Rudy's preset (it lowers
   completely).

## 10. Status and corpus results (2026-11-09)

`fx-parse-check --dxc` over the corpus (HexRim NAT, Rudy ENB, Silent Horizons):

| Measure                                      | Result                                                                  |
| -------------------------------------------- | ----------------------------------------------------------------------- |
| Files                                        | 21: 17 parsed with no errors or warnings, 4 encrypted (Silent Horizons) |
| Techniques / passes                          | 97 / 103; 206 of 206 shader stages resolved to an overload              |
| UI variables (`UIName`)                      | 270                                                                     |
| `$Globals` layout vs FXC                     | 16 files, 0 mismatches (`enbsmaa.fx` has no technique to compile)       |
| Default values vs FXC                        | 263 compared, 0 mismatches                                              |
| Passes lowered                               | 103 of 103                                                              |
| Passes compiled by DXC (SPIR-V, both stages) | Rudy: all 62. HexRim: 28 of 41                                          |

The 13 HexRim passes that DXC rejects come from two FXC-only constructs in the preset text, not from the lowering:

- `enbeffect.fx` (all 10 passes): a class declared inside a function reads a local of the enclosing function
  (`MultiLUT_T`'s `func_t::run` reads `dni`). DXC: "reference to local variable declared in enclosing function".
- `enbeffectprepass.fx` (3 passes, not run by Effects11): assignment to a vector constructor,
  `float2(IncomingSnow, OutgoingSnow) = ...;`. DXC: "expression is not assignable".

Both are fixable by preset text patches (`ShaderPatches.json` already patches preset text), or the effect stays on FX11.
A runtime ships with the first; the second needs no action while prepass is not run.

## 11. Extension API

An FX runtime is a likely extension-API piece: a plugin hands CS an `.fx` file (or text) plus the textures it binds by
name, and gets its techniques run as ORG passes at an anchor, with the UI variables exposed through CS's menu. The
parser and lowering are std-only for that reason. Entries are in
[org-extension-api-notes.md](./org-extension-api-notes.md).
