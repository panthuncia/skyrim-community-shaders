# snap3-culls: the main camera's list culls stopped in native (phase 3, milestone 3)

Lead: `culls`, branch `snap3/culls` (base `org_integration` ee0e55a10), stage `s3-culls`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 12-13, 33-48, 53, 66-70, 87-90 of
[render-thread-decisions.md](render-thread-decisions.md); starting point `snap3-lists.md` "Not done".

## What was done

| Piece | State | Commit |
|---|---|---|
| S8 for the decision 46 roots (observe with parity, then switch) | done | 9276c24ce |
| Main list culls not run in native (BuildSceneLists, the accumulation jobs, FirstPersonCulling, the list clears) | done | 9276c24ce |
| Mixed entries stood in (native) | done | 9276c24ce |
| Decision 53 crash, cadence-drop-cells hang | gone in the runs below | |
| Main::RenderWorld replaced in native | not done: inventory and plan below | |
| Item 5 (precipitation, decal occlusion, VL vanilla, cube cameras) | not started | |

### What the list culls wrote, and who produces it now

The engine's main list cull (`BSCullingProcess::Process1` over the scene lists, plus each node's `OnVisible`) and its list build
write these game-visible values (skyrim-engine-notes.md, "Writer/reader table" and "T7f item 8"):

| Written by the list culls | Game reader | Native producer |
|---|---|---|
| `kAccumulated` (bit 26) of DCLF-listed roots | AE 19384 (animation gate), the tree wind clock | DCLF's readback verdicts at Main::Update's entry (already, for every listed root) |
| `kAccumulated` of the attached cells' animated roots DCLF has no verdict for | AE 19384 | S7 (snap3-lights) |
| Fade state of DCLF-listed fade roots (including mixed and not-yet-admitted entries, actors) | AE 19384, the actor pre-cull | FadeStateCS and the write-back ("DCLF owns and writes every listed root from the moment it is listed") |
| Fade state of the decision 46 roots | AE 19384 (`currentFade > 0`) | **S8 (new)** |
| The portal map's shared nodes (BuildSceneLists' `AddVisibilityToMap`) | `Main::IsRoomVisible`'s three callers | DCLF's RoomVisibility (already answers all three calls from PortalViews) |
| Cell roots' `kAccumulated` (BuildSceneLists) | the engine's light receivers only | none needed: render only, native does not use the engine's geometry light lists |
| `OnVisible` of switch, billboard, ordered, BGSDecalNode, multibound, room and portal nodes, geometry appends (particles included) | none (the registration, the billboard turn, the alpha group, the tree manager's multibound cache for trees DCLF already stands in for) | none: render only (decision 8) |
| FirstPersonCulling: the first-person root's fade, `kAccumulated` and the first-person process list | the first-person accumulator's registration only | none: DCLF draws first person from its tables and lists its fade root |

Unlisted fade roots that are neither animated references nor DCLF's (no game reader besides the renderer) keep their last fade.
Decided by the agent, to confirm (CU-c below).

### S8 (`Snapshot/SideEffects.cpp`)

`StepFade` is the main cull's work on a fade root in view without the children: BSLeafAnimNode's and BSTreeNode's LOD step (AE
105588/105604: FUN_14147b110 and FUN_14147a430, stamp), BSFadeNode::OnVisible's fade part (the LOD type 6 step, otherwise the fade
state machine SE 98849 / AE 105486, then the +0x13C stamp with the fade frame counter), and the tree LOD fix-up. The main list
processes have `cameraRelatedUpdates` set and the tree height test off. The camera is the world camera's position and LOD adjust,
as AE 19384 reads it. It runs in `WriteAnimatedVisibility` (AfterRenderHalf, once per game update, rendered or skipped; decision
39) for every S7 root in view whose node is a fade plan and is not written by DCLF (`FadeWrittenByDclf`).

