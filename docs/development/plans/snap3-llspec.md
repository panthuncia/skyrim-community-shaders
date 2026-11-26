# snap3-llspec: Linear Lighting's upload hooks in DCLF's rows

Lead `llspec`, branch `snap3/llspec` (base `org_integration` 00d95ac1d), stage `s7-llspec`. Found by snap5-menus (decision 150).

## The bug

Linear Lighting (LL) installs six hooks at the `Unmap` calls of the engine's Setup functions. While LL is on, each rewrites
colours SetupMaterial or SetupGeometry just wrote, from sRGB to LL's working space (linear sRGB, or ACEScg):

| Hook | Variable | DCLF before |
|---|---|---|
| `LightingMaterialUpload` | Lighting PS 25 SpecularColor (Specular passes, not True PBR) | not applied |
| `EffectMaterialUpload` | Effect PS 15 BaseColor (not GrayscaleToColor or Blood) | not applied, in the record nor in the per-frame patch |
| `LightingGeometryUpload` | Lighting PS 4 DirLightColor (also when the sun is overridden) | not applied |
| `LightingGeometryUpload` | Lighting PS 8 EmitColor (times `emitColorMult`) | not applied |
| `LightingGeometryUpload` | Lighting PS 2 point light colours (only without Light Limit Fix) | n/a: DCLF zeroes these rows and draws LLF's lights, which LLF converts |
| `EffectGeometryUpload` | Effect PS 0 PropertyColor (not GrayscaleToColor, not membrane) | not applied |
| `EffectGeometryUpload` | Effect PS 11 DLightColor and PS 8-10 point light colours (Lighting effects) | not ported here (open item) |
| `WaterMaterialUpload`, `WaterGeometryUpload` | Water colours | already ported (`WaterPortEvaluate`) |
| `SetDirectionalAmbientColors` | DirectionalAmbient | converted at the source, before the global DCLF reads |

DCLF's world rows (guest and native: both draw DCLF's rows) never pass through those hooks, so every converted value above was
sRGB. The capture parity run of 2026-11-20 (`1120-210554-nfg-guest-dclf-capture-parity-t4`) shows the DirLightColor one: `PS
PerGeometry variable 4 component 0: DCLF 8.912677, native 3.1830988` on 434544 draws, the sun overridden by Sky Sync (DCLF kept the
HDR multiplier the override drops). SpecularColor showed 0 differences there because Whiterun's specular colours are white, which
the conversion keeps.

## The fix

Each conversion is applied where DCLF makes the value, so every consumer (guest and native world rows, reflection faces, the
menu scenes) gets it:

- `Integrations/LinearLighting/` (new): an After material contributor, `Linear Lighting`. Its sample is active while LL is on and
  holds the working space (`WorkingSpace`, ACEScg or not, also its witness word, so a change re-evaluates every record). Its
  evaluate writes PS 25 rgb = `ToWorking(specularColor) * specularColorScale` for Specular, non-TruePbr passes, as the hook does.
  `LiveWorkingSpace()` and `FrameWorkingSpace(samples)` give the space to the other places; `ToWorking` calls LL's new static
  `LinearLighting::SRGBToWorking(color, acescg)` (the member overload now delegates to it).
- Effect port (`EffectMaterial.cpp`): the record's BaseColor rgb converts before the scale (not GrayscaleToColor, not Blood), from
  the frame's samples. BaseColor is also a frame variable that TextureTransforms re-reads every frame, so `MaterialFrameVariable`
  gained `workingColour` / `workingColourExceptBits` (additive) and the patch converts those entries before the scale.
- `SampleShading` (Lighting EmitColor): working space times `emitColorMult` as well; `SampleEffectShading` (PropertyColor): working
  space unless GrayscaleToColor.
- `GeometryPort::SamplePipelineFrame` (DirLightColor): while LL is on or the sun is overridden, `LightColorToWorking(light)` times
  fade times the HDR multiplier, or times fade alone for an overridden sun, as `WriteDirectionalLight` does.

