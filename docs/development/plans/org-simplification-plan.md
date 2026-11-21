# ORG simplification plan

Status: done (2026-11-20; see the progress table and decisions 99-113). Proposed 2026-11-20. It runs **before** the library extraction plan (`org-library-extraction-plan.md`). The
cleanup plan is merged, so every engine draw is ported, programs build at setup and Birth defines first contents. Many
layers from the porting era now have no job. This plan removes them, so less code moves into ORG and OMS later.

It comes from six read-only surveys of `integ/hard-ports` at `08266cfa6`: graph core, DCLF, diagnostics, ports, mode
layers and libraries. Line counts are estimates, about ±25%.

## Handoff: start here

### State when this plan was written

- **Checkout and branch:** the main checkout is `c:\Users\matth\source\repos\SARP\external_refs\cs-org-dxvk`, on branch `integ/hard-ports`.
  - The surveys read `08266cfa6`. The plan itself is `f67265dce`, plus this handoff commit.
  - Nothing is pushed. PRs target `dev`, never `main`.
- **Submodules:** both are on local branch `cleanup/integ`: ORG (`extern/OpenRenderGraph`) at `73b7d81`, OMS (`extern/ORGModuleServices`) at `5b2d206`.
  - **These commits exist only locally.** Older tips are kept as the branches `cleanup-p4-birth-tip` and `cleanup-p5-reload-tip` in each submodule.
  - OMS has `update = none` in `.gitmodules`, so check out its commit by hand.
  - Worktrees get their submodules from the main checkout through `init-submodules-local.sh` (below), never from a remote.
- **Where the history is:**
  - Decisions: `docs/development/plans/director-decisions-2026-11.md`, items 1–98. 98 holds the answers below.
  - Per-merge log: `docs/development/plans/org-cleanup/director/director-log.md`.
- **Tests:** the last full set passed at `08266cfa6`. That covers mode smoke ×3, resize ×3, window-resize/hdr-resize/window-drag/resize-drag in guest and native, dclf-toggles and more (decisions 95–96).
- **Known open items carried in** (not part of this plan unless a phase touches them):
  - `RegisterCopy` compares adopted sizes.
  - ImageSpace's chain follows a resize one revision late.
  - `CS_ORG_ENGINE_DRAW_TAP` is off by default. Withheld draws and clears are logged, not fatal (decisions 78, 90).
  - LLF's slot count can't change in a session (decision 88).

### Survey material (in the repo)

`docs/development/plans/org-simplification/surveys/`:

| File | Content |
|---|---|
| `core.md` | Graph core (FrameAnchors, Declare, programs, builds, fatal paths), with line references. |
| `ports.md` | Feature and engine-step ports; RC-removable sizes per port. |
| `dclf.md` | DCLF production code. |
| `diag.md` | Diagnostics, ranked, with what each check catches and what is kept. |
| `native.md` | Native, guest/DXVK, targets, presenters, the DXVK fork. |
| `libs.md` | ORG/OMS, and who else uses them (BasicRenderer, SARP). |
| `switch-usage-matrix.txt`, `switch-usage-short.txt` | For each of ~350 switch names: which cs-test configs, cs-test Python, cs-ab files and sources use it. |
| `scripts/switch_matrix.py` | Rebuilds the matrix: `python scripts/switch_matrix.py <repo> <out.txt> scripts/switch-names.txt`. |
| `scripts/library_reach.py` | Which ORG/OMS public headers CS reaches: `python scripts/library_reach.py <repo>`. |
| `extraction-libs-inventory.md`, `extraction-shared-systems.md` | The ORG/OMS inventory and shared-systems survey behind `org-library-extraction-plan.md`. |

The survey notes are condensed. Line numbers refer to `08266cfa6`, so check each one before you cut. The phase lists
below are the authority. The surveys give the detail and the "not removable" lists, which are the things that look
dead but aren't.

### How the work was run (and how to continue it)

- **Who writes, who builds.**
  - A director session splits each phase into parts. Each part goes to a code-only lead in its own worktree, which writes code and commits only.
  - The director merges into `integ/hard-ports` in the main checkout, builds, tests and fixes. It is the only one that builds.
  - Brief leads with `docs/development/plans/org-cleanup/director/lead-brief-codeonly.md` plus the plan items and survey file for their part. Use `lead-brief-building.md` when one integration agent builds instead of the director.
  - Run at most about six leads at once. A worktree build takes about 7 GB, so cap building agents at about 8.
- **Worktrees.**
  - Create: `bash docs/development/plans/org-cleanup/director/mkwt.sh <part> [base]` creates `../org-port-worktrees/simplify-<part>` on branch `org-native/simplify-<part>` and initialises its submodules from the main checkout.
  - Before removing a worktree, fetch any submodule commits it made into the main checkout's submodules.
  - Remove merged worktrees and branches promptly.
