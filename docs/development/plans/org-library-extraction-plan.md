# ORG library extraction plan

Status: revised (2026-11-20) after `org-simplification-plan.md` landed. First written after the cleanup plan
(`org-cleanup-refactor-plan.md`) was merged into `integ/hard-ports` (decision 97). The simplification plan ran first
(decision 98). It finished some items here, changed the target of others, and removed a few. This revision checks every
item against the code at the commit below.

## Handoff: start here

### State when this plan was revised

- **Checkout and branch:** the main checkout is `c:\Users\matth\source\repos\SARP\external_refs\cs-org-dxvk`, on branch
  `org_integration`. Its tip is one squash commit, "feat(org): hard ports, cleanup and simplification", which holds
  all of `integ/hard-ports` (the cleanup and simplification plans, 892 commits, last code commit `0dbf0795d`). That
  branch is kept as the detailed history; commit hashes in the decisions file and the director log point into it.
  New work branches from `org_integration`.
  - Nothing is pushed. PRs target `dev`, never `main`.
- **Submodules.** Each is on a local branch. **These commits exist only locally.** Nothing is pushed.

  | Submodule | Commit | Local branch |
  |---|---|---|
  | ORG (`extern/OpenRenderGraph`) | `7bd4aef` | `simplify/p8-lib` |
  | OMS (`extern/ORGModuleServices`) | `b2a5cb2` | `simplify/p9-final` |
  | BasicRHI (`extern/BasicRHI`) | `bcf4232` | `cs/simplify-p2-org-diag` |
  | DXVK fork (`extern/dxvk`) | `4addc47d` | `simplify/p5` |

  - The cleanup's tips are still there as `cleanup/integ` (ORG `73b7d81`, OMS `5b2d206`). The simplify branches build on
    them. Older tips are kept as `cleanup-p4-birth-tip` and `cleanup-p5-reload-tip`.
  - OMS has `update = none` in the main checkout's `.git/config` (not in `.gitmodules`), so check out its commit by hand.
  - Worktrees get their submodules from the main checkout through `init-submodules-local.sh` (below), never from a remote.
- **Cleanups merged:** p8-cs (CS dead code, export audit), p8-lib (ORG dead code, docs, tools), p8-backbuffer (native
  uses one back buffer), p9-final (dead pipeline description chain, docs, tools) and p9-i18n. The lines above are the
  state after them.
- **Where the history is:**
  - Decisions: `docs/development/plans/director-decisions-2026-11.md`. 97 wrote this plan, 98 put the simplification
    first, 99–109 came from the simplification.
  - Per-merge log: `docs/development/plans/org-cleanup/director/director-log.md`.
  - What each simplification part actually did: the "decided by the agent, to confirm" sections at the end of
    `org-simplification-plan.md`. They differ from that plan's phase lists in places. They are the authority for the
    current code.
  - Surveys behind this plan: `docs/development/plans/org-simplification/surveys/extraction-libs-inventory.md` and
    `extraction-shared-systems.md`. Their line numbers are from before the simplification. Use this plan's references.

### How the work is run

- **Who writes, who builds.**
  - A director session splits each phase into parts. Each part goes to a code-only lead in its own worktree, which writes
    code and commits only. A library item is one branch in each touched submodule plus one CS branch.
  - The director merges into `org_integration` in the main checkout, builds, tests and fixes. It is the only one that builds.
  - Brief leads with `docs/development/plans/org-cleanup/director/lead-brief-codeonly.md` plus the plan items. Use
    `lead-brief-building.md` when one integration agent builds instead of the director.
  - Run at most about six leads at once. A worktree build takes about 7 GB, so cap building agents at about 8.
- **Worktrees.**
  - Create: `bash docs/development/plans/org-cleanup/director/mkwt.sh <part> [base]`. It makes
    `../org-port-worktrees/simplify-<part>` on branch `org-native/simplify-<part>` and initialises its submodules from the
    main checkout. The prefix is fixed in the script: name parts `extract-p1-...`, or change the prefix there.
  - A new worktree needs `CMakeUserPresets.json` copied from the main checkout, and `-DAFTERMATH=OFF` to configure.
  - Before removing a worktree, fetch any submodule commits it made into the main checkout's submodules.
  - Remove merged worktrees and branches promptly.
- **Build:** `cmd //c "docs\\development\\plans\\org-cleanup\\director\\build-devfast.cmd"` from the repo root. This is
  Dev-Fast: Ninja, /Od, the DLL only. The script builds the main checkout.
- **Library tests:** ORG's and OMS's own unit tests (`extern/OpenRenderGraph/tests`, `extern/ORGModuleServices/tests`).
  ORG's tests bring a task service through `tests/TestTaskService.h`. CS's standalone tests are in `tests/` (for example
  `tests/DrawcallLimitFixCPU/KeptStateTests.cpp`, `SceneExecutorTests.cpp`).