`MenuScenesNative.cpp` still applies the SpecularColor and Effect BaseColor conversions itself after `MaterialPort::Evaluate`. They
now write the same values a second time; snap5-menus2 owns that file and can delete both blocks.

## Evidence (guest capture parity, LL on, Whiterun autoload)

The capture parity compares DCLF's rows with the engine's own Lighting draws (which pass through LL's hooks) in guest mode.

| Build | Run | Steady-state window |
|---|---|---|
| base (`nfg`, org_integration 0a609eca8) | `s7-llspec-parity-base` | 158300 of 201804 draws mismatched: 20700 material, 131468 per-geometry |
| this branch | `s7-llspec-parity2` | 3900 of 202500 mismatched: 0 material, 300 per-geometry |

Base samples: `PS PerMaterial variable 25: DCLF 0.38745102, native 0.11901281` (sRGB 0.387 is linear 0.119), `PS PerGeometry
variable 8 (EmitColor): DCLF 0.5882353, native 0.11296551`, `variable 4 (DirLightColor): DCLF 23, native 10` (Sky Sync's
overridden sun; DCLF kept the HDR multiplier). With the fix no SpecularColor, EmitColor or DirLightColor sample is left. What is
left predates this item: MTLandLODBlend terrain LOD descriptors (bit 0x20) and LOD VS variable 6 float rounding (32768 vs
32767.998).

The first try of the fixed build (`s7-llspec-parity1`) crashed during loading inside the parity diagnostic itself:
`CaptureParity::OnNativeLightingDraw` -> `Coordinator::FindObject` read `tracked` while the scene work thread changed it. A
diagnostic race, not this change; the rerun passed.

## Runs

| Config | Label | Result |
|---|---|---|
| guest+dclf-capture-parity (base `nfg`) | s7-llspec-parity-base | passed; 20700 material, 131468 per-geometry mismatches |
| guest+dclf-capture-parity | s7-llspec-parity1 | hang: the parity diagnostic's own race (above) |
| guest+dclf-capture-parity | s7-llspec-parity2 | passed; 0 material, 300 per-geometry (pre-existing) |
| guest+quick | s7-llspec-fin-guest-quick | passed |
| native-vulkan+quick (`CS_SNAPSHOT_DETECT=1`) | s7-llspec-fin-native-vulkan-quick | passed; 801 live reads, 35 engine render calls per world frame |
| native-d3d12-quick | s7-llspec-fin-native-d3d12-quick | passed |
| native-vulkan+native-interior | s7-llspec-fin-native-vulkan-native-interior | passed |
| native-vulkan+cadence-drop-cells | s7-llspec-fin-native-vulkan-cadence-drop-cells | passed |

Detector: the change adds no engine read macro and no engine call. The live-read count moves between runs of the same base
(668 to 754 in the director's quick runs today), so 801 is not read as a change.

## Decisions, to confirm

- Decided by the agent, to confirm: the geometry-side hooks (DirLightColor, EmitColor, Effect PropertyColor) are fixed here too,
  outside "material port" in the narrow sense, since they are the same bug and the parity counts them. GeometryPort,
  LightingConstants and EffectProperty each got a small block.
- Decided by the agent, to confirm: guest's picture changes. Guest draws DCLF's rows, so it had the same bug; the fix makes it
  match the engine path. Guest-only code is untouched.
- Decided by the agent, to confirm: the shading rows (EmitColor, PropertyColor) and the frame patch read LL's live state
  (`LiveWorkingSpace`); the material records read the frame's contributor sample. Records re-evaluate on a space change through
  the witness. Shading rows already sampled when LL is toggled at runtime keep the old space until their property changes (LL's
  toggle reloads every shader anyway).

## Open items

1. Effect lighting colours (`EffectGeometryUpload`: DLightColor PS 11, point light colours PS 8-10 for Lighting effects) are not
   checked: the capture parity compares Lighting draws only, and DCLF's Effect rows for lit effects were not traced.
2. `MenuScenesNative.cpp`'s own SpecularColor and Effect BaseColor blocks are now redundant (snap5-menus2).
3. The capture parity diagnostic races the scene work thread in `Coordinator::FindObject` (crash in `s7-llspec-parity1`).
