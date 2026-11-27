# snap7-sky: the sky's per-pass values and the frame values' producer split

Lead `sky`, round `snap7`, branch `snap7/sky` (base `org_integration` 1b59a188a), stage `s7-sky`. Items: [snap7-hold.md](snap7-hold.md)
section 5 items 1 (sky BuildPass's per-pass reads) and 2 (the producer split, [snap7-ticket.md](snap7-ticket.md) open item 1). Still
today's single-threaded frame: no render thread.

Status: **done** for both items. What is left is section 6.

## 1. The sky's pass values

`Native::PassObject` (`SkyNative.h`) is everything a sky pass's draw reads of its geometry and property: the sky object type, the blend
colour and value, the cloud layer, the fade flag, the two-sided flag (+0x38 bit 36), the alpha property's test bit, the base and blend
textures (whether set, and their D3D11 views), the world and previous world transforms, the renderer data and triangle count, the shader,
and the geometry pointer as a key only. `TakePass(geometry, property, shader)` reads it from the engine.

- The ticket's sky slot (`TicketPart::SkyRoot`, `RootObjects`) now also holds a `PassObject` per root object of kind Pass, the sun query's
  pass (`TakeSunPass(SunQueryGeometryNow())`) and the sky globals (`SkyGlobals`: VParams, the 32 cloud layers' offsets, the three sky
  colours, the default, white and noise textures' views).
- `BuildPass` takes a `PassObject` and reads nothing of the engine. Native's main view (`BuildMain`), native faces (`BuildFace`) and
  native's sun query (`SubmitSunQuery`, which now takes the `SunQueryBlock` and draws the ticket's sun while it is still the block's
  geometry) read the ticket's values and globals.
- Guest is unchanged: the engine-batch paths (`BuildBatch`, `BuildClouds`), guest's faces and guest's sun query take a `PassObject` from
  the live pass at the draw (`TakePass`, counted by the `Scene` mark there) and read the globals at the draw.
- CS's sky hooks have overloads taking the sky object type and the geometry key: `State::UpdateSkyShaderPermutation(type, geometry)`,
  `Effects11::ModifySky(type)`, `CloudShadows::ModifySky(type, geometry, face)` (`FindCloudLayer` takes the geometry key). The pass
  overloads call them, so guest's hooks behave as before.
- `SunQueryGeometry(block)` (a render-side read of the sun global) is replaced by `SunQueryGeometryNow()` (game side).

Observe, then switch: the first build compared each captured `PassObject` and the globals with a live `TakePass` at the draw
(s7-sky-a-nvk-tour, native tour, Whiterun and Dragonsreach): 21,615 passes and 386 globals compared, 0 differences. The scaffolding is
deleted.

## 2. The producer split

`FrameValues` now runs two job kinds on one ordered chain (`chainKicked` / `chainDone`; each job waits for the one before it, so the kept
rows have one writer at a time and no lock):

- **Sampling job**, one per ticket, kicked by the game half (`FrameValues::KickSample`, from `GameFrameStart` where the one-shot inputs
  were taken): the plans, `MarkDue` (move keys, the owed nodes' lease), the items' and roots' samples, the root view samples, the shading
  rows, `TrackSpheres`, and the game-side pools' samples. Every engine-read lease of the frame values is taken here. The pools' frame
  starts and the game-side pools' and palette rows' kicks run in `KickSample` too. A dropped update's ticket samples all the same.
- **Upload job**, one per render, kicked by the render half (`FrameValues::Kick(globals)`): leases the backings, then on the chain marks
  the late rows dirty, fills the CPU slot (`StoreLiveSpheres`, `StoreForwardRows`, `PostSnapshotSpheres`, `Direct::OnFrameValuesSampled`),
  runs the per-render pools' samples, builds the late rows, uploads and signals the frame wait value (`seq`, per render as before). It
  runs inside `EngineReadWindow::RenderSideJob`, so a lease asked for there is counted. A repeated render kicks an upload only.
- **Floors.** A sampling job opens its builds at the floors the last upload leased at (`sampleFloors`, a registered pool's `sampleFloor`):
  floors only rise and the journal sends a holder below its floor everything, so an older floor only trims less.
- **Sizes.** Each sampling kick raises the rows the backings must hold (`neededRows`, a game-side pool's `needed`) before it takes its
  place on the chain; an upload takes its place before it reads them, so it leases for every sampling job before it.
- **Pools.** `Api::FrameValuePool::perRender` marks a pool kicked and sampled with each upload from render-side state: fade CPU states
  (the render's view batches and placement rows) and pixel rows. Every other pool is game side. A producer's `Newest()` compares with the
  newest job of its kind.
- `WaitSampled` (the window writers) waits for the newest sampling job. Plans are released in the game half once their sampling job is
  done. `FrameTicket::OneShot`, `TakeOneShot` and `FrameValues::PrependRootViews` are deleted; an unrendered ticket is counted when the next
  is made.

New diagnostic (no switch): `EngineReadWindow::RenderSideJob` marks a thread running a render-kicked job; leases asked for on it are
counted in the engine-read window line (`render-side job leases N`, with `CS_DCLF_STATS=1`). In the base build the whole producer was
render-kicked and tagged.

## 3. Numbers

Stage `s7-sky`, Dev-Fast. Base: org_integration 1b59a188a plus the lease counter (72d9fb679). Detector runs with
`CS_SNAPSHOT_DETECT=1 CS_DCLF_STATS=1`.

Render half (record), live reads of game-mutated objects per world frame:

| Site | base quick | after quick | base tour | after tour |
|---|---|---|---|---|
| SkyNative.cpp:534 sky BuildPass (Scene) | 20.85 | 0 | 20.49 / 21.67 | 0 |
| WetnessEffects.cpp:984 (Sky, snap7-precip) | 2.00 | 2.00 | 2.00 | 2.00 |
| Skylighting.cpp:819, :873 (Sky, snap7-precip) | 1.00 + 1.00 | 1.00 + 1.00 | 1.00 + 1.00 | 1.00 + 1.00 |
| OccluderRules.cpp:82 (Sky, snap7-precip) | 1.00 | 1.00 | 1.00 | 0.72 |
| GrassTables.cpp:445 (Ini), ScreenSpaceGI.cpp:829 (ImageSpace) | 0.01 + 0.01 | 0.01 + 0.01 | 0.00 | 0.00 |
| **Total record, game-mutated** | **25.87** | **5.02** | **26.39** | **4.72** |

What is left in the record half is precipitation (snap7-precip) and two one-time reads (a Grass ini value, SSGI's screen quad). Frame end:
no game-mutated reads before or after. Whole-run live reads per world frame: quick 546 -> 523, tour 442 -> 395.

Leases asked for in render-kicked jobs (`render-side job leases`, per 300-frame report, native tour): base 1,844-5,930 (the producer's own
thread only; its parallel chunks ran on other workers and are not in this count), after 0 in every report. Main-thread leases 0 in both.

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick | s7-sky-base-nvk-quick | passed (base) |
| native-vulkan+native-tour | s7-sky-base-nvk-tour | passed (base, 3 shots) |
| native-vulkan+native-tour | s7-sky-a-nvk-tour | passed with the observe scaffolding: 0 differences |
| native-vulkan+quick | s7-sky-b-native-vulkan-quick | passed |
| guest+quick | s7-sky-b-guest-quick | passed |
| native-vulkan+sky-native | s7-sky-b-native-vulkan-sky-native | passed (4 shots: Dragonsreach, river noon, sky noon, night) |
| native-vulkan+cloud-shadows | s7-sky-b-native-vulkan-cloud-shadows | passed (4 shots: storm clouds in the faces, opaque, exterior, interior) |
| native-vulkan+native-tour | s7-sky-b-native-vulkan-native-tour | passed (3 shots) |
| native-vulkan+cadence-drop-cells | s7-sky-b-native-vulkan-cadence-drop-cells | passed (3 shots) |
| native-vulkan+update-cap30 | s7-sky-b-native-vulkan-update-cap30 | passed (3 shots) |
| native-vulkan+update-cap240 | s7-sky-b-native-vulkan-update-cap240 | passed (3 shots) |

Skipped: native-d3d12-quick (no ORG, BasicRHI or native device code changed).

## 4. Decided by the agent, to confirm

- SKY-a. **Guest keeps reading the sky passes at the draw** (engine batches, faces, sun query); only native reads the ticket. Guest's face
  render comes later in the same DrawFrame than the capture, so reading the capture there could change guest. The ticket captures the
  pass values in both modes (a few dozen objects), guest ignores them.
- SKY-b. **The sun query's pass is captured with the sky root in the game half**, not in the snapshot's `SunQueryBlock`: the block is
  captured at the DrawFrame call, before Main::Draw's SetupSunAndSky point where the root is taken; the draw takes the ticket's sun only
  while it is the block's geometry (as `SunQueryGeometry` checked).
- SKY-c. **A cloud layer index past the engine's 32 fails the pass** (it read past the offsets array before). Not seen.
- SPLIT-a. **Pools are game side unless they say `perRender`.** Only fade CPU states and pixel rows read render-side state. The pools'
  frame starts and kicks now run once per ticket in the game half (before: once per render); in today's frame both run at the
  SetupSunAndSky point, so their inputs are the same.
- SPLIT-b. **Floors one upload late** (above) instead of moving `BeginBuild` to the upload job: no pool's sample changes and the journals
  stay correct; they trim a little later.
- SPLIT-c. **The backings' needed rows only rise** (atomic max) for built-in and game-side pools, as the kept arrays never shrink.
  Built-in asks were already monotonic; a game-side pool's backing no longer shrinks after a long smaller stretch.
- SPLIT-d. **`TrackSpheres` stamps the next upload's sequence** (the last finished upload's plus one) from the sampling job.

## 5. Proposed decision-log entries

- **Native's sky reads its passes from the ticket** (`Native::PassObject`, `TakePass`, the sky slot's passes, sun pass and `SkyGlobals`):
  `BuildPass` reads no engine memory; guest takes the same values from the live pass at the draw. CS's sky hooks take the object type and
  a geometry key. Native quick, record half game-mutated reads: 25.9 -> 5.0 (precipitation and two one-time reads left).
- **The frame values' producer is split** (closes snap7-ticket open item 1): a sampling job per ticket from the game half takes every
  lease; an upload job per render from the render half reads only sampled rows, on one ordered chain. Pools are game side unless
  `perRender`. Leases asked for in render-kicked jobs: thousands per 300 frames -> 0 (`RenderSideJob` counter).

## 6. Open items

1. **Rows the render thread reads while a later sampling job writes.** `RowsIfDone` and the forward rows hand the render thread the live
   kept arrays; today no sampling job can run during a render's reads. With a render thread they must be views (the kept arrays' `View()`
   or `Shared()`), or the render reads a copy the upload job makes.
2. **Pool state both halves touch.** A game-side pool's kick state (the seeds' queues, the texture transform list) is now game-half only;
   `MakeRegistered` can run first on either thread; with a render thread it belongs at install.
3. **Render-half engine reads the detector never counted:** fade CPU states' kick (`FadeState::SampleFrame`, `MainCamera`), still per render.
4. Snap7-hold section 5 items 3-5 (scene pairing, family lane requests, the ticket's raw pointers' lifetimes) are unchanged; the sky's
   `PassObject` adds texture views and renderer data to item 5's list.
5. The "engine globals read off the render thread with no frame's capture bound" error (FrameGlobals.cpp) shows in base and branch runs
   alike (7 and 9 in a tour), not from this branch.
