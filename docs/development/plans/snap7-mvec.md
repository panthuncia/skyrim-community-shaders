# snap7-mvec: previous values are the last render's

Lead `snap7-mvec`, branch `snap7/mvec` (base `org_integration` 3b3425bd8), stage `s7-mvec`. RTX 5080, Dev-Fast build throughout.

User report: things in motion, mostly the character, look blurry or garbled in native. The rule: every previous value that
feeds motion vectors, TAA or upscaler history is what the last render drew, not the previous game update. A repeat draws
nothing moving that did not move between renders. A render after skipped updates moves from the last drawn frame.

## What was wrong, per input

| Input | Before | Now |
|---|---|---|
| Skin palettes' previous bones (characters, head parts, NPCs) | the engine's `prevBoneMatrices`: the previous game update | the palette the last upload drew |
| Placement rows' PreviousWorld | the world of the last sample (once per game update since the producer split, decision 223) | the world the last upload drew |
| Repeated renders | rows and palettes kept the last update's motion on every repeat | each upload draws still what the last upload drew moving and no sample wrote again |
| Renders after skipped updates | previous = the skipped update's world, never drawn | previous = the last drawn world (a row written since the last upload keeps its previous) |
| First-person members (root view samples) | engine previous bones, previous update's world | as above |
| Main camera previous view-projection, eye | per render already (`FrameCamera::Build`, keyed on the render frame) | unchanged |
| Jitter | per render already (`Main_UpdateJitter` runs in each render; `PostChain::UpdateJitter` steps CS's index) | unchanged |
| History reset (TAA, Streamline) | per render already: `Snapshot::HistoryBreak()` folds skipped updates' events and is not set again on a repeat | unchanged |
| Streamline constants | camera matrices and jitter from `FrameCamera` (per render); mvec scale 1 | unchanged |
| Sky, water | previous posAdjust from the frame camera (per render) | unchanged |
| Grass wind time | `prevTimeBase` exchanged at each depth commit | unchanged (see open items) |
| Tree wind, fades | slots keyed on the render serial (decision 227); fades step 0 on a repeat | unchanged |
| Particles (palette row classes) | no previous rows; previous = current | unchanged (no particle motion vectors) |

The skins were the main fault: the stats line counted 16 to 55 skin palette blocks per render drawn with stale motion
on repeats at a 30 update cap (9,307 blocks over 259 renders in the turning part of mvec-turn).

## Fix (8f801029f)

`FrameValues` keeps a small `Drawn` record on the producer chain:

- each slot and palette block has the serial of the sampling job that last wrote it, and the chain knows the newest
  sampling job an upload sent (`uploaded`). A row written after the last upload is not drawn yet: its previous already
  holds what was drawn. Otherwise its world (or current palette) is what was drawn;
- the sampler (native only, continued slots and blocks) takes the previous transform and palette from that;
- rows and blocks written moving are listed. The next upload draws them. The upload after that sets previous = current
  for those no sample wrote again (`DrawStill`), so a repeat draws them still;
- a native upload opens its own builds (`BeginBuild`), so what it marks is a new content epoch every backing is sent.
  Before, marks made outside a sampling job (the late rows' `MarkLateDirty`) joined an epoch the backings already held.

Billboard rows are skipped (post-bugs B1). LOD keeps its current transform. Guest takes none of it (`SampleOut::native` is
`FrameCamera::Owned()`): one render per update, the engine's previous is right there.

New counters on the frame values stats line (`CS_DCLF_STATS=1`): rows and palette blocks drawn still, and uploads that
sent a later ticket's rows (below).

## Evidence

New config `mvec-turn` (d4344faa3): the pinned scene at a 30 update cap, the player turned 4 degrees per update with the
camera following, shots mid-turn with TAA and with DLSS. At cap 30 the render thread draws about 4 frames per update, so
most frames are repeats.

- before (s7-mvec-before-nvk, the base build): the player's armour and fur are smeared mid-turn under TAA.
- after (s7-mvec-after-nvk): as sharp as guest (s7-mvec-guest-turn) mid-turn.
- Side by side (before, after, guest), cropped to the character: `C:\Modding\CS-AB\runs\snap7-mvec-turn-taa-before-after-guest.png`.

## Runs (cs-test, stage s7-mvec)

| Config | Label | Result |
|---|---|---|
| native-vulkan+mvec-turn (base build) | s7-mvec-before-nvk | passed; smear |
| native-vulkan+mvec-turn | s7-mvec-after-nvk | passed; sharp |
| guest+mvec-turn | s7-mvec-guest-turn | passed (reference look) |
| native-vulkan+quick | s7-mvec-nvk-quick | passed, references pass |
| native-vulkan+update-cap30 | s7-mvec-a-update-cap30 | passed, references pass |
| native-vulkan+update-cap240 | s7-mvec-a-update-cap240 | passed, references pass |
| native-vulkan+first-person-switch | s7-mvec-a-first-person-switch | passed, references pass |
| native-vulkan+upscalers | s7-mvec-a-upscalers | passed, references pass |
| guest+quick | s7-mvec-a-guest-quick | passed, references pass |
| native-vulkan+cadence-drop-cells (first build) | s7-mvec-a-cadence-drop-cells, -2 | failed twice: Dragonsreach drawn as the exterior (see below) |
| native-vulkan+cadence-drop-cells (final build) | s7-mvec-b-cdc-rawglobals, -2 | passed twice |
| native-vulkan+mvec-turn (final build) | s7-mvec-b-turn | passed; 8,914 palette blocks drawn still in the turning part |

Skipped: native-d3d12-quick (no ORG, BasicRHI or device change), guest+upscalers (guest takes none of the new code).

No reference was re-accepted: every referenced shot still passes.

### The cadence-drop-cells failure and what it shows

The first build kept a `shared_ptr` to the newest sampling job's `FrameGlobals` for the ahead-upload counter. With it,
Dragonsreach rendered as the exterior (mountains, LOD, untextured snow quads, no interior) in two of two runs. Holding a
raw pointer instead (only compared) passes. So something downstream depends on a ticket's `FrameGlobals` being released
on time: it holds `shadowMaskHeld` (an engine view), the pipeline frame and the material frame. Keeping one extra ticket's
alive across the load into an interior was enough. Not chased further here; a lifetime dependency like that is fragile
(decision 235 found DCLF bindings keyed on engine view pointers).

## Open items

- **Uploads ahead of their ticket.** The render's upload sends the kept rows as they stand at its chain turn. When the game
  half of update n+1 kicked its sampling job before the render of n kicked its upload, that render draws objects at n+1
  with the camera of n. Counted at cap 30: 14 to 29 of about 275 renders (5 to 10%); at cap 240: 53 of 255 (21%). The window is the game
  half: a render (fresh or repeat) of ticket n that kicks its upload between update n+1's `GameFrameStart` and its publish. Motion vectors stay consistent with
  what is drawn (the drawn-previous rule), but the character leads the camera by one update on those frames: a small
  judder. The sample cannot simply move to the publish: it reads live engine memory under the read window, and the engine's
  animation jobs wait for it (`WindowWriters::BeginWriter`). Proposed fix: split the sampling job in two. Read the engine
  into job-local staging at `GameFrameStart` (as now, under the lease), and apply the staging to the kept rows in a chain
  job kicked at the update's publish, computing the drawn previous at apply time. Then an upload kicked before the
  publish sends ticket n's rows. Not done here: it reshapes the producer's write paths (items, deferred class items, roots,
  root views, registered game-side pools). Proposed for the next round.
- Grass's `prevTimeBase` is right if the depth commit runs once per render; unchecked whether a repeat commits again.
- Menu scenes (`MenuScenesNative`) still take the engine's previous bones; menus draw without TAA history, so not visible.
- Improved Camera is off in native runs; its body uses the same rows when present.

## Decisions, to confirm

- Decided by the agent, to confirm: the drawn-previous record lives in the frame values' producer chain, not in the
  snapshot. The chain already orders samples and uploads, and an upload is exactly one render, so "last drawn" is the
  last upload's rows. No new switch: the old per-update rule only ever brought back the smear.
- Decided by the agent, to confirm: a native upload opens its own builds of the placement and palette arrays. It fixes
  marks made between samples joining an epoch the backings already held (the late rows' marks on repeats had the same
  flaw). Guest keeps one build per sample.

Proposed decision-log entry: native's placement rows and skin palettes take their previous values from the last render
(the last frame values upload), and each render draws still what the last one drew moving unless a newer sample wrote
it; a render after skipped updates moves from the last drawn frame. Camera, jitter and history resets were per render
already. Open: uploads ahead of their ticket (5-21% of renders draw the next update's objects with this update's camera).