- **Build:** `cmd //c "docs\\development\\plans\\org-cleanup\\director\\build-devfast.cmd"` from the repo root. This is Dev-Fast: Ninja, /Od, the DLL only.
- **Stage:** `python tools/cs-test/cs_test.py stage . --name nfg --preset Dev-Fast --base-aio "C:/Modding/MO2/mods/CS AB nfg"`
- **Run one config:** `CS_TEST_BACKGROUND=1 DXVK_DEBUG=hang python tools/cs-test/cs_test.py run <mode>+<config> --stage nfg --wait 3600 --label <label>`.
  - Modes: `guest`, `native-vulkan`, `native-d3d12`. Quick runs: `guest-quick`, `native-quick`, `native-d3d12-quick`.
  - Configs are in `tools/cs-test/configs/`; the format is in `tools/cs-test/README.md`.
  - Results go to `C:\Modding\CS-AB\runs\<stamp>-nfg-<config>-<label>\`. Each holds `CommunityShaders.log`, `result.json`, shots, and on a hang `CommunityShaders-Hang-*.txt/.dmp`.
- **Merge helpers:**
  - Plan-doc conflicts between branches that each add sections: `python docs/development/plans/org-cleanup/director/keepboth.py <file>`.
  - PP batch: `docs/development/plans/org-cleanup/director/ppbatch.sh`.
- **After changing switches or strings:**
  - Switches: after a build, `python tools/cs-test/cs_test.py run list-switches --stage <name>`, then `python tools/cs-test/cs_test.py schema`.
  - `python tools/extract-i18n.py --write` and `python tools/sort-i18n.py --write --prune-extra`.

### Traps seen during the cleanup

- **Linking:** never link while a cs-test run is going. The running game holds the build PDB and the link fails with LNK1201.
- **Unity builds** (batches of 8): files that land in one batch collide on anonymous-namespace names. Rename locally (`MaterialPorts.cpp`'s `PortTable()`, for example).
- **`Port<T>`:** port classes need out-of-line ctors and dtors (`X::X() = default;` in the cpp), or the `unique_ptr<Impl>` destroys an incomplete type.
- **After a hang or kill:** DXVK's `vkCreateDevice` can fail with OUT_OF_HOST_MEMORY on every launch until a reboot.
- **Shots:** brightness varies between runs (eye adaptation). Check reproducibility before calling a shot a regression.
- **Perf:** one sample per version. Say the build is Dev-Fast /Od when quoting times. Use `game.uncapped`.
- **Formatting:** clang-format is pinned at v23.1.3. Don't format whole files; the branch isn't format-clean.
- **Timeouts:** never raise a test timeout to fix a slow run. Find the cause, usually a cold per-variant shader cache.
- **MO2:** it may be closed before runs. The active test mod folder is `CS AB nfg`; don't create other variants.

### Progress (update as phases land)

| Phase | State | Branches | Notes |
|---|---|---|---|
| 0 Defects | done | p0-defects | t98 and first-watch fixes (decision 106); held-build luminance ids. |
| 1 Dead code and fixed switches | done | p1-core, p1-ports, p1-dclf, p1-noorg | Context1 fatal reverted (105). No-ORG build gone, with the OMS/DCLF guards (p8-cs). Native's sync present path stays (p5-modes, p8-backbuffer). |
| 2 Diagnostics | done | p2-org-diag, p2-dclf-diag, p2-harness, p2-registry | Capture parity checked (99). Outside-ledgers and undeclared access logged, not tap errors (107). One switch table; schema is the game's dump. |
| 3 Graph core and ports | done | p3-anchors, p3-declare, p3-declare2, p3-resources, p3-rest | Exports with D3D11 readers stay (p4-state, p8-cs audit). ImageSpace lag before registration (110). |
| 4 DCLF | done | p4-install, p4-paths, p4-state | The occlusion re-entry stays: rain needs it (111). |
| 5 Mode layers | done | p5-dxvk, p5-modes, p8-backbuffer | Native query stubs silent (108). One native back buffer. |
| 6 Libraries | done, not pushed | p6-libs, p6-pipes, p6-staging, p8-lib | Push for review still awaits the user (decision 89). 6.11: see decision 113. |
| 7 Guest context like native | done | p7-guest | Flush latch gone; Grass Optimizations off in graph mode. |

Later decisions override some of the agent sections at the end of this file: 105 (p1-ports' Context1 fatal), 107
(p2-org-diag's fatal outside-ledgers and undeclared access), 108 (p5-modes' query tap errors), 111 (p4-paths' occlusion
re-entry) and 113 (the host options in every mode). The next plan is `org-library-extraction-plan.md`; its handoff
section is the place to start.

Record each decision made while doing this plan as a new item in `director-decisions-2026-11.md`, numbered from 99,
with "to confirm" where the user hasn't seen it. Add one line per merge to `director-log.md`.

### Checks that need a run before cutting

- Phase 2: does capture parity still compare anything in graph mode? The engine's draws are skipped there.
- 4.3: do the engine-only casters' draws count as skipped today? Run `CS_DCLF_TEST_REFUSE` with the residue census, before phase 2 removes the census.
- 4.9: does LoadHold's `BeginReentry` ever fire? Run a tour with a load and a teleport.
- 5.6: which native emulation services does the engine still reach? Run one `native-tour` with `tripwire_log`.
- 6.11: can guest run with each native-only ORG host option on? One A/B per option.
- 3.10: which exports serve engine draws DCLF doesn't claim (menu, map and loading-screen 3D)? Use the residue census, before it goes.

Several of these use diagnostics that phase 2 deletes. Run them before phase 2 lands.

### Open questions: phase 2, graph diagnostics (decided by the agent, to confirm)

- **Which org: commands go.** Only the stream timing commands go (epochs ... early-flush). The build-recording A/B commands
  (async-compute, pass-queries, heap-dedupe, list-heaps, list-barriers, rhi-globals, queue-boundary) and ts-dump, gpu-window and
  nvperf stay. They set library diagnostics, which phase 6 can drop.
- **The D3D11 fallbacks raise a tap error.** The four NoteD3D11 callers raise the new `TapError::D3D11Path`, not
  `report_and_fail`, so Release stays soft. Cloud Shadows' sites include the engine-drawn faces. If the engine still draws a
  cloud layer in a graph mode (menus, the map), dev runs now stop there.
- **Hang mode turns on all of this.** `DXVK_DEBUG=hang` turns on native crash markers (DRED on D3D12), the adopted device's
  BasicRHI markers (a new `AdoptedVulkanDeviceInfo::Diagnostics::crashMarkers`) and the detailed Aftermath checkpoints. Every
  cs-test run sets hang mode, so the detailed checkpoints now run in timing runs on NVIDIA too.
- **`CS_GPU_CRASH_ANALYSIS` goes.** Players set `DXVK_DEBUG=hang` instead; CS no longer sets it. `BASICRHI_VK_CRASH_MARKERS`
  and `SARP_D3D12_DRED` are BasicRHI's own, and other consumers read them, so they stay.
- **EPOCH_STATS takes its whole report.** That includes the `[ORG] declared frame`, `world recipe`, `frame arena`,
  `post description`, `Async epochs`, `Run memory`, submission-census and segment-census lines. FrameDriver's per-window
  counters and FrameAnchors' `insideHeld` are now written and never read. Phase 3 removes them with Placement.
- **The tripwire self-test still runs in the plugin.** It needs the plugin's handles and REL, so
  `tests/Tripwire/TripwireSelfTest.cpp` is compiled into the plugin (cmake/XSEPlugin.cmake), and `CS_ORG_TRIPWIRE_SELFTEST`
  stays. No tests/ target can build it alone.
- **Census leftovers (worker).** `OutsideLedgers` and `UndeclaredAccess` are now fatal in dev builds. If the loading screen's
  uploads or guest's undeclared reads are common, every dev run stops. `native-census` keeps its name as a plain native run.

## Decisions taken for this plan (2026-11-20)

| Question | Answer |
|---|---|
| Modes | Native Vulkan, native D3D12 and guest stay. Plain D3D11 (`CS_ORG=0`) is dropped at release candidate (RC), not before. |
| No-ORG build (`CS_HAS_RENDER_GRAPH` off) | Dropped **now**. It loses no runtime mode, since plain D3D11 runs with ORG compiled in. |
| DCLF pipelines | The CPU and GPU pipelines and all cull modes stay. |
| ORG/OMS consumers | `cleanup/integ` is **CS-only**. BasicRenderer and SARP stay on their own lines (ORG `8dfc78f`, OMS `0185120`) and re-adopt the smaller libraries later. |
| Library APIs | They may change freely. |
| Release profile | **Stays soft**: tap errors, recipe mismatches and failed clients count and warn in Release; they are fatal in dev builds. |
| Diagnostics | Cut aggressively, with these exceptions: **capture parity stays**, the compatibility monitor and the diagnostic capture stay (product features), and the telemetry tallies stay (BasicTelemetry, RenderThreadBudget, Zones). |
| DCLF "stays native" class toggles | Removed. |
| Engine-only shadow casters | The subset is deleted. A refusal is logged at classification. |
| Absent-pass build keys | Kept. |
| Guest context like native | Included, as the last phase. |
| DXVK fork guest-device exports | Deleted. |
| DCLF Api single-instance contracts | Kept (layout rule). |
| LoadHold re-entry | Verify with a run, then cut. |

## Rules

- The standing rules hold throughout: errors fatal (soft in Release only where the table says), no waits, retries or
  fallbacks, render thread never waits, changes at frame boundaries, recordings replayable.
- A cut must not lose a function. Each item names what replaces it.
- Delete a switch together with its alternative path. A switch that is always on becomes no switch.
- Tests: each phase lists its runs. Don't rerun a test for code a phase didn't touch.

## Phase 0: defects the surveys found (fix first)

| Defect | Where | Fix |
|---|---|---|
| The Skeleton base group builds a `ClientDesc` but never registers it, and still logs "registered at the … anchor". | `Engine/Skeleton/Skeleton.cpp:1218-1237` | Register it, or delete it and the log line. Check whether anything expects it. |
| ImageSpace hooks read `chain.levels` / `menu.levels` from the newest build, not from the one the frame holds. | `ChainSteps.inl:1224`, `MenuSteps.inl:356` | Read from the held build. |
| Scaleform's scratch is sized once at setup, so a window that grows refuses larger GFx targets. | `Scaleform/Passes.cpp:1279,1388`, `Renderer.cpp:960` | Make it a declared, display-sized texture. |
| `Unwrap(x)->GetDesc` dereferences null for an unbacked guest handle. | DCLF captures: Precipitation, Sky, Water, ShadowMask, MenuScenes, BloodSplatter (~15 sites) | Call `GetDesc` on the handle. Use `Targets::Identify` for the `Unwrap(a)==Unwrap(b)` compares in 7 ports. |
| Two checks repair state instead of reporting it. | `NeutraliseDanglingSlots`, `ValidateTrackedSlice` | Make them fatal (dev) / tap errors. |
| Real defects reported as warnings through the observer bus. | `LogShortBuffers`, `LogUnfilledFrameBuffer`, `DescribeUnresolvedFrameTexture` (CommitLog.cpp:9-42), `NoteLeak` (FeatureInstall.cpp:32) | A tap error at the site. |
| The `CS_DCLF_STATS` line stalls Present for 30 ms every 300 frames. | `Vram::ReportLine` (VramLedger.cpp:622) | Make it cheap, or take it off the stats line. |
| The test contributor still names the old epoch "cs.anchor.prepass". | `RenderGraphRuntime.cpp:1895` | Removed with the contributor (phase 6). |
| Stale texts. | `ORGCloudShadows.cpp:666` ("stays on D3D11"), `ORGIBL.cpp:229`, `ImageSpace.cpp:446`; `org-native-frame-graph.md` documents `CS_ORG_ABSENT_PASSES`, which no longer exists. | Fix the texts. |

Tests: mode smoke ×3, resize ×3 (native and guest), map, menu-resize, hdr-resize.

### Phase 0 open questions

- Skeleton base group: deleted, not registered. Nothing registered it since the recipe table (43fe89a79); its clears run at hosts
  and places. The recipe table's names for the three group ids went too. Decided by the agent, to confirm.
- `Unwrap(a)==Unwrap(b)` compares: not replaced. Each site handles a DXVK object on purpose (the field is a handle, the bound view
  is DXVK's; Terrain Blending's backups are DXVK views saved before the handles). `Identify` answers only handles, so the swap
  waits for phase 7 (the guest context keeps handles). Decided by the agent, to confirm.
- Scaleform scratch: four persistent declared textures on DisplaySize (RGBA8, about 33 MB each at 4K). The old images shared the
  frame arena; a declared texture cannot be Transient. Decided by the agent, to confirm.
- `Vram::ReportLine`: taken off the stats line and deleted, not made cheap. No profile is possible without a run; the cost is the
  whole snapshot (every DXVK row attributed, copied twice). Decided by the agent, to confirm.
- New tap error kinds `SceneMismatch`, `DrawInputs`, `EngineLeak`. The scene checks raise theirs on the scene lane, not the render
  owner. `NoteLeak` raises its error where it finds the leak, still inside its observer, since the check needs the frame set.
  Decided by the agent, to confirm.
- ImageSpace's `AdaptedLuminanceIdOf` still reads `chain.levels` (the newest build) to find the adapted pair. Not in the row; it
  may need the same fix.

## Phase 1: dead code and fixed switches (S, mechanical, about 4k LOC)

**Dead code:**
- **FrameAnchors:** `SetWindowsClosedHook`, `StagesHoldValues`, the duplicate `HasPasses`, `MarkCurrent`, and the `EngineDepth`/`EngineCubemap` import fallbacks.
- **Builds and staging:** `Builds::InManagedBuild`, `FrameStaging::Desc::maxBlocks` and its fallback.
- **Native and targets:** `Native::FrameTransientScope`/`FrameGroup`, `Targets::Find`/`FindImage`, `Native::OnGraphReady`, and the `DxvkImageImport` forwarders (callers move to ImageRegistry).
- **Tripwire:** `WatchSet`.
- **ImageSpace:** dead state (`postOn`, `engineCalls`, `WorldStepsOn`, `ChainStepsOn`, the `.on` flags, `desc.declared`, `Status()`, `MenuSteps::wantedFor`) and the `SpecProgramsBuilding` side channel.
- **Single ports:** Bink's no-revision branches and dead `HoldForPrograms`, Scaleform's duplicate generation compares, the impossible `ID3D11DeviceContext1` warning, `NoteD3D11` in branches graph mode never reaches, SSGI/SSS re-creation fallbacks, `onGraph` bookkeeping, HDR `Composed::Output`, and `ORGDeferredComposite::Impl::registered`.

**Forwarders:**
- **Builds and segments:** `Builds::AttachStaged`/`RequestAtAdoption`; `Rebuild`/`StructuralRebuild` merged into one function with a flag; the duplicate `SegmentEpochs`; `RunSegment`; `Port.cpp`.
- **Bink and Scaleform:** `Bink::Setup`, `Scaleform::Setup`.
- **DCLF:** `Runtime/EventQueue.h` and `Hash.h` (36 includers), the `SceneScheduler` facade (79 sites), `SceneCapture::SetFrame`/`Frame`.
- **"Is graph mode":** five predicates become one.

**Switches fixed as constants, with their alternative paths deleted:**
- **Native host:** `KEEP_LIVE`, `COALESCE_SUBMITS`, `MERGE_HANDOFFS`, `UPLOADS_WITH_BATCHES`, `KEEP_GATED`, `SUBMIT_THREAD`, `MERGE_TICKETS`, `QUEUE_PRESENT`. This removes native's synchronous present path.
- **Guest:** `PINNED_IMPORTS`, `BATCH_SUBMIT`, `EARLY_FLUSH`, `RESTING_LAYOUTS`, `EXTERNAL_HOME`, `COMPUTE_QUEUE`, `COPY_QUEUE`, `UPLOAD_QUEUE`, `QUEUE_UPLOADS`.
- **Graph core:** `FRAME_ARENA`, `GPU_GATES`, `ENGINE_TARGETS_DCLF`, `MISSED_WRITER_CHECK`, `TICKETS_AHEAD` (its loop never runs).
- **Engine handles:** `CS_ORG_ENGINE_HANDLES_GUEST_CONTEXT` (always defined).
- **Duplicate of `MismatchFatal`:** `CS_ORG_RECIPE_FATAL`.
- **Off by default, deleted with their code:** `CLOSED` (an interop fallback), `SCOPED_FRAME_WAIT` (a wait mode).
- **Removed with their old path:** the port-narrowing switches `WATER_PARTS`, `SHADOW_MASK_VIEWS`, `IMAGESPACE_EFFECTS` (they hand views to the engine).
- **DCLF switches whose 0 restores an old path:** `SHADOW_MERGE`, `LAZY_ROWS`, `PRECOMPILE`, `FANOUT`, `SHADOW_ORDER`, the `MAIN`/`TERRAIN`/`DECAL_PLAIN` switches, and `SHADOW_DEPTH_CLAMP` (a device without depth clamp becomes fatal at setup).
- **DCLF A/B bits:** the 12 `kExp*` bits, always on, and their 5 check bits (the SSE kernel stays as the CPUID fallback).
- **Tuning:** `WORKERS`, `FRAME_WORKERS`, `ASYNC_PRIORITY`. Keep these as constants unless a config sets them.

**The no-ORG build:** about 1k stub lines and 326 `#if` guards (`cmake/RenderGraph.cmake:10-35`).

