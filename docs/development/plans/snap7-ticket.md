# snap7-ticket: DCLF's frame start in a game half and a render half

Lead `ticket`, round `snap7`, branch `snap7/ticket` (base `org_integration` a9182dd5f), stage `s7-ticket`.
Plan: [snap7-split.md](snap7-split.md) sections 6 and 7 item 1. Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md), phase 7.
Still today's single-threaded frame: no render thread.

## 1. What changed

### The ticket

`DCLF::FrameTicket` (`Core/Render/FrameTicket.h`) is what DCLF's frame start took of the game for one game update:

- the frame globals (`FrameGlobals`), the scene step, the installed publication's sequence, `loading`, whether the engine's utility
  shader exists, and the late-pose plan;
- the frame values' one-shot inputs (`FrameTicket::OneShot`): the placement plans, the shading items and fields, and the first-person
  members' rows and palettes sampled before the first-person shift (`FrameValues::TakeRootViews`). They sit behind an atomic pointer:
  the ticket's first render takes them; a ticket no render took hands them to the next ticket, oldest first.

Tickets retire through the snapshot retire queue's engine lane (their plans hold engine references): a rendered one after the newest
frame begun (`Retire::AfterThisFrame`), an unrendered one at once (`Retire::Push(..., 0)`). `[DCLF] frame-start tickets: N made, N
retired unused, N made again for a repeat that adopted a newer scene` reports them every 300 frames.

### The two halves

`FrameDriver::SceneFrameStart` (from `DrawcallLimitFix::BeginSceneFrame`, the SetupSunAndSky point in both modes) makes the ticket and
renders it:

- **Game half** `GameFrameStart(snapshot)`, inside an `EngineReads::CaptureScope` (its reads are the update's capture) and, after the
  globals' capture, a `FrameGlobals::Scope`: the load hold's start, the engine-read window's release, the scene pass mode, the classify
  inputs, `FrameView::BeginFrame` (the scene step, the lighting shader, `FrameGlobals::Capture`, the frame inputs), the primary cull's
  camera, the root states' and palette rows' frame starts, `loading`, the utility shader, the hand-over of the adopted publication, the
  views' start (first person's skeleton walk and target import), the sun exclusion's frame start, the claims, the report gates, the LOD
  gates (`ComponentsAdopt`), the load screen note and `ServeFrameRequests` (switch catch-ups, category captures), the shadow sources'
  frame start (LocalLightCull), the main renderers' refresh, the draw families' decision (grass stand-down and Grass Collision's stood-down
  update, the mirrors' drains), the scene-list filter, the shading fields' capture, `LatePoses::PrepareFrame`, the plans, shading items
  and the first-person samples.
- **Render half** `BeginFrame()`, inside the new `EngineReads::FrameStartScope`, from the ticket alone: the build point (when Begin did
  not run it), `FrameGlobals::Adopt` (the ticket's globals become `Current()` on the render thread), the frame tables' sync, the lookup
  inputs, the coverage decision with its tap errors, the ahead context, the root states' kick, the visibility readback, the views' work
  after tree LOD (the reflection's Prepare), the CPU frame seal, `FrameValues::Kick` with the ticket's inputs, the frame payload.

Both halves run at the same point today, game half first.

### The six conflicts (snap7-split section 6)

1. **Adoption and coverage vs the claims' consumers.** The game half hands over the publication the frame revision adopted and installs
   its claims while DCLF draws. Coverage stays a render-half decision. The first installation, and the first after a stop, waits for a
   render whose claims epochs were covered (`FrameDriver::coveredLast`, set by the render half, cleared at `source.left`), so DCLF's first
   drawn frame comes one render after its first covered one (the engine draws the frame between, in guest too). An uncovered frame while
   DCLF draws is still the render half's tap error.
2. **View coverage vs the first-person samples.** The game half samples the root views' members whenever a root view is registered and
   claims exist, whether or not the render covers the view: a render that draws first person must find its rows, and a sampled row nobody
   draws is harmless.
3. **FrameValues::Kick's sampling vs its GPU half.** The first-person sampling and the root view plans moved to the game half
   (`TakeRootViews`). The plans, shading items and fields are taken once per ticket and handed to its first render's producer. Kick
   (leases, versions, the frame wait, the dispatch, the pool kicks) stays the render half's, once per render, because the producer also
   fills the CPU frame slot's forward rows and the view batches' spheres for that render (`Direct::FrameStart` runs before it).
4. **FrameGlobals::Current().** `Capture` no longer hands the capture to the render thread; `Adopt` in the render half does
   (it sets the render thread's identity and `latest`). The game half binds its capture with a `Scope`.
5. **sceneStep.** It stays a per-render counter: the game half's `FrameView::BeginFrame` moves it for a new ticket, `StepRepeat` for a
   repeated render, so the numbers are today's. The epochs' once-a-frame guards (`SubmitSharedEpoch`'s `submittedFrame`, the commits'
   `committedFrame`) key on it, and a repeated render must draw its forward epoch again. An unrendered update moves it too (its game half
   runs `FrameView::BeginFrame`).
6. **The engine-read window.** Only the game half releases the render half's hold. A repeated render therefore runs with the window held
   (from its DrawFrame call to Present): its producer's lease-taking sampling is refused and handed to the next window, which is what the
   render thread will see.

### Cadence

- **Repeat.** A render whose snapshot repeats (`Snapshot::Render().repeat`) with the ticket made for it renders the ticket again: no game
  half, a producer with no new inputs, its own scene step. The scene source takes no new fragment at such a Begin (new
  `FragmentSource::takes`, `FrameDriver::HoldsScene`), so the installed publication stays the ticket's and every slot the frame draws has
  its rows. A frame revision adopted with a fragment held before the repeat can still install a newer scene; then the ticket is made
  again (counted as "made again").
- **Drop.** An update the cadence renders no frame for runs the game half all the same, from the snapshot hook
  (`Snapshot::RegisterUnrenderedUpdate`, called with the update's snapshot when its render loop ran zero times, world snapshots only). Its
  ticket retires unused when the next is made, and its one-shot inputs go to the next ticket.

### Engine reads moved out of the render half

| Site (before) | What | Now |
|---|---|---|
| FrameView.cpp:285 `IsLoadingScreenUp` (UI) | the frame's `loading`, the load screen note | game half; coverage's engine state reads `FrameGlobals::Current().loading` |
| LoadHold.cpp:366 (UI) | the map's start | game half |
| LodFeature.cpp:34, WaterSceneFeature.cpp:33/35, SceneCapture.cpp:769, FrameRequests.cpp:263/314/333/420/583 | category captures, switch catch-ups | game half (`ServeFrameRequests`) |
| FirstPersonView.cpp:36/50/73/144, Resources.cpp:1106 | first person's frame start and depth import | game half (views' start) |
| SceneLists.cpp:48/121 | the scene-list filter | game half |
| GrassMirror.cpp:84, GrassDraws.cpp:69 | the grass mirror's clock, Grass Collision's stood-down update | game half (draw families) |
| Skin Wetness.cpp:14, Surfaces.cpp:122/131, Characters.cpp:168 | shading fields | game half |
| MaterialLookups.cpp:280, ShadowCapability.cpp:151, ShadowRevision.cpp:262 (`globals::game::utilityShader`) | the shadow techniques' presence | `FrameTicket::utilityShader` (`TicketUtilityShader()`) |
| ParticleRows.cpp `Kick` (raw `kDrawWorldEye`, not counted) | the first-person eye | a frame-globals contribution (`first-person-eye`), read from the ticket's globals |

## 2. Detector numbers

The live-read detector gained `EngineReads::FrameStartScope`: live reads inside it are also counted as frame-start reads, logged under
`CS_SNAPSHOT_DETECT=1` as `[Snapshot] frame-start engine reads ...` and `frame-start read site: ...` lines, and reported as
`engine_reads.frame_start` in the test status. The baseline build (s7-ticket-base2-nvk-quick) opened it around the whole old
`FrameDriver::BeginFrame`; the branch opens it around the render half. Game-half reads are captures (its `CaptureScope`).

native-vulkan+quick, live reads at DCLF's frame start per world frame:

| Site | Before (whole BeginFrame) | After (render half) |
|---|---|---|
| FrameView.cpp:285 IsLoadingScreenUp (UI) | 3.00 | 0 |
| LodFeature.cpp:34 (TES) | 2.52 | 0 |
| Skin Wetness.cpp:14 (Water) | 1.59 | 0 |
| FrameRequests.cpp:583 (Data) | 1.57 | 0 |
| WaterSceneFeature.cpp:33 (TES), :35 (Water) | 1.26 + 1.26 | 0 |
| FirstPersonView.cpp:144 (Player) | 1.26 | 0 |
| MaterialLookups.cpp:280 (Shaders) | 1.00 | 0 |
| SceneLists.cpp:48 (ShaderManagerState, two reads), :121 (TES) | 1.00 + 1.00 + 1.00 | 0 |
| GrassMirror.cpp:84 (ShaderManagerState) | 1.00 | 0 |
| FrameRequests.cpp:263 (TES) | 1.00 | 0 |
| ShadowCapability.cpp:151 (Shaders) | 1.00 | 0 |
| FirstPersonView.cpp:50 (Renderer), :36 (Player) | 1.00 + 1.00 | 0 |
| LoadHold.cpp:366 (UI) | 1.00 | 0 |
| GrassDraws.cpp:69 (Camera) | 0.99 | 0 |
| Resources.cpp:1106 ImportViewDepthTarget (Renderer) | 0.93 | 0 |
| ShadowRevision.cpp:262 (Shaders) | 0.93 | 0 |
| Skin Characters.cpp:168, Surfaces.cpp:122/131 (Player) | 0.47 + 0.08 + 0.01 | 0 |
| FrameRequests.cpp:314/333/420 | 0.13 each | 0 |
| SceneCapture.cpp:769 (Data) | 0.11 | 0 |
| FirstPersonView.cpp:73 (Player) | 0.06 | 0 |
| Resources.cpp:1149 ImportShadowDepth (Renderer) | 0.02 | 0.02 |
| ShadowCapability.cpp:61/156/165 (Renderer) | 0.01 each | 0.01 each |
| ReflectionView.cpp:97 (Renderer) | 0.01 | 0.01 |
| **Total** | **26.46** (33 sites) | **0.04** (5 sites, 8 reads in the run) |

What is left are one-time reads of the Renderer's depth-stencil and cubemap target tables at DCLF's first frames: the shadow catalog's
formats, the shadow maps' import, the reflection cube's format. That memory is render-owned in native (CS's target set writes it;
snap7-split section 2), not game-mutated. The raw `kDrawWorldEye` read in the particle rows' kick, which the detector never counted, is
gone too. Whole-run live reads per world frame: 612 / 634 on the base runs, 587-602 on the branch (scene noise; the moved reads became
captures).

## 3. Runs

Stage `s7-ticket`, Dev-Fast, all with `CS_SNAPSHOT_DETECT=1`. Build b: the split (cafd137be); build c: final (adds the coverage
`loading`, the particle eye, carrying an unrendered ticket's first-person samples).

| Config | Label | Build | Result |
|---|---|---|---|
| native-vulkan+quick | s7-ticket-base-nvk-quick | base | passed; 612 live reads per world frame |
| native-vulkan+quick | s7-ticket-base2-nvk-quick | base + frame-start counter | passed; 26.46 frame-start reads per world frame |
| native-vulkan+quick | s7-ticket-a-nvk-quick | split, first build | passed; 1.05 (coverage still read the UI) |
| native-vulkan+quick | s7-ticket-b-native-vulkan-quick | b | passed with the shot reference; 0.04 |
| guest+quick | s7-ticket-b-guest-quick | b | passed |
| native-vulkan+cadence-drop-cells | s7-ticket-b-native-vulkan-cadence-drop-cells | b | passed, 3 shots |
| native-vulkan+cadence-repeat | s7-ticket-b-native-vulkan-cadence-repeat | b | passed, 3 shots |
| native-vulkan+first-person-switch | s7-ticket-b-native-vulkan-first-person-switch | b | passed, 7 shots |
| guest+quick | s7-ticket-c-guest-quick | c | passed |
| native-vulkan+quick | s7-ticket-c-native-vulkan-quick | c | passed |
| native-d3d12-quick | s7-ticket-c-native-d3d12-quick | c | passed |
| native-vulkan+native-interior | s7-ticket-c-native-vulkan-native-interior | c | hang at the first loading screen, before any world frame: main thread asleep in hdtSMP's update hook (FUN_140fa4020), no CS code on the stack; the known flake snap7-split section 8 lists |
| native-vulkan+native-interior | s7-ticket-c2-native-vulkan-native-interior | c | failed: the player lay dead in Dragonsreach (ragdoll on the floor in the shots, rendered normally), so the shots missed their references |
| native-vulkan+native-interior | s7-ticket-c3-native-vulkan-native-interior | c | passed with shot references, 4 shots |
| native-vulkan+native-tour | s7-ticket-c-native-vulkan-native-tour | c | passed, 3 shots |
| native-vulkan+first-person-switch | s7-ticket-c-native-vulkan-first-person-switch | c | passed, 7 shots |
| native-vulkan+post-effects | s7-ticket-c-native-vulkan-post-effects | c | passed, 13 shots |
| native-vulkan+shadows-blend | s7-ticket-c-native-vulkan-shadows-blend | c | passed, 7 shots |
| native-vulkan+native-river | s7-ticket-c-native-vulkan-native-river | c | passed, 2 shots |
| native-vulkan+screen-blood | s7-ticket-c-native-vulkan-screen-blood | c | passed, 2 shots |
| native-vulkan+menu-scenes | s7-ticket-c-native-vulkan-menu-scenes | c | passed, 5 shots |
| native-vulkan+update-cap30 | s7-ticket-c-native-vulkan-update-cap30 | c | passed, 3 shots |
| native-vulkan+update-cap240 | s7-ticket-c-native-vulkan-update-cap240 | c | passed, 3 shots |
| native-vulkan+window-resize-stress-taa | s7-ticket-c-native-vulkan-window-resize-stress-taa | c | passed, 13 shots |
| native-vulkan+cadence-drop-cells | s7-ticket-c-native-vulkan-cadence-drop-cells | c | hang at the first loading screen, before any world frame (same engine stack, FUN_140e01980) |
| native-vulkan+cadence-repeat, `CS_DCLF_STATS=1` | s7-ticket-cs-native-vulkan-cadence-repeat | c | passed; tickets per 300 frames: 100 made, 0 unused, 0 made again (repeat:3) |
| native-vulkan+cadence-drop-cells, `CS_DCLF_STATS=1` | s7-ticket-cs-native-vulkan-cadence-drop-cells | c | passed, 3 shots; tickets per 300 rendered frames: 300 made, 200 retired unused (drop:3) |

The director's test-scope note arrived after the suite had run; every config in the prompt's list ran.

## 4. Decided by the agent, to confirm

- TICKET-a. Both halves still run at the SetupSunAndSky point, game half first: the game half needs the publication the frame revision
  adopted at this frame's Begin, and the shadow casters after the point (snap7-lights) read the render half's state. In phase 7 the
  game half picks the scene and the render thread adopts the ticket's (`FrameSnapshot::scene`, decision 141's open item).
- TICKET-b. DCLF's first claims install one render after the first covered render (conflict 1), in guest too. The engine draws that
  frame, so nothing shows; it is the price of the game half not reading the render side's coverage of its own frame.
- TICKET-c. First person is sampled whenever a root view is registered (conflict 2), not only when the render covers it: a few dozen
  extra row samples in third person.
- TICKET-d. `sceneStep` stays a per-render counter (conflict 5). Making it a per-update number would skip the forward epoch on a repeated
  render (`SubmitSharedEpoch` keys "once a frame" on it).
- TICKET-e. `FrameValues::Kick` stays per render in the render half (conflict 3), with the ticket's one-shot inputs on its first render
  only. Splitting the producer into a game sampling job and a render upload job is the next step (open item 1).
- TICKET-f. A repeated render keeps the window held (conflict 6), so its producer's sampling is refused and handed on.
- TICKET-g. A dropped update runs the game half from the snapshot hook (new `Snapshot::RegisterUnrenderedUpdate`) rather than not at all,
  so the cadence mode exercises "a ticket no render took". Its one-shot inputs merge into the next ticket.
- TICKET-h. A repeat whose Begin adopted a newer DCLF scene anyway (a frame revision adopted with a fragment held before the repeat)
  makes the ticket again rather than rendering mismatched rows; counted on the tickets line.
- TICKET-i. The one-time Renderer target-table reads left in the render half (0.04 per world frame) stay: render-owned memory.

## 5. Open items

1. **The frame values' producer.** It still samples due movers under engine-read leases on DCLF's pool, kicked once per render, and the
   pool kicks run on the render thread. In phase 7 the sampling must be a game-side job per ticket (`Sample`) and the render side's
   per-render job only uploads and fills the CPU frame slot's forward rows and spheres (`Upload`, `StoreForwardRows`, `TrackSpheres`).
2. **Scene pairing.** The ticket names the publication the render side adopted at the update's first Begin. Phase 7 inverts it: the
   game half picks the newest posted scene into the ticket (or `FrameSnapshot::scene`) and the render thread adopts that one;
   `FragmentSource::takes` then becomes "adopt the ticket's scene".
3. **Render-side decisions the game half reads.** `coveredLast` and the draw families' decision (made in the game half from DCLF scene
   state) are same-thread reads today; in phase 7 they become messages (an atomic latest value) from the render thread.
4. **Game-half calls into render-side state.** First person's depth import (`ImportViewDepthTarget`, from its view start) adds a graph
   extension, and `PassCapture`/`ParticleSnapshots` hold state both halves touch; fine on one thread, to be messages later.
5. **DCLF's other points** (DepthCommitted's `LatePoses::ConfirmFrame` walk, EarlyPrepass, Prepass, BeforeOpaquePass, BeforePresent,
   EndFrame's `NoteLoadingScreen`) still read the engine; snap7-split section 6 lists them.
6. **snap7-lights.** The shadow sources' frame start (LocalLightCull), the sun and light candidates and the sun exclusion's frame start
   are in the game half. This branch does not move `ShadowCastersSelected` or the shadow coverage decision (`BeforeShadowCasters`), so
   snap7-lights' `Native::CaptureSet` after them is unaffected.

## 6. Proposed decision-log entries

- **DCLF's frame start has a game half and a render half joined by a ticket** (phase 7, snap7-split item 1): `GameFrameStart` reads and
  writes the engine once per game update inside a capture scope and makes a `FrameTicket` (globals, scene step, installed publication,
  `loading`, utility shader, late-pose plan, the frame values' one-shot inputs and first-person samples); `BeginFrame` renders from the
  ticket alone. DCLF's frame-start live reads went from 26.5 to 0.04 per world frame (render-owned target-table reads left).
- **Cadence for tickets:** a repeated snapshot renders its ticket again (no game half; the scene source takes no new fragment,
  `FragmentSource::takes`); an update no frame renders runs the game half and its ticket retires unused, its one-shot inputs going to
  the next ticket; tickets retire through the engine lane.
- **Claims are the game half's:** installed from the adopted publication while DCLF draws; the first installation waits for a covered
  render (one render later). `sceneStep` stays per render. FrameGlobals reach the render thread by `FrameGlobals::Adopt`.
- **The live-read detector counts frame-start reads** (`EngineReads::FrameStartScope`, `frame-start read site` lines,
  `engine_reads.frame_start`).
