# snap7-mvec2: each render draws one ticket's objects

Lead `snap7-mvec2`, branch `snap7/mvec2` (base `org_integration` 90280b7d4, org_integration dccd7ece5+ merged later for the
post-land audit), stage `s7-mvec2`. RTX 5080, Dev-Fast builds throughout.

Follows `snap7-mvec.md`. Its open items: uploads ahead of their ticket (judder), the fragile FrameGlobals lifetime, grass
`prevTimeBase`, menu scenes' previous bones.

## 1. Uploads ahead of their ticket (fixed)

The fault: the game half kicks update n+1's sampling job at its `GameFrameStart`, before n+1 is published. The sample wrote the
kept rows directly. A render of ticket n whose upload ran after that job sent n+1's objects with n's camera: 5-10% of renders
at cap 30 and 21% at cap 240 (snap7-mvec's counts). The character led the camera by one update on those frames.

The sample cannot move to the publish: the engine's animation jobs wait for it.

### Design

- The kept arrays split in two. The sampled rows (`rows`, `palettes`, `shading`) are what the newest sampling job read; only
  sampling jobs touch them. The drawn rows (`Drawn::rows`, `palettes`, `shading`) are what uploads send.
- Each sampling job opens its sampled builds at their own content epoch (`BeginSampledBuild`), so the journal holds only that
  job's changes. At the job's end `StageSampled` copies the changed runs and their elements into a `Staged` record for its
  sequence. It also keeps, per written row, how the drawn row takes its previous transform, and per written palette block
  whether its previous half is what was drawn. Shading field owners move with the record.
- The ticket carries its sampling job's sequence (`FrameTicket::valuesSample`, returned by `KickSample`). The render passes it
  to `Kick`.
- The upload job opens the drawn builds at its leases' floors, applies every staged record up to its ticket's sequence in
  order (`ApplyStaged`), then runs `DrawStill` and sends. A later ticket's record waits for a later upload.
- The drawn-previous rule (snap7-mvec) moved from the sampler to the apply. The apply reads the drawn row before it overwrites
  it: a continued object's previous transform is the drawn row's previous while no upload sent it since, else its world. Rows a
  sample did not write the transform of (a root's sun entry or fade node) keep the drawn previous. New objects, billboards, LOD
  and guest take the sampled row whole. Palette blocks the same way.
- The sampled rows now keep the engine's previous transform and palette, as in guest. Settling (a mover resampled until a
  sample changes nothing) works on them unchanged.
- Spheres for moving-slot tracking come from the drawn rows at the apply.
- Safety valve: without renders, records would pile up. Past 8 staged records the sampling job applies the oldest early (at
  the last upload's floors). An upload that then finds a later ticket applied counts on the stats line.
- Guest: one render per update, so every upload applies exactly its ticket's record. The drawn rows equal the sampled rows.

No new switch.

Stats line (`CS_DCLF_STATS=1`, the frame values line): "N uploads sent a later ticket's rows, M held a later ticket's back".
The first must be 0. The second counts the renders the fix protected.

### Measurements

| Run | Uploads sent a later ticket's rows | Held a later ticket back |
|---|---|---|
| mvec-turn, cap 30 (4 report windows, ~1150 uploads) | 0, 0, 0, 0 | 33, 50, 15, 21 |
| mvec-turn at cap 240 (`--set saved_settings.Advanced={"Update Rate Cap"=240}`, 338 uploads) | 0 | 92 (27%) |

update-cap240 itself prints no frame values line (its world part is shorter than a report window), so the cap 240 count comes
from mvec-turn at cap 240.

mvec-turn's turn shots stay as sharp as snap7-mvec's fix and guest
(`C:\Users\matth\AppData\Local\Temp\claude\f5be53fdb6b18c23\c77789dc-39f2-41f3-80a1-db34faa4e8e2\scratchpad\mvec2\turn-taa-prev-new-guest.png`:
snap7-mvec's, this branch's, guest).

## 2. The Dragonsreach stall (fixed by snap7-mvec2b)

snap7-mvec2b took over here (same branch and stage). Its first notes on the FrameGlobals hold are kept below.

### What the extra view reference really did

Nothing waited on the shadow mask view. The extra reference only shifted timing. The stall happens without it too:
post-corrupt saw it on org_integration 5f8108e70 with no hold (1 of 2 runs), and so did the director's 1128-024517.

### Cause

- A routed game half takes DCLF's scene fragment out of the post into its ticket (`FrameTicket::scene`). The ticket's first
  render hands it to the frame revision at its Begin.
- A ticket can retire with no render: the cadence skips its snapshot, or a frame on the main thread (the loading menu's hold
  runs on `Main::DrawFrame`) replaces the snapshot. `NativeTicketSuperseded` only covers a routed snapshot replacing a routed
  one. The scene was then dropped with the ticket.
- The scene builder builds the next scene only once a Begin took the last one (`PublicationGateClosed`: taken < posted serial).
  So it never built again. Its stats show it spinning in the recordings stage with 0 builds started.
- The frame revision planned after the interior's variables (staged DCLF graphs for the interior conditions) has DCLF as a
  member and waits for a fragment recorded for it. None came, so it was never adopted, and the staged interior graphs never
  attached.
- DCLF kept drawing the exterior's publication with the interior camera (the white squares post-corrupt saw belong to it),
  through Riverwood too. The load hold ended "released after ~900 frames, no progress; claims not installed" every load after.

### Fix

`FrameTicket::~FrameTicket` puts an unhanded scene back in the post (`Revision::ReturnPostedFragment`, which only fills an
empty post). It is skipped when a Begin already took a newer scene. Nothing can render the ticket once it is destroyed, so this
never takes a scene from a render. The counters it needs never die, since a ticket can retire during process teardown. The
ticket line (CS_DCLF_STATS) counts returned and released scenes.

### Diagnostics added (always on, log only)

- `[ORG] frame revision N is not adopted 600 frames after it was planned ...: waits for <step>; sources: ...`, once per
  revision. This named the wait at once: "a fragment of drawing source 0; DCLF's scene: member true, drawing true, held none".
- The load hold's stall line ends with "Claims gate: ..." (not drawing yet, last coverage, stops, coverage state).

### Repro and runs

Repro: the old recipe (a static `com_ptr` holding `a_job.globals->shadowMaskHeld` in `FrameValues::Impl::RunSample`, gated on
`CS_AB_PROBE`, not committed) in native-vulkan+cadence-drop-cells.

| Build | Runs | Stalled |
|---|---|---|
| hold, before the fix (s7-mvec2-b-repro1..6) | 6 | 2 (repro4, repro6) |
| hold, with the fix (s7-mvec2-b-fix1..5) | 5 | 0; 3 runs returned one scene each, every revision adopted |

Failing runs: `C:\Modding\CS-AB\runs\1128-040128-s7-mvec2-native-vulkan-cadence-drop-cells-s7-mvec2-b-repro4`,
`1128-041127-...-s7-mvec2-b-repro6` (with CS_DCLF_STATS).

### The previous lead's notes on the hold (kept)

FrameValues no longer holds or compares a `FrameGlobals`: the ahead counter compares sampling sequences, so a freed capture's
address can't alias either. In their runs, holding the whole FrameGlobals, only `material.held`, or only `shadowMaskHeld` each
broke the interior about half the time, a dummy allocation did not. That was timing, as above.

## 3. Small items

- Grass wind time (fixed): `WriteDrawRow` exchanged `prevTimeBase` at every depth commit. It now steps once per render, keyed on
  the commit's scene step, so a second depth commit in a render sends the same pair. New tables start without wind motion (they
  started from time 0, one frame of large motion). The second part also applies to guest (a fix there too).
- Menu scenes' previous bones (recorded, not changed): menu scenes render on their producing thread once per engine update, so
  the engine's previous bones are the last drawn ones. No TAA history in menus.

## 4. B5 (post-land), what this change does to it

B5 is placement rows wrong after a coc out. This branch does not change which items are sampled, when, or the refused-lease
retry path. It changes how rows reach the GPU: only through staged records applied in ticket order, with the same journal and
backing floor logic as before on the drawn arrays. A record is never dropped: an upload applies every record up to its ticket,
and the safety valve applies the oldest when too many wait. So the change neither causes nor fixes B5 by design.

In snap7-mvec2b's land-cocs run (s7-mvec2-b-land2) the audit found no B5: each exterior shot 0 land rows wrong (about 175 of
9851 rows held other places, all particles and lights that move). One run is not proof that B5 is gone.