Tests: mode smoke ×3, menu, dclf-toggles ×3, resize ×3, native-vulkan window-resize, guest+fg, native-fg,
latch-census-short, perf-quick (one sample per mode; say it's Dev-Fast /Od).

### Phase 1 single ports (p1-ports): decided by the agent, to confirm

- `SpecProgramsBuilding` stays as `StepGroup::For`'s hold signal until 3.3 replaces `Table` plus `HoldForPrograms`. Only `WantMenuPrograms`' save and restore of it went, since `For` clears it before each build.
- The missing `ID3D11DeviceContext1` case became a fatal check, not a deleted branch. It costs nothing and graph mode can't run without it.
- `NoteD3D11`: only SSS's call was truly unreachable, so only it went. Terrain Blending's depends on `BeforeDepth` running before `Blend` each frame, EHF's runs in debug mode 3 and Cloud Shadows' counts engine-drawn faces. They stay for phase 2.
- Bink's `DrawPass` keeps an empty `Revision`, so kept-recording reuse is unchanged. `Begin`'s `HeldByStage` recheck went, since stages are frozen at setup.
- Scaleform's "duplicate generation compares" was read as `Submit`'s nested held-status compares. Both generations stay: a reads-only registration keeps the old surfaces drawable. `DisplayPass`'s `RecordsAtExecution` ticket is left for phase 3.
- `localShadowOnGraph` in LLF is a cache of `IsActive`, not dead bookkeeping, so it was left.

### Phase 5 DXVK exports (p5-dxvk): decided by the agent, to confirm

- **Every fork export is required in graph mode, checked once at load.** `DxvkLoader::Load` stops the game when `RenderGraph::Engine::On()` and `DxvkExports::Missing()` is not empty. `RenderGraphRuntime::Initialize` checks again, because VR can start the graph outside graph mode.
- **Plain D3D11 mode doesn't require the fork, as before.** It logs the missing exports once at load. The code it runs keeps its null checks: the presenter, VRAM attribution, streaming, DXC shaders, the submission counter, the trace and the debug exports. Those checks go at RC.
- **A missing `vkQueueSubmit2`, or DXVK rejecting resting layouts, now disables the graph.** In graph mode that is fatal. Before, both fell back: a flush each epoch, or images resting in GENERAL.
- **`CS_ORG_PINNED_IMPORTS`, `CS_ORG_BATCH_SUBMIT` and `CS_UPSCALE_SUBMIT` stay.** They are switches, not missing-export fallbacks.
- **DCLF's `EmitCommands` failures still count as "not queued".** They don't fail the game. Only the messages that blamed a missing export changed. Making them fatal belongs to phase 4.4.
- **`DXVK_ORG_INTEROP_VERSION` stays at 7** after the guest exports leave the fork, so the struct layouts don't change.

### Phase 5 mode layers (p5-modes): decided by the agent, to confirm

- **The presenter interface has two more duties than listed.** `LayerPaces` replaces the present hook's pacing branch, and `LayerVsync` replaces the frame generation controller's native vsync check. DXVK's preset knobs (`PresetSyncPresent`, `PresetTearing`, `HasFrameGenerationInterop`) stay on `DxvkPresenter`, since they set DXVK before it makes a swap chain.
- **Native's torn-down step is read when the replacement runs, not when it is asked for.** `PresenterBase` stores one step. Native's default step (`SetPresentationTornDown`) went with it.
- **One unbacked rule for both modes: clears drop silently, other work is a tap error.** Guest's rule. Native used to report a clear of a target not marked "dropped" as an unbacked use. The `dropClears` column of `kUnbackedColours` is gone. Map, Unmap and UpdateSubresource on an unbacked handle are now tap errors in guest too. A native import of an unbacked handle is still an unbacked-use tap error. A guest import still just fails.
- **Engine pool constant-buffer Map:** the native context keeps Map for CS-owned buffers. The engine constants answer every pool buffer's map in native mode. A pool buffer mapped before the pools exist (before the engine made its last one) is an EngineWork tap error, and still answered by the context.
- **End and GetData:** an EngineWork tap error each, and GetData answers zeros at once (occlusion hidden, timestamps disjoint), so a poller never loops.
- **ExternalWork is native-only and fatal.** Guest never used it. `ExternalWork::Start` makes it with the native device. A submission that fails, or one with no client, stops the game. After `Shutdown` (window close) it returns false and nothing is submitted. Its capacity is 256 and the immediate context has 64 slots, since DCLF's commands share both now.
- **DCLF's EmitCommands in native mode now fails the game when it cannot submit.** Before, a failed queue submission counted as taken.
- **Native textures made before `RenderGraphRuntime::Initialize` stay host-made.** The ORG runtime exists from stage 3 now, so they could be tracked from creation. That changes which textures are ORG textures, so it is left for a later change.
- **Exempt textures still get their graph resource on the first `ImmutableTextureOf`.** Making it at creation would make thousands of graph resources and fix the SRV format before any reader asks. Lookups after that go through ImageRegistry. `IsTrackedTexture` and `PromotedTextureOf` stay: they read the record's ownership, not a resource.
- **Native's synchronous present path stays.** With one submission path it is still needed for three cases. Presents before the graph host exists have no submission thread: the thread is the GraphHost's, made in `RenderGraphRuntime::Initialize` after the anchors are reserved. A loading screen's thread that does not own rendering cannot take the render owner's present waits or pacing counters. A `DXGI_PRESENT_TEST` present does no work. The present probe was the fourth case and goes in phase 2.
- **Item 4's second half (one back buffer, the kFRAMEBUFFER slot from stage 3) is not done.** The engine target set declares kFRAMEBUFFER as a versioned twin at `Engine::Setup`, after the engine's `InitD3D` asked for the back buffer. Native would have to make that slot at stage 3 and the set adopt it, which changes how the set makes twins. p3-resources is changing the import entry points that touches, so it waits for that. Done by p8-backbuffer (below).

### Phase 5 item 4, one back buffer (p8-backbuffer): decided by the agent, to confirm

- **Native makes kFRAMEBUFFER's slot with the swap chain at stage 3.** Its first version is a graph-owned texture (`TextureRequest::beforeGraph`), since the ORG runtime and the external client exist then. The startup `backBufferRecord` and the set's separate slot are gone. Every present copies the slot's current version through the ledger.
- **The set joins the slot instead of adopting native's image.** At `Engine::Setup` the kFRAMEBUFFER twin makes its own export as before, but publishes it into native's slot (`GraphResources::Options::joinSlot`) instead of making a slot. Taking native's image as the twin's first version would need a twin over an image it did not make. The cost is one more image at setup, which is released once the engine let go of its startup views.
- **A resize before the set adopted the slot publishes native's next version into it,** tagged "native back buffer" for the boundary census. It runs before the first frame in practice. A format change is now fatal before adoption too, since every version of a slot has one format.
- **Presents before the set adopts the slot now present.** Before, they took no swap chain image, since the startup back buffer had no ledger resource until a graph imported it.
- **The synchronous present path stays for two cases, not three.** `DXGI_PRESENT_TEST` returns before any present work. A present before `RenderGraphRuntime::Initialize` has no submission thread to queue on: the thread belongs to the GraphHost, and ExternalWork submits the copy but cannot run the swap chain present later. A loading screen's present from a thread that does not own rendering cannot take the render owner's pacing counters. Both copy the slot's current version through ExternalWork, like a queued present. The sync path still drains the owner's epochs and runs any present waits first, so a waiting copy-queue signal is not left behind.

## Phase 2: diagnostics (about 15–18k LOC, 4k tool lines, 3k generated)

**Cut:**
- **Parity checks that change the main path.**
  - Go: BuildParity, WalkParity, ChangeLog, Resident, Accumulated, CapturePoint, Foliage (with its shader, defines and `RecordFoliagePixels`/`FreezeTreeWind`), StrictLightParity (with `FilterLightCulls`), FadePort, Skylight, and the replay, commit, candidate and shadow-set parities.
  - Their `Policy` questions go too (`ScenePassInline`, `SerialFanout`, `RebuildShadowSets`, the `Check*` set), except what capture parity needs.
  - This makes DCLF's inline scene-pass mode deletable (`ScenePump.cpp:35-187`, a render-thread wait).
- **Other parities and observers.**
  - Go: the persistent-parity observer set, the per-feature parities (LodGate, Portal, TreeWind, LightSelection, Grass, TreeLod, TextureTransforms, SunView, FaceCamera, Focus, ShadowVolume, Skeleton, DecalOrder, the AdvancedSkin, TruePBR and LLF integration parities) and Residue.
  - One `CS_DCLF_PARITY` switch keeps set parity, the kept-store rebuild check and mirror parity's probe, which the compatibility monitor needs.
- **Closed and one-off probes and traces.**
  - Go: OpenDefectProbes, TARGET_PROBE, SHADOW_DEBUG_OUTPUT and its `[TEMP]` block in Lighting.hlsl, TargetDump, REF_CHECK/REF_TRACE, the DEPTH_TRACE/DRAW_TRACE hooks, FrameTrace and ScheduleDump with their tools, PrecisionCensus, MirrorWatch, TreeLodAudit, HiddenWatch, FadeLog, PortalCapture, LightCulls, BillboardTurns, LandTrace, CELL_TRACE, ActorProbe, MenuViewsProbe, ShadowMaskProbe, ColourProbes, TRACE_TEXTURE_PATHS.
  - Future investigations use a debugger or RenderDoc.
- **Self-tests and samplers no config runs.**
  - Go: FrameAnchorSelfTest, GraphResourcesSelfTest (library logic moves into ORG unit tests), D3D12SelfTest (the native-d3d12 runs cover it), TripwireMeter, RtSampler, AllocCounter (it replaces global new/delete), GpuIdleTrace, the present probe, PP_PROBE/PP_LOG, FrameSequence, RunShapes and `CS_ORG_EPOCH_STATS`.
  - The tripwire self-test moves to `tests/`.
- **Censuses.**
  - Go: the residue census's counting (`CS_ORG_RESIDUE_CENSUS*`, Flush's census half, StateTrace.cpp, the `org.census` preset). The default-on `EngineCensusTap` raises the same failures as tap errors.
  - Go: the native op and write censuses. Keep the "outside the ledgers" line as a tap error.
  - Go: ImageSpace's `NoteEngineDraw` census and `CS_ORG_ADAPT_READBACK`.
  - `CensusWindow` shrinks to one query.
- **UndeclaredAccess** becomes a dev-build tap error with no switch.
- **Fatal-check switches:** `CS_ORG_TAP_ERROR_TEST` and `tap-warning.toml` stay, because Release stays soft. `NoteD3D11`'s four "never comes here" callers become fatal at the site, and `expect.graph_frames.no_d3d11` goes.
- **Guest cost-attribution knobs.** Go: the stream timing knobs (RenderGraphRuntime.cpp:3236-3322 and its setters), the `OrgCommands` attribution commands, and the DCLF `dclf:cut`/`cpu-exp`/`define`/`shadow-probe`/`actor-probe`/`kill-scene-pump` commands. Configs keep `reload-shaders`, `refresh` and `rebuild`.
- **The old test harness.** `Testing/Legacy.cpp`, the `CS_DCLF_TEST_*` rows, the old command names and `tools/cs-ab` go. First move `shotdiff.py` and `compare.py` into cs-test.
- **One switch registry.** DCLF's `kRegistry`, `SwitchRegistry`'s lists and `gen_schema.py`'s scraping become one C++ table; the game's switch dump is the schema. Typed config keys remain for the ~35 switches configs use. Dead rows go (`ADAPT_READBACK`, `DEFERRED_CLEARS`, `IMAGESPACE_LOG`).
- **Crash markers.** `DXVK_DEBUG=hang` turns on all five crash-marker switches.

**Kept:**
- capture parity and its `WithholdFromEngine` policy (see "To verify" below);
- the compatibility monitor (PlayerParity) and the diagnostic capture;
- BasicTelemetry, RenderThreadBudget, Zones and TelemetryProfile;
- HangWatchdog, CrashDump and the crash helper;
- BoundaryCheck (its counting lines can go) and Recipe `Validate`;
- `CS_ORG_TRIPWIRE_LOG`, GPU counters/GPA/NvPerf, `STAGE_DUMP`, `SHADER_DEBUG`, `TABLE_START`;
- the cs-test contract in `src/Testing` and `NoteRead`'s unknown-switch warning.

