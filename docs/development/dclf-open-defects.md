# Drawcall Limit Fix: open defects

This file lists Drawcall Limit Fix's open defects, failing checks, open crashes and accepted differences. Each entry says how
to reproduce it or which check shows it. How DCLF works is in [dclf-architecture.md](./dclf-architecture.md). The switches
and report lines named here are described in [dclf-validation.md](./dclf-validation.md).

## Fixes built but not confirmed in game

The fixes below are in the code. No recorded run has yet shown the check that confirms each one.

-   **Newly loaded objects drawn opaque, then vanishing, then fading in.** `DCLFAlphaFade` fell back to the root row's `Initial` (the
    mirror's fade at listing) until FadeStateCS's state reached t120. The seed (the node at the frame's start) was used only by
    BuildDraws. The pairing of listings with seeds counted 144 generations listed at 1 but seeded at 0 (opaque, then gone, then fading
    in), and 511 listed at 0 but seeded at 1 (a visible object missing for a frame). The fix: the seeds are a frame binding at t117
    (`kFadeSeedsRegister`, answered with the pool's descriptor on the render thread: `FrameBindingSelect::index`), and the pixel stage
    falls back to the seed as `SeedState` does. **Check:** walk into a forest with DCLF; new full trees should fade in like the
    engine's, with no opaque first frame.
-   **Stood-in animated references that never fade freeze.** `SyncRootOwnership` (`Core/Handoff/PrimaryCull.cpp`) clears
    kAccumulated (bit 26) on stood-in roots. The engine's animation gate (`FUN_1402cff60`, which `UpdateAnimationJob` and the
    managed-node update call on `ref->Get3D()`) then stops updating roots that never fade: windmill sails, water wheels, flags,
    Dwemer machinery. The fix: DCLF writes kAccumulated from its GPU visibility at `Main::Update`'s entry, and the gate skips the
    roots whose fade DCLF writes. **Check:** `CS_DCLF_ACCUMULATED_PARITY=1`. `<- ANIM RATE` should show 0 differ, and never-fading
    stood-in calls should be rejected only when written out of view. Run vp10 (tree-forest route) showed 0 differ, but no never-fading
    stood-in root there (0 calls), nor at Riverwood (run rw1, 0 never-fading calls): no stood-in root that never fades was found
    yet, so the never-fading half is unverified.
-   **Stood-in BSTreeNode trees on the tree manager's tree list.** The tree list's loop (`FUN_140437e50`, list `+0x20`) visits
    a tree by its first child's multibound cull result, which no cull refreshes for a stood-in root. A stale "in" animates the
    tree every frame, and a stale "out" freezes it (`skyrim-engine-notes.md`, "Tree manager list (T7b5)"). The fix:
    `TreeAnimation` writes each stood-in tree's multibound result from DCLF's verdict around the loop's call, then restores the
    engine's value. **Check:** in the `tree animation list` line, "loops given DCLF's verdicts" > 0 where stood-in BSTreeNodes
    exist. **Blocked:** no `BSTreeNode` was found at Riverwood, Falkreath, Riften, Raven Rock or Morthal in the test load
    order. `CS_DCLF_TREE_LIST_ALL` stays off until a load order with such trees shows the comparison clean.

## Visible lighting defects

-   **Statics have no room for a few frames after a load (`light data parity MISMATCH`, `DCLF room -1`).** The first report after
    each load into Dragonsreach or Bleak Falls Barrow has 400-3,000 draws with DCLF room -1 against the engine's room; the next
    report is clean. The keys are right (persistent parity's ancestor key check: 0 stale). The cause is the map's latency: DCLF
    copies Light Limit Fix's room map at EarlyPrepass, before Light Limit Fix rebuilds it in Prepass, and the scene lane takes the
    copy on its next pass, so a room that gains a light reaches DCLF's draws one to three frames late. With room -1 a portal-strict
    light is not gated for those objects, so it can light them through walls for those frames. The same lag runs the other way
    when a room loses its last light (once, DCLF room 35 against the engine's -1). Light Limit Fix now keeps a room's number while
    a light reaches it (`LightLimitFix::UpdateLights`); it used to renumber every room whenever the lit set changed, which gated
    DCLF's objects as another room until the next map arrived. Runs `rooms` and `roomswalk` (cs-ab). **Next:** removing the
    remaining frames needs the room resolved on the GPU in the frame: each object's record holds a stable room slot, and a table
    from slot to Light Limit Fix's number is written after its Prepass.

## AMD: the HZB reads back empty (fixed 2026-11-17)

-   **Cause.** `HzbTest` (`HzbTest.hlsli`) returned early on `a_view.Index == 0 || a_view.Mips == 0`. AMD's RDNA2 compiler folds a
    value to the constant past such a guard (AGENTS.md, "Known Driver Bug"), so the four taps read heap slot 0 and every footprint came
    back all-near: occlusion culled nearly everything in view on the RX 6600.
-   **Fix.** The guard is one unsigned relational compare (`min(Index, Mips) < 1u`). With occlusion on, perf-taa reads 0 of 2,693
    footprints all-near, about 460 draws a window are occluded, and the view is whole. `CS_DCLF_CULL` defaults to occlusion on every
    GPU again (`DefaultCullMode`). Details: `org-native-frame-graph.md`, "RX 6600, third pass".

## Open crashes

-   **A freed child detached during cell streaming.** It happens intermittently, 70-100 s into motion runs (`CS_DCLF_TEST_MOVE`,
    cells streaming). It is an access violation on an engine job thread with this stack: `DCLF::DetachChild2At<0>::thunk+0x65`,
    then the engine's detach (`SkyrimSE+0xd1d32e`), whose `call qword ptr [rax+1B8h]` goes through the child's vtable. The vtable
    reads as float data (`3f8000003f800000`), so the child is already freed when the thunk's original call runs. Reruns of the same
    build do not crash. Two dumps are kept in the scratch folder (`refactor2/v3-motion.dmp`, `refactor2/fdt2-motion.dmp`). **Next:**
    check whether DCLF releases a reference to a child that the engine detaches later (engine references are released only through
    `EngineReleases` at Present), and whether the thunk reads the child after the original call. The thunk itself only notes the
    detach and passes the child through, so the caller held a dangling pointer. **Not reproduced** in 120 s motion parity runs on
    the tree-forest route (vp11-vp13, 2026-10-22): set aside until it recurs.
-   **Vertex-stage faults at load.** Device losses a few seconds after a load (or at the main menu, DCLF idle), each Aftermath
    "Shader Instruction Error" at PC 0 of `vertex_01`, a 5 KB vertex shader with hash `0xfb2df49047e00dfa`, 84 warps active. The
    same hash appears in every dump since 2026-10-20 (02:58, 19:58, 20:00 and 21:50 on 10-21), across builds, so it is one
    shader. They are not tied to a switch: the three at 19:58-20:00 had `CS_DCLF_PERSISTENT_PARITY` with `CS_DCLF_TREE_LOD_AUDIT`
    (the audit is CPU-only), the 21:50 one persistent parity alone. The runner retries a launch that never reaches gameplay, which
    hid them: 7 of about 250 launches retried, not all with a dump. No shader debug info was written, so the shader is unnamed.
    One loss with this signature was the index pool's unfilled grown `pool-firsts`, now fixed. **Next:** a soak of launches with
    `DXVK_AFTERMATH_SHADER_DEBUG_INFO=1` (the decoder then maps the shader; `extern/dxvk`'s adapter warns that the heavier
    diagnostics configuration itself produced load-time faults on this driver, so add the flags one at a time).
    **Shelved:** not reproduced on 2026-11-06 in 11 launches with two loads each (`vsoak.env`: shader debug info, 5 launches;
    `vsoak-errors.env`: with shader error reporting too, under which the faults came most often, 6 launches). Pick it up on a repeat,
    with `vsoak-errors.env`.
-   **Use-after-free in actor teardown on a save load.** It happened when a save was loaded after going through a door. A job
    thread in `Actor::Predestroy` -> `FUN_140720830` (middle-high process data, `+0x278`) -> `FUN_14021ca60`/`FUN_14021d140`
    destroyed a pool entry's object (keyed by a `BSLightingShaderProperty`) that was already freed. DCLF holds property and
    material references, but nothing ties the crash to them yet. **Next:** reproduce with `CS_DCLF_RELEASE_GUARD=1`, and with DCLF
    off.
    **A second form, 2026-10-22 (run ex1, 75 s into a motion parity run out of Riverwood):** an actor's skin destroyed on the main
    thread under `Main::Update`: `BSDismemberSkinInstance::~NiRefObject` (`0x140d32470`) -> `NiSkinPartition::~NiRefObject`
    (`0x140d44640`) -> `FUN_140e46810` (its buffers' release) -> Engine Fixes' allocator, an atomic write through a null pointer
    (`xchg [rsi+10h]`, rsi 0). Run wr4's corrupt-heap crash (below, "probably unrelated") was also under an actor's geometry
    (a face morph blend). If DCLF keeps a skin partition's buffers or the partition past the engine's release (Features/Skins), this is
    where it would show. Dump: `ex1-skin-teardown.dmp` (session scratch folder).
-   **A hang 27 s into a motion parity run, after the shadow epoch's builds became one dispatch.** It happened once, on 2026-10-21,
    in the first run of the multi-view shadow build. That build is `ShadowViews` in `BuildDrawsCS.hlsl`: its view table is published
    by `org::PreparedTablePublisher`, `cs.dclf.shadow.build-views`. The log stops with ordinary report lines, about a second after
    the table was published at 4, then 8, then 28 rows. The game then stayed alive until the run's timeout. There was no device loss
    and no Aftermath dump, so it was a wait that never completed: a CPU deadlock, or a submission waiting on a signal that never came.
    The rerun did not hang: 87 s, with the 28-row table published 32 times. **Next:** if it recurs, the scratch folder's
    `hangwatch.sh` writes a full dump when the log stalls for 15 s. Read the threads' stacks with cdb. Suspects are the publisher's
    mutex during preparation, and a kept shadow recording whose table was republished.

-   **A null list in a geometry's render setup, `SkyrimSE+0x14fceb4`.** Once, on 2026-10-22, 85 s into a 120 s motion parity run
    (vp15, frame 10818), on the render thread: `Main::RenderPlayerView` -> `FUN_1414a1540` -> `BSGeometry::sub` (0x1414a2fc0), which
    passes `*(x + 0x70)` (null) to `FUN_1414fcea0`, which reads `[rcx+18h]`. A build's deploy into the MO2 mod folder overlapped the
    run (it finished 90 s after the crash), so the run is suspect. The dump is kept as `vp15.dmp` in the session scratch folder.
    **Next:** watch for a repeat in runs with no deploy during them.
-   **ORG stops frame production during a cell load: "Previous synchronous admission has not terminated".** It has happened twice
    on 2026-10-28, both times right after `coc Whiterun` on the VRAM retention tour, with an exterior cell load in progress and
    on-demand shadow and effect pipelines being built. The first time was run 18 at 15:29, before the scene lifetime changes; the
    second was run p1on5 at 21:01, with the release ring.
    1. The scene revision's Z-prepass epoch recording fails with `Previous synchronous admission has not terminated`.
    2. The colour, shadow and forward epochs then fail with `Frame production has stopped`.
    3. ORG disables the render graph ("Async epochs failed").
    4. DCLF ends on its fatal check: "frame N is not covered by the snapshot it adopted ... DCLF does not hand drawing back to the
       engine once it draws".

    Other runs of the same route and build passed. Logs: `run18_cs.log` and `p1on5_cs.log`, both in the session
    scratch folder. Both came about a second after a rebuild into "interior cell, two-pass occlusion, CPU pipeline", at about
    12 fps, 100-250 ms after `cs.dclf.z.bucket-counts ... grown ... as graph work`.

    Not reproduced on 2026-11-06 (RTX 5080, about 120 fps): 17 interior/exterior changes (`tools/cs-ab/scenarios/cellflip.env`),
    and 36 changes and rebuilds with `CS_DCLF_TABLE_START=small`, 223 growths as graph work (`rebuildstress.env`). The error now
    names the pending admission: the thread that prepared it, whether at a ticket's submission or for an immediate execution, and
    how long ago (`SynchronousAdmission::PendingOwners`, extern/OpenRenderGraph). **Next:** on a repeat, compare that thread with
    the log's: the ORG host thread, the render thread, or DXVK's CS thread.

-   **NVIDIA's driver faults while a shadow view's pass begins.** Six identical crashes on 2026-11-01 between 21:07 and 23:08:
    `nvoglv64.dll+0xE5A9FD` (`mov r9d, [rax+0x1F0]`), called from `DCLF::Draws::ShadowViewPass::Record`
    (`ShadowEpochPasses.cpp:303` at the time, `commands.BeginPass(begins[i])`) under ORG's prepared-batch recording. The driver
    read a bad attachment: probably a shadow map view that was stale or freed when the pass began. Two more driver crashes on
    2026-11-05 at 05:04 (`nvoglv64.dll+0xF63D9`, a null read, 32 s after launch) have no stack. The shadow views' buffers and draw
    data were reworked on 2026-11-03, and none of the 47 runs since 2026-11-04 crashed. **Next:** on a repeat, check that each
    view's target is the one the frame's shadow maps hold (a rebuilt or resized shadow map between preparation and recording).

### Crashes probably unrelated to DCLF

These crashes have no DCLF or Community Shaders frames on the stack and match known engine instability. They are recorded so
that a repeat is recognised. They are not chased unless one recurs with a DCLF or CS frame on the stack.

-   **A corrupt heap in Engine Fixes' tbbmalloc under a fast flight, with or without DCLF** (2026-11-06, `motionsoak.env`: `tcl`
    out of Whiterun at 24 units a frame for about 15,000 frames). The faults are in tbbmalloc's large-object cache
    (`getFromLLOCache`, `putToLLOCache`, `CacheBinFunctor`, `freeOwnObject`), or downstream of it on engine job threads: Havok under
    `MagicCaster::UpdateImpl` (`SkyrimSE+0xADE85B`) and the Papyrus VM. The flight crashed 2 of 4 runs with DCLF on, 3 of 4 with
    DCLF switched off from frame 1000 (`motionsoak-off.env`), 2 of 4 with texture streaming off (`CS_DCLF_TEX_STREAM=0`), and 3 of
    4 without the Skyrim Engine Telemetry plugin. One fault came while texture streaming read a file through
    `BSResourceNiBinaryStream`, but streaming off did not change the rate. This speed is past what the engine streams safely, so it
    is no test of DCLF's crashes. Hunt those at walking or riding speed (a few units a frame), where these faults should be rare.
-   **tbbmalloc read in an NPC's behaviour graph update** (uGridsToLoad 5, moving near Riverwood): `EXCEPTION_ACCESS_VIOLATION` in
    `rml::internal::MemoryPool::getFromLLOCache` (EngineFixes' tbbmalloc) under `BSSpeedSamplerModifier` / `hkbBehaviorGraph`
    "0_Master.hkb". This is a known engine bug.
-   **A combat group's null target array, `SkyrimSE+0x804396`** (three times: 2026-10-21 twice, 2026-10-22 once in run vp19 at frame
    7240, all under `CS_DCLF_TEST_MOVE` on the tree-forest route, about one run in ten there). `GarbageCollector::Add` ->
    `Actor::StopCombat` -> `FUN_140558500` -> `FUN_140804330`, which removes the actor from a combat group's targets
    (`+0x20` data, `+0x30` count): the count is non-zero and the data null (`cmp [r8+rcx*4], ebx`, r8 = 0). An engine job thread,
    no Community Shaders or DCLF frame; the route's wildlife enters combat as the player walks into the forest. Dumps:
    `vp1-804396.dmp`, `vp19-804396.dmp` (session scratch folder). Not chased; a run with DCLF off (`CS_DCLF_TEST_TOGGLE`) would
    settle it if it ever matters.
-   **A corrupt heap in Engine Fixes' allocator under a face morph blend** (once, 2026-10-22, run wr4 at frame 12400, walking out
    of Riverwood under `CS_DCLF_TEST_MOVE`): a write through a bad pointer (`rcx = 0xb5410000`) inside EngineFixes' allocator, from
    `MemoryManager::Allocate` in `FUN_140e47ef0` <- `FUN_140e24ef0` (it blends two geometries' morph data, `+0x130`) <-
    `Actor::sub` (`0x1402e8510`), an engine thread. No Community Shaders or DCLF frame, but the heap was already corrupt, so
    whoever corrupted it is unknown. Dump: `wr4-enginefixes-heap.dmp` (session scratch folder). **Next:** if it recurs, a run with
    page heap or DCLF off would separate the two.
-   **A null shape in a phantom's collision agent, `SkyrimSE+0xa8497f`** (once, 2026-10-23, run xalphathreshold, the Whiterun save
    turning in place, 40 s in): `call [r10+8]` with r10 0 in `FUN_140a847a0` <- `hkpSimpleShapePhantom` <- `hkpConvexListAgent` <-
    `MagicCaster::UpdateImpl`, a job thread. It came in the first runs with `SWF_HOOK_HAVOK_INIT`, whose handler only reads both
    nodes' bodies; four other runs with the hook did not crash. Log: `crash-2026-10-23-02-36-05.log` (session scratch folder).
    **Next:** on a repeat, a run without the hook's subscription.
-   **Havok null write in `FUN_140b660c0` at uGridsToLoad 21** (the Whiterun stress save under `CS_DCLF_TEST_MOVE`). Radius 21 is
    unstable on its own, so runs there are judged by the report lines written before the crash.

## Open hangs

-   **The first save load never finishes, with `set resync` flooding the log.** It happened once in about 35 launches, on 2026-11-02
    (run `clean1`, killed by the runner's 170 s timeout). The run's scripted VRAM snapshots were never written. Presents stayed at
    60 fps with no true frames, and dedicated VRAM held at 8,359 MiB. `CommitMembershipEvents` logged 6,276,331 lines, about 770 per
    scene frame (frame 8176 at the end). The lines sampled read `0 objects, scene not built: new tables or a change log the cursor
    cannot read on`. Four other runs of the same build loaded normally. The flood may be a symptom, and the cause is undetermined.
    The build includes the shadow slot sizing change, which that path does not touch, but one occurrence cannot clear it. **Next:**
    if it recurs, keep the log and take a dump with procdump to see what the load is waiting on, and why the set commits so many
    times a frame with no scene. That run wrote 900 MB of log, so the line is now capped at 8 per 600 scene frames, and the next line
    logged counts the ones skipped. `setStats.resyncs` still counts every resync. The line itself is a load's: while a load screen
    is up, each scene pass resets the slot tables (`BuildScenePhase`), which moves the tables' generation, so the next commit
    resyncs. So the flood says only that passes kept running through a load that never ended. Not reproduced since: about 105
    launches up to 2026-11-06 (70 of them that night) loaded normally. The test build notes now ask testers for a Task Manager dump
    of a hung game.

## Frames with DCLF's objects missing under standing claims

-   **A resolution or upscaler change leaves one colour pass undrawn.** After a graph build or a main-resource change, the frame
    waits at its start for a current snapshot (`IndirectDraws::AdoptCurrentSnapshot`). The main resources, though, are made at the
    colour epoch's `Setup` (`Core/Render/Epochs/MainEpochs.cpp`), so a change is only seen mid-frame. That frame's colour epoch is
    not submitted (logged `main resources made at frame N's colour pass ...`) while its claims stand, so nothing draws DCLF's
    objects for one frame. A resolution change can cost two waits, because the views rebuild their imports at
    `FrameStartBeforeCoverage`. **Fix direction:** detect the change at the frame's start, before adoption (from the render-target
    state the colour capture reads, or the upscaler's render size), so that the frame builds and waits like any other build. The
    alternative is to record the frame's colour epoch synchronously for the new resources.
-   **A target format change: no DCLF epoch until the rebuilt pipeline catalog is installed.** After `SetTargetFormats`,
    `FrameIndirectState()` stays invalid until a publication carrying the new catalog is installed. Until then, `RunEpoch` returns
    before `Setup` (`indirect state invalid`) while the claims stand and the snapshots look current, so DCLF's objects can be
    missing for those frames. Waiting on the catalog at the frame's start could exceed the 5 s bound while the pipelines compile.
    This needs its own design: for example, the old catalog keeps drawing into compatible formats until the new one is installed.

-   **Objects changing cells flickered or vanished (fixed 2026-11-18).** A reference crossing into another cell is moved by one
    `NiNode::AttachChild` on the new cell's node, which first detaches the child from the old one. The detach and the attach were
    pushed as separate events, and the detach woke the scene pass, which often drained it alone: every part of the actor was
    erased and tracked again as new one pass later. The new entries had no slot or binding, so they were withheld while they
    waited for pipelines and materials, for 10 to 70 frames at Riverwood (`CS_DCLF_CELL_TRACE`, runs x2g and x3n: 8 of 9
    crossings split). SceneWriterFramework now flags that detach `SWF_CHILD_REPARENT` (ABI minor 4), and `SceneTracker` holds the
    move's events on the moving thread until its attach pushes them together. The move then applies in one batch, the entries keep
    their slots and are re-filed under the new category node.
-   **A drawn object rebound to a binding not built yet was hidden (fixed 2026-11-18).** Any change that picked a pipeline or
    material not ready yet took the member out of the set while it was still withheld from the engine, so nobody drew it until the
    pipeline compiled. At an actor's first cell change 3 to 8 parts vanished for 4 to 10 frames: its fade sat at 0.983 (one
    fade step, "Actors resting at a fractional fade" below) on the blended fading pipeline, and the crossing reset it to 1 and picked the opaque one. A drawn member
    now keeps its record, claim and current binding while the next one is staged (`Coordinator::StageBinding`, which references its
    pipeline and material so they get built), and the joins switch in the publication that can draw the new one. The rebind of
    every resident after a membership witness change goes the same way instead of ending all residency. In the fading case the
    old binding draws a few frames of the blended pass at alpha 1: no G-buffer write, so no SSAO, screen-space shadows or GI and
    no decals on it, and no shadow-casting lights, as in the engine's fading pass. A fading join also asks the pipeline lane for
    its settled key (`RequestSettledPipeline`), so the switch usually finds it built. Runs fxg and fxn: 19 crossings, none losing a
    part (Alvor's in fxn came while his parts still waited after the load, below).
-   **A newly attached object shows 3 to 7 frames late (open).** In guest and native mode the engine's draw of a lit object draws
    nothing (the flush latch skips it, native's D3D11 calls do nothing), so whether DCLF withholds it changes no pixel. An attached
    object (a tankard, Alvor's hammer) joins the set in the scene pass that tracks it, but a publication is installed about every 2
    to 3 frames and is 2 to 5 frames old by then (`[publication of frame N]` in the cell trace). The one frame of run x5g with no
    engine pass for the hammer was its attach frame, before the stand-in walked the admitted entry again. **Fix direction:** a
    shorter path from the scene pass to the installed snapshot.
-   **Actors after a load are drawn by nobody for 1.5 to 4 seconds (open).** The load hold settles before the actors' 3D is
    tracked (records 0 on its last frames), and their first bindings (the fade-in's blended keys) then wait for pipelines compiled
    on demand, about 100 frames in guest and up to 260 in native (runs fxg, fxn, fig). The `drawn by nobody` count's first window
    after a coc reads about 30,000 passes in guest. **Fix direction:** the hold also waits for the nearby actors' parts, and their
    keys are asked for from the attach records during the load (Phase 5.5.1's sixth step).
    **Update (2026-11-23):** the engine had given every nearby actor its 3D by the hold's first world frame in every load measured
    (`actors without 3D: 0 at the first world frame` in the hold's line). The hole was the hold settling on a publication that did
    not hold the load's objects yet: `Settled` took a commit two frames after the world's first frame as the load's, and the scene
    pass can lag more. One native tour's `coc WhiterunDragonsreach` settled after 9 frames and then drew 365,515 lit passes by
    nobody over 187 frames. The hold now also waits while a lit main-view pass it withholds would be drawn by nobody under the
    frame's claims (`PassCapture` `holdNobody`: an object waiting to join, or one outside the set where the engine's lit draws
    draw nothing; the player's own parts are left out, since some stay the engine's). Its line adds `passes the claims did not
    draw N frames (at most N a frame)` and, at the release, the last frame's first undrawn geometries. Guest interior and river,
    native Vulkan and D3D12 tours: every hold settled (first load about 10 s, Dragonsreach 3.2-3.7 s, others under 2.2 s), and
    the windows after it read at most about 2,300 passes by nobody, from objects attached after the load. **Left:** those late
    arrivals (the next entry's latency).
-   **A re-equip split across scene passes erased and re-added the item (fixed 2026-11-18).** NPCs re-equip by a `DetachChild`
    and an `AttachChild` of the same node on the engine's actor threads (scabbard and dagger, bow, arrow and quiver after a load and
    when an NPC's 3D finishes loading). The detach woke the scene pass, which erased the geometry before the attach tracked it
    again: 5 to 10 in every Riverwood run (`[DCLF] split move`). `SceneTracker` now holds a plain detach outside loads, pinned, until
    an attach of the same child pushes both (applied as a move) or the next frame's start pushes it as a removal. Runs fxg and fig
    had none. Run fxn had two static ones at startup and fqn two 'Land' blocks detached at frame 59 and attached at frame 61, which
    stay a removal and an add.
-   **"Sometimes parts do not come back" (not reproduced).** No part stayed drawn by nobody and every actor's tracked count matched
    its held geometry at the end, over runs l1g, fxg, fxn and fig: 31 crossings and about 36,000 actor-frames at Riverwood and in the
    Bannered Mare, plus 47 crossings before.
-   **Actors resting at a fractional fade (fixed 2026-11-18).** After a load, some actors' fade nodes hold 0.983 (guest, 60 fps),
    0.964 (native, about 28 fps) or 0.950 (20 fps) for good, one frame's fade step below 1. The mirror reads that value, so their
    parts bind the blended fading pipelines (hint 9) while DCLF's own fade state is 1 and draws them at alpha 1. A placement snap
    or reseed (a cell change) clears it. Runs b5r1 to b5diag7g and the tour config in the bug5 scratch folder.
    -   **Vanilla rests at exactly 1.** `FUN_14147a160` (AE 1.6.1170) steps `+0x130`/`+0x140` toward `fadeAmount` by
        `min(dt / time, 0.1)` and stops at it. The snap applies only above 1.5 (`0x141ad2874`) and gives `fadeAmount - 2.0`
        (`0x141ad288c`), so 0.983 is no snap offset. Ghosts use the snap. Their `fadeAmount` ramps to 2.25 and they rest at 0.25
        (actor probe: node 2.250, mirror 2.250, root row 2.250).
    -   **The handover.** The seed copies the node at the start of the frame that first draws the root's generation. The engine's
        own `OnVisible` for that frame's cull still runs on the node, because the writer tables that stop it are published at the
        next `Main::Update` entry. Run b5diag3 caught it for Hod. The seed read `cur 1.0`, `+0x140 1.0` and `fadeAmount 0` (alpha 0
        for one frame at the load), then the engine stepped the node to 0.9639. DCLF's state runs on from the seed, and only a reported
        milestone brings the node in line. `ReportMilestone` took the seed's milestone as already on the node, so a state that
        stays at 1 is never written. **Fixed:** a new generation's
        first update always reports (`FadeStateCS.hlsl` and the CPU stage's `Report`).
    -   **The fade write-back delivered nothing (fixed).** Runs b5diag4 to b5diag7g (guest and native) counted about 1,400
        harvests with 0 events and 0 stores, so every DCLF-owned fade node kept what the engine left at the handover. The graph's
        order was right: a probe word FadeStateCS wrote reached the copied list in the same frame, after the header upload and before
        the copy (run b6d1g). No event was ever appended. `ReportMilestone` guarded the append with two early returns on an `||` of
        equalities (`EventsIndex == 0 || ReportedIndex == 0`, then the written bit or an unchanged milestone), the pattern the
        RX 6600 compiler miscompiles (TextureTransformsCS, the portal programs). Adding counters ahead of the guards, with the logic
        unchanged, made the events flow (run b6d2g: 13,435 events). The guards are now one relational compare and a plain `if` around
        the append. Guest run b6fixg: 10,288 events and 10,280 stores in the first window after the load (the first-update rule makes
        every root report once, 3.3 ms of stores in all), then 2 to 32 a window. Native run b6fixn: 10,531 events, 10,278 stores.
    -   **Result.** The actor probe at rest finds 0 fading actors in guest (b6fixg, b6fin, b6fin2) and native (b6fixn). Before, native
        held up to 9 actors at 0.964 and the actor walking in (1A010003) stayed at 0.90 to 0.93 in both modes. An NPC placed in front
        (`player.placeatme 000A2C8E 1 150 0`, run b6fin2) still fades in on the fading pipelines (lowest fade 0.915, 0.829, 0.764 at
        5 to 40 frames) and rests opaque. The ghost (`000725F6`) rests with its node's fade at 0.250 in both modes, DCLF's state and
        the engine's own snap value, and draws translucent (run b6fin, shot ghost240). Its `+0x140` keeps the engine's last step (0.07
        to 0.13): the store writes only `+0x130`. Console `enable` has no fade, so the bug5 config's disable and enable cycles never
        tested a fade-in.
    -   Also changed: the fade amount and fade snap writers push the mirror update before the key, and the scene pass drains those
        keys before the tracker. A key could be applied against a mirror without its value, leaving the root row at an older alpha.
        Not seen in a run.

## Memory retention

-   **VRAM retention after leaving an exterior: mostly fixed, about 0.9 GB outside DXVK left.** The 2.5 GB that DCLF held after
    leaving an exterior was DXVK chunks that defragmentation could not move (every import locked). Pinless imports fixed it on
    2026-10-29: dedicated VRAM at the end is now 10,997 MiB with DCLF against 11,308 MiB without (the table below). What is left
    is listed under "Still open". The history: on the short retention tour (save, `coc Whiterun`,
    `coc RiverwoodSleepingGiantInn`, `pcb`, 45 s in the inn; `tour.sh` in the session scratch folder, runs p1onT3 and p1offT3 on
    2026-10-28), Task Manager's dedicated usage ends at 13,854 MiB with DCLF and 11,308 MiB without. With DCLF it does not drop
    after `pcb`. The DXVK allocation trace (`DXVK_ALLOC_TRACE=10`) finds no D3D11 difference:

    | After `pcb` | DCLF on | DCLF off |
    |---|---|---|
    | Live device-local, total | 8,464 MiB | 9,129 MiB |
    | SkyrimSE.exe | 4,513 MiB | 5,178 MiB |
    | CommunityShaders.dll | 2,608 MiB | 2,608 MiB |

    So the difference is Vulkan memory the trace does not see. The candidates are DXVK keeping freed chunks resident after DCLF's
    higher peak, and ORG/BasicRHI's own allocations. The engine-reference retention that held about 5 GB of game textures is fixed
    (`dclf-scene-lifetime-survey.md`, section 12).

    **Cause (runs dxon2/dxoff2):** DXVK's chunk statistics (`describeDevicePoolChunks` in the alloc trace) show that defragmentation
    cannot run with DCLF on.
    -   DCLF on, after `pcb`: all 33 device-local chunks hold locked allocations (3,150 of them), and 4,564 of 7,636 MiB is used.
    -   DCLF off: no locks. Defragmentation shrinks the pool from 38 to 24 chunks.

    Every ORG import (`GetResourceInfo` → `ensureBufferAddress`/`LockImage`) locks its resource's address for the resource's whole
    life. About 2,300 of the 3,150 locks are live imports (1,795 buffers, 523 textures); the rest are resources DCLF no longer
    imports. The peak was 11,702 locks.

    **Fixed (2026-10-29): pinless imports.** DCLF's game imports no longer pin anything. The DXVK fork tracks them
    (`dxvkTrackInteropResources`) and reports each move with the old storage (`dxvkPollInteropRelocations`). DCLF takes the moves
    at frame start (`Core/Gpu/ImportRelocations`) and imports again:
    -   textures: the import is superseded and its slot detached; material bindings resolve again;
    -   buffers: the entry goes and the geometry slots go stale.

    The old storage is attached to the old import's owner and released behind the GPU.

    A few things stay pinned:
    -   images and buffers the GPU writes (render targets, depth, storage);
    -   the Resources.cpp imports, which still use `GetResourceInfo`.

    `CS_ORG_PINNED_IMPORTS=1` restores the old pinned path.

    | After `pcb` | DCLF on, pinned (dxon2) | DCLF on, pinless (pinon4) | DCLF off (pinoff2) |
    |---|---|---|---|
    | Device-local chunks | 33, all locked | 25 | 24 |
    | DXVK allocated / used | 11,574 / 8,439 MiB | 9,398 / 7,479 MiB | 10,082 / 9,105 MiB |
    | Dedicated VRAM at the end | 13,742 MiB | 10,997 MiB | 11,308 MiB |

    Still open:
    -   **Outside DXVK.** 2,167 MiB with DCLF on against 1,285 off (BasicRHI's allocator); process usage is 11,565 against
        11,367 MiB.
    -   **Imports that don't follow a move.** Grass, tree LOD and shared bindings keep their superseded imports. That is correct,
        because the old storage stays valid, but it holds the old storage, and its chunk, until they let go. Reported as "old
        storages held" in the "pinless imports" line and the alloc trace.
    -   **A release stall under stress only.** With `DXVK_ORG_RELOCATE_STRESS=64` (run pinstress2), publications 11136 and 11140
        kept their holders from 23:31 to the end, and engine references stopped releasing. VRAM was over budget and the frame rate
        was 12 fps. Not seen in the realistic runs; investigate if it shows up without stress.

## Payload and table defects

-   **An entry write the incremental visibility pass skips.** Under motion, `CS_DCLF_PERSISTENT_PARITY` reports
    `<- WRITES DIFFER; first: root N set wide (reference's alone)` in some windows. It reproduces every time with
    `CS_DCLF_TEST_MOVE` and `CS_DCLF_PERSISTENT_PARITY` set. The suspect is `JudgeEntry`'s skip (`Core/Render/Readback/Visibility.cpp`).
    It skips when `entryWritten` already holds the wanted value and `entryWrittenVersion` is unchanged, so a write made while the
    entry was in another state can hide a later one.
    **No longer verifiable:** 0 in ten moving full parity runs on the tree-forest route (vp1-vp10, 2026-10-21), with the check active.
-   **A material row is keyed per material but packed per pair (latent).** `MainRows::materials[m]` is one row per material slot,
    packed against the pair's pipeline. Two shader permutations of one pass, or a `ModifyShaderLookup` that changed between them,
    could differ in their `PerMaterial` offsets. The row would then keep the last pair's packing, and `ResolvePair`'s
    `material.tables != blocks.tables` guard cannot fire. There is no evidence that it occurs. `Policy::CheckRowPacking` guards it
    (`<- ROW PACKING CONFLICT` in the `[DCLF] main pairs` line, under `CS_DCLF_PERSISTENT_PARITY` or `CS_DCLF_BUILD_PARITY`). If it
    fires: key rows per (material, packing).
-   **No readback check of the material row bytes.** BuildDraws parity checks the rows' addresses only. Nothing reads back a
    backing's material rows and compares them with the kept rows, which would catch a part that the four-part journal
    (`kMaterialRowParts`) failed to send. The extras have this check per backing; the rows do not.
-   **Texture transforms reach epochs before the Z-prepass a frame late.** `cs.dclf.z.texture-transforms` runs first in the
    Z-prepass epoch. An epoch that reads the main material rows before it (the reflection, Skylighting's occlusion) draws with the
    previous frame's `TexcoordOffset`, effect falloff and base colour. No GPU readback checks the patched bytes. Comments in
    `Core/Scene/Model/Tables.h` and `Core/Shading/MaterialPortFrame.h` still name the removed `RefreshMaterialTransforms`.

## LOD gates

-   **A flipped incoming geometry missing from its flip's publication.** In one of four runs with `CS_DCLF_TEST_MOVE`
    and `CS_DCLF_SET_PARITY` set, `TakeDeltas`' check reported
    `1 flipped incoming geometries not claimed by the flip's own publication ... <- GATE HOLE (publication)`. If real, the incoming
    block is not drawn while the outgoing one is already hidden, for the frames that install that publication. Run vl1 (the validation
    layer on, so about 300 slow frames of gameplay after the load) reported 85 in its one window, right after the load: 96 releases
    there came before DCLF drew at all and 14 flips were deferred for an incoming record not yet published. The release-side check
    (the claims the releasing frame installed, the one that shows on screen) read 0 incoming not claimed of the 19 releases it
    checked. So the publication-side count includes flips whose incoming records a later publication claims before the release.
    **Next:** count publication holes apart for flips that were deferred, and judge by the release check.
-   **A hold's known effects.** During a hold, both terrain levels' LOD water registers, and a held block's segments are not
    updated. The map's hide-all is handled by flushing. These effects have not been measured visually.
-   **DynDOLOD LOD shapes without the LOD flags may be drawn by nobody (unconfirmed).** `Eligibility::Static`
    (`Core/Scene/Model/Eligibility.cpp`) admits a `BSSubIndexTriShape` only when its shader property carries Features/Lod's
    `kLODObjects | kHDLODObjects`. Ordinary object LOD (`.bto`) has them and DCLF draws it. DynDOLOD's 3D tree LOD and other
    passthru LOD shapes inside its `.bto` files keep the full model's shader flags, so they come out `Ineligible::NotTriShape`; in
    guest the flush latch then skips the engine's draw, and native has no engine draw. Found by reading the code
    (`docs/development/plans/dclf-tree-wind-object-lod.md`); the test profile has no DynDOLOD output, so it is unconfirmed.
    **Decision (2026-11-18):** admit every LOD shape under the LOD roots, flags or not, kept non-casting (`IsLodObject`), with a
    matching rule for the water reflection. **Next:** confirm on a DynDOLOD install with `CS_DCLF_CENSUS=1` (look for
    `NotTriShape` sub-index shapes under `lodLandRoot`), then admit them.
-   **Tree Wind does not sway object-LOD trees in DCLF.** Blocked by the entry above. Decided design: a buffer owned by Tree
    Wind, read by device reference from record words 4-5, with the bend data built by background jobs under engine-read-window
    leases (plan: `dclf-tree-wind-object-lod.md`). **Improvement target:** that design copies vertices and computes bend data on
    the CPU. A better one would avoid the copies and the CPU work altogether (for example, derive the bend weights on the GPU from
    data DCLF already uploads), and the whole area should be revisited before or instead of building the copy path.

## Grass drawn by the engine on the first takeover after a load

The `grass draws` line counts the engine's grass draws (Grass Optimizations' draw hook) made while DCLF owns grass, and flags them
`<- GRASS DRAWN TWICE`. In every turn-only run on 1.7.104 (2026-10-24, runs grass2-grass7) the count is 0 in every window except
the first after the save loads: 180-360 engine grass draws (about 8 frames' worth, against 44 a frame while the engine owns grass)
between DCLF's first two frame starts with grass owned, while DCLF made one ready depth commit. They come from passes registered
before the decision (none reached the withholding unwithheld), so they look like renders DCLF's frame driver does not see during
the load's first frames. At most that one frame is drawn twice; mid-run takeovers and releases (`dclf:grass=0` / `=1`) are clean
both ways (no overlap, no hole). **Check:** a turn-only run's first `grass draws` line.

## Failing or flaky parity checks

-   **Extras parity after `coc` teleports (`extras parity ... <- DIFFER`).** On the 1.6.1170 heavy save's teleport route (nine `coc`
    stops, `CS_DCLF_PERSISTENT_PARITY` + `CS_DCLF_SET_PARITY`), one or two report windows right after an exterior load find hundreds to
    thousands of objects whose completed extras row is 0 at component 11 where the engine's routines give a value (terrain and mountain
    pieces: `MountainTrim02:4`, `Block (1, 0)`; largest difference up to 262144). It happens with `CS_DCLF_SHRINK=off` too (tp7,
    2026-10-28), so the shrink policy is not its cause; the windows after it are clean. Not chased yet.
-   **Set parity: freed object slots stamped drawn by the GPU (`<- SET PARITY`).** In 3 of 8 moving full parity runs on the
    tree-forest route (vp3, vp6, vp7), always in two consecutive windows around frames 3360-3700 while cells stream in front of
    the player: 5-18 "depth without colour", 8-23 "colour without depth" and 14-62 "drawn outside the set" a window. Every sample
    is a slot with no geometry in the snapshot's tables (`'?'`, not bound, both builds `absent`), and the slots come in pairs
    (14988/14989, 15052/15053, 15116/15117). BuildDraws writes a visibility word only from an input's `objectIndex`, so the GPU's
    input list named slots the CPU's builds did not. The payload's tables are the snapshot's own (checked in vp7), so it is not a
    payload built from newer tables. If the GPU's inputs are stale (a region upload or journal gap), it draws freed objects'
    records for those frames. Since then (2026-10-22, 120 s runs vp11-vp18) it showed in 4 of 8 runs, now one slot a window at
    most, the same way. Established by TEMP parity-only diagnostics, since removed (ORG cleanup, 2026-11-20):
    -   no CPU list named the slot (neither segment's resident region nor input list);
    -   the installed publication's inputs backing, copied after the colour epoch with the words, did not hold it anywhere in
        its 16,384 entries (vp17, slot 174, frame 4997);
    -   the commit paired the payload with the publication made from its body (0 mismatches), the inputs backings' catch-ups
        matched the whole list (0 differ), and no segment committed a frame number twice (0 repeats in vp18).
    So the word was written with this frame's stamp from an input list that is not the installed publication's as copied.
    A per-segment writer record (`Resources::writers`, BuildDrawsCS's `RecordWriter`) was added next and did not reproduce in
    run wr4 (2026-10-22, about 12,400 frames walking out of Riverwood). The set parity's "drawn outside the set" line still
    reports the symptom. To dig further, restore the writer records from git history (before `refactor(dclf): drop set parity
    TEMP records`).
-   **Extras parity on terrain and object LOD after a load (`<- DIFFER`).** In run tog9 (2026-10-25, MO2 1.6.1170 heavy
    exterior, `CS_DCLF_PERSISTENT_PARITY`), the first two windows only: 97 of 1111 and 39 of 1391 objects, largest difference
    262144, first samples `Block (3, 0)` (component 2: completed -30720, reference -4710.4) and `objsnowHD-LargeRef`
    (component 7: completed 0, reference -49152); every later window 0. The first window ended before the run's first pipeline
    switch, so the CPU/GPU toggling there is not its cause. Not seen in v3-full on the same save. Seen again in run fix-full
    (2026-10-26, the 1.6.1170 heavy setup's grass save, turning, persistent and set parity): two windows, 57 of 591 and 167 of 701
    objects, first samples `objsnow` and `Alpha` (component 7). Not investigated.
-   **Build parity's decal check reads only the classes' decal sub-ranges.** With the plain decal buckets
    the decals go to the class plan's decal buckets, which `DrawnDecal` does not read, so the check reports
    every slot culled ("87 slots, 87 culled"). The colour and terrain draws' plain buckets were added to the comparison on 2026-10-22
    (it had compared nothing since the plain buckets came in). **Next:** read the decal buckets the same way.
-   **Accumulated parity on terrain and object LOD geometry (`<- ACCUMULATED DIFFER`).** In run vp10 (`CS_DCLF_ACCUMULATED_PARITY`
    with motion on the tree-forest route), every window: about 60-110 of 29,000 engine-culled-root geometries set by DCLF alone and
    80-180 by the engine alone, all `Land`, `obj`, `obj-LargeRef`, `objsnow` and `objsnowHD-LargeRef` (terrain blocks and object
    LOD), in runs of consecutive frames. Stood-in roots and the animation gate agree (`anim rate`: 0 differ). These geometries are
    not animated, so the bit only matters to whatever else reads `kAccumulated` on them. Not investigated.
-   **Capture parity mismatches under `CS_DCLF_CAPTURE_PARITY` with `CS_DCLF_PARITY_BOTH`**, in every window. Since the observer fix
    (run cp1, tree-forest route) 7-10k of about 340k draws a window (0 material, 3-4k per-geometry, 0 technique); before it about 44k
    of 153k (29k material, about 300 per-geometry, 7k technique), and every LODObjects, LODObjectHD and LODLandNoise draw. The cause, by field:
    -   **Observer fixed (run cp1: 0 material and 0 technique mismatches in every window, of about 340k draws checked):** `CompareMaterial` no longer compares PS PerMaterial 29 (IBLParams, kept from
        the first evaluation by `KeepUnreadFloats` and read by no Lighting stage) or VS 11 (`TexcoordOffset`, which the GPU patches
        into the rows), and the technique compare leaves out the fog (VS 13-15 and PS 19 FogColor, kept by `KeepTechniqueFog`).
        A port's declared frame variables (`MaterialSpace::declaredVSFloats`, `declaredPSFloats`) are not compared by
        `CompareMaterial` (Lighting's layout only), so nothing was changed for them. What remains is 7-10k mismatched draws a
        window: about 3.5k per-geometry and the rest below.
    -   **Loaded terrain blocks drawn with another technique.** `Block (x, 0)` draws with DCLF's MTLandLODBlend (19) pipeline where
        the engine's pass is MTLand (8). This is permutation parity's `vertex 1B000000`, about 900 a window. It is a real
        normal-path difference, not yet chased.
    -   **Not attributed:** permutation parity's `pixel 800000` (about 7.5k draws) and `feature 1C0`, PerGeometry differences on
        facegen tint parts (PS 7) and TextureProj (VS 6), character light t11 on NPC armour (DCLF's frame view against the draw's,
        which the engine alternates), and draw parity's 0-15 differing draws a window.
    -   **Expected:** face capture parity differs on every reflection face draw, because face draws do not match the main rows.
-   **Frame values' texture transforms (parity): frame variables drift (`<- DIFFER`).** Seen in vp5 and vp6 (not vp1-vp4), 1-62 of
    about 14,000 entries in a window, all `frame variable` entries (an Effect material's FalloffData or BaseColor), by small amounts:
    `sent (1, 0.4443106, 0.81752133, 0)`, `now (1, 0.44422826, 0.8172912, 0)`. The check reads the buffer again at the producer's end,
    so a controller that writes a declared frame variable inside the window shows here: the same race as `<- MATERIAL PORT`'s
    FalloffData below. A frame's lag on an animated effect; not visible.
-   **Frame values' moved-only sampling: rows differ (`<- DIFFER`).** In vp1 9 of 1,257 rows (first `row of slot 12678
    ('leftarm:0', not due): sunEntry, lodFadeNode`); in vp2 and vp6 one row a window, always a small loose clutter item not due
    (`'CoinBagMid:1'`, `'Basket04:0'`) with `sunEntry, lodFadeNode` differing: likely an item moved (Havok) without a move event, whose
    sun entry and fade node the sampling kept. The sun entry only selects its shadow cull, so a frame's stale entry is not visible.
    The line after a differing check (`not due yet changed, by owner`) names them: on 1.7.104 (2026-10-23) `BloodyBone`, `skull`,
    `LeftFoot` (MovableStatic, Misc), 0.01-0.1 units, in 2 of 23 windows. (The far larger counts moving runs showed on SE and AE,
    80-85% of windows, were the near trees' update pass: fixed, TreeWalk.h.) The Havok node transforms behind most of them ran inside
    the window ("Animated cell object update", skyrim-engine-notes.md): its update now waits for the sampling (WindowWriters). What is
    left (2026-10-24, 1-6 roots in a window or two a run, and `<- DIFFER` of the rows against the engine in one window of 31) is the
    root's world bound alone (sun entry, fade node; no geometry's world changed), 0.02-0.04 units: a one-off settle, since a watch on
    the bound armed at the difference (`CS_DCLF_MIRROR_WATCH=moved`) saw no write in the next 600 frames. **Next:** a bound watch armed
    at the body part's capture (`Bloody*`, the arms and legs) to name the writer.

-   **Stand-in walk parity: three snow geometries missed at frame 868.** About one full parity run in three reports, in one
    window only, `3 geometries handed to the registration from entries the stand-in would not walk <- STAND-IN WALK`, always at
    `[frame 868]` of the fixed `CS_DCLF_TEST_MOVE` path, and always 'objHD-LargeRef', 'objsnow' and 'objsnowHD-LargeRef'. It is
    likely a cell crossing where the walk decided before the snow members joined. Normal play would drop these for that frame.
    **No longer verifiable:** 0 in ten moving full parity runs on the tree-forest route (vp1-vp10, 2026-10-21), with the check active.
-   **Walk parity: the cooking spit's sun entry moves without an event.** `CS_DCLF_WALK_PARITY` reports 5 of about 113,900
    objects differing per interval, all of them the sun entry on `FireSptiCookingBase:17`, whose bounds drift by about 0.06
    units. A writer DCLF does not watch animates the node, so the delta walk keeps the old bound. The same gap on a larger animated
    object would cull it wrongly. **Next:** find the writer (a controller, or Havok) and add it to the events.
-   **Mirror parity under lane mode flags late or missed node fields with movement.** This is likely asynchronous timing rather than
    a missed writer, but it is not attributed. (The collision-body misses were `bhkWorld::InitHavok`'s, now hooked: see the
    validation doc's engine bugs.)
-   **Mirror parity: a node's current fade missed (`node current fade`, 0-2 a window).** Run h1 (2026-10-23, the Whiterun save,
    turning): animation objects an NPC holds (`AnimObjectSellPotion`, `AnimObjectBucket`, `Weapon  (00013986)`), currentFade a
    fade step apart (0.9876 against 0.988), captured on a loader thread. Likely the cull's per-frame fade step between the probe
    and the fade update's event; `CS_DCLF_MIRROR_WATCH=parity:current` can name the writer.
-   **Mirror parity: single hidden bits missed (`node hidden`, 0-1 a window).** Run h1: `SHIELD` once, and once a node with no
    reference (which the compatibility monitor counted as an offender). Not attributed: `missed:hidden` names the next writer.
-   **Not reproduced on the Whiterun save:** walk parity's cooking spit, the frame values' moved-only clutter rows and
    `<- FADE INPUT` were 0 there (walk, moved-only and fade parity clean); they need the tree-forest route's save, where
    `transform:` and `fade:` watches can name their writers.
-   **`<- FADE STATE`:** 1-3 roots in some interior logs, one fade step apart (`currentFade` 0.982-0.989 against the port's
    0.990-1). It appeared with the actor pre-cull change (FadeStateCS's frustum test for preprocessed roots). **Next:** check the
    port's key and the preprocessed roots' last visible frame.
-   **`<- FADE INPUT`:** an actor root's row fadeAmount is 0.9947 against the node's 1, steady over three windows. Either an
    `Actor::SetAlpha` write was missed by the events, or another writer sets fadeAmount. As a result, FadeStateCS steps amountFade
    toward 0.995.
-   **`<- MATERIAL PORT`:** one difference a window remains on an effect's FalloffData (VS variable 7) after the retake, from a
    controller write the retake missed.
-   **`<- UNPINNED FADE NODE`** fires for requests still in flight at the report. It should fire only for a request left
    unanswered across reports.
-   **Light candidates parity: a dependent with no geometry row (`<- CANDIDATES`).** Seen once (run coc1, 2026-10-22, turn-only
    with `coc WhiterunBanneredMare` at frame 600 and `coc WhiterunDragonsreach` at 1500), in the first run after loads stopped
    rescanning the tracked set. About 30 s after the return to Dragonsreach, one root's light entry (`0x1f862fff100`) listed a
    dependent that the light table holds no geometry row for. It stayed so at every check until the run ended (1 then 5 of 10
    checks a window). The table rewrites an entry only when its verdict or signature changes (`UpdateLightCandidates`). Before
    the change, every load rebuilt the tables from nothing. The run before the change was not repeated. Nothing else failed in
    that run, and the death-reload run (die3) showed no such failure.
-   **`<- COMMIT` at a run's first frames** (`set commit parity at frame 4: 48 slots differ ...`, waiting false kept, true now) and
    **`<- GATE COVERAGE`** (`2 outgoing still claimed` at a gate's release): intermittent, in 6 and 2 of 66 runs' logs (1.7.104,
    2026-10-23/24). Not attributed.
-   **Long-standing flags**, each seen in some runs: `<- CUT`, the PS25 `<- MATERIAL PORT` race and `<- FADE`. (NO CASCADES fired in
    every interior window, where the sun does not accumulate: it now counts exterior frames only. Not seen on the tree-forest route.)
-   **Seen once, watch for a repeat:** capture parity's "2 tracked but excluded" while NPCs crowded the view (the report now breaks
    these down by their verdict; an `eligible` entry there is a verdict DCLF kept stale). In another run, every world particle
    system froze for one window ("newly frozen" in the particles line) right after a live `dclf:first person=1` toggle.

## Unattributed visual defect: flickering fog-coloured bands in first person

The user reported full-width horizontal bands of broken geometry, flickering all over the screen, at the Riverwood river view in
first person with billboards and first person on. The bands went away with first person off and with billboards off, but also on
other actions (equipping a sword, going to third person), so billboards are not established as the cause. The billboard observer
found no leaf turned differently from the engine. It has not appeared since. **Next time:** bisect toggle by toggle in first
person, empty-handed (first person, billboards, particles, effects), and read the bad draw's vertex inputs
(`CS_DCLF_TARGET_PROBE` or a GPU capture).

## Light Limit Fix's local shadow cache is off while DCLF is loaded

Upstream's cache (`LightLimitFix::EnableLocalShadows`, default off) rotates which shadow-casting lights the engine renders each
frame. It vetoes lights through its `BSShadowParabolicLight` / `BSShadowFrustumLight` `UpdateCamera` hooks
(`FilterLocalShadowCaster`, armed by its `CalculateActiveShadowCasterLights` detour). It also copies the maps the engine just
rendered into its cache at `EarlyPrepass`.

DCLF's light selection (`Features/LocalLights/LightSelection.cpp`) is a port of `CalculateActiveShadowCasterLights`. Its
`Visible()` reimplements `UpdateCamera`'s test instead of calling it, and runs after the engine's call returns, by which time
the cache's veto is disarmed. With the cache on, DCLF would draw the shadow maps of the lights it selects and the cache would copy
and assign slices for the lights it chose, so the two would pick different lights.

Until one of these is done, `LightLimitFix::LocalShadowsBlockedByDCLF()` keeps the cache's hooks uninstalled whenever DCLF is
loaded, and the menu explains why:
- DCLF takes the engine's actual selection instead of re-deriving it;
- DCLF's port consults the cache's filter, through an Integrations extension;
- the cache gets a DCLF-aware view of which slices were drawn.

The cache's per-light `LocalShadow` flag, its `FirstPerson` / `WorldEyePosition` StrictLightData words and its
`LocalShadowData` buffer are untested under DCLF. DCLF's own StrictLightData supplies only `RoomIndex` and `ShadowBitMask`.

## Shadow lights past the light mask's 32 are culled whole by the engine

A frame's shadow-casting point and spot lights are numbered as their `Accumulate` runs (`LocalLightCull`'s `TakeOrdinal`). DCLF
tracks a light's bits in a 32-bit mask, so a light past the 32nd gets no ordinal: no empty cull, no engine-only subset, and the
engine culls the whole scene for it (`WholeCull::Reason::PointPastMask` / `SpotPastMask`, counted in the primary cull's
report as `<- WHOLE ENGINE CULL`). It is the last light case without coverage. None was counted in the 2026-11-08 runs (Riverwood,
Dragonsreach, Whiterun); the point lights' line counts them as "past the mask's 32", which only showed before DCLF ran.
**Next:** a scene with more than 32 shadow lights in one frame (heavily lit interiors, light mods); then either widen the mask
(the bits' consumers: `NoteMainRegistration`, `SphereBits`, the accumulators' light bit) or give such a light DCLF's views and
an empty cull without a bit of its own.

## Engine-only shadow casters cost the light culls about as much as a whole cull

Measured 2026-11-08 in Dragonsreach (Dev-Fast, two point and two spot lights), with every actor's skins left to the engine
(`dclf:skin partitions=0`). Point lights' `Accumulate` was 42-47 ms per 600 calls with the engine-only subset, 45-54 with the
whole engine cull, under 1 with empty culls; spot lights 37-45, 36-46 and under 1. The subset walk itself is cheap; nearly all
of it is the engine registering the engine-only casters into each light. In normal play no caster was engine-only in those
scenes, so the cost only appears where something is: a skin over DCLF's limits, a buffer DXVK cannot import, or another lasting
refusal (`Eligibility::EngineCasterReason`). **Next:** find which shapes are engine-only in users' load orders (the log's first
16 `'...' stays the engine's (<reason>, ...)` lines, and `skin shape left to the engine` for skins), and fix the reason DCLF
refuses each, so they join instead of being registered by the engine every frame.

## Outside DCLF

-   **SE 1.5.97: legacy FullScreenBlur adapters are not installed.** Every SE 1.5.97 run logs "Legacy FullScreenBlur stage-count
    opcode contexts do not match the verified 1.5.97/1.6.1170 sequences" and installs no blur adapters, with or without DCLF.
    **Next:** compare the opcode contexts against the SE executable.

-   **The engine creating its render targets a second time is not survivable with DCLF on.** When a monitor turns off and leaves the
    desktop, Windows shrinks the borderless window, and the engine calls `BSShaderRenderTargets::Create` again (not `InitD3D`). With
    frame generation on, the game then crashed in the driver (`nvoglv64.dll+0E5A9FD`, null read) in `ShadowViewPass`'s `BeginPass`.
    With it off, it fell to 4-13 fps as `State::Setup` ran again and DXVK's device memory grew from 5.9 to 11.3 GB. The likely cause
    of the crash is `ImportDepthTexture` keeping a raw image of a shadow map the engine destroyed; that is unproven. The window
    procedure now refuses the resize (`WM_WINDOWPOSCHANGING`), so this path is no longer reached that way, and `[Hooks] the engine is
    creating its render targets again` in the log means something else reached it. **Next:** make the path safe if that line appears.
-   **FSR-FG hung once at startup.** One of 7 runs presented nothing from the wrap on, with a forced focus change in the same second
    as the swapchain recreate. No stacks were taken. It may have been the deadlock fixed in DXVK's `D3D11Initializer` on 2026-11-02
    (the main thread waiting on its lock, held by a texture thread waiting on the GPU). **Next:** dump stacks if it recurs.
-   **Frame generation is not separated from graph epochs.** `Streamline::EvaluateFSRFrameGen` and the DLSS-G path do not call
    `Upscaling::SeparateFromGraphEpochs()` (`render-graph.md`, "Hazard: external Vulkan work directly after a graph epoch"). The inn
    (`coc RiverwoodSleepingGiantInn`, DCLF on) showed no corruption in screen grabs with either method, on 2026-11-01. That is a few
    sampled frames, so the hazard is not ruled out. **Next:** watch the inn in motion before calling it clear. The hazard's cause is also unproven. The workaround stays
    until one of these resolves it: Streamline without `VK_EXT_descriptor_heap`, the graph's last command buffer resetting the
    descriptor-heap state, a capture with a tool that supports descriptor heaps, or a standalone reproduction for NVIDIA.
-   **AMD: FidelityFX writes `rw_luma_history` with an Rgba8 storage format over an R16G16B16A16 view.** Seen 2026-11-06 under
    the validation layer (Radeon integrated GPU, FSR): `Undefined-Value-StorageImage-FormatMismatch-ImageView`. It is inside the
    pinned `amd_fidelityfx_vk.dll`, so not ours to change. **Next:** check a newer FidelityFX runtime.
-   **AMD: the main target sampled in COLOR_ATTACHMENT_OPTIMAL for a few frames at the first graph build.** On 2026-11-06 (Radeon
    integrated GPU, driver 2.0.353, no duplicate limit), ten `VUID-vkCmdDraw-None-09600` on `kMAIN` all came in the window of the
    first graph build after the save loaded, then none in five minutes, including an interior change and its rebuild. It is the
    import's window: DXVK moves the main target into its attachment layout before `LockImage` pins it to GENERAL.
    Since 2026-11-08 the runtime opts in to resting layouts and nothing is pinned, so this window may be gone; it has not been
    re-run under the validation layer. **Next:** check with `amdval.env`, and pin at first description only if it remains under
    `CS_ORG_RESTING_LAYOUTS=0`.
-   **Physical Sky washes the scene out.** With Physical Sky on, a Whiterun exterior at noon is covered in a bright cream haze:
    distant terrain nearly gone, a flat pale sky, glowing silhouette edges. It happens in plain D3D11 mode, in the graph modes,
    and in upstream's own build (`a03587448`, 2.0.0-rc.1, checked by hand on 2026-11-19), so it is not from the sync or the ORG
    ports. **Cause (confirmed 2026-11-19):** Physical Sky's output is calibrated for a linear scene; with Linear Lighting off
    (Post Processing in Vanilla mode) its sky radiance, aerial perspective and 10 lux sun land in the gamma buffer unscaled. With
    Linear Lighting on, plain D3D11 looks normal. A candidate fix that turns Physical Sky off without Linear Lighting is on
    `fix/physsky-washout` (`2e0fdec49`), not merged. **Next:** waiting on the feature author on whether the combination is meant
    to work; left alone until then.
-   **BasicRHI's `frontCCW` under Vulkan.** `frontCCW` has D3D's meaning on both backends. SARP and BasicRenderer set it true for
    glTF content. Their D3D12 behaviour is unchanged, but they have not been run under Vulkan since the mapping changed.

## Graph-mode port gaps

Known gaps in the ORG ports of Community Shaders features, from the 2026-11-18 hard ports.

-   **A cold loading screen can lose its frame schedule.** With a cold shader store and a load that outlasts the intro movie, the
    UI stage's build after the targets are registered again is staged together with the post build, which compiles about 75
    ImageSpace programs one by one through `GetNow` at the graph's build ("Setup did not queue it"). That took 40 s in batch 16,
    and frame 167 of the loading screen had a kMAIN clear with no place in the schedule. Warm stores do not show it.
    **Next:** address with the shader precompile overhaul (queue the programs at Setup and compile them in parallel).
-   **(Closed) A cold cache makes a frame wait for DCLF's on-demand pipelines.** A new variant (for example HDR's Lighting variants in
    native Vulkan with frame generation) is compiled on demand while the frame waits; one took 13 s on 2026-11-19 and the hang
    watchdog fired at 15 s (`native-vulkan+fg-hdr`, passed once warm). The render thread must never wait.
    **Closed (2026-11-23):** that hang's stack (run `1119-174909-rv5`) was not a pipeline: Grass's `Setup` compiled its compute
    programs with `ComputeProgram::Load` inside `BeginFrame`. Those programs are `SetupPrograms` now, queued at CS's setup. A
    native Vulkan quick run on an empty store (`build.shader_cache=cold`, run `1123-134155-pascal-native-quick-cold-vk`) passed:
    the on-demand Utility compiles and shadow pipeline builds after the load all ran on the pipeline lane, and the render thread
    did not stall.
-   **Physical Sky's aerial perspective is its own full-screen pass.** The ORG deferred composite's push block holds 32
    words and uses 26; Physical Sky needs about 7 more and ViewAtmosphere 3. So the aerial perspective runs as
    `cs.physical-sky.composite` right after `cs.deferred` (phase 2, not yet run in the game). Minor cost: one more full-screen
    read and write of kMAIN and a second fp16 rounding. **Next:** fold it back into the composite when the composite takes a
    handle table.
-   **Intermittent crash in DCLF's material capture during a load (guest).** `guest+resize` on build `2fcb06441` crashed at the
    loading screen (2026-11-19 12:12, `crash-2026-11-19-12-12-42.log`): the render thread executed `0x1ffff`, with
    `MaterialPort.cpp:68` (`Capture(*a_material, ...)`) under `SceneCapture.cpp:707` on the scanned stack. Two reruns passed. A
    guess, not proven: a material whose reference count is still above zero while it is being destroyed (its vtable already
    gone). **Next:** check the material's lifetime against the capture in `SceneCapture`, and catch it with a hang/crash dump.
-   **Native Vulkan: Physical Lens's ghosts and starburst lose the device on the RX 6600.** Seen in `cs.pp.physical-lens.request-plans`
    with frame generation off (step 6's lead, `native-vulkan+fg-fx`, 2026-11-19). The lens image mapping and Camera FX are fine.
    **Next:** reproduce with `DXVK_DEBUG=hang` and the RGD crash flow on Vulkan only.
-   **Physical Sky's graph clouds are about 6% darker than D3D11.** Same frame, clear weather, Whiterun: guest's overcast sky
    reads 223 against D3D11's 239. Under the washout (Linear Lighting off) the clear skies already differ, so the gap is not
    pinned. First suspects: the ambient inputs (ground-reflection SH, ambient transport). **Next:** compare with Linear
    Lighting on.
-   **Physical Sky's lightning, storms, voxel clouds, local NDF, middle and cirrus layers and clouds in water reflections are
    untested on the graph.** Only clear weather at one spot ran (`clouds.toml`, `clouds-long.toml`).
-   **(Closed) A wrong JSON type in a Physical Sky setting crashes the game.** `PhysicalSky.enableVolumetricClouds=1` (a number for a
    bool) ends in a JSON type error. Settings must be validated (`.claude/CLAUDE.md`).
    **Closed (2026-11-23):** the throw came from the test harness's `setting` command, which called `LoadSettings` unguarded. It
    now refuses a value whose JSON type differs from the setting's (any number for a number) and puts the old settings back if
    `LoadSettings` throws. Every user path (the settings file, Baselines, scene settings, DevBench, undo) already caught it.
-   **A cold shader store makes boot slow with the clouds:** the 50 cloud programs compile before data-loaded, beyond the 120 s
    load limit on a first run. Warm per mode first.
-   **(Closed) Guest: Physical Sky's volumetric clouds kept the AP shadow, the fog's scattering and the sky tail on D3D11.**
    Phase 4 of `physical-sky-port.md` put the clouds on the graph (`cs.physical-sky.cloud-maps`, `cs.physical-sky.clouds`).
    The three read the shadow volume and mean by their published ids, and the D3D11 fallbacks are gone.
-   **(Closed) Native: no volumetric clouds and no lightning.** The native gate in `PhysicalSky::VolumetricCloudsEnabled`
    is removed. The lightning query reads its candidates through `RenderGraph::RunReadback`.
-   **(Closed) Guest: kMAIN goes non-finite with Exponential Height Fog on.** With the fog and its volumetric fog on, guest's image goes
    black with bright green-haloed foliage and stays so (NaN or inf through bloom). Fog off, native and plain D3D11 are clean.
    Pre-existing: build `87fcac5d6` (before ViewAtmosphere's port) shows it too. `CS_ORG_VIEW_ATMOSPHERE_DEBUG=1` shows kMAIN
    already non-finite before the post-water composite, and `=6` (pass-through) keeps the blobs, so the composite is not the cause.
    **Fixed (2026-11-19, build `1a0b9dde1`):** the fog's temporal history (`CS_ORG_FOG_DEBUG=8` turned it off and the image
    was clean). Guest memory is DXVK's recycled VRAM, so history texels never written held garbage, and the 0.96 blend kept
    it. The scattering shader now rejects history that is not finite or above 65504 (`3bd376e65`, also in D3D11), and
    readers get the volumes only after a run wrote them (`d451564b3`). Guest fog now matches native and plain D3D11. Where
    the unwritten texels come from is not proven. `CS_ORG_FOG_DEBUG=9` (clear the histories every run) is clean too.
    `CS_ORG_FOG_DEBUG=10` (clear every 600 frames by a one-shot client) hung 31 frames after world-ready; it is a
    diagnostic only, not looked into.
-   **(Closed) Native: the fog volumes hold almost no fog.** With the same settings plain D3D11 shows haze on distant hills; native shows
    none. `CS_ORG_VIEW_ATMOSPHERE_DEBUG=3` reads transmittance near 1 across the screen, so the integrated volumes the post-water
    composite reads are near identity. Before ViewAtmosphere's port native had no fog composite at all, so this was hidden.
    **Update (2026-11-19, build `0d994ca13`):** native Vulkan now shows the same haze as plain D3D11 at the same spot
    (`native-vulkan+fog` vs `d3d11+fog`), most likely since the post-water composite flag follows the frame (`b2b46322d`).
    Native D3D12 matches too. **Closed.**
-   **(Closed) A graph build reads PublishedResources off the render thread.** Once per run near loading:
    `[C] [ORG] PublishedResources used on thread N, not the render owner: its entries are not locked`. N is an async graph-build
    thread building the UI stage. The likely caller is DCLF's `DeclarePublishedRead` (`FrameBindings.cpp:136`), which calls
    `ShaderResourceVariant` and `ShaderResourceIndex` during a pass's declare; they read live, unlocked entries. **Next:** carry
    each entry's published state and view in the build snapshot and serve these two calls from it during a build.
    **Closed (2026-11-23):** last logged 2026-11-19 (`post-effects-integ-6b3pp`); none in the 1,204 guest and native runs since,
    after the frame revisions moved declarations to revision starts on the render thread.
-   **Physical Sky's `PHYSICAL_SKY` define in graph builds.** Removing the strip had broken DCLF Lighting and Effect pipelines
    (unmapped t63, t69, t110, s3, s4) in native Vulkan, Water's DXIL build (`ApSampler`) in native D3D12, and the ORG
    `ISSAOComposite` build in guest. Phase 3 of `physical-sky-port.md` maps them and removes the strip; not yet run in the game.
-   **`ORG_FRAME_PLACEMENT=1`: the image goes wrong after a frame placement change.** In `native-vulkan+resize` (2026-11-22,
    build `23b46168e`), the `grid` shot after the ViewAtmosphere grid change (`nearPixelSize`, `middleSlices`) came out dark in
    one run (mean 45 against 94), green with coloured lens-flare sparkles in another, and correct in a third. Placement off is
    correct every time; the `fog`, `fsr` and `taa` shots match. Every running graph had placed from the same frame placement
    version by the shot, so it is not graphs on mixed versions. The likely mechanism is a one-frame glitch when the graphs
    switch to the new version, poisoning eye adaptation and lens flare, which then take seconds to recover. Suspects: the
    descriptor rewrite of a re-placed resource while frames are still in flight, or a reader of an engine target outside the
    graphs that follows its new image only at the frame's end (`Native::FollowGraphTextures`). The switch stays off by default.
    **Next:** register test switches (engine targets as placement candidates; forced new heaps at a version change), since
    cs-test refuses unregistered `--set env.X`, and repeat the run several times with each. Details:
    `plans/org-placement-relocation-log.md`, entry 35.

## Accepted differences

These differences from the engine are known, measured and left as they are. Each entry names what would remove it.

### A root seen again is drawn for one frame with its old fade

**What.** The engine updates a fade root only when its cull reaches it (`BSFadeNode::OnVisible`). A root seen again after
more than a frame unseen is re-evaluated on that same frame, before the draw, and a root that should have faded out meanwhile
snaps out and is never drawn. `FadeStateCS` runs a frame ahead: builds read the previous frame's published states
(`FadeScene::FadeStatesRead`). So on the frame a stood-in root comes back into view, DCLF draws it with the state from when it
was last seen, and it disappears on the next frame.

**Measured** (teleport route with the camera turning). Over five 10-second reports, 2,126 roots were seen again with a fade
above 0, and 1,291 of them snapped out on that frame, each a one-frame draw under DCLF. At a fixed position, none snapped.

**Why it is accepted.** A root comes back into view when its bounding sphere first touches the frustum. The sphere encloses
the mesh with a margin, so on that frame the mesh itself has usually not reached the screen yet.

**What would remove it.** The depth build reading this frame's `FadeStateCS` output, ordered after the pass in the same epoch.
The snap out by distance is already applied: BuildDraws' first phase applies the fade-out distance test (`kObjectFadeTest`)
to every input whose root has no serviced `OnVisible`. That part has not been measured on the teleport route.

### Skylighting parity lags while roots start fading

**What.** The engine's reference render of an occlusion map (`CS_DCLF_SKYLIGHT_PARITY`) culls stood-in roots by their
nodes. The nodes carry the GPU's fade milestones only a few frames after the GPU state (the write-back's readback latency),
while DCLF's map reads the current state.

**Measured.** Every map was within 15 texels by more than 1/256, except in the first 17 s after a teleport, while the turning
camera brought newly loaded roots into view. There, a few maps differed by 2,500-19,000 texels, with DCLF always nearer.

**Why it is accepted.** The difference is the reference's latency, not DCLF's map. The engine's own readers of the nodes
(the tree LOD's crossfade) take the same few frames.

**What would remove it.** A parity check that skips roots whose milestone changed within the write-back's latency.

### Shading that changes during a frame is drawn from the next frame

**What.** An object's shading (MaterialData, EmitColor, SSRParams.w, Linear Lighting's emissive multiplier) comes from
FrameValues (`BindlessShading`, PS t121), sampled at the frame's start for the slots the walk named by their shading events.
The engine's own draw reads the property when it sets the draw up. So a value written after the frame's start reaches DCLF's
draws a frame later than the engine's:
-   a controller on the animation job, which runs alongside the render thread (a candle's flickering emissive);
-   a cull's `GetRenderPasses`: the alpha it leaves on a non-member's property, and the LOD fades a held fade node keeps on the
    property. A member's LOD fades are the draw's own (LodFadeFrame).

**Measured** (the Riverwood bridge, the camera turning). About 25 of 40,000 slots per check changed after the frame's start,
and every one was named by an event for the next frame. Capture parity counts the affected draws on their own line
("shading changed since the frame's start sampled it").

**Why it is accepted.** It is the user's decision: a frame's delay on an animated emissive or fade is not visible, and it
keeps the render thread out of the shading.

**What would remove it.** Sampling the named slots again just before the colour epoch, on the pool, into the frame's shading
backing. That puts a pool job back on the colour epoch's critical path.

### Tree LOD instances the engine leaves hidden after `allVisible`

**What.** The terrain manager's node update (`FUN_140510730`) sets a tree LOD block's `allVisible` (`+0x82`) when the block
no longer needs per-instance hiding (its flags' bit 14 clear), then calls the block's update (`FUN_140503f70`). That update
refills only the groups whose `shaderPropertyUpToDate` (`+0x24`) is clear, and nothing cleared it. So the engine keeps
drawing the records it packed before: every instance that was hidden (scale 0) stays hidden, although `allVisible` says
every instance should show. DCLF draws by the intended rule: when the update finds `allVisible` set, DCLF repacks those
groups with every instance shown.

**Measured** (teleport route, three runs). 10-52 shapes differed in the reports after each load, and none once the camera
had stayed at one place.

**Why it is accepted.** The engine's records are stale, not intended: hiding is for instances whose full tree is loaded,
and `allVisible` is the block saying none are. If the engine's behaviour is visible, it shows as holes in the tree LOD where
full trees have since unloaded, which DCLF does not reproduce.

**What would remove it.** Nothing in DCLF, since the behaviour is the engine's. It has not been confirmed visible on screen. To investigate:
-   **Starting point.** `FUN_140510730` at the `allVisible = 1` write (the branch on the node flags' bit 14), and its call of
    `FUN_140503f70` (`0x1405108ff`). Find what bits 13 and 14 of the node's flags (`+0x40`) mean.
-   **Count it.** `CS_DCLF_PERSISTENT_PARITY`'s "tree LOD mirror" line, `shown by allVisible without a refill`, counts the
    shapes where DCLF and the engine differ. `CS_DCLF_TREE_LOD_AUDIT` counts instances by their reference's state.
-   **See it.** With DCLF's tree LOD off, teleport away from a forest and look back at it. An engine bug shows as gaps in the
    LOD forest where trees were loaded, lasting until the block is refilled for another reason. Then compare with DCLF's
    tree LOD on.