## 5. snap7-mvec2b: final runs

Base: org_integration 5f8108e70 merged (4a33fc9fa). Fix commit b95514dcb. Clean build, no hold. RTX 5080, Dev-Fast.

| Config | Label | Result |
|---|---|---|
| native-vulkan+mvec-turn (CS_DCLF_STATS) | s7-mvec2-b-turn30 | device lost 46 frames after world-ready (the known float-index family, see below) |
| native-vulkan+mvec-turn (CS_DCLF_STATS) | s7-mvec2-b-turn30b | passed; 8 report windows: 0 uploads sent a later ticket's rows (3-36 held back each) |
| native-vulkan+mvec-turn, cap 240 (CS_DCLF_STATS) | s7-mvec2-b-turn240 | passed; 0 sent a later ticket's rows, 5 held back |
| native-vulkan+update-cap30 | s7-mvec2-b-cap30 | passed (burst: spike 0.47, 0 flickering tiles) |
| native-vulkan+update-cap240 | s7-mvec2-b-cap240 | passed |
| native-vulkan+cadence-drop-cells x3 | s7-mvec2-b-cdc1..3 | passed (Dragonsreach drawn right each time) |
| native-vulkan+first-person-switch | s7-mvec2-b-fps | passed |
| native-upscalers | s7-mvec2-b-ups | passed |
| native-interior | s7-mvec2-b-interior | passed (burst: spike 0.28, 0 flickering tiles) |
| native-vulkan+land-cocs | s7-mvec2-b-land | device lost 3538 frames after world-ready, all shots taken (same family) |
| native-vulkan+land-cocs | s7-mvec2-b-land2 | passed |
| native-vulkan+quick | s7-mvec2-b-nvq | passed |
| guest+quick | s7-mvec2-b-gq | passed |

