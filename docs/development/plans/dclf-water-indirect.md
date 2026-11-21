# Water as DCLF scene objects, drawn by indirect draws

Design plan, 2026-11-21. Branch `org_integration`. Nothing was built or run. Every claim below was read in the code or the docs
named next to it, unless it is marked **(unverified)**. No Ghidra session was used for this plan.

`DCLF/` is `src/Features/DrawcallLimitFix/`. "The latch path" is today's water family, `DCLF/Features/Water/`
([p5/water.md](../p5/water.md)).

## 1. Goal and summary

Today every BSWaterShader draw runs the engine's CPU path: `GetRenderPasses`, `BSBatchRenderer`, `SetupTechnique`,
`SetupMaterial`, `SetupGeometry`, CS's and other mods' hooks on them, and then the latch. The latch copies 2,736 bytes a draw and an
ORG pass at the EngineDraws anchor replays it. At Riverwood's river with Unified Water that is about 225 draws a frame, and the
latch alone is about 1.2 ms a frame in a /Od build.

The goal: water geometry becomes ordinary DCLF scene objects. The scene lane classifies them once, the GPU culls them with every
other main-camera object, and indirect draws draw them. The engine's per-draw water CPU work stops for every water object DCLF
claims.

The recommended design, in one paragraph:

- Water is a **material port** (`Api::MaterialPortContribution`, as Features/Effects ports `BSEffectShaderProperty`) with a new
  object class `ObjectClass::Water` and a new program family slot `ProgramFamily::Water`.
