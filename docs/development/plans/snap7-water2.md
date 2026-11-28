# snap7-water2: the river after a resize, and the water flicker

Lead `water2`, round `snap7`, branch `snap7/water2` (base `org_integration` 4bcea57a3), stage `s7-water2`. Dev-Fast throughout.
RTX 5080. Continues [snap7-water.md](snap7-water.md).

Status: **fixed.** Three separate causes. The river after a resize was a zeroed material row (no normal maps). The frame-to-frame
flicker had two causes: the water blend read the wrong normals target on every other snapshot, and Dynamic Cubemaps switched the
reflections cubemap to its fake capture on every repeat render.

## 1. Commits (CS, branch snap7/water2)

| Commit | What |
|---|---|
| b7417fb4d | fix(dyncubemaps): keep the reflection choice on repeats |
| 0d1fe7ce6 | fix(dclf): rewrite every pair's rows after a clear |
| 53a783eb2 | fix(imagespace): water blend parity from the snapshot |
| 697b888ea | test(cs-test): resize stress refs with the river |
| (this file) | docs: snap7-water2 notes |

No submodule change.

## 2. What the "holes" were

The previous round looked at the river through debug outputs (stripes, constant terms) and saw a pattern of missing pixels. With the
real shader the river after a resize is dark, with a ripple pattern and sparse bright dots. Probes on the surface's kRAW_WATER output
(`CS_ORG_SHOT_TARGET=88`), one input at a time:

- Inputs that stay right after a resize: SSR colour and alpha, the depth copy, skylighting, the engine reflection cube, Dynamic
  Cubemaps' environment cube, the water frame row (VPOSOffset), shared data flags.
- The reflection term collapses to the environment cube's level. The reason is the normal: after a resize it is about (-0.57, -0.57)
  with a ripple pattern, which is what a normal map that reads zero gives.
- `Normals01Tex` reads 0 and its `GetDimensions` gives width 0: the null descriptor. The raw material texture index
  (`DCLFMaterialTextureIndex(4)`) is 9147 before the resize and 0 after it, for the river's material.

Cause: new main resources (a resize) or new tables make `MainBuild::BeginRows` clear every material and pipeline row. A resident
region then resolved only the pairs its logs named, new pairs and pairs of moved pipelines. Pairs nothing named kept the cleared row.
The water's deferred-range pairs are such pairs, so their material row stayed zero and every texture index read 0.

Fix (0d1fe7ce6): `MainRows::clears` counts the clears, and a region whose pairs saw an older count resolves every pair once
(`EveryPairMoved`). After it the index stays valid at every size (run x-pairs, 7 shots over three resize cycles).

