# DCLF: Advanced Skin's per-geometry material

Design note, 2026-11-18. Branch `org-native/port-skin` off `org_integration` `ca4354a8c`. Written and implemented without a build or
a run. `DCLF/` is `src/Features/DrawcallLimitFix/`.

## 1. The problem

Upstream's skin editor (#3154, #3175) resolves Advanced Skin's material per geometry. For each Lighting draw
`Skin::BSLightingShader_SetupGeometry` finds the geometry's entry (`Observe`, prepared by `Prepare` in `Skin::Prepass`) and uploads
a 144-byte b7 (`PerGeometryData`): the actor's wetness, four material flags and a whole `SkinData` profile. It also binds up to three
textures, t71 (RFAOS controls), t74 (wetness) and t72 (detail normal), and swaps in its own sampler at s15 while t72 is bound.
RaceMenu overlays get the disabled profile, no textures and no wetness. While the editor is open its drafts replace the saved
material, and its preview wetness replaces the actor's wetness on the surfaces the draft touches.

DCLF draws used to get t71 and t74 from a material contributor keyed by the material's hash key. Skin no longer keys anything by
material, so the sync dropped the contributor. DCLF draws then read the common profile with skin always on, zero material flags,
no t71/t74/t72, and whatever the frame record captured at s15.

Dropping the contributor also broke TruePBR in pulled builds. `DCLFMaterialTextureIndex` decodes t71 as feature slot 0, t74 as 1 and
t80 on from slot 2. With Skin's registers gone, Core gives t80 slot 0, so pulled TruePBR draws read the wrong feature textures.

## 2. Design

Per object, the shading row carries one word: an index into a published table of skin materials. The table holds everything else.

### 2.1 Row field

`BindlessShading` (`DCLF/Core/Shading/LightingConstants.h`) is 128 bytes and full except its three padding words. The first becomes
`skinMaterial`, a new shading field `Api::ShadingRowField::SkinMaterial`. `SampleShading` and the Effect port zero only the two
padding words left, so a reshade keeps the field, as it keeps `skinPerGeometry`. HLSL: `DCLFShadingRow::SkinMaterial`
(`Common/DCLFObjects.hlsli`). Zero is table row 0.

A profile does not fit in the row (112 bytes), and most actor-owned slots are armour that never reads it. So the row holds an index,
not the profile.

### 2.2 The table

A frame-value pool `skin-materials`, 160-byte rows (`SkinMaterialRow`):

| Bytes | Field | From |
|---|---|---|
| 0-15 | `materialFlags` | b7's: t71 bound, t74 bound, t72 is BC5 normal-only, thickness |
| 16-127 | `profile` | b7's `SkinData` |
| 128-143 | `descriptors` | t71, t74, t72 view and s15 sampler, each heap index + 1, 0 for none |
| 144-159 | `wetnessOverride` | x: the editor's preview sweat, w: 1 when it replaces the actor's wetness |

Row 0 is the disabled profile (`skinParams.w = 0`), which is what b7 holds for a geometry that is not a skin surface. Rows are
deduplicated by content and reference-counted by slot, with a free list, so an index stays put while any slot uses it. A frame
binding at t38 hands the frame's backing to every DCLF draw (`kFrameBindingSlots` grows from 5 to 6).

### 2.3 Capture: render thread, frame start

The field is registered for `EntryClass::ActorOwned`, like wetness, with its own membership callback. Its capture runs in
`CaptureShadingFields` at the frame's start, before `FrameValues::Kick`:

1. Drain the GPU texture replies (below). On a `GpuTextures` generation change, drop every binding and re-put every member.
2. For each member slot: the newest publication's geometry, its shader property, and `SkinMaterials::IsCompatible` as a cheap
   filter. A compatible geometry goes through `Skin::ResolveGeometryMaterial`, the same function SetupGeometry now calls, so the
   rules (overlays, readiness, revisions, BC5 normal-only, drafts, preview wetness) have one code path. It also observes the
   geometry, so a geometry only DCLF draws is still prepared by `Skin::Prepass`.