- Water shapes enter the scene through **category roots** (the cells' Water node and the LOD water roots). Today they are not
  tracked at all.
- Per-object values (the water plane, the cell and flow offsets, the stencil bit) are a small **table family**. Per-material values
  that move every frame (normal scroll, fog, colours times the water lighting) are a **frame-value pool** of water material rows
  named by a **shading field**, as Advanced Skin's material rows are. Per-frame values are one frame row.
- BuildDraws culls water in the colour segment into **two new ranges** (mask and surface), bucketed by pipeline and stencil
  reference. Two **shared epochs** draw them at the two engine batch points the latch path already hooks.
- Under Light Limit Fix no per-pass lights are needed. Without it, water stays on the latch path at first.
- The latch path stays during the migration and draws whatever DCLF does not claim. It is deleted at the end.

## 2. What each water draw depends on

The inventory is the latch layout: `Water.h` `DrawConstants` with `kGroupRows {1, 6, 14, 5, 14, 22}` (VS b0, b1, b2, PS b0, b1, b2),
and `Water.hlsl`'s `WaterOrgDraw` and packoffsets (lines 132-152, 300-321, 607-640). The engine functions are SE 100601-100605 /
AE 107362-107365 ([p5/water.md](../p5/water.md), "The shader").

Scope means: **object** (fixed per water shape until an engine write), **material** (per `BSWaterShaderMaterial`), **frame** (the
same for every water draw of a frame), **pass** (differs between passes of one shape).

### 2.1 Constants

| Block | Rows | Values | Scope | Where the engine computes it | DCLF |
| --- | --- | --- | --- | --- | --- |
| VS b0 | 1 | `QPosAdjust` | frame | SetupTechnique: the camera's position adjust (unverified that it is plain `CameraPosAdjust`) | frame row |
| VS b1 | 0-2 | `VSFogParam`, `VSFogNearColor`, `VSFogFarColor` | material x frame | SetupMaterial: the sky's fog, possibly mixed with the material's above-water fog (unverified) | water material row |
| VS b1 | 3-4 | `NormalsScroll0/1` | material x frame | SetupMaterial: `normalScroll1-3` (material +0x100..0x110) times a timer (unverified which timer) | water material row |
| VS b1 | 5 | `NormalsScale` | material | `uvScaleA` (material +0x120) (unverified mapping) | material record |
| VS b2 | 0-3 | `World` | object, moves | SetupGeometry: the geometry's world | placement row (FrameValues) |
| VS b2 | 4-7 | `PreviousWorld` | object, moves | SetupGeometry | placement row |
| VS b2 | 8-11 | `WorldViewProj` | object x frame | SetupGeometry | computed in the vertex stage from the placement row and the frame's view-projection |
| VS b2 | 12 | `ObjectUV` | object x session | SetupGeometry. Unified Water rewrites the globals it reads before each call (`UnifiedWater.cpp`, `BSWaterShader_SetupGeometry`): flowmap width, height, and `1 - local.scale` | water row (scale) + frame row (flowmap size) |
| VS b2 | 13 | `CellTexCoordOffset` | object | SetupGeometry from the property's `flowX/flowY/cellX/cellY` (+0x90..0x9C). Unified Water rewrites them per call from the geometry's world translate and its flowmap offsets. The displacement mesh reads the global `gDisplacementCellTexCoordOffset` instead (528184 / 415129) | water row; a pure function of world position and the flowmap offsets. The displacement mesh: frame row |
| PS b0 | 0 | `VPOSOffset` | frame | SetupTechnique: inverse render size | frame row |
| PS b0 | 1 | `PosAdjust` (w: inverse framebuffer range) | frame | SetupTechnique | frame row |
| PS b0 | 2 | `CameraDataWater` | frame | SetupTechnique: near and far from `DAT_1431d0e68 +0x160/+0x164` | frame row (Effect's `SampleEffectCamera` reads the same fields) |
| PS b0 | 3-4 | `SunDir`, `SunColor` | frame | SetupTechnique: the sun light | frame row |
| PS b1 | 0-2 | `ShallowColor`, `DeepColor`, `ReflectionColor` | material x frame | SetupMaterial: the material's colours. Linear Lighting rewrites them as working-space colours times the water lighting global 527901 / 414848 (`LinearLighting.cpp`, `WaterMaterialUpload`). Whether vanilla also multiplies by a frame value is unverified | water material row |
| PS b1 | 3-7 | `FresnelRI`, `BlendRadius`, `VarAmounts`, `NormalsAmplitude`, `WaterParams.xyz` | material | SetupMaterial from the material fields (`fresnelAmount`, `specularPower`, `flowmapScale`, `sunSpecularPower`, `reflectionAmount`, `alpha`, `refractionMagnitude`, `amplitudeA`, `noiseFalloff`, `reflectionMagnitude`, `sunSparklePower`; packoffset comments in Water.hlsl) | material record |
| PS b1 | 7.w | framebuffer range | frame | SetupMaterial (unverified source) | frame row |
| PS b1 | 8-10 | `FogNearColor`, `FogFarColor`, `FogParam` | material x frame | SetupMaterial: the sky's fog with the material's `aboveWaterFogDistFar` and fog amount | water material row |
| PS b1 | 11 | `DepthControl` | material | `depthProperties` (+0xF0) | material record |
| PS b1 | 12-13 | `SSRParams`, `SSRParams2` | frame | INI values (`fWaterSSRIntensity`, ...) and the render size | frame row |
| PS b2 | 0-3 | `TextureProj` (column-major) | frame, maybe object | SetupGeometry. Unverified whether it includes the object's world or only the camera | frame row if camera-only; otherwise computed in the shader |
| PS b2 | 4 | `ReflectPlane` | object x frame | SetupGeometry from the property's `plane` (+0xA0) (unverified whether in world or camera space) | water row (plane) + shader transform |
| PS b2 | 5 | `ProjData` | frame | SetupGeometry: projection terms | frame row |
| PS b2 | 6-21 | `LightPos[8]`, `LightColor[8]` | pass | SetupGeometry from the pass's scene lights, light passes only. Linear Lighting rewrites `LightColor` (`WaterGeometryUpload`) | not under LLF (section 4) |
| PS b3 | - | Light Limit Fix's `StrictLightData` | object | LLF's water SetupGeometry hook (`LightLimitFix.cpp:1158`) runs `_Before` and `_After` only: `NumStrictLights` 0, `ShadowBitMask` 0, `FirstPerson` 0, and `RoomIndex` from the geometry's room | object record's ancestor value (LLF's room index), already in DCLF |
| permutation | - | CS's descriptors, `EffectRadius` | key x frame | `State::permutationData`. The pixel stage reads `ExtraShaderDescriptor`'s InWorld flag | pipeline row, as Lighting's |

### 2.2 Textures and samplers (t0-t11, s0-s11)

| Slot | What | Scope | Source |
| --- | --- | --- | --- |
| t0 | reflection | none | never sampled by the surface (`SlotRead`) |
| t1 | refraction: kMAIN's copy | frame | the engine copies kMAIN before the water (`ListInputs`) |
| t2 | displacement: kWATER_DISPLACEMENT or its swap | frame | ImageSpace's simulation (`CS_ORG_IMAGESPACE`) |
| t3 | the reflection cube | frame, maybe material | the property's or `DAT_14302c8f0` (p5/water.md). The cubemap fix nulls it in interiors (`WaterReflectionCubemapFix.cpp`). Unverified whether there is more than one cube |
| t4-t6 | normal maps | material | `normalTexture1-3` (material +0x40..+0x50) |
| t7 | depth: kPOST_ZPREPASS_COPY | frame | FinishAccumulating's second copy |
| t8, t9 | flow map and its normals | frame | the ImageSpace port's flow copies (`kFlowMapId`) or Unified Water's flowmap |
| t10, t11 | SSR | frame | kSSR, kSSR_RAW |
| s0-s11 | sampler modes | technique | SetupTechnique. Physical Sky swaps s13 in its hook (`PhysicalSky::ModifyWater`) |

The headers (t17-t103, 18 registers in `kHeaderRegisters`) and t98 (the sun's shadow record) are frame resources. DCLF's frame
record already serves the same registers to Lighting (`FrameTexture`, `FrameGraphBuffers`, "Frame graph textures" in
dclf-architecture.md). Physical Sky's handles are frame values.

### 2.3 Per-pass state

| What | Values | Source |
| --- | --- | --- |
| technique | 0 surface, 1-7 lights, 8 underwater, 9 LOD, 10 stencil (mask), 11 simple | GetRenderPasses (100238 / 106945): surface, stencil and, when lit and not underwater, a light pass |
| flags | Vc, NormalTexCoord, Reflections, Refractions, Depth, Interior, Wading, VertexAlphaDepth, Cubemap, Flowmap, BlendNormals | GetRenderPasses from the property flags and globals. Which bits come from globals is unverified. CS drops Reflections, Cubemap and Interior (`State.cpp:1516`) |
| colour targets | mask: kNORMAL_TAAMASK_SSRMASK or its swap (by frame) + kMOTION_VECTOR. Surface: kMAIN, or kRAW_WATER when `DAT_143233639` | SetupTechnique |
| blend | mode 2 (additive) for light passes, write mode 9 for the mask, 1 for the surface | SetupTechnique |
| depth, stencil | see the table in p5/water.md: the mask writes the water type's bit (`property +0x8C`, 2..0x80) with ref = the bit; surfaces test NOT_EQUAL on bit 0, ref 1; wading and hint-13 passes use other modes and refs | SetupGeometry |
| cull | none | SetupTechnique |

### 2.4 What DCLF can compute and what needs a hook

- **DCLF computes it** from the property, material and geometry: every material value, the plane, the stencil bit, the cell and
  flow offsets (from world position and Unified Water's flowmap offsets), the pass list and the descriptors (a port of
  GetRenderPasses, as `DeriveLightingDescriptors` ports Lighting's).
- **Frame captures** on the render thread at the frame start or the colour commit, as `EffectFrame.cpp` does: camera data, sun,
  fog, SSR INI values, the render size, the water lighting global, the scroll timer, `DAT_143233639`, Unified Water's flowmap
  globals and the displacement mesh offset.
- **Engine events needed:**
  - water shapes attached and detached: the cells' Water node, `TESWaterSystem::AddWater` / `RemoveWater`, and Unified Water's
    `BGSTerrainBlock_Attach` / `_Detach` (it already calls `DrawcallLimitFix::WriteHidden`, so its hides are events);
  - property flag writes that change the pass list, at least `kUnderwater` (bit 5). Unverified who writes it and when;
  - the property's `unk8C` (the stencil bit) and `plane`, if they change after attach (unverified);
  - material writes (Unified Water's `InitializeWater_SetWaterShaderMaterialParams` runs at init; weather may change materials:
    unverified).

## 3. How water enters DCLF's scene

### 3.1 What DCLF sees today

- The coordinator tracks only the cells' Static, Dynamic, MultiBound and Actor nodes, `TES::lodLandRoot`, the portal graphs' roots
  and first person (`Coordinator.h:105`, dclf-architecture.md "What is tracked": "Markers and water are not tracked").
- So cell water (cell child 6, skyrim-engine-notes.md "6 Water") is never walked.
- A water shape that is walked anyway (for example LOD water left under `lodLandRoot`) has a non-Lighting property and no port, so
  `Eligibility::Static` returns `Ineligible::NotLightingShader` and it stays the engine's. Whether any LOD water is under
  `lodLandRoot` is unverified: a terrain block's water node is detached from its container and registered with the water system
  (skyrim-engine-notes.md, "Terrain block"), and Unified Water attaches its tiles to `gWaterLOD` (516171 / 402322), whose parent
  is unknown.
- In interiors the water system's node is in list 0 (skyrim-engine-notes.md). That is probably `TESWaterSystem::waterRoot`
  (+0x90), unverified.

### 3.2 The path in

1. **Category roots.** `Api::ObjectClassContribution::categoryRoot` returns one root. Water needs several: every attached cell's
   Water node, the LOD water roots and the interior water node. Two options:
   - add the cell Water node to Core's cell category capture (beside Static, Dynamic, MultiBound, Actor), plus one or two plain
     roots through `categoryRoot`;
   - or widen the contribution to a gather of roots (like `UnheldRootsContribution::gather`, but held).
   Phase 0's census says which roots exist. The second option keeps Core free of water knowledge and is preferred.
2. **The port.** `MaterialPortContribution` with `propertyRtti` = BSWaterShaderProperty, `materialVtable` =
   BSWaterShaderMaterial, `materialBytes` 0x180, `textureFields` {0x38, 0x40, 0x48, 0x50, 0x58}, the VS and PS stage spaces of 2.1,
   and `capture`, `derive`, `evaluate`, `shading`. `evaluate` is SetupMaterial as a pure function. With the port on, the existing
   port branch in `Eligibility::Static` classifies water BSTriShapes (`Extensions::DerivePort`). No new geometry type is needed.
   Both TESWaterObject's shape and Unified Water's tiles are BSTriShape.
3. **Frame variables.** The material values that move every frame (scroll, fog, coloured colours) are more than the port's four
   frame variables (`kMaxMaterialFrameVariables`). So they go in a **water material pool** instead: a frame-value pool of one row
   per water material, sampled each frame, read at a free texture register, with a new `ShadingRowField::WaterMaterial` naming the
   row per slot. This is Advanced Skin's pattern (`SkinMaterials.cpp`: a pool at t38 and a shading field per slot).
4. **Per-object water row.** The plane, the cell and flow offsets, the stencil bit and the local scale are a **table family**
   (`Api/TableFamilies.h`) with its row in the object record's feature block (`recordWord`), as tree wind's slot is. They change
   only on events, so they are not FrameValues.
5. **Membership.** Main phase only (`kSetMain`). No shadow, occlusion-map, reflection or first-person phase:
   - the caster rule rejects non-Lighting properties today (unverified for a port's objects: check `ShadowReject`);
   - `TESWaterReflections::Update` hides water around the cube faces, a render-time hide already captured with the frame;
   - the engine draws no first-person water.
6. **Withholding.** `PassCapture`'s `WithholdMain` withholds a member's main passes whatever its shader. So once a water shape is a
   member, its mask, surface and light passes never reach `BSBatchRenderer`, and SetupTechnique/SetupMaterial/SetupGeometry do
   not run for it. A non-member's passes still reach the batch, where the latch path draws them. This is how both paths coexist,
   object by object.
7. **The engine's water cull** (`FUN_140644f60` -> `FUN_140521c80`, the water system's process `0x14338c648`) keeps running until
   phase 5. It calls GetRenderPasses for every visible water shape. Removing it is the last CPU saving and the riskiest (section 6).

### 3.3 Why not a table family like grass and tree LOD

Grass and tree LOD use draw families (`Api/DrawFamilies.h`) because their instances are not scene graph objects: grass instances
and tree LOD instances are records the engine keeps elsewhere. Water shapes are BSTriShapes in the scene graph. The scene path gives
them withholding at registration, hidden events, placement rows, the geometry pool, vertex pulling and the census for free. A draw
family would need its own mirror, its own add and remove events, its own cull and a new `DrawFamilyBuild` mode. The table family is
still used, but only for the per-object water row (3.2, item 4).

Unified Water's tiles are many instances of one mesh. A draw family could draw them instanced. At about 225 draws a frame the GPU
cost of separate indirect draws is small, so this is not worth a second path.

### 3.4 Culling

- BuildDraws culls water in the **colour segment** with a new view bit `kViewWater` (bit 12 is free: Core's bits are 0-4 and 8-10,
  features use 5-7 and 11; `GpuLayouts.h:193-205`).
- Frustum, then the HZB the depth epoch built. Water never writes the HZB: it is not in `kDepthViewBits`.
- Bounds are the placement row's world bound. Unified Water's distant tiles are large (4096 x size units), so the frustum test
  keeps most of them. That is the engine's behaviour too.
- The engine may also limit water by its multibound AABBs (`AddWater`'s `a_multiBoundShape`). DCLF would not. Unverified whether
  that changes what is drawn.

### 3.5 Ordering and the two draw points

The engine draws water at two batches (p5/water.md, "The draws"):

| Batch | Where | DCLF epoch |
| --- | --- | --- |
| the mask (Stencil technique) | `FinishAccumulating` `RenderBatches(0x5C00006D..0x5C000070)` after the stencil clear, after DCLF's colour epoch and the blended decals | `water.mask`, triggered at the mask batch thunk |
| the surface | `FinishAccumulatingPostResolveDepth` `RenderBatches(0x5C000071..0x5C006071)`, after the under-water blended draw and group 10, before ISWaterBlend and DCLF's forward epoch | `water.surface`, triggered at the surface batch thunk |

- Both epochs share `main.colour`'s shape and draw what this frame's colour commit culled, as the forward epoch once did through
  `IndirectDraws::SubmitSharedEpoch`. The GPU forward ranges were deleted in simplification 4.4, so the "range culled in colour,
  drawn in a later epoch" plumbing must be rebuilt for water. The decal groups' ranges and the refraction range's old layout are the
  models.
- **Stencil.** The stencil reference is not an indirect argument. Each range is bucketed by (pipeline, stencil reference), and the
  pass sets the reference per bucket (`CommandList::SetStencilRef`, BasicRHI abi 9; `RasterProgram` already uses it). The mask has at
  most 7 water-type bits (2..0x80), the plain surface one reference (1). Wading (7 refs) and hint 13 (0xFF) add a few buckets.
- **Order inside a batch.** The engine sorts passes by pass id, so the mask's stencil writes and the surfaces' tests are separated by
  batch, and inside the surface batch the light passes follow the surface pass. DCLF keeps that by bucket order. Overlapping water
  surfaces of one technique draw in the engine's order today and in bucket order with DCLF. Water does not write depth in the surface
  pass (depth mode 1), so two overlapping surfaces could differ (unverified that any real content overlaps).
- **Geometry group 10.** Its batch (`kGroup10Batch`) holds water fade and cross-fade passes beside Lighting cross-fade copies.
  `WithholdMain` already withholds hint 10 copies of members. DCLF draws a member water shape once, at the surface point. Unverified
  whether a water shape in group 10 needs its fade (water has no fade node as far as the code shows: check in phase 0).
- **Under water.** The engine's blended draw has two call sites, before the water when the camera is under water and after it
  otherwise (`MainEpochs.cpp:114`). The forward epoch already follows the engine's trigger. Water's epochs follow theirs, so the
  order is the engine's in both cases.
- **kRAW_WATER.** When `DAT_143233639` is set the surface draws into kRAW_WATER for ISWaterBlend. The epoch imports both targets
  and picks one per frame, as the latch path's client does.

### 3.6 Programs and pipelines

- A new `ProgramFamily::Water` slot with a new build mode (`ProgramBuild::WaterMaskAndSurface`): per key, a mask pipeline (Stencil
  technique, two colour targets, write mode 9, the stencil mode of the water type's bit) and surface pipelines (one per target
  format and stencil mode).
- `Water.hlsl` gains a `DCLF_BINDLESS` path beside its `CS_ORG_BINDLESS` latch path: World and PreviousWorld from placement rows,
  material values from the material row and the water material pool, per-object values from the water row, headers from the frame
  record, vertices pulled (`DCLF_PULLED_VERTICES`; Water.hlsl already pulls through `DCLFVertexPull.hlsli`), LLF's room index from the
  object record through `NSCB_ALIAS` (dclf-architecture.md says the alias is kept off Water today).
- Keys come from the derived pass descriptor, not a registration, as for Lighting and Effect. The descriptor CS looks up is the
  vertex descriptor (`State::modifiedVertexDescriptor`), because the pixel stage reads Wading at run time.

## 4. Lights

- **With Light Limit Fix** (the normal configuration), water's pixel stage takes point lights from LLF's clusters (t35-t37) and
  its local shadows (t102, t103), all frame resources. `StrictLightData` for water carries only `RoomIndex` (LLF's water hook never
  fills strict lights). DCLF's object record already carries the room index. So no per-draw light data is needed.
- The engine still makes light passes (techniques 1-7) when lights touch the water. Under `LIGHT_LIMIT_FIX` their pixel stage adds
  nothing (`Water.hlsl` 1447-1462: the light loop is compiled out, `finalColor` stays zero; p5/water.md says the light techniques'
  pixel stage reads nothing). DCLF does not draw them. They are withheld with the member, so the engine does not draw them either.
  This should be pixel-identical. Check it in phase 3's shots.
- **Without Light Limit Fix** the light passes carry up to 7 lights each in PS b2. Options:
  - keep such water on the latch path (refuse the port's light passes when the "light limit fix" classify input is off), as the
    Effect port leaves its Lighting variant native without LLF;
  - later, read DCLF's point light table (PS b3, 128 lights, "Point lights without Light Limit Fix") and select per object, folding
    the lights into the surface pass. Additive light passes over an opaque surface equal one pass that adds the lights, up to
    rounding. Linear Lighting's colour rewrite (`WaterGeometryUpload`) and the `affectWater` test must be ported too.
  Recommended: the first option until someone needs water without LLF.

## 5. Phases

Each phase has a test gate. Shots are compared with `cs-test compare` against the latch path's shots from the same stage
(`river`, `rain`, `guest-river`, `native-river`, `guest-interior`, plus an underwater shot config to add). Runs are short: one load,
early exit. With `CS_DCLF_STATS` and the census on where noted.

### Phase 0: inventory and parity on the latch path (no drawing change)

- Add a water census to the latch path: per latched draw, the geometry's parent chain up to its root, the property's flags,
  `unk8C`, the pass id, the material pointer, and whether the shape is Unified Water's.
- Write the port as pure functions (descriptors from the property, material rows, per-object row, frame row) and run it as a
  parity observer beside the latch: for every latched draw, predict the six constant groups and compare bit for bit
  (`ConstantBlock::SameBits` style; a `<- WATER PORT` line). This finds every value of section 2 marked unverified.
- Ghidra checks to settle: SetupMaterial's fog and timer sources, TextureProj and ReflectPlane spaces, who writes `kUnderwater`,
  which object is the displacement mesh, GetRenderPasses' global flags, where vanilla LOD water and `gWaterLOD` hang.
- **Gate:** zero port mismatches over `river`, `rain` and an interior run, with Unified Water on and off; the census names every
  root that holds a drawn water shape.

### Phase 1: water tracked and classified, not claimed

- Category roots (3.2 item 1), the port registered, `ObjectClass::Water` registered with the claim off (classification and
  census only), the water row table family, the water material pool.
- **Gate:** the census lists the same water shapes each frame as the latch path draws (by geometry pointer, within the frustum),
  no `NotLightingShader` water left, no shot change, no new `[E]` lines, no scene-lane time regression above noise.

### Phase 2: the mask by DCLF

- `kViewWater`, the mask range in the colour segment, the `water.mask` shared epoch, the mask pipelines, `Water.hlsl`'s
  `DCLF_BINDLESS` Stencil technique (no textures, no headers).
- Claim plain water objects (no Wading, no vertex-colour hint 13, not underwater). Their mask and surface passes are withheld
  together, so phase 2 must draw the surface too or keep claims off. Recommended: phases 2 and 3 land together behind one switch
  (`CS_DCLF_WATER`, default off), with the mask checked first by a debug view of the normals target and the stencil.
- **Gate:** the normals target's blue channel, the motion vectors and the stencil (RenderDoc or `SARP_DEBUG_OUTPUT_TYPE`-style
  view, or a stencil visualisation) match the latch path at the river.

### Phase 3: the surface by DCLF, with Light Limit Fix

- The surface range, the `water.surface` epoch, surface pipelines, the full `DCLF_BINDLESS` pixel stage (material slots, headers,
  t98, Physical Sky), techniques 0, 9 and 11, kMAIN and kRAW_WATER.
- Objects needing anything not ported stay non-members and the latch path draws them.
- **Gate:** `river`, `rain` (ripples), `guest-river`, `native-river` shots match the latch path within the usual eye-adaptation
  variance; the latch census shows no plain water draws left; the DCLF report counts water members; render-thread time drops by
  about the latch's 1.2 ms (/Od) plus the engine's setup time; GPU time unchanged within noise.

### Phase 4: the remaining water

- Underwater (technique 8), wading and the displacement mesh, hint 13 vertex-colour water, interiors, the cubemap fix's interior
  rule, water without LLF (only if wanted, section 4).
- The displacement mesh moves every frame: it is a mover by event if its position write is hooked (Unified Water's
  `TESWaterSystem_UpdateDisplacementMeshPosition` hook is that point), otherwise it stays on the latch path.
- **Gate:** an underwater shot, a wading shot (player in the river), an interior with water (for example a Dwemer ruin pool), each
  matching the latch path; the latch census empty in those runs.

### Phase 5: remove the engine's water CPU work and the latch path

- Skip the engine's water cull for claimed objects, or entirely, once phase 0's checks show nothing reads its results
  (`TESWaterReflections::Update`'s choice of reflection, wading and ripples, `kAccumulated`). Unverified; see risks.
- Delete `DCLF/Features/Water/` (the latch path) and its EngineDraws clients once the census shows no refused water in the tour
  configs. Keep the batch thunks only as the epochs' triggers.
- **Gate:** a full tour (`guest`, `native-tour`) with no water refusals, shots unchanged, and the water cull's time gone from the
  main thread timeline.

### Fallback during the migration

- The latch path stays unchanged until phase 5. Any water shape DCLF does not claim (not ported, waiting for a pipeline or texture)
  reaches the batch and the latch path draws it, in the same frame position.
- A waiting member is withheld and drawn by nobody until it is ready, as for every DCLF class (dclf-architecture.md, "An object
  DCLF cannot draw yet is nobody's"). For water that is a visible hole for a frame or more. The program prebuild manifest
  (`engine-draw-programs.txt` style) should include the water keys so this does not happen at the first sight of a water type.
- `CS_DCLF_WATER=0` returns every water shape to the latch path, by not registering the object class.

## 6. Risks and unknowns

- **Roots (unverified).** Where vanilla LOD water and Unified Water's `gWaterLOD` hang. If a root is parentless or owned by the
  water system, it needs the held-gather contribution and add and remove events of its own.
- **Engine writes without events (unverified).** `kUnderwater`, `unk8C`, `plane`, the flow and cell fields (Unified Water writes
  these per draw today; DCLF must compute them instead and must not depend on the hook having run). Weather changing a water
  material. A missed write shows as wrong water, which the phase 0 parity must catch first.
- **Per-frame inputs drawn from SetupMaterial (unverified).** If the scroll or fog depend on per-pass state rather than the frame,
  the water material pool is wrong. Phase 0 settles it.
- **Hook-dependent values.** Unified Water, Linear Lighting, Light Limit Fix, Physical Sky, the cubemap fix and frame annotations all
  hook `BSWaterShader::SetupGeometry` or SetupMaterial. A member water shape runs none of them. Each one's effect must be ported as
  data (listed in section 2). A new CS feature that hooks water will silently not apply to DCLF water; the architecture rule ("a
  hook on a shader setup function must not assume every call is a real draw") must be stated for water too.
- **Removing the engine's water cull (unverified).** The cull may feed the water reflection's choice of cube, the displacement and
  ripples, and `kAccumulated` on water shapes. Phase 5 only after checking each.
- **The shared-epoch range.** The GPU forward ranges were deleted; rebuilding a colour-segment range drawn by a later epoch is new
  Core work. It must hold the "an epoch's draws never read two publications" rule.
- **Stencil semantics.** The engine's stencil and depth modes (2-9, 14-20, 30, 32-38) were read from Ghidra in p5/water.md but their
  D3D11 descriptions are not checked here. The latch path already maps them through `Family::DescribeStates`; DCLF's pipelines should
  reuse that, captured inside the right pass as `DrawPipelines::CaptureEngineStates` does for decals.
- **DGC.** If the water ranges go through the main pipeline set's device-generated commands, the stencil reference cannot change per
  sequence. Plain bucketed indirect draws avoid this, as the reflection faces do.
- **Overlap order.** Overlapping water surfaces may draw in a different order (3.5). Unverified that it matters.
- **Native mode.** The plan follows DCLF wherever DCLF runs. In native D3D12, where DCLF is off, the latch path (or its native
  equivalent) must stay until DCLF runs there.
- **Hidden water around cube faces.** Captured with the frame today for Lighting. Check that a water member's hide around the faces
  is not taken as a scene change.

## 7. Size

Rough, in new lines of C++ and HLSL. No code was written.

| Phase | Work | Size |
| --- | --- | --- |
| 0 | census, pure port functions, parity observer | 1,000-1,500 |
| 1 | category roots (Core), port registration, object class, water row family, water material pool | 1,000-1,500 |
| 2-3 | `kViewWater` and ranges in BuildDraws (C++ and HLSL), two shared epochs, program family and build mode, `Water.hlsl` DCLF path, frame row | 2,000-3,000 |
| 4 | underwater, wading and displacement mesh, hint 13, interiors | 800-1,500 |
| 5 | cull skip; deletion of the latch path (about -1,200 lines) | +200, -1,200 |

Total about 5,000-7,500 new lines, about 4,000-6,000 net after phase 5. Phases 0-3 are the bulk and give most of the CPU saving.
The longest unknowns are phase 0's Ghidra checks and the shared-epoch range in Core.

## 8. Phase 0 findings

Written with phase 0's code (branch `dclf/water-p0`, 2026-11-21). Read with GhidraMCP on SE 1.5.97 and AE 1.6.1170. Both runtimes
compute the same values in the same order; only the addresses differ. Nothing here was run in game yet: the parity observer
is what checks it.

### 8.1 The code

- `DCLF/Features/Water/Port/WaterPort.h`, `WaterPortEvaluate.cpp`, `WaterPortCapture.cpp`: the port. `CaptureFrame` and
  `CaptureObject` read the engine. `EvaluateTechnique`, `EvaluateMaterial`, `EvaluateObject`, `EvaluateGeometry`, `EvaluateLights`,
  `Assemble` and `DeriveRenderPasses` are pure. `Assemble` writes through a shader's constant tables in the engine's order, so a
  variable the shader lacks lands where the engine's write does (`CanonicalTables` is Water.hlsl's packoffsets).
- `WaterParity.cpp`: the observer, called from `WaterCapture.cpp`'s `TryLatchDraw` after a draw is latched. `WaterCensus.cpp`: the census.
- The switch: `CS_DCLF_WATER_PARITY=1` (a Parity switch, read once at install). It needs the latch path (graph mode, `CS_ORG_WATER=1`).
- Log lines: `[DCLF] water port: differs: <stage> b<n> c<row> <variable>.<component> (<context>), latched ..., port ... <- WATER PORT`
  once per field and context, `[DCLF] water port: N draws checked in the last 600 frames (...) <- OK` (or `<- WATER PORT`),
  `[DCLF] water census: '<shape>' (<RTTI>) technique ... chain ...` once per shape and technique, and `[DCLF] water census: N roots
  held drawn water in the last 600 frames ...`.
- Unified Water gained two read-only accessors for the port: `GetFlowRewrite()` (what its SetupGeometry hook rewrites from) and
  `GetWaterLODRoot()`.

### 8.2 Settled from Ghidra

Ids are Address Library ids, SE / AE.

- **Fog (SetupMaterial).** The scene graph's fog property: `BSShaderManager::State.shadowSceneNode[State.sceneGraph]->fogProperty`
  (+0x220). Near and far colours at +0x38 and +0x44, distances +0x50 and +0x54, `power` +0x84, `clamp` +0x88. Without a fog
  property both colours are the global 523935 / 410515 and the distances 0. `VSFogParam` and `FogParam` are `(near / (far - near),
  1 / (far - near), material +0x158, +0x15C)`. The near colours' w is the material's +0x160, the far colours' w is the fog's clamp.
  The fog's `power` also lands in `NormalsScale.w` and `FresnelRI.y`.
- **The timer.** `ReflectionColor.w` is `State.timerValues[0]` (513213 / 390953). The normal scrolls are the material's own
  `normalScroll1-3` (+0x100..+0x11F) copied whole, not times a timer. So the scroll moves only when the material is written.
- **The water lighting.** Vanilla multiplies all four components of ShallowColor (rgb and sun sparkle power), DeepColor and
  ReflectionColor (before its w is set) by the global 527901 / 414848. Linear Lighting then rewrites rgb only.
- **The framebuffer range.** `State +0x9C` (CommonLib's `invFrameBufferRange`, 513233 / 390973) is `PosAdjust.w` and `QPosAdjust.w`;
  `WaterParams.w` is 1 / it.
- **QPosAdjust** is plain `RendererShadowState::posAdjust`, w as above.
- **SSR.** `SSRParams = (ImageSpaceManager +0x1C0's +0x10 byte ? 513715 : 0, 513717, 1 / width, 1 / height)`, `SSRParams2 = (513719, 0,
  0, 0)`; the render size is 524972 / 411453 and the next word.
- **TextureProj** includes the object's world: `texture space (0.5 scale and offset on x, y, z) x transpose(world x viewProj)`, with
  the world less `posAdjust`. It is written for every technique but the light ones (1-7).
- **The view-projection.** WorldViewProj and TextureProj use the jittered `cameraData.viewProjMat` when 513713 / 391664 is set,
  otherwise `viewProjMatrixUnjittered`.
- **PreviousWorld** is the current world with `previousPosAdjust`, not the object's previous transform.
- **ReflectPlane** is the property's plane as stored (+0xA0, world space) with w = `(n.y e.y + e.x n.x + n.z e.z) - d`, e the
  current accumulator's `eyePosition`. **ProjData** is `cameraData.projMat` [0][0] and [1][1], zw not written.
- **ObjectUV.** Property flag bit 8 (0x100): x = 1 only. `kEnableFlowmap` (0x8000): `((float)527644, 528164.x, 1 - 528164.y)`.
  Otherwise x = 0 only. Unified Water rewrites 527644 and 528164 before each call (width, height, 1 - local scale).
- **CellTexCoordOffset** (descriptor Flowmap only): `(flowX, (527644 - flowY) - 1, cellX, -cellY)` from the property, or 528184 /
  415129 with the Wading bit (the displacement mesh).
- **Light passes** write `LightPos = (NiLight world translate, radius.x)` (world space, no posAdjust) and `LightColor = diffuse x
  (fade x lodDimmer) x material +0xAC`, w 0. A light that does not affect water gets position 0 and colour 512012 / 388604.
  `NumLights` is not in CS's Water.hlsl, so its table entry is 0 and the engine writes the light count over PS b2's first float.
- **Components never written:** BlendRadius.w, ProjData.zw, ObjectUV.yz without kEnableFlowmap, VPOSOffset without the Depth bit,
  CellTexCoordOffset without the Flowmap bit, TextureProj in light passes, lights past the count. They hold stale bytes, so the
  observer does not compare them.
- **GetRenderPasses' globals.** The gate is 513342 / 391120 (TESWaterSystem::Enable writes it); render modes 12-17 make no passes;
  527793 / 414746 takes the local map's path. Interior is `State.interior` (+0x44). NormalTexCoord is property bit 0 or the
  vertex desc's TEXCOORD0 offset nibble (`desc & 0xF00`). Vc is property bit 8 or 9. Refractions is bit 7, Depth bit 2, Reflections
  `kUseReflections`, VertexAlphaDepth bit 9, Cubemap `kUseCubemapReflections`. The stencil pass is 6E (or 70 with hint 13) when the
  vertex desc has a colour offset, and it is cached at property +0xC0 the first time.
- **kUnderwater** is written by GetRenderPasses itself: it copies the global 528166 / 415111 into property bit 5 (and clears the
  passes when it changes). `TESWaterSystem::SetUnderwaterVariables` writes the global.
- **Where LOD water hangs.** 516171 / 402322 is `BGSTerrainManager`'s water node, named "WaterLOD", made beside "LandLOD" and
  "DistantRefLOD" and attached to "LODRoot" (a BSClearZNode, 516928). Vanilla LOD water is attached under it by
  `TESWaterSystem::AddLODWater` from `BGSTerrainChunk::Attach`; Unified Water's tiles go under it too. Vanilla LOD water has the
  property's LOD bit (0x2); Unified Water's tiles never do, which is how the census tells them apart.
- **Hooks that change bindings, not constants.** Physical Sky's ModifyWater only swaps sampler s13. The cubemap fix only unbinds
  t3 when it holds kREFLECTIONS in an interior. Light Limit Fix's water hook only writes PS b3 (its strict lights). None of them
  changes the six groups. `EvaluateBindingRewrites` gives the first two.

### 8.3 Not settled

- **LODRoot's parent** and the cells' water node: the census names them.
- **The displacement mesh.** Which object it is was not found. The census marks water system objects and shapes under
  `TESWaterSystem::waterRoot`; `defaultDisplacementWaterObject` (+0x128) is a candidate.
- **Who writes `unk8C`** (the stencil bit) and `plane` after attach, and who writes SunDir and SunColor (528168, 528172).
- **Light Limit Fix's PS b3** (the room index) is not ported here; DCLF's object record already has it (plan section 4).
- **Bit-exact matrices.** The port uses DirectXMath, which CS's D3DX9MathUpgrade puts behind the engine's D3DX imports. With
  `CS_NO_D3DX_UPGRADE=1` the engine runs the real D3DX and the matrices may differ in the last bit.
- **Linear Lighting's light colour** is computed in its own file, which is built with /fp:fast. The port computes the same
  expression with precise floats, so a reassociated product would show as a LightColor mismatch.
- **Frame scope.** The observer captures the frame values at the frame's first latched draw. A value that changes between the
  mask and the surface batch shows as a mismatch, which is the check that the frame row is a frame value.
- **FresnelRI.x (2026-11-21).** In game the latch holds 0 for every water draw and the port reads 0.1 from material +0x170.
  Ghidra does not explain it. SetupMaterial (AE 0x1414e168b, SE 0x1412f63d0) stores material +0x170 to the FresnelRI entry
  (table byte +0x44) and nothing later stores to that float. Linear Lighting's hook only writes table entries 1-3. The material
  is the pass property's +0x78 (RenderPassImmediately, AE 0x1414f3e8b, SE 100854), the same object the observer reads. Only two
  functions write +0x170: TESWaterForm to material (AE 0x140300e20, from AddWater; it copies the form's DNAM fresnel at +0xE8)
  and CopyMembers (0x1414ee24d). No CS hook writes it. CommonLib calls it `fresnelAmount`. So the engine should latch what the
  port reads. The next step is a runtime probe in Linear Lighting's WaterMaterialUpload, before its Unmap: log material +0x170
  and the mapped float at table[4]. If they agree with the port, the capture path (EnginePools' two copies, BufferContents)
  loses the value. If they show 0, something writes the material outside the code searched here.

## 9. Phase 1 notes

Written with phase 1's code (branch `dclf/water-p1`, 2026-11-21). Nothing was built or run when this was written. Ghidra reads are
SE 1.5.97 and AE 1.6.1170; ids are Address Library ids, SE / AE.

### 9.1 The code

- `DCLF/Features/Water/Scene/`: `WaterSceneFeature.cpp` (the switch, `ObjectClass::Water`, its roots), `WaterScenePort.cpp` (the
  material port), `WaterSceneCensus.cpp` (the census), `WaterSceneRows.cpp` (the water row family, the material pool, the frame
  row). `WaterScene.h` holds the constants and the GPU row layouts. `WaterFeature.cpp` calls `WaterScene::Install` before its
  graph-mode test, so the scene side does not need graph mode.
- The switch: `CS_DCLF_WATER`. 0 (default) registers nothing. 1 registers the class, the port, the census, the family and the
  pools, and claims nothing: the derive returns `Ineligible::WaterUnclaimed` for plain water and `WaterVariant` for the rest.
- Log lines: `[DCLF] water scene: '<shape>' under '<parent>' (reference '<ref>', geometry <ptr>): <placement>, flags ..., stencil
  bit ..., surface ... hint ..., stencil pass ..., <refusal>` once per shape (at most 1,024 a session), and at the report
  schedule `[DCLF] water scene: N shapes tracked: ... tiles, ... LOD, ... procedural, ... cell or static; wading ..., hint 13 ...,
  vertex colours ..., flowmap ...; plain ... (claimed from phase 2), refused: ...; verdicts now: ...; camera under water ...;
  material pool ... rows for ... members`. The schedule is 300 frames with any parity or diagnostic switch set, 18,000 without.

### 9.2 Roots and events

- **WaterLOD** is `TES::objLODWaterRoot` (TES +0x90). The TES constructor (SE 0x140150ca0) takes it from the global SE 516930,
  which holds the same node as `BGSTerrainManager`'s water node (516171 / 402322). It hangs under "LODRoot" (SE 516928), a
  `BSClearZNode`, beside "LandLOD" (`TES::lodLandRoot`) and "DistantRefLOD". SE 35555 builds all three.
  - Vanilla: `BGSTerrainChunk::Attach` (30934 / 31737) calls `TESWaterSystem::AddLODWater` (31404 / 32209) per water shape:
    `AddWater`, then `WaterLOD->AttachChild` of the chunk's 'Water' `BSMultiBoundNode`.
  - Unified Water: `BGSTerrainBlock_Attach` builds its tiles under the detached 'Water' node, runs `AddWater` and writes the
    water flags, then attaches the node under WaterLOD. `_Detach` detaches the tiles by index and the node from WaterLOD.
  - Each of these is `NiNode::AttachChild` or a detach, so SceneWriterFramework's child events cover them. No new hook.
- **The water system's root** is `TESWaterSystem::waterRoot` (+0x90), a plain `NiNode` that the constructor (31381 / 32172,
  SE 0x1404c5050) attaches under `TES::objRoot` ("ObjectLODRoot", SE 516929). `AddWater` (31388 / 32179) attaches
  `BGSAutoWater`'s procedural clones under it with `AttachChild`. It is not a `BSMultiBoundNode`, so Core's gather never took it.
- **Cell water and water in statics** hang under `objRoot`'s `BSMultiBoundNode` children (the census's
  `ObjectLODRoot -> '' BSMultiBoundNode -> '' NiNode -> 'Water1024'`). Core already gathers those as category nodes, so these
  shapes were tracked all along and refused as `not-lighting-shader`. With the port they take the port's verdict. No root needed.
- **No double tracking.** An entry is keyed by its geometry and filed under its nearest category node, so a shape is never
  tracked twice. The horse trough's Lighting shapes and its water shape are each one entry under the same multibound. WaterLOD
  and the water system's root are under no other category node.
- **AddWater attaches no shape** it is given. It writes the property's plane (+0xA0) and flag bit 0, then calls
  `BSShaderProperty::SetMaterial`, a property event, so the record is captured again after both writes.
- The roots come from a new `Api::ObjectClassContribution::categoryRoots`, a gather into `Api::CategoryRootSink`, read where
  `categoryRoot` is read: every frame in the category inputs, and in each signature read and category capture.

### 9.3 Classification

The derive runs `Port::DeriveRenderPasses` on the record's capture with a frame-free input (water on, no interior, camera above
water), so a verdict never depends on the camera.
- **Placement** (the census, from the mirror's parents): under WaterLOD with the LOD bit (0x2) is LOD; under WaterLOD without it
  is a tile (Unified Water never sets the bit); under the water system's root is procedural; anything else is cell or static.
- **Attributes**: wading (flag bit 0, the Wading descriptor bit), hint 13 (the surface pass's hint), vertex colours (stencil pass
  6E or 70), flowmap (0x8000), skinned. In the port, wading and hint 13 come from the same bit.
- **Refusals** (`WaterVariant`): skinned, no single water-type bit at +0x8C, wading or hint 13, or Light Limit Fix not loaded.
  Plain water is `WaterUnclaimed` until phase 2 returns `None` there.
- **Underwater** is the frame's: GetRenderPasses writes `kUnderwater` from the camera. So the capture clears bit 5 and the census
  reports the camera's state instead.
- **The key**: the surface descriptor (raw in `rawVertex`/`rawPixel`, CS's lookup in `vertex`/`pixel`), and the stencil index
  (1-7) in `LightingDescriptors::familyKeyBits`, which `KeyJoin` puts in the family's spare bits 21-23.
  `WaterScene::MaskStencilRefOfKey` reads it back. `kRasterWater` comes from the program family's registration, as for Effect.

### 9.4 Rows and registers

- **Water row**: table family "water rows", `WaterObjectRow` (48 bytes: the cell and flow offsets as the property holds them, the
  plane, the flags, the stencil bit, the surface descriptor, the generation). Record word 4 with the tag in 5
  (`DCLFTreeParams.xy`, which only a tree's shading block fills). The "water-rows" component writes it on residency.
  `Port::ObjectRow` is this row with the frame's flowmap values and Unified Water's rewrite from the world translate applied
  (`Port::EvaluateObject`), so the shader computes it from this row, the frame row and the placement row.
- **Material pool**: t58, `WaterMaterialRow` (320 bytes: VS b1 c0-c5 and PS b1 c0-c13 of `Port::EvaluateMaterial` with this
  frame's inputs, unwritten components 0). Row 0 is none. `ShadingRowField::WaterMaterial` (`BindlessShading::waterMaterial`,
  the old first padding word; `DCLFShadingRow.WaterMaterial` in HLSL) names each slot's row.
- **Frame row**: t59, one `WaterFrameRow` (18 float4 rows, `WaterScene::FrameRowEntry`): `Port::TechniqueRows` with the Depth
  bit, ProjData, the previous eye offset, the eye and timer, the flowmap and Unified Water values, the displacement offset, the
  water lighting, flags and the view-projection.
- The frame inputs are `Port::CaptureFrame` in the shading field's capture at the frame start. Both pools evaluate at the kick.
- `kFrameBindingSlots` grew from 8 to 10, and `kMaterialKinds` from 2 to 3 (water is kind 2).

### 9.5 Not settled

- **Frame inputs at the frame start.** The camera rows (posAdjust, the view-projection, the eye) may be the previous frame's
  there. The colour commit may be the right point; phase 0's parity observer can tell.
- **The local scale** (`ObjectUV.z` under Unified Water) is not in the row, because the capture sees only the property. Under
  WaterLOD the world scale is the local scale when the 'Water' node's scale is 1 (unverified).
- **The material record** holds `Port::EvaluateMaterial` with a neutral frame (no fog, unit water lighting), plus SetupMaterial's
  textures (SE 100604): t3 the material's +0x38 unless its +0x17D byte picks the global cube, t4-t6 +0x40, +0x48 and +0x50,
  t9 +0x58 with the Flowmap bit. The flow normals' fallback (a global default normal map) is not bound.
- **No cell category 6 root.** The census found no drawn water there. Interiors without a portal graph are unverified.