The culling lead from the previous round does not hold: the river luma in its `dclf.cull=frustum` run was as dark as the others
(58.7 against d3d11's 102). Decided by the agent, to confirm: the previous round's single "right" sample was read from a crop.

## 3. Flicker 1: Dynamic Cubemaps on repeat renders

`DynamicCubemaps::Reset` takes `activeReflections` from `State::activeReflections`, which the reflection faces set. A repeated render
draws no faces, so the flag was false on every repeat and the next capture of the reflections cubemap used the fake (no sky)
variant: a mostly black cube with sparse geometry texels. Measured (temporary counters): in the world, every frame without faces was a
repeat (85 of 300 frames). Water probes of the reflections cube on consecutive frames switched between about 190 and 25.

Fix (b7417fb4d): a repeat keeps the choice of its snapshot's first render. Guest has no repeats, so it is unchanged.

## 4. Flicker 2: the water blend's parity

The native water blend (`WorldSteps.inl`, `ISWaterBlend` variant and `WaterBlendStep`) picked its normals target and water history
from the live `gFrameCounter`, which the game thread advances. The DCLF water mask writes the normals target from the snapshot's frame
counter (`RenderScene`'s `normalsTarget`). On every other snapshot the blend read the normals target the mask did not write. It then
discarded every pixel, and the river was missing for that frame (dark, see-through). post-flicker saw the same pattern without a resize:
the first render of every other snapshot is dark.

Fix (53a783eb2): `EngineFrameCounter` takes the snapshot's counter when there is one, as `EngineNormalsParity` already does.

## 5. Evidence

River crop (x 0.78-0.92, y 0.06-0.16), 30 consecutive render-side shots at the start, after resizing to 1280x720, and after
resizing back (scratch configs `wflick-*.toml`). The "f2f" columns are the mean and max frame-to-frame luma difference.

| Run | start | small | back |
|---|---|---|---|
| d3d11 (y-flick-d3d11) | 3.70 / 12.4 | 2.73 / 6.5 | 2.98 / 10.7 |
| native, fixes 1 and 3 only (y-flick-native) | 4.14 / 12.3 | 12.74 / 23.4 | 9.58 / 42.0 |
| native, all three (z-flick-native2) | 4.71 / 14.5 | 4.43 / 13.7 | 4.12 / 16.0 |
| native, all three, AI off (fl3-native-vulkan) | 2.74 / 5.7 | 2.92 / 3.5 | 2.96 / 4.0 |
| guest, all three, AI off (fl3-guest) | 3.82 / 5.4 | 2.20 / 3.2 | 3.97 / 5.0 |

Riverwood (`native-vulkan+river-flicker`, post-flicker's region x 1400-1600, y 520-640): luma 79-83 on 30 consecutive frames,
frame-to-frame 4.9 / 6.2; along the river 2.0 / 5.8. post-flicker measured the same region switching between 58 and 83 before.

window-resize-stress-taa, river crop luma per shot (start, then small/large pairs): d3d11 82-87, native 80-89, guest 80-103. Guest's
resized shots are brighter overall (whole frame 72 against 65): exposure, which varies between runs. Guest's flicker probe after a
resize is at d3d11's level (83.8 and 85.6).

In y-flick-native the river was missing on single frames (luma 59-61 against 80). In z-flick-native2 there is no such frame. The
"back" burst there had a step from 82 to 68 that stayed: the save's guard hit the player (camera shake, blood), not water. The final
configs turn off AI (`tai`, `tcai`).

## 6. Runs (stage s7-water2)

Labels start `s7-water2-`. Native Vulkan unless named.

| Label | What | Result |
|---|---|---|
| a-prim | primitive-id colour in kRAW_WATER | no holes: the holes were not geometry |
| b-classify, c-magnitude | output value classes | no NaN, inf, negative or huge values |
| d-base | unmodified, kRAW_WATER | dark river after the resize; device lost after small0 (known) |
| e-terms | refraction, specular, opacity | specular collapses (one shot) |
| f-inputs | SSR alpha, depth | stable |
| g-cubes | DC cubes, engine cube | DC reflections cube 0 with speckles in one shot; device lost (known) |
| h-reset | DC capture reset on resize (experiment, reverted) | start bad too: not resize related |
| i-seq | 9 shots one frame apart | reflections cube flickers frame to frame |
| j-index..j-index6 | t30 descriptor index | stable index: content flicker (j-index..5: probe build errors, two scene stalls) |
| k-dims | t30 dimensions | stable |
| l-hold | fix 1 with counters | 7 good shots; repeats = frames without faces |
| m-raw | fix 1, real shader | the river still dark after every resize |
| n-sky, o-ssr, o-ssr2, p-terms, q-spec, r-shared, s-refl | inputs one by one | reflection term collapses; inputs fine (o-ssr lost the device, known) |
| t-normal | normal and R | normal wrong after a resize |
| u-nmap, v-dims, w-row | normal map sample, dimensions, raw material index | index 0 after the resize |
| x-pairs | fix 2 with the index probe | index valid in all 7 shots |
| y-flick-native, y-flick-d3d11 | flicker probes | section 5 |
| z-flick-native2 | all fixes | section 5 |
| fl3-native-vulkan, fl3-guest | flicker probes, AI off | section 5 |
| rs-native-vulkan, rs-guest | window-resize-stress-taa | passed; references accepted (697b888ea) |
| suite-native-river, suite-native-vulkan-taa-water, suite-guest-taa-water | water configs | passed |
| suite-native-vulkan-quick, suite-guest-quick | quick | passed |
| suite-native-vulkan-cadence-drop-cells | cadence | passed |
| suite-native-vulkan-river-flicker | Riverwood bursts | passed, steady (section 5) |

Not run: native-d3d12-quick (no ORG, BasicRHI or native device change). The ImageSpace parity fix also applies to native D3D12,
where post-flicker saw the same flicker; it is not measured there.

## 7. Files touched outside water

- `Core/Render/Payload/MainBuild.cpp/.h`, `MainRows.h` (DCLF's row build): one counter and one check in `EveryPairMoved`. snap7-devloss5
  works in DCLF's commit path; this does not touch it.
- `RenderGraph/Engine/ImageSpace/WorldSteps.inl`: `EngineFrameCounter` only.
- `Features/DynamicCubemaps.cpp`: `Reset` only.

## 8. Decisions to confirm

- SW2-a. A repeat render keeps Dynamic Cubemaps' reflection choice from its snapshot's first render. Decided by the agent: it follows
  the cadence rule (state stepped per snapshot), and guest has no repeats.
- SW2-b. A clear of DCLF's rows makes every resident pair resolve once. It costs one full pair pass per resize or table change.
- SW2-c. The water blend's parity comes from the snapshot's frame counter in native, the live one elsewhere. Water history does not
  advance on repeats, which is what the mask does too.
- SW2-d. The previous round's culling lead is dropped (section 2).

## 9. Open items

- Native still loses the device intermittently after resizes (d-base, g-cubes, o-ssr: `fault invalid read`), snap7-devloss5's
  problem. The resize stress run itself passed once.
- The ISWaterBlend history (kWATER_1/2) does not ping-pong on repeats. Not visible in the probes; a longer history test could check it.
- `CaptureFrame` in the water frame row still reads two engine globals (snap7-resizeui's open item), unchanged here.