**To verify:** capture parity relies on the engine drawing the same draw too ("both draw"). In graph mode the flush latch
skips engine draws. Check that capture parity still compares something on `integ/hard-ports`. If it doesn't, it needs
the engine draw let through on its runs, or an engine-state capture at the hook.

Tests: mode smoke ×3, menu, dclf-toggles, shader-reload ×3, latch-census-short (becomes a plain guest tour),
native-census, guest-interior, native-tour, capture-parity run, a compatibility-monitor run, `cs-test check-schema`.

### Phase 2 old test harness (p2-harness): decided by the agent, to confirm

- DevBench's `communityshaders.dclf` line (`RunCommandLine`) now takes cs-test step names: `<command>[=<arg>]` runs a registered command (the `dclf:` and `org:` prefixes included), any other line is a console command. The `dclf:<generic>` and `dclf:<org command>` aliases went with it. The reply lost its `harness` progress, and an empty command is an error.
- The `feature` step needs `<ShortName>=0|1`. The bare `0|1` form, the old `dclf:feature`, went.
- `CommunityShaders.env` stays: render-graph.md documents it for setting switches by hand. cs-test's checks for a killed cs-ab run's leftovers stay too, as they only read files.
- `gen_schema.py` keeps the scenario-derived key types as a frozen `SCENARIO_TYPES` table, so configs keep `DXVK_DEBUG`, the `VK_*` keys and the rest. Regenerating gives today's schema minus the `CS_DCLF_TEST_*` keys.
- `tools/cs-ab/dump/*` (process-dump texture chains) and `census.py` were deleted, not moved.
- `shotdiff.py` is standard library now, with its own PNG reader. Its percentiles are whole numbers, not interpolated.
- The `fxc-corpus` and `dxc-slice` scenarios became cs-test configs, since docs and `dxc_survey.py` point at them.

### Phase 2 one switch registry (p2-registry): decided by the agent, to confirm

- The one table is `kSwitches` in `src/Testing/SwitchRegistry.cpp`: name, key, type, default, choices, kind, description, in
  alphabetical order (a `static_assert` checks it). DCLF keeps its `Switch` enum and a list of `{id, name, Reduced}`; it looks
  each name up in the table for its kind, and a missing row is fatal at the first read. Why: the brief puts new switches there.
- DCLF's features' `RegisterSwitch` rows stay with the features (they are DCLF's extension API). The dump lists them, with no key.
  Their type is inferred: on by default (`Zero`, `UnlessOne`) is a bool, as is a description starting `1:`.
- cs-test's schema is `tools/cs-test/switches.json`, a checked-in copy of the dump. `cs_test.py run list-switches` makes the dump
  and `cs_test.py schema` copies it. Why: no source is parsed, and cs-test runs without the game; only regenerating needs a build.
  `schema.toml` and `gen_schema.py` are deleted. The first `switches.json` came from a one-off parse of the new table, not a real
  dump: regenerate it from a build, and `check-schema` should then show no differences.
- Typed keys stay for the 33 switches configs, tests and docs use. Every other row is set by name in `[env]`. `[env]` now takes
  only names in the dump or in its prefix families (`CS_GPU_COUNTERS_`, `CS_NVPERF_`, `DXVK_`, `VK_`), like the game's check;
  `BASICRHI_*` is no longer allowed wholesale. All 115 configs resolve to the same contracts as with the old schema.
- `use_instead` for `CS_DISABLE_FEATURES` lives in `cstest/schema.py`, since it is cs-test's rule, not the game's.
- Rows gone: `CS_ORG_DEFERRED_CLEARS`, `CS_ORG_IMAGESPACE_LOG`, `CS_ORG_D3D12_DEBUG` (no reader); shader define names
  (`CS_FFT`, `CS_GAME_GAMMA`, `CS_GPU_AMD`, `CS_HAIR`, `CS_SKIN`, `BASICRHI_SHADER_API_*`, `ORG_GENERATE_MIPS_ARRAY`); file
  magics (`ORG_GRAPH_REPLAY`, `ORG_GRAPH_SEQUENCE`); and the ORG unit tests' variables (`ORG_SERVICES_TEST_DXC_DIR`,
  `ORG_TEST_DEVICE`, `ORG_UPDATE_GOLDEN`), which the game never reads. `ADAPT_READBACK` was in no list any more.
- `CS_UPSCALE_SUBMIT` and `CS_STREAMLINE_REFLEX` left DCLF's `Switch` enum (Upscaling reads them by name), so DCLF's startup
  summary no longer lists them. DCLF's "UNKNOWN (read by nothing)" list now checks the whole table, so a set `CS_ORG_*` in the
  switches file or a `CS_DCLF_MAP_HOLD` is no longer listed as unknown.
- `CS_TEST_LIST_SWITCHES` ends a test run with outcome `passed`, so cs-test can make the dump.

### Phase 2, DCLF diagnostics: decided by the agent, to confirm

- `CS_DCLF_PARITY` replaces `CS_DCLF_SET_PARITY` and `CS_DCLF_MIRROR_PARITY`. "The kept-store rebuild check" is read as `Draws/KeptChecks.cpp` (object records, geometry draws, draw bounds, extras, the kept shadow build). The persistent parity's other checks go. Why: the persistent parity is listed with the observers that go.
- Set parity keeps its shading-row sampling check (`CheckShadingSampled`), moved from the deleted `FrameValuesParity.cpp`, and its withheld-pass counts (`Engine/Registrations`). Why: both ran under `CS_DCLF_SET_PARITY`.
- The mirror's probe runs every frame with `CS_DCLF_PARITY`. Otherwise only the compatibility monitor switches it on. The mirror *read* parity (`SceneMirrorReads.cpp`) goes, which is what needed `SerialFanout`. Why: the monitor never used the reads.
- Capture parity loses the inline scene pass it asked for (`ScenePassInline` returned true for it). Its tables may now lag the engine's draws by a frame. Why: the brief says to delete the inline mode. Check with the capture-parity run.
- AdvancedSkin's and TruePBR's per-draw checks stay, since capture parity calls them. AdvancedSkin's persistent row-wetness check goes. `CS_ORG_LLF_PARITY` (in `Features/LightLimitFix.cpp`) is left to the ORG-side lead.
- `CS_DCLF_CENSUS` (the registration census) goes with the residue census. `CS_TEST_SHADER_DEFINES` goes with `dclf:define`.
- `NeutraliseDanglingSlots` and `ValidateTrackedSlice` stay as Policy questions, gated on `CS_DCLF_PARITY`, for the phase 0 lead. `DEPTH_TRACE`/`DRAW_TRACE` rows stay in DCLF's registry for the ORG-side lead.
- Skylighting's and the precipitation mask's engine reference render goes with `referenceDue` (SkylightParity was its only setter). `RegisterConditionInputs` goes with `dclf:cut`, since it only declared the cuts' variable.

## Phase 3: graph core and ports (about 4–5k LOC)

1. **Per-anchor epochs and the reservation system go.**
   - Why: every anchor is a stage member. `Run` already makes other work `TapError::OwnEpoch`, and `EngineCubeClear` is never reserved.
   - What goes:
     - the own-epoch branches in FrameAnchors;
     - the 28 `Segment::Anchor*` values;
     - `Role::Placed`;
     - `Reserve`/`HasEpoch` (47 guards become `IsActive()`), `Port::ReserveAnchors` (29 sites) and 13 direct `Reserve` calls;
     - the constant `HeldByStage()`, `StageStepsAt()` and `RecordsAtExecution()` branches.
   - Profiler and contributor names move to `cs.segment.*`. About 650 LOC.
2. **One registration path.**
   - `FrameAnchors::Register` becomes private to Declare. Its 10 direct users move to `DeclareClient`: Skeleton ×5, CloudShadows, GrassCollision, Upscaling, ORGDeferredClears, GpuLoadPass. Revisions then wait for their builds, which removes their lag.
   - One `DeclareClient` returning `DeclaredClientOf<State>` (State may be void).
   - One `Placement{stage, generation, draws, reason}` replaces `Lag`, `Placement`, `HeldStatus`, `StageState` and `Driver::Scheduled`.
   - Skeleton's hand-rolled registration goes with this: about 350 LOC, three copy builders become one, `TakeDepthClear` loses its duplicate, and the remade-texture lag paths go.
3. **Programs: one slot queue.**
   - `DeclareProgram`'s private build machinery moves onto the Programs queue.
   - `Programs::Table` plus `HoldForPrograms` (5 callers) become `DeclareProgram`s keyed on observed values.
   - The ~20 blocking `Load` calls go through the queue.
   - `DeclaredClient`/`DeclaredProgram::Invalidate` become `Observed<T>`.
4. **Port boilerplate.**
   - `Port<T>` gets a table of anchors. It reserves them itself and runs the Setup prologue.
   - Setup factories become fatal: `RunGate::Create`, `DeclareClient`, `Declare*`, `Texture2D`. This drops the per-caller "could not be made" checks.
   - About 700 LOC.
5. **ImageSpace dedupe.**
   - Today four step files each carry their own copy of the `ClientDesc` fill, the Find switch, the HDR step list and the draw-to-key mapping. Each becomes one helper.
   - About 250 LOC.
6. **Sizes.** One `Vars::DisplaySize()` plus one per-frame dynamic-area helper from the latch replace the seven size sources and the dead `display.width ?` fallbacks.
7. **Observed constants.** Published ids and `loaded` flags move out of `Observed<>` into setup keys (16 ports). This is the first step of retiring Observed.
8. **Facades and pools.**
   - Call the descriptor service directly instead of `RenderGraph::Bindless`.
   - `FrameStaging` and `UploadRing` merge onto ORG `FrameArena`, which gains an oversize region; see phase 6.
9. **Imports.**
   - Eight import entry points become three.
   - Five feature-owned D3D11 textures become declared twins (CloudShadows, Screenshot, Upscaling, ImageSpace, Skeleton).
10. **Exports used only as lookup keys.**
    - DCLF `FrameTexture` and the deferred composite take published ids.
    - Exports stay only for real D3D11 readers: HDR's UI texture, the CloudShadows layers, and guest Upscaling.
    - Check each one against engine draws DCLF doesn't claim (menu, map and loading-screen 3D).
11. **One fatal call.**
    - `RenderGraph::Fatal(area, why)` replaces the ~16 local `Fail*` helpers and 33 inline `report_and_fail` calls.
    - Release keeps its soft path through `MismatchFatal`.
12. **Wasted D3D11 work in graph modes.** Gate on `!On()`:
    - PostProcessing's per-effect `SetupResources`;
    - the D3D11 shader compiles in SSGI, Skylighting, TerrainShadows and HDR.

Tests: mode smoke ×3, menu, resize ×3, hdr-resize ×2, window-resize ×3, PP batch, imagespace-transitions, map, river,
bink ×2, upscalers, fg, native-fg, screenshot-clean, cloud-shadows, shader-reload ×3, latch-census-short, native-census.

## Phase 4: DCLF (about 4–5k LOC)

1. **DCLF installs only in graph mode.**
   - What goes:
     - runtime on/off and hand-back: `switchedOn`, `UpdateActive`, `SetActive`, `ForceOff`, the `WatchGraphFailure`/`WatchScenePump` hand-back branches, the `PassCapture` bypass;
     - the `a_running == false` branches;
     - the render-thread waits that only `SetActive(false)` uses (`DrainAsync`, `RootStatesWait`, `WaitVisibilityReadback`).
