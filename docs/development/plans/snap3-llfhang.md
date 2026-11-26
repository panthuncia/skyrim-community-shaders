# Snapshot roadmap: the resize hang (lead `llfhang`)

Branch `snap3/llfhang` (base `org_integration` e2dff8585), stage `s3-llfhang`. Follows `snap3-world.md` section 3 and decisions 94
and 106.

## Cause

Light Limit Fix's cluster grid reached the frame two ways, and they disagreed on the first frame of a revision.

- The culling (`ORGLightCulling`) writes the light grid with the adopted revision's grid (`AdoptedClusterGrid`). LLF's Prepass sets
  `clusterSize` from it and `SetFrame` latches it.
- The readers (DCLF's colour passes, CS stages, views) take the grid from the feature data block
  (`SharedData::lightLimitFixSettings.ClusterSize`), which `LightLimitFix::GetCommonBufferData` packed from the `clusterSize`
  member. `State::UpdateSharedData` packs it in `EarlyPrepasses`, before LLF's Prepass. On a revision's first frame the member
  still held the previous revision's grid.

So on the frame that adopts new LightGrid/LightIndexList versions, the culling wrote the fresh versions with the new layout and the
readers indexed them with the old one. A fresh version holds no data (its memory is reused placed memory). Shrinking with an
upscaler (FSR: run grid 14x8x32 inside a 20x12x32 capacity) the readers' old 20x12x32 indices land on entries the culling never
wrote, so `lightCount` is garbage and Lighting's unclamped light loop runs until the TDR. With LLF off there is no loop, so the
stress passed. Clamping the count would have hidden it; it was not done.

## Proof

A temporary trace (not committed) logged, for 24 frames after each grid version change, the culling commit's latched grid and the
grid and t35-t37 indices DCLF's main colour commit packed (`s3-llfhang-trace1`, FSR, device lost at the third resize):

```
frame 1166 cull commit: grid 14x8x32 capacity 20x12x32; LightGrid version 4 (7680 elements)
frame 1166 dclf main colour: reads grid 20x12x32, cull committed 14x8x32 at frame 1166; indices [175, 7879, 7877] current [175, 7879, 7877] <- GRID DIFFERS
frame 1167 dclf main colour: reads grid 14x8x32 ...            (agrees from the next frame)
(device lost; hang detected 2.2 s later)
```

The growing resize showed the same one-frame mismatch (frame 1053: reads 14x8x32, culling 20x12x32), harmless there because the
reader's indices all fall on written entries. The bindless indices always matched the current versions, so the stale-latch
hypothesis is disproved: the index was right, the layout was wrong.

## Fix

`LightLimitFix::GetCommonBufferData` packs `AdoptedClusterGrid` when the graph culling is active (the member off the graph). The
adoption happens at the frame's Begin, before any packing, so every reader of the frame and the culling use one grid. One function,
in LLF; no DCLF, ORG or BasicRHI change.

With the trace still in, `s3-llfhang-fix1` passed twelve resizes with no mismatch in 288 traced commits.

## Runs (stage `s3-llfhang`, Dev-Fast)

All on `44489435f` (the fix, no trace) unless named. `wr-stress-taa` is `window-resize-stress` with an `upscale taa` step first
(scratch config; snap3-world's `native-vulkan+wr-stress-taa`).

| Config | Label | Result |
|---|---|---|
| native-vulkan+window-resize-stress, trace, base | `s3-llfhang-trace1` | device lost at the 3rd resize; the grid mismatch above |
| native-vulkan+window-resize-stress, trace, fix | `s3-llfhang-fix1` | passed, 12 resizes, no mismatch |
| native-vulkan+window-resize-stress x5 | `s3-llfhang-v-wrs1..5` | all passed (5 in a row) |
| native-d3d12+window-resize-stress x2 | `s3-llfhang-d-wrs1..2` | both passed |
| native-d3d12+wr-stress-taa x2 | `s3-llfhang-d-wrs-taa1..2` | both passed |
| native-vulkan+wr-stress-taa x2 | `s3-llfhang-v-wrs-taa1..2` | device lost at the first grow (separate defect, below) |
| native-vulkan+wr-stress-taa, validation | `s3-llfhang-v-taa-val1` | device lost; no validation error before it |
| native-vulkan+wr-stress-taa, LLF off | `s3-llfhang-v-taa-nollf1` | device lost the same way: not LLF |
| native-vulkan+window-resize | `s3-llfhang-v-wr` | passed |
| native-vulkan+resize-drag | `s3-llfhang-v-drag` | passed |
| native-vulkan+hdr-resize | `s3-llfhang-v-hdr-resize` | passed |
| native-vulkan+llf-local-shadows | `s3-llfhang-v-llf-ls` | passed |
| native-vulkan+llf-slots-live | `s3-llfhang-v-llf-slots` | passed |
| guest+quick | `s3-llfhang-g-quick` | passed |
| native-vulkan+quick | `s3-llfhang-v-quick` | passed |
| native-d3d12-quick | `s3-llfhang-d-quick` | passed |
| wr-stress-taa on viewport experiments | `s3-llfhang-v-taa-mask1`, `-vp1..4` | device lost (see below) |

## A second defect: TAA, native Vulkan, growing the window

Not LLF (it reproduces with LLF off), and not a TDR: the device is lost 10-30 ms after the swap chain is resized, before any revision
adopts. With FSR the stress passes. D3D12 passes with TAA.

Cause, as far as followed: native families latch the engine's live viewport (`Family::LatchViewport`, `RendererShadowState::viewPort`),
and the native shadow mask also its live `kSHADOW_MASK` size. The engine takes the new size at the resize at once, while the frame
draws into the adopted target versions until the revision that brings the new ones. With TAA the render size is the display size, so
growing from 1280x720 draws a 1920x1080 viewport into 1280x720 targets in shared placed memory. With FSR the new render size
(1280x720) still fits the old targets, which is why only TAA shows it. The crash markers move with each fix:
`cs.shadow-mask.world.draws` (base), then `cs.sky.main.draws` (mask fixed), then `cs.dclf.z.depth` (latched viewports mapped).

The partial fix is parked on branch `snap3/llfhang-viewport` (`7d03d864f`, not for merge): in the resize window it maps a latched
viewport from the bound target's written shape (`Vars::Written(EngineTargetShapes)`) onto its adopted shape, and takes the mask's texel
scale from the adopted shape. Outside the window nothing changes.

DCLF's main shapes are the remaining overrun: `RunEpoch` puts the engine's live viewport into the revision inputs. Two attempts were
wrong and reverted: mapping it by the captured engine target's size (the engine's viewport and targets change on different frames,
so it faulted at the first shrink), and sizing it from DCLF's main resources times the ratios (its passes draw into the graph's
adopted engine targets, not into a size of DCLF's own). The right size is the display size of the revision whose versions the
fragment is recorded against (`RevisionInputs::frameVersions`), which the producer does not have yet. Decided by the agent, to
confirm: left open as its own item, since it is a native viewport ownership question (snapshot phases: the frame, not the engine,
owns the viewport), not part of LLF's hang.

## Decided by the agent, to confirm

- Guest packs the adopted grid too (its culling is the same graph culling). It differs from before only on a revision's first frame,
  where the old value was the bug.
- No permanent reader-versus-culling check was added: the readers' grid now has a single source, so there is nothing left to
  compare.

## Open items

- The TAA resize defect above (`snap3/llfhang-viewport` holds the partial fix and the plan).
- FSR's view destruction on resize (`VUID-vkDestroyImageView-imageView-01026`, decision 94) is a separate defect, not touched.
