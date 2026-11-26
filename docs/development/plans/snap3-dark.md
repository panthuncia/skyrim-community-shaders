# snap3-dark: native world shots a quarter as bright since 3f5b50150

Lead: `dark`, branch `snap3/dark` (base `org_integration` 245ec15e5), stage `s7-dark`.

## Symptom

Native world shots were about a quarter as bright as guest from the merge 3f5b50150 on: mean luma of
`native-vulkan+quick`'s `start.png` about 18 against about 64 for guest and for each lead's own branch run. Interiors
(Dragonsreach) were as dark as exteriors, the sky too. Per region, the linear luminance was about 0.09 of guest's
everywhere, so it looked like exposure. It was not exposure or lighting: it was the permutation block.

## Bisect (native-vulkan+quick, one run each)

| Build | Start luma |
|---|---|
| 67397b196 (facegen + post + dclf) | 60.2 (good) |
| 45dea5e8c + snap3/depth (facegen + post + depth) | 17.7 (bad) |
| f672f9012 + snap3/depth (facegen + depth) | 63.8 (good) |

So the interaction was snap3-post with snap3-depth; facegen and dclf were not involved.

Experiments at head that did not help: clearing the dynamic-resolution lock where FaceGen::UpdatePending ran, or at
native RenderDepth's start; `Util::GetEyePosition` from the frame camera; SetCameraData(world) at RenderDepth; writing the
current camera (414580) in the masks; Volumetric Lighting off. Experiments that fixed it: the engine's RenderShadowMasks in
place of CS's, or `Engine::ApplyPendingState()` back in DCLF's depth capture inside native RenderDepth. The captured
frame textures were the same either way, so the flush itself mattered, not what it bound.

## Cause

`Hooks::BSGraphics_SetDirtyStates` (the engine's state flush) calls `State::Draw`, which uploads `permutationData` into
`permutationCB` and copies it to `permutationDataPrevious`. `State::Reset` sets `permutationDataPrevious` to all ones at
every frame end so the next flush uploads again. `SharedConstants::Sources` committed `permutationDataPrevious` ("the last
block permutationCB uploaded") for every segment.

Each branch alone still had one engine flush in the world frame before the world's passes:
- snap3-depth alone: Main::Draw's VL block (VolumetricLighting::Render's draws);
- snap3-post alone: the engine's RenderDepth (its draws) and DCLF's `ApplyPendingState` in the depth capture.

snap3-post jumps over the VL block and snap3-depth ports RenderDepth and the masks (and skips that `ApplyPendingState` in
native). Merged, nothing flushes until the UI, so every world segment committed the all-ones block. A logged commit in the
bad build showed it: world segments `extra 101 / ffffffff`, `ui 0 / ffffffff` (now / uploaded); the UI segment, after the
menu's engine draws, matched. With every ExtraShaderDescriptor bit set (IsReflections, IsSun, AdditiveLighting,
SuppressExternalEmittance, ...) and RenderToUI nonzero, the world passes shaded about a quarter as bright.

## Fix (`src/RenderGraph/SharedConstants.cpp`, `Sources`)

Native commits `state->permutationData` as it is at the commit, which is what an engine flush at that point would have
uploaded. Guest keeps `permutationDataPrevious` (what D3D11 has bound), so guest is unchanged. Commit ab6517413.

Decided by the agent, to confirm: the native branch reads `permutationData` directly rather than adding a CS-side upload
at each segment, since native has no D3D11 permutation buffer reader left. The per-draw fields (VS/PS descriptors of the
last engine shader setup) are meaningless to CS passes in both forms.

## Runs (stage `s7-dark`, Dev-Fast)

| Config | Label | Result | Luma |
|---|---|---|---|
| native-vulkan+quick (67397b196) | s7-dark-bis-67397 | passed | 60.2 |
| native-vulkan+quick (fg+post+depth) | s7-dark-bis-fgpost-depth | passed | 17.7 |
| native-vulkan+quick (fg+depth) | s7-dark-bis-fg-depth | passed | 63.8 |
| experiments A-G, logs | s7-dark-exp*, -ftlog-*, -permlog, -fix1-nvk-quick | passed | as above |
| native-vulkan+quick | s7-dark-fix-native-vulkan-quick | passed | 64.8 (guest 64.2) |
| guest+quick | s7-dark-fix-guest-quick | passed | 64.2 |
| native-vulkan+facegen | s7-dark-fix-native-vulkan-facegen | passed | racemenu 75.5 (good 79.6, bad 41.6) |
| native-vulkan+native-interior | s7-dark-fix-native-vulkan-native-interior | passed | 72.9-74.5 |
| native-vulkan+post-effects | s7-dark-fix-native-vulkan-post-effects | passed | 60.8-66.9 (good 60-65, bad 18-19) |
| native-d3d12-quick | s7-dark-fix-native-d3d12-quick | passed | 63.7 |
| native-vulkan+cadence-drop-cells | s7-dark-fix-native-vulkan-cadence-drop-cells | passed | whiterun 73.0 (bad 23-31), dragonsreach 23.8 |

Detector numbers are not affected: no engine call or engine read is added or removed.

## Open items

- `cadence-drop-cells`' riverwood shot is fully black (luma 0.0). It is not this regression: it has been black since about
  16:30 in other leads' runs and the director's (`dir-s7ti`, `dir-s7fp`, `dir-s7sp`), and stays black with this fix.
- Any other CS state that only `State::Draw` refreshes at an engine flush is stale in native's world frame in the same way;
  the permutation block was the only one the shared constants read. `Util::GetEyePosition` and DCLF's ProjectedUV projection
  still read the engine's posAdjust at record time (did not affect brightness).
- cs-test still passes wrong shots (decision 176); a reference compare in the quick configs would have caught this.