Parity (temporary scaffolding, removed): before the render half each root was copied and stepped on the copy; after it the copy was
compared with the node wherever the engine's OnVisible had stamped it this update (`s3-culls-s8par1`, native-vulkan+cadence-drop-cells):
**8,746 engine-stepped roots compared, 0 differ** (fade, flags 14/15/27, +0x140, LOD blend and levels). 32,722 roots in S7's
frustum were culled by the engine's portal or occluder test (most in Dragonsreach), the trade decision 46 accepted.

### The list culls stopped

- `SideEffects::SkipListCulls` (native install, bytes checked, fatal on a mismatch): Main::Draw (SE 35560 / AE 36559) jumps over the
  BuildSceneLists lock, QueueJob, unlock and Begin (+0x12A to +0x16F), its Finish (+0x177 to +0x183), FirstPersonCulling with its
  argument setup (+0x1EB to +0x1F7), the ClearLists job list's creation, jobs and Begin (+0x3CB to +0x519, `xor r15d, r15d` kept: the
  block zeroes the function's zero register) and the ClearLists Finish (SE +0x836 / AE +0x846). The offsets were read from both
  programs in Ghidra; only the last differs.
- `NativeCasters::SelectCasters` queues no list accumulation jobs, makes no portal records or culling contexts. It still calls 107132's
  call after the full-frustum cull through its call target (DCLF's PrimaryCull prepares its frame there; the engine function sets culling
  globals), then `PrimaryCull::NoListJobs` (the observers' beforeListJobsWait and listJobsDone, the cut's AfterListJobs).
- `PrimaryCull::mixedStoodIn` (native): a mixed entry is stood in like a whole one. Its engine-drawn parts were already not drawn in
  native (no registration since snap3-lists), and its fade and `kAccumulated` were already DCLF's, so this only moves DCLF's own
  bookkeeping (the FadeStateCS stood-in bit, the stand-in root set). No game-visible value changes, so no parity run.

### Detector (native Vulkan quick, river save, per world frame)

| Site | Before (`s3-lists-f-nvk-quick`) | After (`s3-culls-off1-nvk`) |
|---|---|---|
| `JobList::Finish #1` (BuildSceneLists, in RenderPlayerView) | 1.01 | 0 |
| `JobList::Finish #2` (ClearLists, in RenderPlayerView) | 1.01 | 0 |
| `Main::FirstPersonCulling` | 1.01 | 0 |
| `JobList::Finish` (list jobs, in CalculateAndDrawShadowCasterLights) | 1.01 | 0 |
| `Main::AfterFullFrustum` (helper) | 1.01 | 1.01 (CS calls through the site) |
| engine render calls | 66.4 | 63.0 (pre/post resolve vary 1.65-1.75 a frame) |
| live engine reads at exit | 49 | 53 |

`BSCullingProcess::Process` left in a native world frame: one, the screen splatter's cull in Main::RenderWorld.

## Not done: Main::RenderWorld in native

RenderWorld (AE 107142, SE 100424) accounts for about 28 of the 63 calls: its own 9 sites (StartAccumulating, RenderSSAO x2,
DispatchCopySubRegion, FinishAccumulatingPostResolveDepth, RenderFirstPersonViewEnd, the screen splatter's
`BSCullingProcess::Process` and `SubmitAccumulator`, `Sun::SubmitOcclusionQuery`) and the pre/post resolve's batch calls under it
(RenderBatches, RenderGeometryGroup, the decal batches, RenderReflections, WaterBlend). What a CS replacement has to drive, in call order,
from CS's frame:

1. `Sun::UpdateOcclusionQueries` (game-visible: the sun glare reads the query ring CS fills, SkyHooks).
2. The pre-resolve's CS points: Deferred's opaque start (DCLF BeforeOpaquePass), the sky batch (SkyNative), the opaque decals, the
   `opaque.after` trigger (Deferred's BlendedDecals hook: DCLF AfterOpaquePass, terrain blending), the water mask batch trigger, the
   Skeleton's clears and the FinishAccumulating depth copy (106583 +0x39D), RenderReflections.
3. RenderSSAO (native in ImageSpace), the kMAIN copy or the snow SSS pass, RenderSSAO's composite.
4. The post-resolve's: the water surface batch trigger, WaterBlend (ImageSpace), `blended.draw` (Forward), the camera data.
5. The screen splatter: the blood family is latched today (BloodSplatterCapture); a native pass from ScreenSplatter's game data is the
   roadmap's phase 3 item and must come first, or blood disappears.
6. The camera-jump globals (DAT_1435ef504 and the saved camera) and the sun query draw (Sky.h).

The ORG epochs record in call order, so the replacement has to keep the order, the render targets the engine binds between steps
(SetRenderTarget x4-6) and the shadow state flags. Left for the next lead, together with the blood pass.

## Runs (stage `s3-culls`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+cadence-drop-cells (S8 parity build) | `s3-culls-s8par1` | passed; 8,746 compared, 0 differ |
| native-vulkan+quick, detector | `s3-culls-off1-nvk` | passed; 63.0 render calls (66.4 before); shot normal |
| native-vulkan+cadence-drop-cells with `snapshot.cadence=repeat:3` (decision 53: repeat plus three cocs) | `s3-culls-d53-repeat-cells` | passed, no crash |
| guest+quick | `s3-culls-f-guest-quick` | passed (guest unchanged: nothing installs) |
| native-d3d12-quick | `s3-culls-f-d3d12-quick` | passed |
| native-vulkan+cadence-drop | `s3-culls-f-drop` | passed |
| native-vulkan+cadence-drop-cells x3 | `s3-culls-f-drop-cells1/2/3` | all passed, no hang |
| native-vulkan+cadence-repeat | `s3-culls-f-repeat` | passed |
| native-vulkan+shadows-blend | `s3-culls-f-shadows-blend` | passed |
| native-interior, detector | `s3-culls-f-interior` | passed; 47 render calls (51 before) |
| native-river | `s3-culls-f-river` | passed |
| sky-native | `s3-culls-f-sky-native` | passed |
| native-vulkan+stealth-lights | `s3-culls-f-stealth` | passed; light level 1.8 with the torch, as before |
| native-tour | `s3-culls-f-tour` | passed |
| cadence.py, native-vulkan+cadence-still | `s3-culls-f-cad-*` | all four passed; exterior repeat 16.5 / drop 14.3 against a floor of 1.5, interior 2.6 / 3.0 against 0.14: the same sizes as snap3-lists (17.0 / 13.3, 2.6 / 3.0) and snap3-scene, the known wind and pose artefacts; "outside noise" by the rule only because this floor is low |

The side-effect lines in drop-cells: actors faded in equal the actors listed (52.3 of 52.4, 21.7 of 21.7), 11-105 S8 fade steps an
update, stealth sums as before.

## Decisions (agent, to confirm)

- **CU-a.** S8 uses the engine's own CPU fade functions (as AE 19384 does on the game thread) with the BSFadeNode type 6 branch ported,
  once per game update, for S7's roots in view that DCLF does not write. Leaf/tree LOD steps are included so the fade sees the same levels.
- **CU-b.** In native the main list culls, FirstPersonCulling and the list clears are jumped over in Main::Draw; the full-frustum call's
  engine function (culling globals) still runs.
- **CU-c.** Fade roots that are neither DCLF-listed nor animated references are not stepped in native: no game code reads their fade.
- **CU-d.** Mixed entries are stood in in native.
- **CU-e.** Render-only OnVisible effects (billboards, switches, ordered nodes, decal nodes, multibound caches, particle appends, the
  engine's light receivers) stop with the culls (decision 8).

## Open items

- Main::RenderWorld in native (above), after a native screen blood pass.
- Item 5 of the milestone (precipitation, decal occlusion passes, VL's vanilla path, reflection cube cameras from water data).
- SE is untested (every new address is an SE/AE pair; Main::Draw's offsets come from the SE disassembly).