2. **The "stays native" class toggles are removed.**
   - What goes: the 22 feature toggles plus core `shadows`/`projectedUv`, their seeds, the toggle placement machinery, `EngineState::Off`, the gate epochs, `DropWhenOff` and the shadows-off branches.
   - What stays: cull mode, pipeline, CPU record mode and snow sparkle.
3. **The engine-only caster subset is deleted.**
   - What goes: `EngineOnlyCull`, `EngineCasters.h` and the coordinator side.
   - A refusal is logged at classification instead.
   - Before cutting, confirm with `CS_DCLF_TEST_REFUSE` that the subset's draws are skipped today.
4. **The GPU forward-family path is deleted.**
   - What goes: `FORWARD_DIRECT=0`, `FORWARD_PLAIN`, `ForwardDrawPass`/`RefractionDrawPass`, the forward bucket plan, the first-person engine fallback.
   - What stays: `core.forward-sort`, which also sorts plain decals.
   - Precondition: guest always has DXVK's `EmitExternalCommands`. Phase 5 makes every fork export required.
5. **The CPU pipeline's Parallel record mode and PerSnapshot cadence are dropped.** Both are off by default, and native forces them off. Serial and Indirect stay.
6. **`HostEventRing` merges into `RunReadback`.** RunReadback gets a growable, count-prefixed part and one stamp scheme. It then becomes ORG's readback service (phase 6).
7. **`SharedChunks` folds into `KeptChunkStore`.** First check the release threads, because SharedChunks avoids `use_count` sharing on purpose.
8. **Schedule leftovers.** `kAsyncOrder` duplicates `kFrameOrder`. The runtime's built-in epoch lists become fatal when unconfigured.
9. **LoadHold re-entry.**
   - First, a run checks whether `BeginReentry` ever fires now that phase 3 declared the engine targets. Use a tour plus a load and a teleport.
   - If it never fires: make an uncovered shadow or occlusion epoch a tap error, and delete the re-entry and the per-frame `ImportShadowDepth`.
10. **The DCLF frame clock is kept.** It is the scene step, which holds in menus and loads; it is not a copy of `FrameClock`. Rename it to say so.

Tests: dclf-toggles ×3, dclf-family, mode smoke ×3, guest-interior, native-interior, native-tour, flicker-taa, rain,
river, map, shader-reload ×3, perf-quick (one sample per mode).

## Phase 5: mode layers (about 2k LOC)

1. **One table of required DXVK fork exports** (`src/DxvkExports.h`).
   - What goes: the optional-export fallbacks ("older DXVK" flushing, missing queueSubmit2, `CanWrapTextures*`), DxvkPresenter's own export struct, and ad-hoc `GetProcAddress` in about 15 files.
   - A missing export is fatal when a graph mode is asked for; everywhere at RC.
2. **The presenter interface takes the per-frame present duties.** Today these live in mode branches in `Hooks.cpp:612-660` and in direct `DxvkPresenter::Get()` calls.
   - The interface gets `BeginFrame`, `CopyFrame`, `HoldPresent`, `AfterPresent` and `SetColorSpace`.
   - A shared base holds the serials, the colour-space rule and the step storage.
   - NativePresenter is implemented inside NativeDevice instead of through 9 `Native.h` functions.
3. **One unbacked-target mechanism.** The tripwire unbacked handle serves both modes.
4. **One way to submit native work outside the graph.** The ORG runtime and the `ExternalWork` client are created at native device creation. `Immediate::RunRhi`, `Route` and `Commands` become one path. Once that exists, the swap chain uses one back buffer: the kFRAMEBUFFER slot from stage 3.
5. **Native image lookups go through ImageRegistry only.**
6. **Native emulation services the engine no longer reaches shrink to stubs:** the End/GetData query map, and engine pool constant-buffer `Map`. Confirm with one `native-tour` run with `tripwire_log` first.
7. **DXVK fork:** delete the guest-device and headless swap chain exports (`3c6affbd`, about 760 lines).

Tests: mode smoke ×3, menu, resize ×3, window-resize/window-drag/resize-drag ×3, hdr-resize ×3, guest+fg, native-fg,
native-d3d12-fg, upscalers, native-tour.

## Phase 6: libraries, CS-only (ORG about −3.8k, OMS about −1.3k, CS about −700)

1. **GraphHost APIs with no caller.**
   - What goes: held tickets, the singular `AttachStagedEpochGraph`, `DropStagedEpochGraph`, `CanUseEpochRecording`, `FramesExecuted`, `ExternalRunActive`, `AttachStatsTotal`, `DestroyGraph`, rest-at-last-use, and closed epochs.
   - The closed-execution mode goes too (`m_closed` through admission and the plan cache).
2. **`IUploadService` recorded uploads go.** Nothing produces them: `SubmitRecordedUploads`, `RecordStagedUploads(Routed)`. Check the `taken.supported = false` synchronous branch.
3. **One pipeline stack: OMS.**
   - DCLF's draw pipelines move from OMS `PipelineService` onto `PipelineRegistry`. `PipelineService` is then deleted.
   - ORG's `PipelineLibrary` and `PipelinePermutations` are deleted, along with `pipelineCompiler`, `declaredPipelines`, and `RequestPassPipelines`/`AwaitPassPipelines`. The last of these blocks.
   - `GenerateMipsPass` and `ResolveMultisamplePass` move to OMS on CS's programs. CS's `GenerateMips.*` is then deleted.
   - This **replaces** the extraction plan's phase 3 "decision first".
4. **One readback service.** ORG's `IReadbackService` becomes the kept-recording readback (RunReadback + HostEventRing from phase 4). The old capture path is deleted.
5. **One thread pool.**
   - CS passes ORG an `ITaskService` over its `Executor` (about 60 lines added).
   - `ThreadPoolTaskService` is deleted.
   - Phase 6.3 must land first, because it removes `AwaitPassPipelines`, which would deadlock a shared pool.
6. **Dead library code.**
   - ORG: `PassId.h`, `DescriptorSnapshots.h`, `ShaderCompilerService.h`.
   - OMS: `ShaderServiceAdapter`, `Services.h`, `JournaledBufferSet`.
   - BR's buffer journal/image mode (`BufferJournal`, `VersionedBufferFamily`, `BufferImage`, the journal half of `VersionedBufferBuild`). DCLF's `ChangeJournal` is then the one design.
   - `MpscQueue`, replaced by CS's `EventQueue`, which moves into ORG `Render/Runtime/` (ORG can't depend on OMS).
   - The unused `ExternalPassDesc` options.
7. **One `ShaderCompiler` instance.** `DxcShaderStore` uses the runtime's.
8. **FrameStaging moves onto `FrameArena`** with an oversize region. `RunSlots` stays.
9. **The contributor ABI leaves CS.** It exports no game targets, and it warns on exceptions instead of failing. ORG's contributor code stays for SARP.
10. **Library env switches.**
    - Behaviour overrides become `Desc` fields or are deleted.
    - Traces CS's tools cover are deleted, including the decision trace unless `org_trace.jsonl` is still read.
    - OMS stops reading `SARP_GPU_LIFETIME_TRACE`.
    - `ORG_VALIDATE_UNDECLARED*` stays.
11. **The CS switches for ORG's native-only host options** (coalesce, merged handoffs, uploads-with-batches, keep-live, keep-gated) are fixed in phase 1. Run one guest A/B per option. Where guest can run with the option on, ORG drops the off path.

Push `cleanup/integ` upstream for review before this phase pins anything new (decision 89).

Tests: mode smoke ×3, shader-reload ×3, dclf-toggles ×3, post-effects, dynamic-cubemaps, latch-census-short, resize ×3,
perf-quick (one sample per mode), the ORG and OMS unit tests.

## Phase 7: the guest context works like native (L, about 1.5k LOC)

The guest engine-context handle stores binds and drops draws and dispatches, as native does. Only the native service set
reaches DXVK: Create, Map/Unmap, UpdateSubresource, Copy, Clear, GenerateMips, End/GetData, Flush.

This removes:
- the latch half of Flush;
- StateTrace;
- `UnwrapArray` forwarding for 89 methods;
- `GuestDropsUnbacked`;
- the "handle reached DXVK" class of crashes.

Precondition: no CS D3D11 draw in guest graph mode may depend on engine-bound state reaching DXVK. Known cases are
`PayOwed`, the shader binds at `Hooks.cpp:283-293,1326-1373`, and ImGui's `OMGetRenderTargets`. Each gets a fatal
check first. Then move each one to explicit state, or to the graph.

Tests: every guest config, plus mode smoke for native (shared tripwire code).

## Removable at RC (not in this plan)

When plain D3D11 drops, about 12k LOC go:
- **Feature D3D11 fallbacks (about 11k).** PostProcessing 4k, VolumetricClouds 1.15k, CloudMaps 900, DynamicCubemaps 620, Upscaling 540, SSGI 480, FxRuntime 450, ViewAtmosphere 330, LocalShadows 300, VolumetricShadows 240, EHF 230, SunRays 210, SSS 200.
- **Mode gates:** about 135 one-line `On()` gates.
- **Streamline:** the DXVKInterop command ring (about 800–1,000).
- **Unwrap:** about 110 calls.
- **D3D11 plumbing:** `D3D11Wrapper` and the twins' D3D11 exports, `CS_NATIVE_D3D11`, DxvkPresenter's plain role, and ImGui's dx11 backend.
- **LLF parity:** it needs D3D11 culling as its reference.

## Effect on the extraction plan

| Extraction item | After this plan |
|---|---|
| Phase 1 EventQueue → OMS | Goes to **ORG** `Render/Runtime/`, replacing `MpscQueue`. |
| Phase 1 violation policy | Unchanged. Release stays soft, so the policy is still needed. |
| Phase 1 frame clock | **Corrected**: DCLF's clock is the scene step, not a copy. Only CS's `FrameClock` is a candidate. |
| Phase 1 kept state | The "two journal designs" risk **vanishes**: BR's journal mode is deleted (6.6). `SharedChunks` is already folded in (4.7). |
| Phase 1 retirement chain | The DCLF forwarding headers are already gone (phase 1). |
| Phase 2 CPU recording pools | Shrinks to `CullArena`; the Parallel mode's list pools are gone (4.5). |
| Phase 2 SIMD sphere cull | Simpler: the level comes from CPUID only (the `kExp*` bits are gone). |
| Phase 2 UploadRing core | Merged with FrameStaging onto ORG `FrameArena` (3.8, 6.8). |
| Phase 3 "decision first" (pipeline compiler adapter) | **Replaced** by 6.3: ORG's pipeline stack is deleted, and OMS's is the only one. |
| Phase 3 programs | One queue with no hold protocol (3.3). The "explicit Want + HoldForPrograms API" fix vanishes. |
| Phase 3 frame staging | Already done (6.8). Only `RunSlots` moves. |
| Phase 3 readback | **Already done** as ORG's one readback service (4.6, 6.4). |
| Phase 3 bindless zero views | The CS facade is already gone (3.8). Only `Null(ViewKind)` moves. |
| Phase 4 DCLF build state, recipe check | Unchanged. |
| Phase 5 DCLF revision participant | **Vanishes**: its cache is already `EpochRecordings::Cache`. |
| Phase 5 keyed pass schedule | **Vanishes**: the recipe and DCLF's schedule do different jobs. |
| Phase 5 declarations | Smaller and cleaner after 3.2, 3.3, 3.4 and 3.7. |
| Phase 5 native presenter | Smaller, behind the mode-free interface from 5.2. |
| Zone tallies, RenderThreadBudget | Unchanged. They stay as product telemetry. |

