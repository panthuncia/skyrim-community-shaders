# Feature bugs found by DCLF's parity checks

Bugs in other Community Shaders features, found while Drawcall Limit Fix's capture parity compared DCLF's
draws with the native ones. Each is its own PR. DCLF works around neither; the parity checks report both.
The investigation that found them is in [drawcall-limit-fix.md](./drawcall-limit-fix.md), "Scene events
before the walk".

## TruePBR: a loader thread writes MATO data into materials that are being drawn

**Where:** `TESBoundObject_Clone3D` in `src/TruePBR.cpp` (installed on `TESObjectSTAT`'s vtable, slot
0x4A). It applies a static's material object (MATO) with `ApplyMaterialObjectData`.

**What happens.** For a static with a PBR MATO, the hook visits every geometry of the freshly cloned model
and applies the MATO's roughness, specular level, base colour scale and glint parameters to the geometry's
`BSLightingShaderMaterialPBR`. Two things are wrong:

-   **It writes into shared materials.** The cloned model's materials are shared with references already in
    the scene. The fork-before-write check only forks when the material already has a *different* owner
    (`prevOwnerRefID != 0`) with a different MATO. A material nobody has claimed yet has owner 0, so it is
    written in place, and every reference sharing it picks up this reference's MATO values.
-   **It writes from a loader thread while the frame renders.** `Clone3D` runs on a loader thread. The write
    lands in the middle of a frame, so draws of that material earlier in the frame use the old values, and
    later ones use the new values.

**Evidence** (temporary logging, a flight from the standard save):

```
[4300] MATO apply thread 4300 ref 0005C108 material 0x251c9a7b580 prevOwner 00000000 roughness 1 -> 0.7 forked false
[4300] MATO apply thread 4300 ref 0005C108 material 0x251c9a7ba00 prevOwner 00000000 roughness 1 -> 0.7 forked false
[4300] MATO apply thread 4300 ref 0005C108 material 0x251c9a7b880 prevOwner 00000000 roughness 1 -> 0.7 forked false
```

-   The render thread was 22340.
-   The three materials were already being drawn by other references, `DirtCliffsCornerIn01`,
    `DirtCliffsCornerOut01` and `L2_Roots` (refs 00029EC2, 00029EC3, 00029ECA), with roughness 1.
-   From that frame on, those references were drawn with ref 0005C108's roughness of 0.7.
-   Capture parity saw it as `PS PerMaterial 22` (`ParallaxOccData`, which TruePBR's `SetupMaterial` fills
    from the projected material's roughness and specular level): DCLF 1, native 0.7, on one frame around
    frame 171 after the save loads. It happened on about half of the runs, depending on load timing.

**Proposed fix:** fork before writing whenever the material is not exclusively this reference's:

-   Fork if another property already uses it, not only if another reference already claimed it. The
    material's reference count, or the ownership map `BSLightingShaderMaterialPBR::All` keeps, can tell.
-   A fresh copy is not drawn by anything until the clone is attached, so writing it from the loader thread
    is then safe.

**What to check in the PR:**

-   References that share a model but have no MATO, or a different one, keep their own values after a
    reference with a MATO loads next to them.
-   The number of materials forked on a cell load, which is a memory cost.

**DCLF side:** nothing to change. DCLF learns of the write through `NoteWritten`, which
`ApplyMaterialObjectData` already reports, and corrects its record on the next frame. The one-frame
difference is gone once the write no longer targets a drawn material. Capture parity reports any remaining
case as `mismatched materials: ... written after its last mismatch`.

## Subsurface Scattering: `IsBeastRace` stays set from the previous face or skin draw

**Where:** `SubsurfaceScattering::BSLightingShader_SetupSkin` in `src/Features/SubsurfaceScattering.cpp`,
called from its `BSLightingShader::SetupGeometry` hook.

**What happens.** In the deferred pass, the hook sets or clears
`ExtraShaderDescriptors::IsBeastRace` in the permutation buffer only when the property has `kFace` or
`kFaceGenRGBTint`. For every other draw it leaves the bit as the last such draw set it. `Lighting.hlsl`
(line 3016) writes the bit into `Masks.y` for the `SSS` + `SKIN` permutation, and `SeparableSSSCS.hlsl` uses
`Masks.y > 0` to choose the human or the beast scattering profile.

A skin draw without either flag therefore gets the profile of whichever face or tinted skin was drawn last.
Draws that aren't skin carry the stale bit too, but their permutation doesn't read it.

**Evidence.** Capture parity's permutation check ("permutation parity ... extra 4", `IsBeastRace` being
`1 << 2`) found draws where native had the bit set and DCLF did not. They were NPC skin (`MaleUnderwearBody1`,
`HandMaleBig3rd`, `MaleUnderwearBodyLightArmor`, refs 000977F5 and FF000E9B) and flora (`FloraThistle01`),
52-99 draws in an interval. It happens only on some runs, because it depends on draw order.

**Also to check.** The actor is taken from `geometry->GetUserData()`, and `isBeastRace` defaults to `true`
when that isn't an `Actor`. Skinned body parts may not carry the actor in their own user data (it is usually
on a node above them). If so, every such draw is treated as beast race. Walk up the parents to the owning
reference, as `CaptureParity`'s `Describe` does, and default to human.

**Proposed fix:**

-   In the deferred pass, decide the bit for every lighting draw.
-   Skin (the draws that compile the `SKIN` permutation): set it from the owning actor's race keyword, found
    by walking up to the reference.
-   Everything else: clear it.

The value then depends only on the draw's own object.

**DCLF side, a follow-up to this PR:** DCLF never sets `IsBeastRace` (its permutation's extra field is
`InWorld` plus `SuppressExternalEmittance`), so it matches today only on human skin. Once SSS decides the bit
per draw, DCLF has to model it:

-   the owning actor's race, recorded per object for skin objects;
-   a per-object permutation bit, like `kObjectSuppressExternalEmittance`.

Capture parity's permutation check will then compare it on every skin draw.