3. Turn the views into descriptors, build the row, find or add its table index, and put the index when it changed.

The put carries the row's sealed descriptor owners, so FrameValues keeps the views alive until the frame's GPU work is done.

Not covered, by decision: a skin surface not owned by an actor (Skin's D3D11 path would still give it a material by its texture).
It gets row 0, skin off. Such surfaces are rare (skin-textured statics, mannequin-like props); covering them would need a scan of
every published shading change, as Landscape seams do. This is a known difference from the D3D11 path.

### 2.4 Publication at frame boundaries

New and changed rows are queued on the render thread and handed to the frame's producer by the pool's kick, in the same
`FrameValues::Kick` as the field values. The producer writes them into its journal (`KeptWholeArray`) and sends only the changed
runs. Field values and rows from frames without a producer wait for the next one together. Indices never move while used, and a
slot whose row changes is put again in the same capture, so a frame never sees an index from one capture with rows from another.

### 2.5 Readiness, no waits

- A geometry Skin has not prepared yet resolves as Skin's own pending state: the common profile, no textures, detail off. It is
  prepared in this frame's `Skin::Prepass` and picked up at the next capture.
- Each view is asked of `GpuTextures::Request` once. Until the reply arrives the row has no descriptor for it: its flag is 0, and for
  t72 the profile's detail is off. The next capture after the reply writes the full row.
- A binding superseded by a DXVK relocation is asked for again. The old binding stays in use until the new one arrives.
- A rejected import is fatal. Skin validated the DDS when it loaded it, so a rejection is a bug, not content.
- The s15 sampler is created once per device from `Skin::DetailSamplerDesc()`, the description Skin's D3D11 sampler uses. A failure
  is fatal.

### 2.6 Shader, `DCLF_BINDLESS_DRAW` only (`features/Skin/Shaders/Skin/Skin.hlsli`)

- `StructuredBuffer<DCLFSkinMaterialRow> DCLFSkinMaterials : register(t38)`, read at the object's `SkinMaterial`.
- `GetSkinData()` returns the row's profile. `skinMaterialFlags` reads its flags.
- `skinPerGeometry` is the row's preview override when set, else the shading row's wetness.
- `TexSkinExtraSampler`, `TexSkinWetnessSampler`, `TexSkinDetailNormal` and `DetailSampler` become macros for heap reads at the
  row's descriptors, with `NonUniformResourceIndex`. Skin.hlsli is included after Lighting.hlsl declares t71 and t74, so the
  declarations stay and go unused. No Lighting.hlsl edit, and t72 and s15 are not declared in DCLF draws.
- Every read sits behind a material flag or the profile's detail switch, which is 0 whenever the descriptor is.
- A table index past the table's end cannot happen: the rows are sent with the index that names them.

### 2.7 TruePBR's slots

The feature texture decode moves to `t80 + n -> slot n` (`DCLFMaterialTextureIndex`, `Manifest.cpp`'s `DecodedFeatureSlot`), and
Skin's registers leave `kFeatureTextureRegisters`. t71 and t74 are no longer material-row slots.

### 2.8 Parity

`DrawSkinTextureParity` checks each native draw's t71, t74 and t72 against the views the object's row was built from, and b7's flags
and profile (`Skin::lastGeometryData`) against the row. It notes the frame a geometry first resolves, when the capture ran before
that frame's Prepass. `currentWetness` became `lastGeometryData.skinPerGeometry`.

## 3. Open decisions

1. **Rejected imports: decided.** A texture whose import fails logs an error once, and the surface draws without it (its flag or
   the detail switch off). The view is not asked again until the device changes.
2. **Non-actor skin surfaces: decided.** They get skin off (section 2.3). Tracking every slot (as Landscape seams do) would cover
   them at the cost of a scan of every published shading change.
3. **The capture resolves every compatible member every frame**, as SetupGeometry does per draw. Skin's revisions could gate it
   later if the cost shows (zone `CS.DCLF.Capture.SkinMaterials`).
4. **t38** is a new frame binding register. Any free register works.