## Size

| Phase | LOC |
|---|---|
| 0 | small |
| 1 | about 4k (plus 1k stubs from the no-ORG build) |
| 2 | about 15–18k, plus about 4k tool lines and 3k generated |
| 3 | about 4–5k |
| 4 | about 4–5k |
| 5 | about 2k, plus 760 in the DXVK fork |
| 6 | about 5k in ORG/OMS, 700 in CS |
| 7 | about 1.5k |

In total, about 35–40k lines leave CS and about 5k leave the libraries, without losing a function. RC removes about
12k more.

## Order and parallel work

- Phase 0 first. Then the "checks that need a run" (handoff section), since some use diagnostics phase 2 deletes.
- Phases 1 and 2 touch mostly different files and can run in parallel.
- Phase 3 before phase 4 (DCLF registration uses `DeclareClient`).
- Phase 5 can run beside phase 4.
- Phase 6 after phases 3–5. 6.3 lands before 6.5.
- Phase 7 last.

## Open questions

### Phase 1, DCLF (`org-native/simplify-p1-dclf`)

Decided by the agent, to confirm:

- **`SceneScheduler` stays, smaller.** Its executor and worker-count forwards are gone. `Scope()` and `Graph()` stay, because they own DCLF's scope and scene graph.
- **The five check bits went with their checks.** The wide-kernel and portal-batch compares, the scene lists' full-build check and the cull and portal profiles were reachable only through `dclf:cpu-exp`. With the command gone they had no switch.
- **The AVX `PlaneGroup` kernels are deleted.** Only the per-group portal walk used them, and op-major was always on. `PlaneGroupSse` stays under the SSE chunk kernel.
- **The executor tuning switches are core switches.** `CS_ORG_WORKERS`, `CS_ORG_FRAME_WORKERS` and `CS_ORG_ASYNC_PRIORITY` live in `RenderGraph/Async/Lanes.cpp`. No config set them, so they are constants now.
- **A device without depth clamp fails in `DrawPipelines::Impl::Initialize`.** That is DCLF's first pipeline setup on the graph device.
- **The `tools/cs-ab` scenarios for the removed switches are deleted** (`rx6600-exp-nomerge.env`, `rx6600-exp-pulled.env`). Phase 2 deletes `tools/cs-ab` anyway.
- **The stage dump still writes `"lazyRows":true`**, so `tools/shader-compare` reads old and new dumps alike.

## Open questions: phase 1, graph core part (decided by the agent, to confirm)

- `CS_ORG_EARLY_FLUSH` was off by default (the code read `=1`; its comment said "default on"). It was fixed off and its path
  deleted. The `dclf:early-flush=1` test command still flushes, since the stream timing knobs go in phase 5 (C2).
- Native's synchronous present path stays. Test presents, `CS_ORG_NATIVE_PRESENT_PROBE` and a loading screen thread that does
  not own rendering still present on the calling thread. Only `CS_ORG_QUEUE_PRESENT` went.
- `RunSegment` stays. It is not a forwarder: it finds the stage segment an anchor submits in. It goes with the per-anchor
  epochs (phase 3).
- `CS_ORG_TRIPWIRE_WATCH_ALL` stays as a per-handle flag when `WatchSet` goes. Phase 2 decides the switch.
- The one graph-mode test is `RenderGraph::Engine::GraphMode()` (`Engine.h`). It keeps VR out, as `Engine::On` did, so the
  predicates that did not (`Native::GraphMode`, the latch, the pipeline descriptions, texture streaming's windows, DCLF's
  families) now also say no on VR. `Port<T>::On` stays as the ports' one-line gate (an RC item) and asks it.
- `EngineDepth`/`EngineCubemap` return null before the engine target set is declared, instead of importing.

## Open questions: phase 3, anchors and port boilerplate (`org-native/simplify-p3-anchors`, decided by the agent, to confirm)

- **No anchor table in `Port<T>`.** With reservations gone a port has nothing to do with its anchors before the graph starts.
  `Register`'s recipe check already refuses a pass at an anchor no step names it at.
- **The prologue is a helper, not a templated `Setup`.** `Port<T>::BeginSetup()` returns the host, or null outside graph mode or
  once set up; `SetupHost()` serves the five ports that set up again (`impl.reset()` first). The ports' `Setup` signatures
  differ too much (bool or void, their own arguments) for one template to call a `Make`.
- **Which factories are fatal where.** `RunGate::Create` and the `Declare*` twins are fatal in every build. `GraphResources`
  `Texture2D`/`Texture3D` are fatal only in graph mode, since outside it the runtime is off and a soft miss is the D3D11 path.
  None of them is a "failed client", so Release does not soften them. `FrameAnchors::Register` keeps its soft returns (p3-declare).
- **`HeldByStage` and `RecordsAtExecution` stay, smaller.** Their fixed answers went at the call sites. `HeldByStage` still
  answers no before the runtime freezes and outside graph mode (Scaleform's displays, the present hook's capture, Sky's
  `RecordsLate` for the shared `EngineQueries`). `RecordsAtExecution` keeps the shared anchors' answer (the `EngineDraws` families).
- **Skeleton's cube group stays, with no anchor.** Setup marks it failed and its clears are dropped, as before. Deleting
  `Group::Cube` is left to the Skeleton registration rewrite (3.2). Its shared-anchor hosts keep `Segment::Count` as their
  segment, which is what `RunSegment` answered in effect; `prepass` and `deferred-passes` name their stages.
- **`commitBefore` loses the anchor segments with no replacement.** They were never submitted. Adding the UI stage's segment in
  their place was not done, since it would change when the world stages commit.
- **The census's `FrameSegmentOf`** drops the UI anchors' slot 9; the UI stage keeps the default slot until phase 2 cuts it.
- **The self-tests** lost only their `Reserve` calls, so they build until phase 2 deletes them.

Open after this part:
- The ports' bool `Declare*` wrappers (Exponential Height Fog, Upscaling, Volumetric Lighting, Glints noise, the deferred
  composite, Cloud Shadows' copy client) can only return true now; they can become void with their callers' checks.
- The `if (!programs)` and `if (!made->program)` checks wait for the programs queue (3.3).
- The DCLF families' soft run-gate warnings are dead code now (phase 4).
- Upscaling's `evaluateShared`/`inputsShared` are always made; their null checks can go.

## Open questions: phase 3, resources part (decided by the agent, to confirm)

- **The deferred composite's unregistered client is lag, not a tap error** (decision 103). Before the client registers, and
  while it registers again for new inputs, nothing composites and D3D11 does not either. Only SetFrame's refusals (Skylighting's
  probe array null, an unknown normals target, an engine target that cannot be imported) are tap errors, as `Refused`. A tap
  error there would stop the game in its first frames.
- **The composite reads inputs by published id, and the D3D11 view only says the slot is bound.** The compressed cubemap ids
  are dropped there, since Deferred binds the uncompressed ones. The producers' exports stay until no D3D11 reader is left.
- **DCLF `FrameTexture` still maps views back to exports** (`Extensions::GraphTextureIndex`). Dynamic Cubemaps lists two ids
  at one register and its view picks between them, so "published" alone cannot choose. It needs an id callback in
  `Api::FrameTexture` (the producer names the id it reads this commit). Not done in this part.
- **Only Cloud Shadows became a declared twin.** The other D3D11 imports are live in graph mode: Screenshot's HDR clean capture
  and Upscaling's hudless source are HDR's D3D11 clean composite, and ImageSpace's flow map is the engine's texture. Skeleton's
  copy is left for p3-declare. So the eight import entry points stay for now.
- **Post Processing sets up only Color Grading's D3D11 resources in graph mode.** LUT loads its textures in Prepass, and the
  settings page still sets an effect up when it is opened. `ClearBorderMotionVectorsForFrameGen` stays: it runs before frame
  generation tags the motion vectors.
- **HDR Display treats graph mode as "HDR output ready"** in `PostChainTarget` and `UsesDeferredPresentComposite`, since its
  D3D11 output shader is no longer compiled there. Its D3D11 users still compile it on first use.
- **`Vars::DynamicArea()` reads the live engine ratios, as `Util::ConvertToDynamic` does.** "From the latch" was read as a
  per-frame value at the commit; no latch field carries the ratio yet.

## Open questions: phase 3 declare part (`org-native/simplify-p3-declare`, decided by the agent, to confirm)

- `Placement` keeps a fifth field, `built`. Screenshot, Terrain Blending, Bink and Skeleton ask whether the stage's built graph
  holds the registration, and that is not derivable from `draws` and `reason`. `held` became `InStage()`.
- `Placement` and `Reason` live in `Frame/Placement.h`, so `Declare.h` carries `Held`'s placement without `FrameAnchors.h`.
- `FrameAnchors::Stage` returns a `Placement`: reason None while open, Absent with no schedule yet, NoStage otherwise.
- `Driver::NoteScheduled` takes a `Reason` (None, Behind, Absent). The stats line is unchanged.
- `Held<void>` exists: a void client's `Held` gives its placement, and its bool is `draws`.
- Upscaling's formats are fixed at setup, so its three program tables became `Programs::Queue` handles, not `DeclareProgram`s.
- Upscaling's evaluate client was registered inside the upscale client's make. It is its own declared client with a constant key.
- Deferred clears, GPU load, Grass Collision and Cloud Shadows' layer ops are `DeclareClient<bool>` with a constant key. They
  register at the next revision start (setup's build point at setup). GPU load declares wherever `SetupLoad` runs on the render owner.
- Not done here, left for a follow-up: Skeleton's registration (so `FrameAnchors::Register` stays public), and the program
  tables of ViewAtmosphere, ImGui (with its `HoldForPrograms`), Bink, ImageSpace (with WorldSteps' hold and
  `SpecProgramsBuilding`) and Scaleform. `HoldForPrograms` and `Programs::Table` stay until those move.

## Open questions: phase 3 declare part 2 (`org-native/simplify-p3-declare2`, decided by the agent, to confirm)

- **Skeleton keys on observed facts.** Each group's anchor-host and UI-place clients are `DeclareClient<BuildKey, GroupBuild>`.
  A frame start observes each group's views and textures (`Observed<GroupFacts>`) and each copy's textures. Equal facts give one
  shared build, so every client of a group registers the same gates. The frame uses the adopted registration's build
  (`DeclaredClientOf::Adopted`); stage places use `Held`'s. Setup adopts, so frame 0 has a build.
- **A remade target still lags one revision.** Its clears and copies log one reason line until the revision with the new key
  is adopted. Nothing can clear a texture the adopted graph does not hold. The separate `stale`, copy and Build-wait lag paths
  are one path now.
- **Skeleton clear programs are a `ProgramSet`.** Setup's formats join the setup batch; a new format is wanted at the frame start
  that sees it, before its facts are observed, so the revision that registers it waits for the program.
- **`Sites::Flush` is gone.** It only rebuilt stale groups. The bind-targets hook is now a pass-through, left patched.
- **The two self-tests** (`CS_ORG_GRAPHRES_SELFTEST`, `CS_ORG_ANCHOR_SELFTEST`) declare their clients on an `Observed` of their
  resources, so `FrameAnchors::Register` has no user outside Declare. It lives in `Frame/DeclareRegistration.h`.
  `Unregister` stays public: ImGui's shutdown calls it.
- **DCLF programs are `SetupPrograms::Compute` handles.** They are queued in `DrawcallLimitFix::SetupResources` and built by CS's
  setup join. That covers every lazy frame-time `ComputeProgram::Load`, the async `Draws::LoadProgram` cache, and `PrefixSum::Load`.
  None of them depends on frame state. Their descriptions read only setup switches (RefTrace, the portal switches). The foliage
  parity program is always queued, because its diagnostic can be turned on later. The 18 portal batch programs (9 entries, with
  and without the context atlas) are all queued.
- **`Programs::Table` and `HoldForPrograms` are gone.** Frame-met key sets use `Frame::ProgramSet` (Frame/ProgramSet.h): an
  `Observed` of the wanted keys, plus one `DeclareProgram` whose build loads a revision's new keys in one pool job. Late keys
  therefore compile one after another.
- **ImGui** is a `DeclareProgram` on `HdrEnabled`, keyed on the UI target's format. **Bink** queues both formats at setup.
  **ViewAtmosphere** is keyed on an Observed reload count.
- **ImageSpace with a missing program.** Its part keeps its registered key, which has the same effect as the old hold. Setup now
  evaluates once, so a key that setup's evaluation meets registers one revision later.

## Open questions: phase 3 rest (`org-native/simplify-p3-rest`, decided by the agent, to confirm)

- **One fatal call's text.** `RenderGraph::Fatal(area, why)` logs `[ORG] fatal: <area>: <why>` and shows
  `Community Shaders: <area>: <why>`. The cs-test `no_log` patterns that named the old texts ("a frame declaration failed",
  "a frame revision failed", "Post Processing failed on the render graph", "Effects11's FX chain failed on the render graph")
  now match `fatal: <area>:`. A tap error that stops the game logs `[ORG] fatal: frame tap error: ...`, so "frame tap error"
  still matches it.
- **DCLF keeps the old names for now.** `RenderGraph/PortFatal.h` is a forwarding shim, and `FrameAnchors::EngineTarget`,
  `EngineDepth` and `EngineCubemap` are inline forwarders to `EngineTexture`. Only DCLF calls them. Its merge is in flight.
- **Helpers that build a message stay, and end in Fatal.** `Native::Fail`, `Native::RequireDclf`, `FaultIsFatal`, Frame
  Anchors' frame-revision `fail` lambda and the twins' adopt lambdas keep their text. Soft helpers that are not fatal stay as
  they are: Scaleform's `Failure` log, Skeleton's `FailedRun` (a tap error), the builders' `Fail` records, Effects11's
  `fail` lambdas, and `FailClients`' Release path through `MismatchFatal`.
- **Not touched by 3.11:** the presentation code's `report_and_fail` calls (Upscaling's DxvkPresenter, FramePresenter,
  PresentationRevision, Streamline), Hooks.cpp, PostProcessing.cpp, Linear Lighting's hooks, XSEPlugin and DxvkLoader. They
  are neither the render graph nor its ports.
