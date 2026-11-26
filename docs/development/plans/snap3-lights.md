# snap3-lights: stealth light membership, animated-root visibility, shadow light selection

Lead: `lights`, branch `snap3/lights` (base `org_integration` 7800570fb). Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 10, 12, 13, 33-40 of
[render-thread-decisions.md](render-thread-decisions.md); designs from [snap1-sidefx.md](snap1-sidefx.md). Native only; guest
installs nothing new.

## 1. S12/S13: stealth light membership (built)

`src/Snapshot/StealthLights.{h,cpp}`, installed by `SideEffects::InstallHooks`. `AIProcess::CalculateLightValue` (SE 38900,
AE 39946) gets four call-site thunks (each checks the engine's call first, fatal on a mismatch):

| Site (SE / AE offset) | Engine call | CS |
|---|---|---|
| +0x123 / +0x123 | `Actor::CalculateLOSLocation` (36755 / 37771) | passes through; keeps the actor for the NPC sum |
| +0x161 / +0x16B | the mask loop's light pick (41661 / 42744) over the engine's shadow list | returns true without a pick (the mask is unused now) |
| +0x1C9 / +0x1D3 | player sum `ShadowSceneNode::GetLuminanceAtPoint` (99725 / 106362) | sums over CS's membership |
| +0x247 / +0x25C | NPC sum over the skin property's light list (100989 / 107776) | sums over CS's membership |

**Membership** (what `ProcessQueuedLights`, SE 99701 / AE 106335, would leave, read under `lightQueueLock`, not applied):
active and shadow lists; each queued add joins the list `IsShadowLight` picks unless a light with the same NiLight is there
(the engine then keeps the old entry); each queued remove leaves its list by pointer; unless `disableLightUpdate`, active
lights whose NiLight is null or has refcount 1 drop out (the engine's orphan sweep, active list only).

**Player sum**: active lights, then shadow lights that pass the engine's own pick (42744) from the LOS point, bit per index as
the engine's mask (32 at most), luminance through the engine's `BSLight::GetLuminance` (101303 / 108292), ambient and sun
terms from the sun light's NiLight (x 1/3, the constant at SE 247774 / AE 198129).

**NPC sum**: the third-person geometry carrying `MiddleHighProcessData::lightingProperty`; each active light whose NiLight is
not hidden, has fade above 0.05 (the attach walk's entry test, AE 0x1414a2530) and whose radius sphere reaches
the geometry's world bound. Shadow lights are never in geometry light lists (first parity run: 22 of 22 NPC sums in
Dragonsreach differed while shadow lights were counted; 0 after). No portal test.

Ghidra (AE): ProcessQueuedLights' order is adds, removes, then the orphan sweep; removal (0x1414a1370) is by pointer and also
nulls `shadowLightsAccum` entries; luminance reads only NiLight data (and writes `BSLight+0x10`).

**Parity** (temporary scaffolding, removed): every player and NPC call also ran the engine's sum over its own lists; at
every update CS's membership before the render half was compared with the engine's lists after it.

| Run | Player sums | NPC sums | Membership checks |
|---|---|---|---|
| `s3-lights-st1` (stealth-lights, normal) | 30 logged, 0 differ (0.0 at the start, 2.1 with the torch, 2.8 after the drop) | 6, 0 differ | 1,800, 0 differ (adds of +1, +2 seen) |
| `s3-lights-st1-drop` (drop:3) | 0 differ | 0 differ | 0 differ; adds of +2, +3 queued across skipped updates matched |
| `s3-lights-s7sw-interior` (Dragonsreach, coc out) | 22, 0 differ | 38, 0 differ (after the shadow-light fix) | 1,800, 0 differ |

In drop mode the engine applies the queues only on rendered updates; CS's membership includes them at every call, so the
light level no longer lags. The final build counts calls that saw queued lights (`with queued lights counted early` in the
side effects line). Removal through the queue was not exercised by the torch steps (taking the torches did not dequeue the
held light); cell changes in `cadence-drop-cells` and `native-interior` queue every active light for removal and matched.

## 2. S7/S8: animated roots DCLF has no verdict for (S7 built, S8 open)

`SideEffects.cpp`: `DecideAnimatedVisibility` (AfterCapture) gathers the roots the animation jobs test with AE 19384
(SE 18972): `UpdateAnimationJob`, `UpdateManagedNodesJob` and `UpdateAnimatedRefsJob` walk the attached cells'
`LOADED_CELL_DATA +0x40` handles and test `Get3D()`. Roots with a DCLF verdict (`Extensions::ListedVerdictOf`) are skipped:
DCLF writes those at Main::Update's entry, as diffs, so a CS write would stick. The rest go through the same SIMD sphere cull
as S6 (`MainPlanes`/`CullSpheres`, now shared with the pre-cull) against the snapshot's main camera, and
`WriteAnimatedVisibility` (AfterRenderHalf, before the stage signals) sets or clears bit 26, so CS's value is the last write
before the next update's jobs, on skipped updates too.

Parity (observe build, writes off; `s3-lights-s7obs-interior`): about 500-800 animated roots an update, 94-97% DCLF's.
Of the rest, river/Whiterun exteriors and Dragonsreach: 18,584 + 5,239 compared, **0 where the engine had a root in and CS
out**; 1,980 + 1,779 where CS had it in and the engine out (the engine's portal and occlusion culls, which the frustum test
does not repeat, as D-c accepted for S6); 0 roots mid-fade. Effect: those roots animate as if visible (cost, no visual change).

**S8 (fades) not built.** For these roots the engine's main cull still steps the fade on rendered updates
(`BSFadeNode::OnVisible`, AE 105485, which calls the step AE 105486 and stamps +0x13C with the fade frame counter AE 390886),
and AE 19384 steps unflagged roots itself. No root was mid-fade in the parity runs. Producing it needs either the engine's
OnVisible step suppressed for CS's roots (a detour that skips the step but keeps the child walk) or the roots listed in DCLF's
tables with a "listed only" bit (sidefx's design, step 3). In drop mode their fades step only on rendered updates.

## 3. Shadow caster light selection in native (investigated, not built)

Baseline in native (`s3-lights-s7sw-interior`, `CS_SNAPSHOT_DETECT=1`): 57 engine render calls per world frame. The shadow
ones all run every world frame: `Main::CalculateAndDrawShadowCasterLights` (AE 107132) with AE 106367,
`BSShadowDirectionalLight::Select`, 106365, 108492, `Main::AfterFullFrustum`, `CalculateActiveShadowCasterLights`, the
accumulation `JobList::Finish`, six cascade calls each of 76815/76816/76819/16341, then `Main::RenderShadowMaps` and
`Main::RenderShadowMasks`.

The new log line in `ShadowMapsNative.cpp` counts each light's Render outcome per 600 shadow frames: Whiterun exterior
1,872 native, 749 Renders outside `RenderShadowMaps`, 29 with views not DCLF's (after a load); Dragonsreach 2,400 native,
none to the engine; Whiterun again 1,086 native, 119 outside. So inside `RenderShadowMaps` every covered light is native; the
engine fallback left is the Render calls from elsewhere (UI3D's scene and the frames right after a load).

What 107132 does (AE decompile): resets `shadowLightsAccum` (106367), sets up the sun's cascades (vfunc 0x80, 106365 /
`Select`, AE 0x141511f30 with the main list process), adds the sun to the accumulate array and mask (`DAT_14338c90c`),
signals stage 5, runs AE 0x1414a0840 with the main cull process, queues
`FirstListAccumulationJob`/`ListAccumulationJob` per culling context on `gJobList_SceneListAccumCulling`, and runs
`CalculateActiveShadowCasterLights` (local light selection) while they run. DCLF's `ShadowViews` and `PassCapture` read the
accumulators and cameras it fills. Replacing it is the "culling and scene lists from the scene graph" item, not a
self-contained port.

Bookkeeping readers (Ghidra xrefs, AE): the free mask (391715) is read and written only by `BSShadowLight` functions;
`shadowLightsAccum` reset (106367) is called only by 107132, `ResetCalculatedShadowCasterLights` and `UI3DSceneManager::Render`;
`State::SetCameraData` has only renderer callers (S25a). **No game code reads any of it**, confirming S25 "goes away":
nothing needs producing on the game thread when the engine stops.

Design for the port (next lead): (1) sun cascades from the snapshot camera and sun direction with DCLF's
`ShadowCameras` (the cascade split and stabilisation of 0x141511f30); (2) local shadow light selection from CS's light
membership (item 1), as `CalculateActiveShadowCasterLights` ranks them (distance, `fShadowDistance`, the shadow light
budget); (3) caster lists from DCLF's own cull per view instead of the accumulation jobs; (4) skip 107132 and the Render
calls in native, keeping stage 5 (already CS's, S1). Observe-then-switch: log CS's selected lights and cascade cameras next to
the engine's each frame first.

## Runs

Stage `s3-lights` (Dev-Fast). See the table at the end for the final build.

| Config | Label | Result |
|---|---|---|
| native-vulkan+stealth-lights | `s3-lights-st1` | passed; parity above |
| native-vulkan+stealth-lights, drop:3 | `s3-lights-st1-drop` | passed; parity above |
| native-interior (S7 observe) | `s3-lights-s7obs-interior` | passed; S7 parity, NPC shadow-light finding |
| native-interior (S7 switched, detector) | `s3-lights-s7sw-interior` | passed; 0 NPC differences; engine render baseline |

## Decisions (agent, to confirm)

- **L-a.** The light level's membership is rebuilt at each call, not cached per update: calls are every ~0.5 s for the
  player and per actor for NPCs, and a fresh read is exact for drop mode.
- **L-b.** NPC light lists are modelled as radius-against-skin-bound with no portal test. A light in another room within
  range now counts for an NPC. Parity showed no difference in the scenes run.
- **L-c.** The engine's mask loop keeps running over the engine's list but its picks are thunked to "true" (no raycasts);
  the engine's list is still read for the count. Phase 7 can remove the loop with the rest of the engine lists.
- **L-d.** S7 covers the cell animated references only; roots with a DCLF verdict stay DCLF's. Frustum only (no portal or
  occluder test), the same trade as D-c.
- **L-e.** S8 for these roots, and item 3, are left to the next phase 3 step (above).

Final build (68d3709b5, scaffolding removed), all **passed**:

| Config | Label |
|---|---|
| guest+quick | `s3-lights-f-guest-quick` (no side-effect line: nothing installs in guest) |
| native-vulkan+quick, `CS_SNAPSHOT_DETECT=1` | `s3-lights-f-nvk-quick` (74 engine render calls per world frame at the river save) |
| native-d3d12-quick | `s3-lights-f-d3d12-quick` |
| native-vulkan+cadence-drop | `s3-lights-f-drop` |
| native-vulkan+cadence-drop-cells | `s3-lights-f-drop-cells` (845-1,137 animated roots an update outdoors, 73% DCLF's; 54-131 of the rest in view) |
| native-vulkan+shadows-blend | `s3-lights-f-shadows-blend` |
| native-vulkan+llf-local-shadows | `s3-lights-f-llf` |
| native-interior | `s3-lights-f-interior` |
| native-vulkan+stealth-lights | `s3-lights-f-stealth` |
| native-vulkan+stealth-lights, drop:10 | `s3-lights-f-stealth-drop10` (light level 1.8 to 2.9 with the torches, as in the normal run) |