Skipped: native-d3d12-quick (no ORG, BasicRHI or device change).

The two device losses: turn30 `C:\Modding\CS-AB\runs\1128-045512-s7-mvec2-native-vulkan-mvec-turn-s7-mvec2-b-turn30` (HostWait,
fault instruction pointer only) and land `C:\Modding\CS-AB\runs\1128-053021-s7-mvec2-native-vulkan-land-cocs-s7-mvec2-b-land`
(fault invalid read at 0x4b36000, timeline read UINT64_MAX). Both retried clean. They look like the render-thread device loss
snap7-devloss7b owns; nothing in this branch touches GPU work. No one-frame flicker or broken frame seen in the passing runs.

Decided by the agent, to confirm:
- The scene goes back to the post from the ticket's destructor, not from each place a ticket can be dropped. The destructor runs
  only when nothing can render the ticket, so it covers every path (cadence skip, main-thread replacement, loads) at once.
- The stuck-revision line and the claims gate text are kept, always on: they only log, once per stuck revision or stalled hold.

Proposed decision-log entry: "A ticket that retires without handing its DCLF scene to a render puts the scene back in the post.
Lost scenes stopped the scene builder and left a frame revision waiting forever, so DCLF drew the last publication after a load
(Dragonsreach drawn as the exterior). A frame revision not adopted 600 frames after it was planned logs what it waits for."