- **The void Declare wrappers keep their real checks as Fatal.** A failed publish, an exported twin without its D3D11 export
  (Upscaling) and Exponential Height Fog's `Complete()` are fatal where they are found. Only the null-twin and null-client
  checks went. Other ports still check `DeclareClient` results that cannot be empty (Grass Collision, Physical Sky, Post
  Processing, View Atmosphere, Bink, Scaleform, Skeleton, ImageSpace's `DeclareSteps`); left for a later pass.
- **3.10 is only the API field.** `Api::FrameTexture::graphId` names the id a producer reads this commit. The registrations
  (`Integrations/*`) and `Extensions::GraphTextureIndex` are inside DCLF, so they wait for its merge. Until GraphTextureIndex
  reads by `graphId`, every producer's `Export::SRV` stays: the lookup still matches the view against the export.
- **3.9's three entry points** are `EngineTexture(TargetRef)`, `ImportTexture(id, ImportSource)` and `ShareImage`. Callers
  name the full id, so `cs.d3d11.<name>` is a convention and every id is unchanged. `FrameAnchors::Colour`, `Depth`,
  `Cubemap` and `ColourCopy` are the TargetRef names re-exported. `EngineTexture` also answers `ColourCopy` and `Volume`
  targets with their slots, which no caller asks for yet.

Open after this part:
- DCLF: move to `Fatal` (its `PortFatal` callers, `SequenceLimitFailure`, and the inline `report_and_fail` calls, most in
  `Core/Extensions/Registry.cpp` and `Core/Render/Epochs/ScheduleExecutor.cpp`), and to `EngineTexture`. Then delete
  `PortFatal.h` and the three forwarders.
- DCLF 3.10: set `graphId` in Dynamic Cubemaps' registration (and wherever a view picks between ids), read it in
  `GraphTextureIndex`, then drop the `Export::SRV` of each producer whose export only that lookup reads.

## Open questions: phase 6, libraries (`org-native/simplify-p6-libs`, decided by the agent, to confirm)

- **The `ExternalPassDesc` options stay.** None is unused inside ORG: the readback capture path sets `PinToQueue`,
  `InterruptsFrame`, `RegisterByName` and `CollectStatistics(false)`, the host's upload extension sets `CollectStatistics(false)`,
  and the contributor host maps `ORG_RG_PASS_GEOMETRY` and `ORG_RG_PASS_DISABLE_STATISTICS` onto the other two. They go with
  the old capture path in 6.4. Only `copyPass` went, with `ORG_PERSISTENT_COPIES_ON_COPY_QUEUE`.
- **`DestroyGraph`, `AttachStatsTotal`, `TakeAsyncStats` and `TakeRunMemoryStats` stay.** ORG's destructor and its own tests
  use them (the contributor test checks retirement after `DestroyGraph`; the host tests read the stats).
- **Every execution is open.** The ledger lost `closeToHome` and `restAtLastUse`, the admission its plan cache and out-of-order
  pending admissions, and `ExternalClient::Desc` its `closed` flag. The host tests' `open` argument now only names the output.
- **CS's `EventQueue` header is a one-line alias of ORG's**, with `QueueTeardown::Leak`, so the 35 DCLF includes did not
  change (the DCLF diagnostics merge runs in parallel). ORG's queue frees its ring by default; CS's process-lifetime queues
  still leak. ORG's version adds `Pop` and `Count` for its own users.
- **The `taken.supported = false` branch is already fatal.** A ticket whose uploads are not pointer-targeted buffer copies
  (tracked uploads, a non-buffer copy) fails with "Async epochs cannot record these uploads". No synchronous fallback is left:
  `RecordPendingUploads` had no caller and went.
- **Sharing validation keeps its API.** `ORG_VALIDATE_SHARING` went; it is on in debug builds and through
  `SetSharingValidation`. The alias admission check went with `ORG_VALIDATE_ALIAS_ADMISSION`. `ORG_GRAPH_REPLAY_OUTPUT` stays
  for ORG's replay test.
- **The decision trace went whole**: nothing reads `org_trace.jsonl`. `PreparedBackingState::debugName` went with it, so
  BasicRHI's command census still labels lists by pass but no longer fills resource names.
- **OMS (worker):** DXC game shaders compile only once the render graph runtime has a compiler; `ShaderCompiler::CompileHere`
  keeps the shader cache's compiles on its own thread. The old leaked static compiler is now the runtime's, so a cache compile
  during runtime shutdown would use a freed compiler.

## Open questions: phase 4 state part (`org-native/simplify-p4-state`, decided by the agent, to confirm)

- **4.6 list readbacks.** `RunReadback::CreateList` holds one growable, count-prefixed part: one region per stable run (16,
  as `RunRanges` gave), one buffer set per capacity (`Grow`, `SetFor`), `Wanted` for growth. Lists use RunReadback's sequence
  stamps: `CommitList` reads the region's last run, then returns this run's stamp, and the depth commit writes it into header
  word 1. The scene frame no longer stamps the lists, so `VisibilityEventsCS` stops writing it. One change: a run whose
  visibility pass dispatched nothing (no slots) now gives an empty batch, not a missed one. The cull counters' `CounterReadback`
  is a third scheme and is left for phase 6.
- **4.7 lookups.** `SharedChunks` is `KeptChunkStore<T, 64, KeptSharing::kByStamp>` (`LookupChunks`). It keeps SharedChunks'
  rule: a stamp moved by `Snapshot` decides sharing, never a `use_count` a release thread moves. It retires and reuses nothing,
  so a replaced chunk and the owners its entries hold go with their last holder, as before. A shrink resets the dropped
  entries. The non-const `operator[]` is off in that mode, so reads never copy.
- **3.11 DCLF fatal calls.** Every call uses the area "Drawcall Limit Fix" (SetupPrograms keeps "DCLF"). The message text
  is unchanged; the log line is now `[ORG] fatal: Drawcall Limit Fix: ...`. `SequenceLimitFailure` stays as a helper that ends in
  Fatal.
- **3.10 graphId.** `Extensions::FrameTextureIndex` reads the id `graphId` names (declared and published) and matches no view.
  Set for Dynamic Cubemaps (`boundReflectionsId`, set in PostDeferred, and the environment cubemap) and Atmosphere and Fog's
  medium. Only the medium's exports were dropped: its views had no other reader. Its frame textures read null until the
  medium's ids are declared, where they used to import the export. Every other candidate keeps `Export::SRV`, because a
  D3D11 reader or a setup check still uses it: Dynamic Cubemaps (composite, IBL, engine draws), Skylighting (Deferred.cpp,
  EHF, View Atmosphere, Skylighting.cpp), IBL and LLF local shadows (EHF's D3D11 path; IBL's setup checks `SRV()`), Terrain
  Shadows (Sun Rays, EHF, TerrainShadows.cpp), Terrain Blending mask (TerrainBlending.cpp), Screen Space Shadows (`Prepass`),
  glints noise (TruePBR.cpp), Cloud Shadows occlusion (CloudShadows.cpp), EHF integrated (ExponentialHeightFog.cpp).
  Physical Sky slots, Volumetric Clouds aux and Volumetric Shadows copy were not audited. Each needs a per-feature pass that
  moves its D3D11 readers first.
- **Dead DCLF paths (worker).** The unpinned-geometry mirror recheck went with `NoteMirrorRead` (it only fed report counts); the
  `[DCLF] mirror reads` line loses its verdict. FadeFrame's reserved word is now uploaded as 0, not ~0u; no shader reads it.
  kBuildDraws and kBuildDrawsView were one description, so the view setup reads kBuildDraws.

## Open questions: phase 4, install, toggles and casters (`org-native/simplify-p4-install`, decided by the agent, to confirm)

- **Outside graph mode DCLF does not install, and CS's SceneWriterFramework does not elect.** PostPostLoad and PostLoad test
  `RenderGraph::Engine::GraphMode()`. A setup without the graph, its host or the indirect pipelines is fatal (`RequireDclf`).
- **The scene pump watchdog stays as a fatal check.** Only its hand-back went. A failed epoch is fatal at once
  (`OnGraphEpochFailed`), not at the next frame start, so `WatchGraphFailure` and `GraphEpochFailure` went.
- **`ShellState::Running` went with every branch on it.** After install it was always true. `SceneTracker::Stop` and the PassCapture
  bypass went with `ForceOff`. The fade write-back wait (`WaitFadeWriteBack`, the root states' `wait`) and `WaitAhead` went with
  `DrainAsync`, their only caller.
- **`DropWhenOff` became `DropWhenNone`.** First person still drops its shape when none is made (no targets or claims); only the
  toggle test and its shapes-key bit went. Deleting it would keep first person's last shape.
- **The engine-only colour subset went with the caster subset.** A reflection face that skips an LOD root no longer adds back the
  engine-only roots under it; graph mode skips their engine draws anyway. The whole-cull counts moved to `Core/Handoff/WholeCull`.
- **The refusal log** names the first refused geometry of each `Ineligible` reason that casts a shadow (caster rule none or
  volumetric only), as "'<name>' stays the engine's (<reason>, caster rule <rule>): it casts no shadow".
- **Plain snow sparkle is the one table flag left.** The menu's "What DCLF draws" is now "How DCLF draws". Saved `Toggles` entries
  for the removed toggles are ignored. `Ineligible::ProjectedUV` is kept, unused, so the census's indices stay.
- **cs-test has no DCLF sweep families.** `gen_schema.py` no longer marks DCLF switches `sweep`; `dclf.*` globs match nothing and
  the sweep examples use `features.*`.

## Open questions: phase 4, paths (`org-native/simplify-p4-paths`, decided by the agent, to confirm)

- **Graph mode needs DCLF's schedule.** With the built-in epoch lists gone, `RenderGraphRuntime::Initialize` fails when DCLF did
  not configure the frame (`CS_DCLF=0`, an unsupported game), and that is fatal in graph mode. dxc-slice's "DCLF off" note says so.
- **The forward epochs must record at execution.** `UpdateGraphConditions` stops the game when a GPU-driven graph's forward or
  first-person epoch does not (`RecordsAtExecution`). Guest's in-stream list and native's inline recording always include them.
- **The GPU forward staging stays in the shaders, unused.** BuildDrawsCS's forward bucket and class staging paths and the first-person
  forward staging get `forwardCapacity` 0 or no section, so they stage nothing. `Requirements.cpp` still sizes the forward halves in the
  sequences buffer. Deleting them is a shader and layout change left for later.
- **Undrawable shadow views and occlusion maps are held during any hold**, as before (`LoadHold::NoteUndrawable`), so a load's hold
  still waits for them. Only outside a hold are they tap errors (`Uncovered`); before the world draws and under a loading screen they
  are neither, as the re-entry was.
- **`ImportShadowDepth` stays at placement build and per capture.** Only the per-frame re-import went: since phase 3 the engine target
  set refills a re-created depth target with its own texture, so the shadow maps' textures never change in a session.
- **The CPU pipeline's EmitCommands failures are fatal**, the hand-off cancel and the frame signal included; `notQueued` went with them.

## Open questions: phase 6, pipelines and tasks (`org-native/simplify-p6-pipes`, decided by the agent, to confirm)

- **DCLF's builds go through OMS `BuildPipeline`, a free function next to `PipelineRegistry`.** The lane already keeps its own
  record of what it asked for, so the service's keys, dedupe and active set had no job. A target or format change now builds a key
  again instead of finding the service's active pipeline.
- **The OMS passes record through `org::services::ComputeKernel`**, a two-method interface. `ComputeProgram` implements it, so the
  passes need no CS type. A pass is made only once its kernel is built (Fixed::Get, DeclareProgram), so nothing waits.
- **Native's context GenerateMips still loads its two programs with `ComputeProgram::Load` when the compiler comes up.** That is
  setup, not a frame, and a context GenerateMips called before a queued build finished would leave the mips wrong.
- **The shaders ship under `Data/Shaders/ORGModuleServices`.** The old `package/Shaders/OpenRenderGraph` stays ignored for old
  checkouts.
- **ResolveMultisamplePass moved but has no CS user.** No in-game test covers it.
- **Task priorities on the executor:** frame-critical to `DispatchFrameCritical`, streaming (graph builds) to the preparation pool,
  background (retirement, readback, upload, command list resets) to the blocking threads.
- **ORG's scopes stay counting scopes in the adapter**, as ORG's pool had them, so `Wait` works on an executor thread too (the
  executor's own scopes refuse that). A task that throws is fatal; ORG's pool swallowed it.
- **`Runtime::Desc::tasks` is required.** ORG's tests and OMS's WARP tests use the old pool as a header-only `TestTaskService`
  (`OpenRenderGraph/tests/TestTaskService.h`).

## Open questions: phase 6, staging and readback (`org-native/simplify-p6-staging`, decided by the agent, to confirm)

- **The readback service is a per-reader object, `org::RunReadback`** (`Render/RunReadback.h`), not a runtime singleton.
  `Runtime::Readbacks()` and `IReadbackService` are gone. Its errors throw `std::runtime_error` after a critical log, since ORG has
  no fatal hook; CS's commit paths do not catch them, so they still stop the game.
- **Part readbacks register in ORG for the staging-run move.** The stage commit calls `org::RunReadback::MoveStagingRun` next to
  `RunSlotsRegistry::MoveAll` and `LatchBlock::CopyStableRun`.
- **`CounterReadback` folds in as a third scheme, region readbacks** (`CreateRegions`, `RegionOf`, `RecordCopy`, `ReadRegion`).
  Its arm, stamp and read logic stay in DCLF: the counters copy DCLF's own latch stamp, which the parts' stamp scheme cannot
  replace without changing `BuildDrawsLatch`. Grass's and Tree LOD's cull-count readbacks (`GrassCounts`, `TreeLodCounts`) use the
  same per-range scheme and can move onto region readbacks next; `RunRanges` then goes.
- **The ExternalPassDesc options that went:** `InterruptsFrame`, `PinToQueue` (with `RenderPassBuilder::PinToQueue`, which had
  no caller) and `RegisterByName`. `GeometryPass` and `CollectStatistics` stay: the contributor host and the upload extension set them.
  The schedule window lost its capture view with the old path.
- **FrameStaging is one function, `RenderGraph::AcquireStaging`, on `FrameArena::AcquireOversize`.** An oversize region has a
  chunk of its own at offset 0 (so the views the clients bind need no offset), is not zeroed, and is freed by its last holder
  through `FreePinned`. The header keeps its name, since it holds `RunSlots` and 15 files include it.
- **UploadRing merges only for the engine constants.** Their chunks are now arena staging regions, kept for the process. The
  dynamic ring's chunks stay their own D3D11-wrapped buffers: the engine binds each through a DXVK wrap with vertex and index
  usage, made once per chunk, and they are released behind D3D11 work, not ORG's frontier. UploadRing's suballocation stays.

## Open questions: phase 7, the guest context (`org-native/simplify-p7-guest`, decided by the agent, to confirm)

- **A captured slot takes CS's bind first, then the engine's.** In guest mode the engine's binds stay in its context handle and CS's
  reach DXVK's context. DCLF's `CaptureBindings` reads both, buffer and view slots preferring CS's (it binds after the engine's flush,
  as the old shared context saw it), unwrapped to DXVK's objects. Targets and the viewport come from the engine's handle alone.
- **Begin is still forwarded in guest mode**, beside End and GetData, so a disjoint query keeps its pair. Native's Begin does nothing.
- **ClearState resets only the kept binds.** DXVK's context keeps CS's state across the engine's per-frame ClearState now.
- **The guest context refuses `ID3D11DeviceContext1` and later**, as native does, so no engine bind can reach DXVK past the handle.
- **Grass Optimizations does not own grass in graph mode.** Its indirect draws took the engine's state from DXVK's context. DCLF
  draws grass there; native already kept the feature off (no `ID3D11DeviceContext1`).
- **The engine-constants tracker also hooks the handle's binds** in guest mode, with the buffers unwrapped as the map hooks name them.
- **Another plugin's draw on the engine's context is logged as an error** in guest mode too, as native logs it.
- **`GuestDropsUnbacked`** was already folded into one `DropsUnbacked` by phase 5; it stays, since the forwarded services still need it.

Open after this part:
- `CS_TEX_CENSUS` hooks DXVK's `*SetShaderResources`, which no longer see the engine's binds in guest mode.
- Capture parity's draw sub-parity hooks DXVK's `DrawIndexed`, which no engine draw reaches (it checked 0 draws already, decision 99).
- `tools/generate-tripwire-interfaces.py` still writes `TW_UNWRAP_ARRAY` into the context's forward arguments, which nothing expands.

## Open questions: phase 1, the no-ORG build (`org-native/simplify-p1-noorg`, decided by the agent, to confirm)

- `CS_RENDER_GRAPH` and the `CS_HAS_RENDER_GRAPH` define are gone. No submodule read the define. A missing library under
  `CS_ORG_ROOT` is now a configure error, not a warning.
- `CS_HAS_ORG_MODULE_SERVICES` and `CS_HAS_DCLF` stay. OMS is already required (`ShaderStore.cmake`), so both are always on;
  dropping them is a follow-up.
- `GpuCounters::Trace::Statistics::Available()` went, with its "built without BasicTelemetry" branches. It was always true.
- Comments that say "without the render graph" and also hold at runtime (outside graph mode) were left as they are.

## Open questions: phase 8, CS cleanups (`org-native/simplify-p8-cs`, decided by the agent, to confirm)

- **Grass's and Tree LOD's cull counts read in their pass's Record, as before.** Only the storage moved: one region readback
  each (`CreateRegions`, `RegionOf`, `RecordCopy`, `ReadRegion`), and `RunRanges` went. A kept recording that replays does
  not call Record, so a replayed run is not summed. That was already so.
- **A missing ORGModuleServices is a configure error**, like the other render graph libraries. DCLF's tree is always built.
- **ImageSpace's step declarations are void.** A kept texture whose engine target is missing and a failed publish of the
  adapted pair are fatal where they are found, with their own text, not "its textures or clients could not be declared".
- **The skeleton's two bind sites are no longer patched**, so no check stops another plugin patching those two calls.
- **Every audited export stays** (3.10 follow-up). Physical Sky's slots, Volumetric Clouds' aux textures and Volumetric
  Shadows' copy all have D3D11 readers (Physical Sky's, View Atmosphere's, EHF's, Deferred's and the clouds' own binds, the
  shadow copy at t18) or setup checks on `SRV()`, and DCLF's engine-draw handles read the exports too (`IsExportOf`,
  `ExportedId` in the Physical Sky integration). No `graphId` was set, since it would change what the commit reads while the
  D3D11 writers remain.