- **Stage:** `python tools/cs-test/cs_test.py stage . --name nfg --preset Dev-Fast --base-aio "C:/Modding/MO2/mods/CS AB nfg"`
- **Run one config:** `CS_TEST_BACKGROUND=1 DXVK_DEBUG=hang python tools/cs-test/cs_test.py run <mode>+<config> --stage nfg --wait 3600 --label <label>`.
  - Modes: `guest`, `native-vulkan`, `native-d3d12`. Quick runs: `guest-quick`, `native-quick`, `native-d3d12-quick`.
  - Configs are in `tools/cs-test/configs/`; the format is in `tools/cs-test/README.md`.
  - Results go to `C:\Modding\CS-AB\runs\<stamp>-nfg-<config>-<label>\`. Each holds `CommunityShaders.log`,
    `result.json`, shots, and on a hang `CommunityShaders-Hang-*.txt/.dmp`.
- **Merge helpers:**
  - Plan-doc conflicts between branches that each add sections: `python docs/development/plans/org-cleanup/director/keepboth.py <file>`.
  - PP batch: `docs/development/plans/org-cleanup/director/ppbatch.sh`.
- **After changing switches or strings:**
  - Switches: there is one table, `kSwitches` in `src/Testing/SwitchRegistry.cpp`. The schema is regenerated from the
    game's dump: after a build, `python tools/cs-test/cs_test.py run list-switches --stage <name>`, then
    `python tools/cs-test/cs_test.py schema`. `cs_test.py check-schema` compares them.
  - `python tools/extract-i18n.py --write` and `python tools/sort-i18n.py --write --prune-extra`.
- **Record decisions** as new items in `director-decisions-2026-11.md`, numbered from 110, with "to confirm" where the
  user hasn't seen them. Add one line per merge to `director-log.md`.

### Traps

From the cleanup and the simplification:

- **Linking:** never link while a cs-test run is going. The running game holds the build PDB and the link fails with LNK1201.
- **The Dev-Fast PDB grows past what the linker can write** (6.4 GB, LNK1318; decision 109). Delete
  `build/Dev-Fast/CommunityShaders.pdb` and `.ilk` before linking, and the rotated `old.*.pdb` copies.
- **Unity builds** (batches of 8): files that land in one batch collide on anonymous-namespace names. Rename locally
  (`MaterialPorts.cpp`'s `PortTable()`, for example).
- **`Port<T>`:** port classes need out-of-line ctors and dtors (`X::X() = default;` in the cpp), or the
  `unique_ptr<Impl>` destroys an incomplete type.
- **After a hang or kill:** DXVK's `vkCreateDevice` can fail with OUT_OF_HOST_MEMORY on every launch until a reboot.
- **Shots:** brightness varies between runs (eye adaptation). Check reproducibility before calling a shot a regression.
- **Perf:** one sample per version. Say the build is Dev-Fast /Od when quoting times. Use `game.uncapped`.
- **Formatting:** clang-format is pinned at v23.1.3. Don't format whole files; the branch isn't format-clean.
- **Timeouts:** never raise a test timeout to fix a slow run. Find the cause, usually a cold per-variant shader cache.
- **MO2:** it may be closed before runs. The active test mod folder is `CS AB nfg`; don't create other variants.
- **Test profile modes:** `post-effects` and `post-lens` need the profile's Post Processing mode set to Post Processing.
  The configs set it through `saved_settings`; don't change the profile by hand.
- **A run set stops after two failures.** When several configs run in one batch, the batch stops at the second failed
  config. Rerun the ones it skipped after fixing.
- **Library errors:** ORG has no fatal hook. `org::RunReadback` throws `std::runtime_error` after a critical log, and CS's
  commit paths don't catch it, so it still stops the game. CS's task adapter makes a throwing task fatal.

### Known open items carried in

Not part of this plan unless a phase touches them:

- **Decision 107:** work outside the ledgers and undeclared access are logged, not tap errors, until their producers move
  onto the ledgers. The producers: loading-screen and menu uploads (`UpdateSubresource` on `Native::Buffer`),
  CloudShadows' layer and engine target clears, Scaleform's GFx texture read (`Scaleform/Passes.cpp:692`), and
  TextureFeedback's read by handle (`TextureFeedback.cpp:55`).
- **dclf-parity:** the sun's GPU and CPU cascade tests differ by about 7 of about 7.3k synthetic draws.
- **dclf-parity** raises a scene-mismatch tap error, "tracked under a category node it no longer has (a missed detach)"
  (`ApplyEvents.cpp:822`).
- Both dclf-parity findings are real DCLF defects. They showed up once the parity checks reported instead of repairing.
- **The submodule branches are not pushed** (the table above, and `cleanup/integ`). Decision 89 wants them pushed and
  reviewed before anything new is pinned. Waiting for the user.
- **6.11 done (decision 113):** guest runs with all five native-only host options on, so CS sets them in every mode.
  ORG still has their off paths and `GraphHost::Desc` flags; deleting them is library work (phase 0 below).
- **effects11** fails on this machine: the game folder has no `enbseries\enbeffect.fx`, so Effects11 cannot own the
  tonemap and the mode falls back to Vanilla ("Post Processing Mode: Vanilla (chosen Effects11)"). effects11-programs
  passes. Not a code defect; the config needs the ENB effect files installed.
- **OMS pin trap:** OMS has `update = none`, so after a merge that bumps it, check its commit out by hand before any
  `git commit -a`, or the commit pins the old one back.
- Also from the simplification records:
  - `RegisterCopy` still compares adopted sizes. ImageSpace's chain follows a resize one revision late (decision 96).
  - LLF's slot count can't change in a session (decision 88).
  - DXC game shaders use the runtime's compiler, so a shader cache compile during runtime shutdown would use a freed
    compiler (p6-libs).
  - OMS `ResolveMultisamplePass` has no CS user and no in-game test (p6-pipes).
  - Grass's and Tree LOD's cull-count readbacks still use DCLF's `RunRanges`. They can move onto region readbacks, and
    then `Core/Render/Readback/RunRanges.h` goes (p6-staging).
  - `CS_HAS_ORG_MODULE_SERVICES` and `CS_HAS_DCLF` are always on and can go (p1-noorg).

### Progress (update as phases land)

| Phase | State | Branches | Notes |
|---|---|---|---|
| 0 Review and push | not started | | Push the submodule branches for review (decision 89). Merge p8 first. |
| 1 Small containers and async pieces | not started | | EventQueue already done. p1-frame (code only, not built): StarvationWatch and FrameClock to OMS `Async/`, the violation policy to OMS `ViolationPolicy`; CS keeps `Frame/Starvation.h` and `FrameClock.h` as facades. Branches `org-native/extract-p1-frame`, OMS `extract/p1-frame`. |
| 2 Services with clean cuts | not started | | Executor and retirement join need A/B perf runs. |
| 3 Programs and GPU primitives | not started | | Pipeline-stack decision, frame staging and readback already done. Items 3–6 can start beside phases 1–2. |
| 4 Build and upload services | not started | | Needs phase 2's executor. |
| 5 Coordinators | waiting | | Only once a second user exists. |

### Checks that need a run

- **Phase 2 executor and retirement join, phase 4 build service:** one A/B perf sample each, before and after (Release,
  or Dev-Fast /Od stated).
- **Multisampled SRVs** (in "Library work"): check whether ORG still lacks them. The old note in `pipelines.md` went
  with the pipeline library, so there is no reference left.

## Goal

Move code that other OpenRenderGraph consumers will want out of Community Shaders and into the libraries:

- **ORG core** (`extern/OpenRenderGraph`) takes graph, RHI, recording and readback primitives. It ships no shaders and no
  shader compiler.
- **OMS** (`extern/ORGModuleServices`) takes optional modules built on ORG: async containers, schedulers, build and
  version services, upload helpers, shader tooling, and GPU passes. OMS links ORG (`CMakeLists.txt:40`), so it can hold
  adapters over ORG interfaces. Its passes record through `org::services::ComputeKernel`, and their shaders ship under
  `Data/Shaders/ORGModuleServices`.
- **CS** keeps Skyrim, D3D11 and DXVK specifics, its frame structure, its settings and its diagnostics.

CS should end up with thin adapters (aliases, injected callbacks, host enums) where it has whole systems today.

## Rules for every move

- The standing rules stay: errors are fatal, nothing waits or retries, the render thread never waits, changes are
  published at frame boundaries, recordings are replayable. Release stays soft (tap errors count and warn).
- Policy stays with the host. A library returns or reports an error. It never calls a fatal function itself. CS passes
  its fatal handler in (`RenderGraph::Fatal`, `src/RenderGraph/Fatal.h`). Where a library throws today (`org::RunReadback`),
  decide the convention once (see "Library work").
- Constants that are CS's choice become parameters. Examples: `FrameClock::kSlots`, `StarvationWatch::kStarvedAfter`,
  thread layout and worker counts.
- Host vocabulary stays in the host, as OMS's README requires. Examples: `TapError` kinds, `SceneArtifact`,
  `PayloadPart`, segment ids, `Placement`.
- No CS names, switches (`CS_*`) or decision numbers in library code or comments. Library options are `Desc` fields; the
  libraries read no environment switches since phase 6.10.
- Tests move with the code. Several are already standalone (`tests/DrawcallLimitFixCPU/KeptStateTests.cpp`,
  `SceneExecutorTests.cpp`).
- Wait for a second user before moving a large coordinator (frame-revision plan step 4c). Those items are in phase 5.
- Each move is one branch per library plus one CS branch. CS pins the submodule commit. The submodule branches are local
  only. Push them and have them reviewed upstream before phase 1 pins anything (decision 89).
- `cleanup/integ` and its successors are CS-only. BasicRenderer and SARP stay on their own lines (ORG `8dfc78f`, OMS
  `0185120`) and re-adopt the smaller libraries later. Library APIs may change freely (decision 98).

## What the libraries already have

From the cleanup:

- ORG: recording tags (`SetRecordingTag`, `RecordingContext::Tag`), `RegisteredResource`, texture fill and zero fill
  uploads (`QueueTrackedStreamingTextureFill`/`ZeroFill`, `TextureFillWriter`), all-or-nothing staged attach
  (`AttachStagedEpochGraphs`, `AttachRefusal`, `EpochsDeclaring`), `VersionedResource::SetAdoptionObserver`, stale
  recording errors.
- OMS: pipeline versions and pins in `PipelineRegistry`, `TextureContents::Fill`, `Variables.h`,
  `VersionedTextureGrowth`, `ComposedVersions`, `NamedVersions` for textures.

From the simplification (phase 6 and the parts it pulled in):

- ORG:
  - `org::runtime::EventQueue` (`include/Render/Runtime/EventQueue.h`). It replaced `MpscQueue`. CS's
    `src/RenderGraph/Async/EventQueue.h` is a one-line alias with `QueueTeardown::Leak`.
  - `org::RunReadback` (`include/Render/RunReadback.h`), the one readback, per reader: part readbacks with sequence stamps,
    growable count-prefixed lists (`CreateList`), and region readbacks (`CreateRegions`). It replaced CS's `RunReadback`,
    DCLF's `HostEventRing` and ORG's `IReadbackService`, whose capture path is deleted. `MoveStagingRun` runs at the stage
    commit.
  - `FrameArena::AcquireOversize` (`include/Render/FrameArena.h:82`): a region with a chunk of its own, for staging data.
    CS's `FrameStaging` is now `RenderGraph::AcquireStaging` on it.
  - Hosts bring the task service (`Runtime::Desc::tasks`, required). `ThreadPoolTaskService` is gone; CS passes
    `Frame::OrgTaskService()` (`src/RenderGraph/Async/Lanes.cpp:215`), an `ITaskService` over its `Executor`.
  - Deleted: the pipeline library (`PipelineLibrary`, `PipelinePermutations`, `pipelineCompiler`,
    `RequestPassPipelines`/`AwaitPassPipelines`), the built-in mips and resolve passes, recorded uploads, closed
    executions, the decision trace, the library's env switches, `PassId.h`, `DescriptorSnapshots.h`,
    `ShaderCompilerService.h`.
  - CS no longer uses the contributor ABI. ORG keeps it for SARP.
- OMS:
  - `BuildPipeline` (`PipelineRegistry.h:80`), a one-shot async pipeline build. DCLF's draw pipelines use it.
    `PipelineService` is deleted, so OMS has the only pipeline stack.
  - `Passes/ComputeKernel.h`, `Passes/GenerateMipsPass.h`, `Passes/ResolveMultisamplePass.h`. CS's `ComputeProgram`
    implements `ComputeKernel`.
  - `ShaderCompiler::CompileHere`. One compiler instance: `DxcShaderStore` uses the runtime's.
  - Deleted: BasicRenderer's journal mode (`BufferJournal`, `VersionedBufferFamily`, `BufferImage`, `JournaledBufferSet`,
    the journal half of `VersionedBufferBuild`), `ShaderServiceAdapter`, `Services.h`, the env switches.

## No longer needed

| Item (old phase) | Why |
|---|---|
| EventQueue to OMS (1) | Done: it is ORG's `org::runtime::EventQueue` (6.6). CS keeps a one-line alias. |
| DCLF `Runtime/EventQueue.h` and `Runtime/Hash.h` forwarding headers (1) | Deleted in simplification phase 1. |
| Two journal designs (risk, 1) | BasicRenderer's journal mode is deleted (6.6). DCLF's `ChangeJournal` is the one design. |
| DCLF frame clock as a duplicate (1) | It is the scene step, which holds in menus and loads. Renamed `SceneClock` (`Core/Engine/SceneClock.h`, 4.10). It stays in DCLF. |
| The pipeline-stack decision (3) | Done by 6.3: ORG's stack is deleted. OMS's is the only one. |
| `GenerateMips.*` (3) | Deleted. OMS `GenerateMipsPass` runs on CS's programs (6.3). |
| Frame staging to ORG (3.4) | Done: `RenderGraph::AcquireStaging` on `FrameArena::AcquireOversize` (6.8). Only `RunSlots` is left (phase 3 item 4). |
| Kept-recording readback (3.7) | Done: `org::RunReadback` (4.6, 6.4). The `RunRanges` remainder is a CS follow-up (open items). DCLF keeps the visibility decoder and the counters' arm and stamp logic. |
| "Explicit `Table::Want` + `HoldForPrograms` API" (3.2) | `Programs::Table` and `HoldForPrograms` are gone. Frame-met keys use `Frame::ProgramSet` (p3-declare2). |
| UploadRing core to OMS (2) | Changed to stays in CS. The engine constants' chunks are arena staging regions now (6.8). What is left serves only the dynamic ring, whose chunks are D3D11-wrapped buffers released behind D3D11 work (`ReleaseAfterD3D11`, `UploadRing.cpp:66`). |
| Frame transients to ORG (4) | They already place on ORG's `FrameAliasArena` and frame positions (`Frame/Transients.cpp`). What is left maps CS's `SegmentId` stages to positions. |
| DCLF revision participant (5) | Its recording-variant cache is `EpochRecordings::Cache` (`Core/Render/Impl/Producer.h:104`). `Core/Render/Revision/Participant.h` now holds DCLF's per-`ShapeKind` participants, which are DCLF's own. |
| Keyed pass schedule (5) | The recipe and DCLF's schedule do different jobs. `kAsyncOrder` went (4.8). |
| Two `DeclareClient` overloads (5) | One public `DeclareClient<Key, State>` returns `DeclaredClientOf<State>` (`Frame/Declare.h:460`, 3.2). |
| Pooled list pools per thread (2) | The Parallel record mode is gone (4.5). Only one pool for DXVK's CS thread is left (phase 2). |

## Phase 0: review and push (before anything moves)

- Push the ORG, OMS, BasicRHI and DXVK branches above for review upstream (decision 89). Waiting for the user.
- Delete ORG's off paths for the five host options CS now always sets (decision 113): `reuseLiveRecordings`,
  `coalesceSubmissions`, `mergeHandoffs`, `uploadsWithBatches`, `keepGatedInline`. The flags leave `GraphHost::Desc`.

## Phase 1: small containers and async pieces (OMS, effort S each)

All are standard-library or TBB code with no engine includes. They can be done in parallel. DCLF paths are under
`src/Features/DrawcallLimitFix/`.

| Item | CS files (LOC) | Target | Notes |
|---|---|---|---|
| LatestSlot | `src/RenderGraph/Async/LatestSlot.h` (35, 15 files) | OMS `Async/` | Unchanged. Latest-wins, any number of producers. Document it next to `PublicationExchange` (SPSC, 3 slots). |
| PublicationLog | DCLF `Core/Runtime/PublicationLog.h` (73) | OMS `Async/` | Unchanged. Lossless SPSC log with many cursors. OMS has nothing like it. |
| Retirement chain | DCLF `Core/Runtime/Retirement.h` (126) | OMS, beside `RetirementClock` | Ownership-based reclamation plus `Retired<T>` (hand the last drop to another thread). It uses ORG's `EventQueue` now, which OMS can include. The forwarding headers are gone. |
| Slot table | DCLF `Core/Runtime/SlotTable.h` (155) | OMS, beside `LeasedArraySlots` | Unchanged. Generational slots with references; frees after the last publication. |
| Kept state | DCLF `Core/Runtime/KeptState.h` (1152) | OMS | Shrunk in scope: `SharedChunks` is already folded in as `KeptSharing::kByStamp` (`KeptState.h:417`; `LookupChunks` in `Core/Scene/Model/Lookups.h:35`). The journal choice is made (OMS's is deleted). The `reuseKeptStorage` test global (`KeptState.h:25`) becomes an option. |
| StarvationWatch | `src/RenderGraph/Frame/Starvation.{h,cpp}` (61) | OMS `Async/` | Unchanged. `kStarvedAfter = 4 * FrameClock::kSlots` and an `onStarved(what, why)` callback become parameters. CS reports `TapError::Starved` from the callback. |
| Violation policy | `src/RenderGraph/Frame/FrameDriver.cpp:590-646` (`RaiseViolationWarning`, `MismatchFatal`, `NoteTapError`) | OMS, small | Unchanged; still needed since Release stays soft. Fatal or count, the first of each kind logs, warnings last the session. The `TapError` enum stays in CS. StarvationWatch, BoundaryCheck and the recipe check report through it. |
| Frame clock | `src/RenderGraph/FrameClock.h` (55, 14 files) | ORG `GraphHost` reading, or OMS | Shrunk: only CS's clock. One clock owned by the host. CS keeps a static facade for its users. |

Done when: CS includes these from OMS, and the moved tests run in OMS's tests.

## Phase 2: services with clean cuts (OMS, effort S–M)

| Item | CS files (LOC) | Target | Cut |
|---|---|---|---|
| Executor and lanes | `src/RenderGraph/Async/Executor.{h,cpp}` (625), the generic part of `Async/Lanes.{h,cpp}` (425: `Lane`, `JobLane`, `MakeLane`), and `OrgTaskService()` (`Lanes.cpp:215`) | OMS, the first concrete `org::async::GraphScheduler`, with its ORG `ITaskService` adapter | Grown: since 6.5 ORG's tasks run on it too. It already takes `ThreadStart`/`ThreadPriority` callbacks (`Executor.h:59-73`). `TaskClass` constants and `Layout` (`Executor.h:26-33`) become a default the host overrides. The adapter's counting scopes stay, and its "a task that throws is fatal" becomes a reported error. CS keeps `SharedExecutor()` (Win32 thread names and priorities, fatal refusal) and the worker-count constants, about 100 LOC. Risk: medium (threading). |
| Retirement join | `src/RenderGraph/Async/Retirement.{h,cpp}` (253) | OMS, beside `RetirementClock` | Unchanged. A source interface (`SubmittedPoint`, `SealedGpuPoint`, `Sealed`). OMS gives a GraphHost source. CS picks the CPU timeline and calls the seal hooks. Risk: medium, since it decides when memory is reused. |
| EpochRecordings | `src/RenderGraph/Frame/EpochRecordings.{h,cpp}` (341) | ORG (GraphHost companion header) or OMS | Mostly unchanged. Only uses `GraphHost`, `IHostExecutionData`, `RunRef`. Return failures instead of the `RenderGraph::Fatal` calls (`EpochRecordings.h:130`, `.cpp:32-37`). Replace "`Request` false means async off" (`.h:53-55`) with an explicit result. `Cache::Find` hashes the key itself. Owner-thread fields move out of the locked entry. DCLF's recording-variant cache is one of its users. |
| Birth texels | `src/RenderGraph/Frame/Birth.{h,cpp}` (256) | OMS `VersionedTextureGrowth` | Unchanged. `EncodeTexel` is keyed on a DXGI format (`Birth.h:97`); key it on `rhi::Format`. Add `TextureContents::Value(float4)`, lowered to a Fill writer. Drop `BirthTexels`/`Birth::FillFn` (`Birth.h:25,47`) for ORG's `TextureFillWriter`. CS keeps `Birth` as a declaration option and the history default. `GraphResources.cpp:878-1070` becomes an OMS call. |
| Pooled-backing defaults | DCLF `Core/Gpu/PooledBackings.{h,cpp}` (167) | OMS `BackingPool.h` | Unchanged. "Default hooks for an ORG GraphHost": allocate off the render thread, retire on the cleanup lane, spare and shrink policies. The DCLF switches it reads (`SceneCopies`, `Shrink`; `.cpp:87,97`) become parameters. |
| CPU recording pools | DCLF `Core/Render/Direct/SiteFrame.h:53-73` (`ListPool`, `Worker`) + `Core/Render/Direct/Cull.h:44` (`CullArena`) | OMS | Shrunk: `ListPool` is now one pool of DXVK's CS thread's secondaries per frame slot, and `Worker` is just a `CullArena`. A bump arena reset at frame-slot retirement, plus that pool. Retirement comes in as a callback. |
| SIMD sphere cull | DCLF `Core/Render/Direct/CullSimd.h` (82), `CullAvx2.cpp` (128), `CullAvx512.cpp` (106), the scalar part and CPUID/XCR0 detection of `Cull.cpp` (`Cull.cpp:735-750`), about 450 | OMS, optional CPU module | Simpler: the level comes from CPUID only (`Simd::Supported`), since the `kExp*` bits are gone. The AVX files also hold the parabolic kernels (`Cull.cpp:243`), which stay in DCLF with the portal kernels. |
| Zone tallies | DCLF `Diagnostics/Zones.{h,cpp}` (219), the mechanism of `Core/Engine/RenderThreadBudget.{h,cpp}` (258) | OMS or BasicTelemetry | Unchanged; they stay as product telemetry. Enums become template parameters. Report lines stay. |
| Shader reflection | DCLF `Core/Gpu/Pipelines/SpirvReflection.{h,cpp}` (359), the generic part of `DxilStages.{h,cpp}` (about 120 of 305) | OMS, beside `ShaderCompiler` | Unchanged. Neither library has reflection. The pulled-build and frame-record preprocessing stay. |

Done when: CS's executor, retirement join, recordings cache and birth encoding are OMS types used through adapters, and
mode smoke in all three modes plus dclf-toggles, shader-reload ×3 and perf-quick (one sample per mode) show nothing changed.

## Phase 3: programs and GPU primitives (OMS/ORG, effort M–L)

The decision that opened this phase is made: OMS has the only pipeline stack, and ORG ships no shaders. So every GPU pass
moves to OMS `Passes/` and records through `ComputeKernel`. Items 3–6 don't depend on the programs move and can start
beside phases 1–2.

1. **Pipeline pins** (`src/RenderGraph/PipelineVersions.{h,cpp}`, about 150 of 269 LOC) to OMS, beside `PipelineRegistry`
   and `BuildPipeline`. Unchanged: `VersionFor`, `Attach` through the recording tag, `BuildPin`, `EveryEpochAt`
   (`PipelineVersions.h:35-53`). The `ShaderGeneration` producer stays in CS. ORG's recording tag still has one owner per
   graph (`RenderGraph.h:762`), so key it per extension first (see "Library work").
2. **Programs** to OMS. Shrunk, since the hold protocol is gone:
   - `ComputeProgram.{h,cpp}` (296; it already implements `ComputeKernel`), `RasterProgram.{h,cpp}` (557),
     `ProgramBuild.{h,cpp}` (149), `ShaderTarget.h` (36), and the generic core of `ProgramService.{h,cpp}` (417):
     `Fixed<P>`, `Detail::Slot`, `Queue`, `Collect`, `Pending`, `JoinSetup`, the pool, `Load`.
   - Injected: a defines provider (today `Util::GetCommonShaderDefines`, `ProgramBuild.cpp:63`, resolved on the render
     thread because CS's defines read State), a shader root, and the binary format taken from the device's backend.
   - CS keeps the defines, `DeclareProgram`, `Frame::ProgramSet` (`Frame/ProgramSet.h`) and `DeclareLane::ProgramsReady`.
     They go with the declarations in phase 5. DCLF keeps `Core/Gpu/SetupPrograms.*`, its handles over `Queue`.
   - Fix the coupling where the pool takes at most half the preparation threads so DCLF's compile pump can't deadlock
     (`ProgramService.cpp:30-49`): the executor gets a blocking domain or an explicit dependency.
   - Swap `ProgramBuild::HashBinary` (FNV-1a, `ProgramBuild.h:29`) for OMS's content hash.
   - Needs phase 2's executor: the pool runs on its task classes.
3. **Run gates and clears.** `RunGate.{h,cpp}` (325, 51 files) and `FirstRun` (`RunGate.h:105`) go to ORG, beside
   `LatchBlock`. `RunGate::Create` is fatal in CS now; in ORG it returns an error. Changed target:
   `UavClearPass.{h,cpp}` (145) goes to OMS `Passes/`, since it needs a kernel.
4. **`RunSlots<T>`** and `RunSlotsRegistry` (`src/RenderGraph/FrameStaging.h:29-134`, 17 files) to ORG `Render/`,
   beside `LatchBlock` and `RunReadback`, which already move staging runs at the stage commit. Shrunk: the staging half is
   done. Inject `kGameFramesInFlight`. The header keeps `AcquireStaging`.
5. **GPU primitives** to OMS `Passes/` on `ComputeKernel` (changed from ORG `RenderPasses/`): `PrefixSum.{h,cpp}` (186,
   5 files) and `package/Shaders/RenderGraph/PrefixSumCS.hlsl` (141); DCLF's counting-sort scatter
   (`features/Drawcall Limit Fix/Shaders/DrawcallLimitFix/SortSequencesCS.hlsl`, 49) with stride, key word and rank word as
   parameters. `ForwardSortCS.hlsl` stays (engine blended order).
6. **HZB** to OMS `Passes/` (settled: ORG ships no shaders). DCLF `Features/GpuOcclusion/HzbPass.cpp` (211),
   `HzbLayout.h` (29), and in `features/Drawcall Limit Fix/Shaders/DrawcallLimitFix/`: `Features/GpuOcclusion/HzbCS.hlsl`
   (123), `HzbTest.hlsli`, `FidelityFX/ffx_spd.h`. Input is a depth `PixelBuffer`, a reduction (min or max) and a far
   value. Add the FidelityFX licence notice. `package/Shaders/Common/HiZ/SPD.hlsl` looks like a second SPD copy: ship one.
   DCLF keeps its `DepthPyramid` extension.
7. **Latched copies** to ORG, extending `LatchBlock`: DCLF `Core/Render/Payload/Latched.h` (93) and its copy pass.
   Unchanged.
8. **Bindless zero views.** Shrunk: the CS facade is gone (3.8), and the null views now live in DCLF's `GpuTextures`
   (`NullView`, `NullOf`; `Core/Gpu/GpuTextures.cpp:924,984`). Only a descriptor-level `Null(ViewKind)` per dimension goes
   into ORG `IDescriptorService`. The D3D11 null views and the import facade stay.

Done when: `PrefixSum.*`, `RunGate.*`, `UavClearPass.*`, `RunSlots` and the HZB pass are gone from CS, programs are OMS
types, and the post-effects, shader-reload ×3, dynamic-cubemaps, dclf-toggles and dclf-family runs pass.

## Phase 4: build and upload services (OMS, effort M–L)

DCLF paths are under `src/Features/DrawcallLimitFix/`.

| Item | CS files (LOC) | Cut |
|---|---|---|
| Staged build service | `src/RenderGraph/BuildManager.{h,cpp}` (965) | Unchanged. An observer interface for `Frame::Boundary` (attach, refusal, change, adoption point). An `IsSteadyState()` callback replaces the `CensusWindow` query (`BuildManager.cpp:675`). The host passes the GraphHost. CS keeps the `Builds::` facade (Ask, IssueAdoptionRequests, about 150 LOC). Risk: high, since it is on the frame's critical path. Needs phase 2's executor. |
| DCLF build state | `Core/Render/Epochs/BuildState.{h,cpp}` (195), `GraphBuilds.cpp` (132) | Unchanged. The generic "immutable build input with a serial, attached versus built" goes with the build service. Graph conditions stay. |
| Commit uploads | `Core/Render/Payload/Uploads.{h,cpp}` (967), `ContentOwners.{h,cpp}` (106), `Core/Gpu/OwnedRows.h` (78) | Unchanged. Inject a fatal handler and a byte-stats sink. `OwnedRow` is already a template over the row type; make the descriptor-owner type a parameter too. Layered over `IUploadService`/`StagedUploadBatch` and `NamedVersions`. |
| Buffer sets | `Core/Render/Payload/GrowableRows.h` (21), `Versioned.h` (46), `Core/Render/Resources/SceneBufferSet.{h,cpp}` (224) | Unchanged. `UnitGroup` and "latch block as a set member" are added to OMS `GrowableBufferSet`. DCLF keeps `SceneBuffers.h`. |
| Scene graph helpers | `Core/Runtime/SceneGraph.{h,cpp}` (297) | Unchanged. `PostPayloadPart`, `PostTextureWindow` and `PostTextureGate` (`SceneGraph.h:73-82`) become OMS helpers. `SceneArtifact` stays. |
| Payload versions | `Core/Render/Payload/PayloadVersions.{h,cpp}` (737) | Changed target: `VersionedBufferFamily` is deleted (6.6). About 70% is "N named versioned-buffer chains from a kept source". Merge it into `ComposedVersions` and `NamedVersions`, built with `VersionedBufferBuild`. `PayloadPart` stays. |
| Indirect run coalescing | `Core/Render/Direct/Recorder.{h,cpp}`, about 150 LOC of `IndirectDraw`/`IndirectRun` (`Recorder.h:35-49`) and slab ordering | Unchanged. A template on the bucket-to-pipeline binder. Medium risk: the layout is tied to `GpuLayouts.h`. |
| Recipe read-order check | about 150 LOC of `src/RenderGraph/Frame/Recipe.cpp` (`AppendPasses` :98, `Validate` :162) | Unchanged. An ORG diagnostic over `epochOrder`. Inputs: the ordered epoch list, a previous-frame predicate, a transient-span lookup. `RecipeTable.h` stays. |
| ImGui backend | `src/RenderGraph/ImGuiBasicRHI.{h,cpp}` (982) | Changed target: OMS, after phase 3's programs, since its program is a `DeclareProgram` keyed on the UI target's format (`ImGuiBasicRHI.cpp:692-701`). Its other CS ties are `FrameAnchors::Texture` (`.h:100`) and runtime access. |
| Shader bindings | `src/RenderGraph/ShaderBindings.{h,cpp}` (303) + `package/Shaders/Common/OrgResources.hlsli` | Push blocks and handle tables for both targets. The C++ side can go to ORG; the `.hlsli` goes with OMS's shaders, since ORG ships none. |

Done when: mode smoke in all three modes plus menu, resize ×3, dclf-toggles, latch-census-short and perf-quick show no
regression.

## Phase 5: coordinators, only once a second user exists (effort L, risk high)

- **Revision coordinator** (`src/RenderGraph/Frame/Revision.{h,cpp}`, 1928) to OMS `RevisionCoordinator`. Generic parts:
  `InputWait`/`InputProducer` on a variable; latest-wins publishing; the fragment-source protocol (slot, drop reasons,
  build-pin match); the closure of epochs that declare a changed slot; whole adoption at one point.
  Before moving:
  - Split `FragmentSource`'s nine callbacks (`Revision.h:187-218`) into a generic core and a CS extension (`frameStart`,
    `drawsWorld`, `mainThreadOnly`, `epochFailed`).
  - Make `InputWait::pipelineGeneration` (`Revision.h:71`) a typed "build pin at least N" condition.
  - Lift `kMaxFragmentSources = 4` (`Revision.h:142`).
  - Replace the `const void*` and `shared_ptr<const void>` versions with typed ones.
  - Put the setup order (`OnSetupBuildPoint`, `JoinSetup`, `EvaluateAtSetup`, `BuildPending`) behind one call.
- **Declarations** (`Frame/Declare.{h,cpp}`, `DeclareLane.h`, `ProgramSet.h`, `DeclareRegistration.h`, about 1.9k) after
  the coordinator. Smaller and cleaner after simplification 3.2, 3.3, 3.4 and 3.7. Needs CS settings as OMS variables
  (half there in `VariablesLane::SettingIndex`) and descriptions on `rhi`/`org::TextureDescription` instead of
  `D3D11_TEXTURE2D_DESC` (5 uses left in Declare). API fixes still open:
  - The name `Held` is used three times (`Held<State>`, `DeclaredClientOf::Held(anchor)`, `Detail::ErasedHeld`/`HeldOf`).
  - `FirstFrame` is just `!Written` (`Declare.h:631-635`).
  - `Written` could be a per-version flag in ORG `VersionedResource` instead.
  - `Placement` (`Frame/Placement.h`) stays in CS as frame vocabulary.
- **Native presenter** (about 600 of 4961 lines in `src/RenderGraph/Engine/Native/NativeDevice.cpp`) to an OMS presenter
  module. Smaller: the per-frame duties are behind `PresenterBase` (`src/Features/Upscaling/FramePresenter.h:92`) since
  simplification 5.2, and p8-backbuffer moves native to one back buffer. It covers swap chain present with latency
  pacing, replacement on the submission thread, and HDR10. Cut away tripwire handles, `FrameGen::PresentationConfig` and
  DLSS-G (as hooks). Check the shape again after p8-backbuffer merges.
- **Lease window and frame address set** (DCLF `Core/Engine/EngineReadWindow.h` (95), `FrameAddressSet.h` (71)) to OMS
  `Async/` when a second user appears. Unchanged.

## Library work CS needs but does not own

- **ORG array-size versions.** LLF's slot count can't change in a session (decision 88).
- **ORG keyed recording tags**, so more than one extension can tag a graph. Today one tag per graph
  (`RenderGraph.h:762`). Pipeline pins (phase 3 item 1) need it.
- **ORG `Builds{attached, staged}` snapshot readable from any thread.** DCLF prunes its cache on its builder thread only
  because `StagedGeneration` and `EpochRecordingCurrent` (`GraphHost.h:351,687`) are owner-thread calls.
- **ORG multisampled SRVs.** To check (see "Checks that need a run").
- **One error convention for library errors.** ORG has no fatal hook, so `org::RunReadback` throws after a critical log.
  Either a host error callback in `Runtime::Desc`, or documented throws that CS catches at its commit paths.
- **Thread rules in signatures** (tag types or debug asserts) instead of prose and runtime checks.
- **OMS cleanup:** `Variables.h:30` still cites "decision 6a". Reword it as a general statement.
- **6.11 result:** guest runs with every one of these host options (decision 113). Their off paths go in phase 0.

## Stays in CS

- **Engine interception and the scene mirror:**
  - DCLF `Core/Engine/**` (including `SceneClock`), `Core/Handoff/**`, `Core/Scene/**` (the Coordinator alone is about 15k)
  - `Features/**`, `Integrations/**`
  - RE:: types and hook sites.
- **The GPU-driven draw build:** `BuildDrawsCS.hlsl`, `Payload/MainBuild.*`, `Resources/GpuLayouts.h`, `Snapshot/**`,
  `Shadow/**`, DCLF's `Revision/Participant.h` and `Gpu/SetupPrograms.*`. They carry the DrawSequence layout, portal and
  parabolic programs, fade roots and engine draw order. Only the kernels above come out.
- **Shading and material ports:** BSLightingShader permutations, the engine register layout, CS SharedData.
- **D3D11 and DXVK import:** `GraphResources`, `D3D11Wrapper`, `ImageRegistry`, `DxvkImageImport`, DCLF
  `GpuResources`/`GpuTextures`, `RenderGraphRuntime` (in-stream execution, `ReleaseAfterD3D11`), `UploadRing` and the
  dynamic ring.
- **The frame structure:** `FrameDriver`, `TapError` kinds, `RecipeTable.h`, `FrameAnchors` (Place, Stage, clients),
  `Frame/Placement.h`, `AnchorPasses`, `BoundaryCheck`, `StepGroup`, `EvaluateAtSetup`, coverage, `Frame/Transients`,
  `Engine/Targets`, `Port<T>`, `RenderGraph::Fatal`.
- **Native D3D11 emulation** in `Engine/Native` (everything but the presenter core).
- **Presenters** for DXVK and native (`Features/Upscaling/*Presenter*`), apart from the native presenter core above.
- **Settings, switches, diagnostics and testing:** the one switch table (`kSwitches`, `src/Testing/SwitchRegistry.cpp`),
  DCLF `Diagnostics/**`, `src/Testing/**`.

## Size

Estimates, about ±25%:

- **Phase 1:** about 1.7k lines (KeptState is 1.15k of it).
- **Phase 2:** about 3.5k (the executor and lanes about 900, the SIMD cull about 450).
- **Phase 3:** about 3k (programs about 1.3k), plus about 350 shader lines and the SPD header.
- **Phase 4:** about 4.5k (commit uploads about 1.1k, the ImGui backend about 1k).
- **Phase 5:** about 4.5k, moved as interfaces.
- Phases 1–4 take about 12k lines out of CS and DCLF. Each item is replaced by an adapter of a few dozen lines.

## Order and dependencies

- Phase 0 first: merge p8, then push and review the submodule branches.
- Phase 1 items are independent of each other.
- Phase 2: the executor before phase 3's programs and phase 4's build service. The retirement join before the
  pooled-backing defaults and the CPU recording pools, which retire against it.
- Phase 3: items 3–6 (run gates, `RunSlots`, GPU primitives, HZB) need nothing else and can run beside phases 1–2. Item 1
  needs keyed recording tags in ORG. Item 2 needs phase 2's executor.
- Phase 4: the build service after phase 2. The ImGui backend after phase 3 item 2. Payload versions after buffer sets.
- Phase 5 only with a second user. The coordinator before the declarations.

## Risks

- **The executor and the build service are on the frame's critical path.** Move them with A/B perf runs (one sample
  each, Release, or with Dev-Fast's /Od stated).
- **The submodule branches are local.** They must go upstream before anything here is pinned.
- **Library reviewers may want a different API shape.** Keep the CS adapters thin so a rename doesn't spread through
  the 50 users of run gates and the users of programs.
- **The ITaskService adapter now carries ORG's work too.** Moving the executor moves the thread pool every graph build
  and readback uses. Keep `Frame::OrgTaskService()` as the one seam.

### p1-frame decisions

Decided by the agent, to confirm:

- **Frame clock in OMS, not `GraphHost`.** `org::async::FrameClock` (`Async/FrameClock.h`) takes its slot count in the
  constructor. `GraphHost` counts its own executions; CS's clock moves at `Renderer::Begin` and at the sealed present,
  which ORG does not see, and it is read before the host exists. CS's `FrameClock.h` is a static facade over one
  instance and keeps `kSlots = 4` as a `constexpr` (`SlotOf` still divides by the constant).
- **Starvation callback gets the miss count too:** `onStarved(what, why, misses)`, so CS's tap error text stays the same.
  CS's `Frame::StarvationWatch` derives from the OMS class and reports `TapError::Starved`. The unused `SegmentId`
  argument of `NoteMiss` is gone (no caller passed one).
- **Violation policy in the main OMS library** (`ViolationPolicy.h`, `src/ViolationPolicy.cpp`), not `Async/`: it logs
  through spdlog as the rest of OMS does, and the header-only `AsyncPrimitives` target has no spdlog. Kinds are integers.
  The log labels and notes are `Desc` strings, so CS's lines read as before; they lose CommonLib's `[file:line]` source
  field. The policy is made at first use and reads `MismatchFatal` then, so that switch's info line can come earlier
  (at the first violation warning, not the first tap error).
- **CS still raises the menu warning** for the first tap error of a kind (`Outcome::First`); the library does not name
  kinds.
