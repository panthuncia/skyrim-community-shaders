# snap7-water: the river after a resize

Lead `water`, round `snap7`, branch `snap7/water` (base `org_integration` fd0dfd703, with snap7-resizeui and post-fades
merged), stage `s7-water`. Dev-Fast throughout. RTX 5080.

Status: **not fixed. The cause is narrowed further, and an earlier lead is ruled out.** No code commit; this file is the only
change. Native's resize references were not re-accepted and guest's were not added, since the river is still wrong.

## 1. What the defect is

After a resize the river surface leaves holes: about one pixel in ten in the river is not written. The holes form a fixed
screen pattern of short black dashes. They are the same at the small size and after resizing back. At a distance they read as
"no water" at 1280x720 and as "dark water with a ripple pattern" at 1920x1080. It is one defect, not two.

Seen directly: with `CS_ORG_SHOT_TARGET=88` the shot is kRAW_WATER, which only the surface writes (the water blend reads it).
Before the resize the river in kRAW_WATER is solid. After it, the river has the holes, black because the new kRAW_WATER version
is born zero (runs n to t). Guest shows the same holes (run s), so it is shared code, not native's target placement.

## 2. Ruled out (each with a temporary debug build, all reverted)

| Tried | Result | Run |
|---|---|---|
| Clear the stencil to 0 at the surface pass's begin (GPU-driven deferred range 1) | holes stay | b |
| Surface stencil test off (`SurfaceState`, mode 0) | holes stay | o |
| Surface depth test off (depth mode 0) | holes stay; the water draws over the terrain without holes, the river part keeps them | k |
| NaN or infinity in the surface's output (marked magenta) | none; the holes stay black | m |
| Water blend off (the surface draws into kMAIN, no ISWaterBlend) | holes stay | e |
| Clear kWATER_1/2 and kRAW_WATER every frame | invalid test (it also wipes the surface's output); the blend runs over the river | c |
| Only the surface's first bucket, or without its third | the river is in bucket 2 alone, holes stay | p, q |
| Output a constant per pixel (opacity terms) | holes stay at the same pixels | r |
| Engine reflection cube | intact after the resize (sampled at fixed directions) | h |
| Normals, view direction, reflection vector | intact; the holes overlay every stripe alike | i, l |
| Dynamic Cubemaps off | not testable in native: a menu-scene fatal (no Utility VS for LoadScreenTrollCave) | f, f2 |

So the snap7-resizeui lead (stencil bit 0 in kMAIN over the river, `i-nostencil`) does not hold: with the stencil test off or
cleared the holes remain. Decided by the agent, to confirm: resizeui's `i-nostencil` judged the water from a downscaled shot,
where the holes average into "water present".

What is left: the holes do not depend on depth, stencil, the shader's values or the blend. The surface draw itself does not
write those pixels after a resize, in both modes. Candidates, in order:
1. **Culling.** With `dclf.cull=frustum` (no occlusion) the shot after resizing back was correct once (run d); the small shot
   was still wrong. The cull's HZB inputs are per main `Resources` (`Build.cpp`: `resources->hzb`, `hzbWidth/Height`;
   `Uploads.cpp`: `hzbUvScalePacked`), and the deferred range passes keep the `Resources` they were made with
   (`DeferredRanges.cpp`, `DeferredRangePass::resources`). If the river is many small water objects, a stale or mismatched
   HZB or object row would drop single far objects, which at this distance are pixel-sized dashes. One sample only: repeat it.
2. **Per-object data.** The holes look like small objects missing or drawn with bad rows. The water's object rows (t42) and
   the per-render frame row (t59, made per render by snap7-resizeui) are worth checking right after a resize.
3. An attachment the pass binds with another size or version than the colour target (render area from the colour commit's
   shape, `frame.width/height`).

Next step: count the surface range's drawn objects before and after a resize (the class counts or `CS_DCLF_STATS`), and repeat
the frustum run for both sizes. A primitive-id colour output would show missing objects directly; `SV_PrimitiveID` in
`main`'s parameters does not compile in Water.hlsl's DCLF path (run t), it needs to go into the PS input struct.

## 3. Facts found on the way

- Native exteriors draw the water mask and surface with DCLF's GPU-driven deferred ranges (`cs.dclf.deferred-range0/1`), the
  surface into kRAW_WATER (set 1). The water family's engine-draw path drew 124 draws per view only before the world settled.
- The mask's buckets all have stencil reference 255 here (no water-type bit), so the mask writes no stencil.
- A resize makes new versions of kRAW_WATER, kWATER_1/2, kREFRACTION_NORMALS and kUNDERWATER_MASK; the reflection cube keeps
  its version.

## 4. Runs (stage s7-water, scratch config `wdbg-native.toml`: start, one resize to 1280x720, back to 1920x1080)

Labels start s7-water-. All native Vulkan unless named; all passed unless named.

| Label | What |
|---|---|
| a-clearlog, b-clearlog2 | logging; stencil clear before the surface |
| c-blendclear | water targets cleared every frame |
| d-frustum | `dclf.cull=frustum` |
| e-noblend | water blend off |
| f-nodc, f-nodc2 | Dynamic Cubemaps off: hang (menu-scene fatal) |
| g-stripes, h-cubedir, i-normals | reflection inputs as stripes |
| j-enginelog | engine-path water draws logged |
| k-nodepth | surface depth test off |
| l-depthviz, m-nan, r-terms | depth, NaN marker, opacity terms |
| n-rawshot, o-nostencil | kRAW_WATER shots; with the stencil test off |
| p-call0, q-skip2 | surface buckets bisected |
| s-guest | guest, kRAW_WATER shots: the same holes; lost the device at the second resize (known) |
| t-prim | primitive-id output: shader failed to compile, no shots |

Not run: window-resize-stress-taa, native-river, taa-water, native-vulkan+quick, guest+quick. No code changed, so there is
nothing for them to check; the references stay as snap7-resizeui accepted them (native small shots with the river wrong).

## 5. Decisions to confirm

- SW-a. The stencil lead is dropped (section 2).
- SW-b. References are not re-accepted until the holes are fixed.