- **Not done here:** DCLF's `Resources.cpp` still checks programs and signatures that `SetupPrograms` and the queue make
  fatal (the BuildDraws, portal query, forward sort, index pool and visibility events checks). ImGui's
  `ImGuiBasicRHI::DeclareProgram` still returns a bool that is always true. `tools/dclf-layout-check.py` still names the
  removed CS_HAS_* macros in its dependency rule.

## Open questions: phase 8, library, docs and tools cleanups (`org-native/simplify-p8-lib`, decided by the agent, to confirm)

- **Payload binding stays in ORG.** `PipelineState` and its overloads went, but `Bind*Pipeline(shared_ptr<const PipelineStatePayload>)`,
  `DescriptorSlotIndices` and the feature-domain check have only ORG's tests as callers. They are ORG's declaration-checked binding,
  so they were kept. `FeatureDomainRegistry` has no CS registrant either.
- **`CommandSignatureState` went** with its `Resolve`/`ExecuteIndirect` overloads: no producer, no caller.
- **OMS's `PipelineDesc.h` holds only what CS uses** (`org::services`): shader sources, the input layout and `GraphicsPipelineDesc`
  without layouts, defines or mesh stages. CS's only users (`PipelineStates`, `PipelineSource`, `AssignLocations`, `WithStrides`,
  `InputLayoutDescription::layout`) have no caller; if p8-cs deletes them, the OMS header can go too.
- **ExternalPassDesc, `ServiceExecutor.h` and `CompletionFlight.h` stay.** Each option has a setter in ORG (contributor host, upload
  extension); the OMS headers serve `ShaderCompiler` and `BuildPipeline`.
- **The tripwire generator forwards interface arrays as they are.** They occur only on the context, whose forward args are never
  expanded.
