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
- **Library errors:** `org::RunReadback` reports errors to the handler each `Create*` takes (p0-lib); CS passes
  `RenderGraph::FatalHandler`. Other ORG throws are unchanged: CS's commit paths don't catch them, so they still stop the
  game, and CS's task adapter makes a throwing task fatal.

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
- **6.11 done (decision 113):** guest runs with all five native-only host options on. p0-lib deleted their off paths
  and `GraphHost::Desc` flags; their behaviour is ORG's only one.
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

| 1 Small containers and async pieces | in progress | `org-native/extract-p1-async`, OMS `extract/p1-async` | EventQueue already done. p1-async (code only, not built): LatestSlot, PublicationLog, SlotTable, KeptState in OMS `Async/`, RetirementChain and `Retired` in OMS `RetirementChain.h`; tests in OMS `tests/Async` (ORGKeptStateTests, ORGRetirementChainTests). |

| 0 Review and push | code done (p0-lib), push waiting for the user | CS `org-native/extract-p0-lib`, ORG and OMS `extract/p0-lib` | Host-option off paths deleted, `RunReadback` error handler, OMS `Variables.h` wording, multisampled-SRV check (below). Not built or run yet. Push the submodule branches for review (decision 89): waiting for the user. Merge p8 first. |
| 1 Small containers and async pieces | not started | | EventQueue already done. |
| 2 Services with clean cuts | not started | | Executor and retirement join need A/B perf runs. |
| 3 Programs and GPU primitives | in progress | p3-gates: `org-native/extract-p3-gates`, ORG and OMS `extract/p3-gates` | Pipeline-stack decision, frame staging and readback already done. Items 3–6 can start beside phases 1–2. Items 3–4 written (p3-gates), not built: `org::RunGate`, `org::FirstRun`, `org::RunSlots` in ORG `Render/`, `org::services::MakeUavClearPass` in OMS `Passes/`; CS keeps `RunGate.h` and `FrameStaging.h` as adapters. See "p3-gates decisions". |

| 2 Services with clean cuts | in progress | `org-native/extract-p2-executor`, OMS `extract/p2-executor` | Executor and lanes moved (code only, not built): OMS `Async/Executor.h`, `Async/Lanes.h`, `Async/ExecutorTaskService.h`; CS keeps `SharedExecutor()`, `OrgTaskService()` and forwarding headers. Needs its A/B perf run. Retirement join needs one too. |

| 2 Services with clean cuts | in progress | p2-recordings: CS `org-native/extract-p2-recordings`, ORG `extract/p2-recordings` | Executor and retirement join need A/B perf runs. p2-recordings: EpochRecordings is ORG `OpenRenderGraph/EpochRecordings.h` (`org::epoch_recordings`); CS keeps a forwarding header whose `Cache` passes `RenderGraph::Fatal`. Not built yet. |

| 2 Services with clean cuts | in progress | p2-retire: CS `org-native/extract-p2-retire`, OMS `extract/p2-retire` | Executor and retirement join need A/B perf runs. p2-retire (code only, unbuilt): retirement join is OMS `RetirementJoin` + `GraphHostRetirement` (CS `Async/Retirement` is a facade); pooled-backing defaults are OMS `GraphHostBackings.h` (DCLF `PooledBackings` passes its switches); CPU recording pools are OMS `RecordingPools.h` (`BumpArena`, `ResetPools`, `RecordingSlot`) + `SecondaryListPool.h`. Tests: OMS `ORGRetirementTests`. |

| 2 Services with clean cuts | in progress | p2-birth: CS `org-native/extract-p2-birth`, OMS `extract/p2-birth` | Executor and retirement join need A/B perf runs. p2-birth (code only, not built): OMS `TexelEncoding.h` (`EncodeTexel` on `rhi::Format`, `ValueFillWriter`), `TextureContents::Value`; CS `Birth.cpp` deleted, `Birth::Fill` takes `org::TextureFillWriter`. |

| 2 Services with clean cuts | in progress | p2-reflect: CS `org-native/extract-p2-reflect`, OMS `extract/p2-reflect` | Executor and retirement join need A/B perf runs. Shader reflection (p2-reflect): OMS `ShaderReflection.h` (SPIR-V parse, `IsDxil`, `ReflectDxil` with a `DxilBindingLayout`, `DxilFragment`), tests in OMS `tests/Reflection`; DCLF keeps `SpirvReflection` (subclass passing its shifts and push register) and `DxilStages` (frame-source preprocessing, aliases for `IsDxil`/`Fragment`). |

| 2 Services with clean cuts | in progress | p2-cpu: `org-native/extract-p2-cpu`, OMS and BasicTelemetry `extract/p2-cpu` | Executor and retirement join need A/B perf runs. p2-cpu (code only, unbuilt): SIMD sphere cull is OMS `ORGModuleServices::SphereCull` (`Cpu/SphereCull.h`, `cmake/SphereCull.cmake`, `tests/Cpu`); DCLF keeps the paraboloid and portal kernels and logs the level. Zone tallies and the render-thread budget are BasicTelemetry `TallyScope` and `ThreadBudget<Bucket, Site, Splits>` (`TallyTests`); DCLF keeps the macros, enums and report lines. |
| 3 Programs and GPU primitives | not started | | Pipeline-stack decision, frame staging and readback already done. Items 3–6 can start beside phases 1–2. |

| 3 Programs and GPU primitives | items 5–6 written, not built | `org-native/extract-p3-kernels`, OMS `extract/p3-kernels` | Pipeline-stack decision, frame staging and readback already done. Items 3–6 can start beside phases 1–2. Items 5–6: OMS `Passes/PrefixSumPass`, `Passes/SortScatter.h`, `Passes/HzbPass` and their shaders; CS `PrefixSum.*`, DCLF `HzbLayout.h` and the moved shaders are gone. See "p3-kernels decisions". |

| 3 Programs and GPU primitives | in progress | p3-pins: CS `org-native/extract-p3-pins`, ORG and OMS `extract/p3-pins` | Pipeline-stack decision, frame staging and readback already done. Items 3–6 can start beside phases 1–2. Item 1 (p3-pins): done in code, not built. OMS `PipelinePins` and `VersionFor`; CS `PipelineVersions` keeps `Install`, `Adopted` and forwards the rest. |

| 3 Programs and GPU primitives | in progress | `org-native/extract-p3-latched` (ORG `extract/p3-latched`) | Pipeline-stack decision, frame staging and readback already done. Items 3–6 can start beside phases 1–2. Items 7 and 8 (p3-latched): ORG `Render/LatchedCopies.h` (types, copy joining, `RecordLatchedCopies`) and `RenderPasses/LatchedCopiesPass.h` (`MakeLatchedCopiesPass`); `IDescriptorService::Null(owner, NullViewKind)` over `NullSrvDesc` (`Render/Runtime/NullViews.h`). DCLF's `Latched.h` is aliases, its pass factory a wrapper, `GpuTextures` null indices use `Null`. ORG test `OpenRenderGraphLatchedCopiesTests`. |

| 3 Programs and GPU primitives | item 2 written, not built | p3-programs: CS `org-native/extract-p3-programs`, OMS `extract/p3-programs` | Item 2: `ComputeProgram`, `RasterProgram`, `ProgramService` (slots, `FixedProgram`, the pool, `Collect`, `JoinSetup`, `Load`), the build helpers and `ShaderTarget` are OMS `Programs/`. CS sets their `ProgramEnvironment` (shader root, backend, compiler, registry, common defines, request defines, pipeline pins, log) in `Programs::InstallEnvironment`; `ComputeProgram.h`, `RasterProgram.h`, `ProgramService.h`, `ProgramBuild.h` and `ShaderTarget.h` stay as forwarding headers. The pool sizes itself with OMS `PlanWaitingPool` from its stated compile domain. Fingerprints are OMS's XXH3 content hash. Tests: OMS `ORGProgramServiceTests`, `ORGAsyncExecutorTests`. See "p3-programs decisions". |
| 4 Build and upload services | not started | | Needs phase 2's executor. |

| 4 Build and upload services | in progress | p4-uploads: CS `org-native/extract-p4-uploads`, OMS `extract/p4-uploads` | Needs phase 2's executor. Commit uploads (p4-uploads, code only, not built): OMS `CommitUploads.h` (`CommitUploads`, `LatchedUploads`, `AcquireStagedBatch`, `CommitUploadHooks` with `onError` and an `IUploadBytesSink`), `OwnedRows.h` (`OwnedRow<Row, Binding>`, `DescriptorIndex`), `ContentOwners.h` (`Snapshot(seal)`); tests in OMS `tests/Uploads` (`ORGUploadsTests`, CPU only). DCLF keeps thin subclasses and aliases with its hooks (`RenderGraph::FatalHandler`, FrameData) and the frame-constants names. See "p4-uploads decisions". |

| 4 Build and upload services | in progress | p4-build: CS `org-native/extract-p4-build`, OMS `extract/p4-build` | Needs its A/B perf run. p4-build (code only, not built): the staged build service is OMS `StagedBuilds.h` (`org::services::StagedBuilds`, on an OMS `JobLane`), with `BuildObserver` (poll scope, attach, refusal), `isSteadyState`, `afterBuild` and a log callback in its `Desc`. The build context is OMS `BuildContext.h` (`DeferToAttach`, `DeferToStaging`, `CurrentBuild`); DCLF's serials, built and attached, are OMS `BuildInputs.h`. CS `BuildManager.h` keeps the `Builds::` facade, `MakeBuildManager` and an alias. Tests: OMS `ORGBuildsTests`. See "p4-build decisions". |

| 4 Build and upload services | in progress | p4-bufsets: CS `org-native/extract-p4-bufsets`, OMS `extract/p4-bufsets` | Needs phase 2's executor. Buffer sets and payload versions (p4-bufsets, code only, not built): OMS `GrowableBufferSet` gains `Make`/`GrowableBufferSpec`, buffer-keyed `Require`/`RequireNewer`/`Units`/`MostAsked`, `LatchRegions` registration, `NamedVersions::UnitsOf`, `GrowableUnitGroup` and `GrowableRows`; DCLF `SceneBufferSet`, `UnitGroup`, `GrowableRows` and `NamedUnits` forward to them. The payload's version chains are OMS `BufferChains` (`BufferChains.h`); `PayloadSource` implements `BufferChainSource`, and `PayloadPart`, the late-bound buffers and the frame payload stay in DCLF. OMS test `ORGBufferChainsTests` (no device). See "p4-bufsets decisions". |

| 4 Build and upload services | in progress | p4-misc: CS `org-native/extract-p4-misc`, ORG and OMS `extract/p4-misc` | Needs phase 2's executor. p4-misc (code only, not built): scene graph helpers are OMS `Async/VersionedBufferPosts.h` (`PostVersionedBuffer`) and `Async/WindowGates.h` (texture windows and gates), `SceneArtifact` stays; indirect run coalescing is OMS `Passes/IndirectRuns.h` (`IndirectRunCounts`, `IndirectRunWriter` on the bucket binder, `RecordIndirectRuns`, `OrderBySlabs`), DCLF keeps `IndirectDraw` and the push words; the recipe read-order check is ORG `Render/RenderGraph/ReadOrderCheck.h` (`CheckReadOrder`), CS formats the findings; shader bindings are ORG `Render/ShaderBindings.h` and OMS `shaders/OrgResources.hlsli`, CS keeps forwarding files at the old paths. Tests: ORG `OpenRenderGraphReadOrderCheckTests`, OMS `ORGIndirectRunsTests`. |
| 5 Coordinators | waiting | | Only once a second user exists. |

### Checks that need a run

- **Phase 2 executor and retirement join, phase 4 build service:** one A/B perf sample each, before and after (Release,
  or Dev-Fast /Od stated).
- **Multisampled SRVs** (checked by reading code, p0-lib): ORG still lacks them for its own textures. BasicRHI has them
  (`SrvDim::Texture2DMS`/`Texture2DMSArray`, both backends; Vulkan maps them to 2D image views). But
  `DescriptorHeapManager::UpdateDescriptorContentsUnlocked` picks the SRV dimension from `isArray`, `isCubemap` and
  `is3D` only: `ViewRequirements::TextureViews` has no sample count, `SRVViewType` has no multisampled entry, and RTVs
  and DSVs are likewise always `Texture2D`/`Texture2DArray`. So a texture made with `TextureDescription::sampleCount > 1`
  gets a `Texture2D` SRV, which D3D12 rejects for a multisampled resource. The contributor C API validates
  `ORG_RG_VIEW_DIMENSION_TEXTURE_2D_MS` views, but `ContributorHost` refuses multisampled resources. OMS
  `ResolveMultisamplePass` reads its source through the default SRV (`Texture2DMS` in the shader), so it would hit this on
  D3D12; it has no CS user. Adding them is ORG work: a sample count in `TextureViews`, an MS dimension at view creation.

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
  Done in code (p0-lib), with the `RunReadback` error handler and the `Variables.h` wording.

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
- **ORG keyed recording tags**, so more than one extension can tag a graph. Done in p3-pins: `RecordingTagKey`,
  `RecordingTags` (`Render/RecordingTags.h`), `RenderGraph::SetRecordingTag(key, tag)`, `RecordingContext::Tag(key)`.
  The single-tag API is gone; its one user was the pipeline pins.
- **ORG `Builds{attached, staged}` snapshot readable from any thread.** DCLF prunes its cache on its builder thread only
  because `StagedGeneration` and `EpochRecordingCurrent` (`GraphHost.h:351,687`) are owner-thread calls.
- **ORG multisampled SRVs.** Still missing (see "Checks that need a run").
- **One error convention for library errors.** Decided (decision 115). `org::RunReadback` follows it (p0-lib): each
  `Create*` takes an `ErrorHandler`, and CS passes `RenderGraph::FatalHandler`. Other ORG throws are not converted yet.
- **Thread rules in signatures** (tag types or debug asserts) instead of prose and runtime checks.
- **OMS cleanup:** done (p0-lib). `Variables.h` no longer cites a decision.
- **6.11 result:** guest runs with every one of these host options (decision 113). Their off paths went in phase 0.

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

### p3-gates decisions

Decided by the agent, to confirm:

- **`org::RunGate::Create` returns null with a reason** (`std::string* error`), as a template `Create<Gate>` so CS makes its
  own derived type. CS's `RenderGraph::RunGate` derives from it and keeps the fatal `Create`, the current-run overloads
  (`Close()`, `OpenDispatch(entry, ...)`, `OpenDraw(entry, ...)`, `Current()`) and `Dispatch(ComputeProgram)`. Derived
  rather than aliased because about 50 files use the no-run overloads and `RunGate::Current()`, and several forward-declare
  `class RunGate`. ORG has no host-wide current run, and OMS's `ComputeKernel` is not visible from ORG.
- **`org::FirstRun::Open` takes the run.** CS's `RenderGraph::FirstRun` derives and fills in the current run, reading it
  only while the work is pending, as before.
- **`UavClearPass` has no shader.** It records `ClearUavFloat`/`ClearUavUint`, not a kernel, so nothing moved to OMS's
  shader folder and no program is built. It went to OMS `Passes/` as the plan says, taking a `UavClearTarget` (resolver or
  plain resource; a `SelfDeclaringResource`) in place of `FrameAnchors::Texture`. Its one user, `AnchorPasses.cpp`, calls
  OMS directly, and CS's `UavClearPass.{h,cpp}` are gone.
- **`RunSlots` reads no frames-in-flight count.** The staging half that used one already moved, so there is nothing to
  inject. `FrameStaging.h` keeps `AcquireStaging` and aliases `RunSlots`/`RunSlotsRegistry`.
- **Tests:** ORG `OpenRenderGraphRunSlotsTests` (no device) and `TestRunGates` in `OpenRenderGraphExternalResourceTests`
  (D3D12 runtime: latch words, open bits, staging copy, close, `FirstRun`).

### p1-async decisions

Decided by the agent, to confirm:

- **Where they went.** LatestSlot, PublicationLog, SlotTable and KeptState are std-only, so they are in `Async/` (namespace
  `org::async`, the header-only `AsyncPrimitives` target). RetirementChain and `Retired` include ORG's `EventQueue`, so they are
  in `ORGModuleServices/RetirementChain.h` (namespace `org::services`), beside `RetirementClock` in the main target.
- **`Retired` teardown.** It takes the queue's `QueueTeardown` (default `Free`) and capacity (default 64). DCLF's `Retired<T>`
  alias in `Core/Scene/Channels.h` passes `Leak`, which is what it used before.
- **The storage-reuse option is process-wide** (`org::async::keptStorageOptions.reuseStorage`), not per array. The journal's
  run path is static and a snapshot carries no array, so a per-array option would mean new plumbing. It still also picks the
  bitmap run path, as before. CS's two readers (`MainBuild.cpp`, `KeptStores.cpp`) read it directly.
- **CS headers.** `RenderGraph/Async/LatestSlot.h` is an alias (11 users). DCLF `Core/Runtime/KeptState.h` is a list of using
  declarations (22 users). PublicationLog, Retirement and SlotTable had two users each: those include OMS, and their uses
  are qualified.
- **Tests.** `KeptStateTests.cpp` moved to OMS without the AdvancedSkin `OwnerValues` test, which is DCLF's. That one stays in
  CS as `tests/DrawcallLimitFixCPU/OwnerValuesTests.cpp` (`DclfOwnerValuesTests`). A short multi-thread `Retired` test was
  added. `ORGRetirementChainTests` needs ORG's headers (`ORG_ASYNC_TEST_ORG_INCLUDE`, the sibling submodule by default).
- **Style.** The moved headers are reformatted to OMS's layout (4 spaces, attached braces). Member names keep their old
  spelling, without OMS's `m_` prefix, so the move stays a move.

### p0-lib decisions

Decided by the agent, to confirm:

- **The kept-copy text stays on `reuseRecordings`.** With `reuseLiveRecordings` gone, a live epoch keeps copies whenever
  `reuseRecordings` is on. `reuseRecordings` itself stays a flag: it was not one of the five, and ORG tests run with it
  off.
- **The RenderGraph setters went too** (`SetCoalescedSubmissions`, `SetMergedHandoffs`, `SetUploadsWithBatches`,
  `SetKeptGatedInline`). They only carried the flags, so a graph built outside `GraphHost` now gets the on behaviour as
  well.
- **`RunReadback::ErrorHandler` is a required argument of `Create`, `CreateList` and `CreateRegions`,** not a `Runtime::Desc`
  field. A readback is made by its reader, not by the runtime, and a required argument makes the compiler find every CS
  site. An empty handler only logs. After the handler returns, `Create*` return null, `Commit` returns 0 (stamps are
  never 0), and a copy or read is left out.
- **CS's handler is `RenderGraph::FatalHandler(area)`** (`src/RenderGraph/Fatal.h`), so other libraries can reuse it. The
  areas are each user's existing fatal area ("Drawcall Limit Fix", the clouds port's and Post Processing's `kName`).
- **ORG tests that assumed the off paths were updated:** "reuse" and "external-reuse" now count runs that bind a kept
  recording, and "leases-wide" accepts a kept copy's run ref. The bounds (`liveReused >= 6` in "reuse") are a guess
  from reading the code; the director's test run confirms them.

### p2-executor decisions

Decided by the agent, to confirm:

- **Names.** `org::async::Executor` (`Async/Executor.h`), `org::async::Lane`/`JobLane`/`MakeLane` (`Async/Lanes.h`) and
  `org::async::CreateExecutorTaskService` (`Async/ExecutorTaskService.h`). The executor and lanes are a CPU-only target,
  `ORGModuleServices::AsyncExecutor` (needs TBB); the task service is in `ORGModuleServices`, since it needs ORG.
- **Layout as a default.** `Executor::Desc` takes worker counts per domain, the parallel domain (ParallelFor helpers and
  frame-critical jobs) and the frame-critical thread count. The old constants (`Coordinator`, `Preparation`, `Scene`,
  `Streaming`, `FrameCriticalThread`, `Layout`) stay as the default layout's, so CS's 30-odd users need no edit.
  `SchedulingLayout()` gives an executor's own layout.
- **`Statistics` per-domain arrays became vectors**, since the domain count is the host's. Only the DCLF capture reads them.
- **Throw report.** With `onTaskError` set, a throwing ORG task is reported with "<name> threw: <what>" and counts as
  finished. Without it, the exception goes to the executor, which counts a failed task. CS passes `RenderGraph::Fatal`
  under "ORG task", as before.
- **Lanes.** An empty `LaneDesc::refused` stops an OMS lane silently. CS's `Frame::Lane`, `Frame::JobLane` and
  `Frame::MakeLane` fill it with the old fatal ("the <name> lane stopped ...") through `FatalOnRefusal`.
- **Tests.** `tests/DrawcallLimitFixCPU/SceneExecutorTests.cpp` moved to OMS `tests/Async/ExecutorTests.cpp`, with layout
  and lane tests added. `tests/Async/ExecutorTaskServiceTests.cpp` compiles the service against ORG's `ITaskService.h`
  from the sibling checkout, so it stays standalone.

### p3-kernels decisions

Decided by the agent, to confirm:

- **Shader layout.** The moved shaders sit flat in OMS `shaders/`: `PrefixSum.hlsl`, `SortScatter.hlsl`, `Hzb.hlsl`,
  `HzbTest.hlsli`, `ffx_a.h`, `ffx_spd.h` and `FidelityFX-LICENSE.txt`. Flat, so CS's configure-time copy
  (`cmake/RenderGraph.cmake`) only needed `*.h` and `*.txt` added to its glob. DCLF's cull shaders include
  `ORGModuleServices/HzbTest.hlsli` through the shader-root include directory. All of them are DXC-only, so no FXC path
  changes.
- **`ffx_a.h` moved too.** `Hzb.hlsl` needs it, and nothing else used DCLF's copy.
- **`Common/HiZ/SPD.hlsl` stays.** It is not a copy of `ffx_spd.h`. It is a separate, trimmed SPD rewrite with D3D11
  register bindings and its own licence header, compiled with FXC for `HiZPyramid.cpp`. Pointing it at OMS's header
  would change D3D11's FXC output and tie the D3D11 path to the OMS copy. So the build keeps two SPD implementations
  but one copy of `ffx_spd.h`.
- **Scatter parameters.** The stride stays a root constant. The rank bits and the "range" words are defines
  (`SortScatterLayout`, `SortScatterDefines`), so DCLF's build folds the same constants as before. Ranges are optional:
  without them an item goes to `offsets[key] + rank`. DCLF's range words are `offsetof(DrawSequence, instanceCount)` and
  `firstInstance`, restored to 1 and 0. I read the plan's "key word" as the word holding the range's first key and
  "rank word" as the packed key and rank word.
- **The scatter pass stays in DCLF.** Its gating reads DCLF's frame shapes, so OMS ships only the shader, the constants
  and the defines. The pass now records through `ComputeKernel::Dispatch`.
- **HZB inputs.** `HzbDesc` takes the depth as a `PixelBuffer` or a `declareDepth` callback. DCLF uses the callback,
  since its depth is a `FrameAnchors::Texture` slot. The reduction is a define (`ORG_HZB_REDUCE_MIN`) carried in
  `HzbProgram`. The far value uses what was the constants' padding word, so the layout stays 24 words. The reduction's
  identity (outside the domain) is 0 for max and 1 for min, which assumes depth in [0, 1].
- **HZB with no kernel.** The pass is still made and records nothing, as DCLF's did. `MakeHzbPass` returns null only
  without a depth, pyramid or counter; DCLF's pass is only made with phase two, which has them.
- **Barriers.** PrefixSum and HZB keep their full memory barrier between dispatches. `ComputeKernel::Dispatch`
  rebinds the layout and pipeline per dispatch, where the HZB bound them once. That is extra commands, not a change in
  results.
- **Defines.** The moved programs keep `commonDefines` on, as before. `HzbTest.hlsli`'s depth margin is
  `ORG_HZB_DEPTH_MARGIN`, defaulting to the old 1e-4.

### p2-recordings decisions

Decided by the agent, to confirm:

- **ORG, not OMS.** It wraps one GraphHost call and its types, and ORG's no-device tests can cover it. It lives beside
  `GraphHost.h` as `include/OpenRenderGraph/EpochRecordings.h`, with `src/Host/EpochRecordings.cpp`.
- **Failures go to an `OnFailure(what, error)` hook** given to the `Cache` constructor. A failed request reports when it
  settles, then settles its entry with the error. A failed kept entry reports at `Find`, then goes and the lookup misses.
  `Request` alone passes `Outcome::Failed` to its done. CS's `Cache` subclass passes `RenderGraph::Fatal`, so both stay
  fatal at the same points with the same messages.
- **`Posting { Posted, Refused }`** replaces the bool from `Request` and `Cache::Post`.
- **The hash is a template parameter** (`Cache<Key, Keep, Rule, Hash, Payload>`). `Find` and `Add` hash the key. Frame
  segments hash the fields only (`SegmentKeyHash`), as before, so its `AnyOf` checks by hash still work.
- **Owner-thread fields stay on `Entry`; the locked part is a separate `Settle` block** that the host's thread holds alone.
  Predicates read it through `Entry::LockedStatus()`. An erased entry's key and payload are now freed before its requests
  settle. Nothing read them after the erase.
- **The counters (`Read()`) stay process-wide in ORG.** A failed request is not counted, as before.
- **`Cache::Post` has a requester overload** (`Post(entry, count, what, requester, settled)`) that the GraphHost overload
  uses. The tests post through it.

### p2-retire decisions

Decided by the agent, to confirm:

- **Two layers for the join.** `RetirementJoin` (std plus ORG's header-only `EventQueue`) does the counting behind a
  `Source` interface (`SubmissionsMade`, `CpuFramesCompleted`), so its logic is tested standalone. `GraphHostRetirement`
  adds the graph host source and the GPU points (`SubmittedGpuPoint`, `SealedGpuPoint`, `Sealed`), which need `GraphHost.h`.
- **The host comes in as a callback** (`GraphHostAccess`, returning the host or null), since CS's host comes and goes.
- **New headers, not `BackingPool.h`.** The join and the pool defaults are in `RetirementJoin.h`, `GraphHostRetirement.h` and
  `GraphHostBackings.h`. `BackingPool.h` stays std-only for its standalone tests, and p1-async adds beside it at the same time.
- **Shrink is an enum** (`BackingShrink::Settled/Never/Eager`) and copies are `PerFrameCopies(asked, framesInFlight)`. DCLF
  still parses `CS_DCLF_SHRINK` and `CS_DCLF_SCENE_COPIES` (empty means 3), so the values are unchanged.
- **Secondaries stay generic.** The list pool holds only BasicRHI types (`rhi::CommandAllocatorPtr`, `rhi::CommandListPtr`),
  nothing of D3D11 or DXVK, so `SecondaryListPool` moved whole to OMS. `ResetPools` and `RecordingSlot` still take the pool
  type as a template parameter, for tests and other pools. DCLF keeps the slot, the worker index and its fatal check.
- **The reset queue is ORG's `EventQueue`**, not TBB's `concurrent_queue`: OMS's core has no TBB. Both are lock-free, any
  thread pushes and one thread (the render thread) takes. "Retirement comes in as a callback" is `ResetPools`' `Defer`,
  which DCLF points at its executor's preparation lane, with the same zone.
- **Library error text** for a structured backing past `uint32` elements drops the `[DCLF]` prefix.

### p2-birth decisions

Decided by the agent, to confirm:

- **The encoder lives in its own OMS header**, `TexelEncoding.h` (included by `VersionedTextureGrowth.h`), so a CPU-only test
  can cover it without a device.
- **OMS converts half floats and R11G11B10 itself**, a bit-for-bit port of DirectXMath's software paths
  (`XMConvertFloatToHalf`, `XMStoreFloat3PK`), since the library is not Windows-only. CS built without F16C, so it ran the
  same software path. `ORGTexelEncodingTests` compares every format against the old DXGI-keyed encoder on DirectXMath.
  DirectXMath's blue channel shifts a value below about 1e-13 by 32 or more (undefined in C++); OMS masks the count to five
  bits as x86 does, and the test skips that range.
- **`TextureVersionRequest` names the value's format** (`valueFormat`), since an image version has no common format
  accessor. Unknown reads it from a `PixelBuffer` or `ExternalTextureResource`. CS passes the declared format, which is what
  `EncodeTexel` was keyed on before.
- **CS still builds the Value writer in `Prepare`**: a guest export takes the birth as DXVK's wrap data, and a format with no
  encoding still fails there with the same message. Graph-only and native exports now go as `TextureContents::Value`.
- **Floating-point mode:** CS's code built with `/fp:fast`, OMS builds with the default. Only the sRGB `pow` could differ, in
  the last bit before rounding to 8 bits. No CS birth uses an sRGB format (they use R16G16B16A16_FLOAT, R16_FLOAT, R8_UNORM,
  R8_UINT, and R32_UINT for a Fill).

### p3-pins decisions

Decided by the agent, to confirm:

- **Single-tag API removed, not kept as a default key.** It had one user (the pipeline pins), so the keyed form replaced
  it. Keys are compared by address, so two extensions can't collide on a name.
- **A tag set is immutable and shared.** Each `SetRecordingTag` makes a new set. A recording list holds one
  `shared_ptr` to the set, as it held one to the single tag, so the per-batch cost is unchanged.
- **OMS `PipelinePins` takes two callbacks** (`Desc::pin`, `Desc::defer`) instead of CS's `BuildManager`. CS's `defer`
  calls `Builds::DeferStaged` then `Builds::Defer`, as before. The pin table is per `PipelinePins` object; CS keeps one
  static object, as it kept one static table.
- **CS `RenderGraph/PipelineVersions.h` stays as the facade.** Its users (Compute and Raster programs, `Revision.cpp`,
  `RenderGraphRuntime.cpp`) don't change. Phase 3 item 2 can call `org::services::VersionFor` directly once programs move.

### p2-reflect decisions

Decided by the agent, to confirm:

- DXIL's binding shifts and the skipped push register (DCLF's `kBindingShift*`, `kFramePushBinding`) became
  `DxilBindingLayout`, a `Parse`/`ReflectDxil` parameter. DCLF's `SpirvReflection` is a subclass whose `Parse` passes
  them, so its users are unchanged.
- No new dependencies: the SPIR-V reader is hand-written (no SPIRV-Reflect or SPIR-V headers). DXIL uses `dxcapi.h`,
  which OMS already finds for `ShaderCompiler`, and the Windows SDK's `d3d12shader.h`. `DxilReflection.cpp` builds only
  on Windows with DXC; otherwise `ReflectDxil` returns false and `DxilFragment` the defaults.
- DXIL reflection still finds `dxcompiler.dll` by name in the process (`GetModuleHandleW`), as before, instead of
  asking a `ShaderCompiler` for its module. DCLF's `DxilStages.cpp` keeps its own per-thread DXC objects for the
  frame-source preprocess.

### p3-latched decisions

Decided by the agent, to confirm:

- The latched copy types and the copy pass go to ORG as two headers: `Render/LatchedCopies.h` (no pass machinery, so
  DCLF's widely included `Latched.h` pulls in nothing new) and `RenderPasses/LatchedCopiesPass.h`. `LatchedTarget`,
  `LatchRegionsHeld` and `SetLatchWords` moved too, since they serve only latched copies.
- `LatchedSource` renamed its host words: `shape` is `owner`, `hosted` is `groups`, `zeros` is `copyFixed`. The
  aggregate order is unchanged, so only one DCLF line (`source.groups`) changed. DCLF keeps `MakeLatchedCopiesPass` as a
  wrapper that passes `!cpuPipeline` as `copyFixed`.
- The copy joining is `org::LatchedCopyRuns`, split out so it can be tested without a device.
- `IDescriptorService::Null` is virtual with a default over `CreateOwnedShaderResourceView` and makes a new descriptor
  each call (no cache in the service), because a cached binding would hold the device owner past a device change. Callers
  keep the binding, as DCLF does.
- `NullViewKind` covers Texture2D, 2D array, 3D, cube, cube array, typed, structured and raw buffers. Every texture kind
  gets R8G8B8A8_UNorm with one mip: D3D12 needs a format for a null texture view, and Vulkan's null descriptor depends only
  on the descriptor type. DCLF's Texture2D, typed (R32_UInt) and structured (stride 4) descriptions are unchanged.
- DCLF's `NullOf` keeps its D3D11 1x1 views for array, cube and 3D slots. Swapping them for per-dimension null
  descriptors would change what the engine's draws read (a D3D11 view through the import, not a null descriptor), so it
  is left for a separate change.

### p2-cpu decisions

Decided by the agent, to confirm:

- **Zone tallies and the budget go to BasicTelemetry, not OMS.** The tally scope wraps `basic_telemetry::Scope` and
  `Callsite`, and the budget is plain CPU timing with no graph types. In OMS both would add a BasicTelemetry dependency to a
  module that has none outside `AsyncStateGraph`.
- **The budget's epoch split is a parameter.** `ThreadBudget` takes `Splits` running totals through a function pointer, read
  at an outermost hook's ends, plus the bucket each goes to. DCLF passes ORG's submit and overhead totals. The claiming site
  (`SceneFrame`) and the rest bucket (`Other`) are `Desc` fields. The thread is a `std::thread::id`, not a Win32 id.
- **The sphere cull gets the plugin's compile options.** CS adds `SC_DEBUG_OPTS`/`SC_RELEASE_OPTS` (`/fp:fast`, `/O2`...) to
  `ORGSphereCull`, so the moved kernels build as they did in the plugin. The per-file `/arch` flags are set in
  `SphereCull.cmake` for the OMS files and stay in `RenderGraph.cmake` for DCLF's `CullAvx2.cpp`/`CullAvx512.cpp`.
- **The SSE pass is `org::cpu::FrustumSse`, taking a plane array and count** in place of DCLF's `PlaneSet`. It still leaves
  dead lanes to the caller, as before. The level is `org::cpu::Supported()` (CPUID and XCR0, no logging); DCLF's
  `Simd::Supported()` wraps it and keeps its log line.

### p3-programs decisions

Decided by the agent, to confirm:

- **Layout.** OMS `include/ORGModuleServices/Programs/` (`ComputeProgram.h`, `RasterProgram.h`, `ProgramService.h`,
  `ProgramBuild.h`, `ShaderTarget.h`), in `org::services`. The build helpers are `org::services::program_build`. CS keeps
  `ComputeProgram`/`RasterProgram` as global aliases, and `Programs::Fixed<P>` and `Programs::Detail::Slot` as aliases of
  `FixedProgram<P>` and `ProgramSlot`, so no feature file changed. The four forward declarations became alias declarations.
- **A process-wide environment.** `ComputeProgram::Load(device, desc)` is static and called directly in about 15 CS files,
  so the injected pieces live in one `ProgramEnvironment` the host sets once (`SetProgramEnvironment`), not in each call.
  CS sets it at the top of `RenderGraphRuntime::Initialize`. Every callback resolves lazily, so the compiler and registry
  are read when a program loads, as before.
- **Injected pieces.** Shader root (`Data/Shaders`), backend (`Backend::Active`, mapped to the binary format by
  `ShaderFormatFor`), compiler, registry, common defines (CS's `WithCommonDefines`, still resolved where programs are queued),
  request defines (`CS_ORG_BINDLESS`, and `CS_GPU_AMD` on AMD, in the old order after the API define), the version a recording
  binds (`PipelineVersions::VersionFor`; without it, the slot's newest), the pipeline-statistics label hook (BasicRHI's
  Vulkan-only function, so OMS doesn't link it) and a log callback. CS's log adds the `[ORG] ` prefix, so log lines read as
  before.
- **Errors.** `ProgramService::Desc::onError` gets the refused build, a failed build at `Collect`, a fixed program read
  before it was built and a stopped lane. CS passes `FatalHandler("Programs")`, so all stay fatal. A stopped programs lane
  is now fatal under "Programs", not "lanes". A refused slot is marked done with its error, so a join doesn't hang when
  the handler returns, and `Collect` doesn't report it twice. Load failures stay null and logged.
- **The pool coupling.** An explicit dependency, not a new domain: `org::async::PlanWaitingPool(executor, runOn,
  waitsOnDomain, fallback)` in `Async/Lanes.h`. `ProgramService::Desc::compileDomain` states where the builds' compiles run.
  The arithmetic is unchanged (half the shared domain's workers, or one lane on the fallback class with one worker), so the
  thread counts are the same. A new blocking domain would have added threads.
- **The hash.** `HashBinary` is OMS's XXH3-128 (`HashShaderBytes`) folded to 64 bits; a raster pair hashes both through
  `ShaderKeyBuilder`. The fingerprint is persisted nowhere: `PipelineRegistry` keeps it in memory and compares it only
  within a session, at a reload. So no on-disk cache is invalidated; the shader store keys on its own variant keys.
- **Log wording.** The pool's line says "on the compile domain's threads" or "on the fallback threads" instead of
  "preparation"/"blocking", and the not-built error says "after the setup join" instead of "after CS's setup".
- **ASCII names.** Entry points and define names are narrowed by casting, as CS's define code did. They are ASCII.

### p4-uploads decisions

Decided by the agent, to confirm:

- **The binding type is the parameter, not the owner type.** `OwnedRow<Row, Binding>` takes the host's binding (an index, an
  owner and an `explicit operator bool`, checked by the `DescriptorBinding` concept). Owners stay `shared_ptr<const void>`,
  since `ContentOwners` and the uploads hold them type-erased. DCLF's `OwnedRow<Row>` is an alias over `GpuTextures::Binding`.
  The old test was `index == GpuTextures::kInvalid`; `Binding`'s `operator bool` is the same test.
- **The seal is a `Snapshot` argument**, not a constructor one, so `SceneBuffers` keeps a plain member. Its one caller
  (`ExecutionOwners::Seal`) passes `GpuTextures::Seal`.
- **Hooks are held by reference** (`CommitUploadHooks`, a static in DCLF's `Uploads.cpp`), so a commit builds no
  `std::function`. The byte sink is an interface with `Enabled()` asked before each write, as `FrameData::Enabled()` was, so
  names are still looked up only when counting. A missing `onError` drops the owners silently, as `ViolationPolicy` does.
- **DCLF's `CommitUploads` and `LatchedUploads` are subclasses**, not aliases, because `Commits.h` and `VertexStreams.h`
  forward-declare them as classes, and so the commits' constructors don't change. DCLF acquires the batch itself
  (`AcquireCommitBatch`) to keep its `CS.DCLF.Stage.AcquireBatch` zone.
- **Stayed in DCLF:** `FrameConstantsPart` and the route-and-name strings (they know the `cs.dclf.frame-constants` buffer), the
  FrameData sink, the fatal area name, and the rest of `Uploads.cpp`, which is DCLF's main commit (scene streams, buckets,
  shadow and class latches, frame textures). Only `CommitUploads::Stage(Versioned)` moved out of it.
- **No OMS test for `CommitUploads`/`LatchedUploads`**: they need staged pages and latch blocks, so a device. The CPU test
  covers `OwnedRow` (default and host bindings) and `ContentOwners` (overlaps, replaced owners, unchanged snapshots, counts).

### p4-build decisions

Decided by the agent, to confirm:

- **Names.** `org::services::StagedBuilds` (`StagedBuilds.h`), `BuildObserver`, `BuildRefusalSite`, `BuildLogLevel`. The build
  context is its own std-only header (`BuildContext.h`) so it is tested without a device. CS keeps `RenderGraph::BuildManager`
  as an alias, so its users (`RenderGraphRuntime`, `Revision.cpp`, DCLF's `GraphBuilds.cpp`) are unchanged apart from
  `MakeBuildManager`.
- **Observer, not callbacks per event.** `BuildObserver::InPoll(point, body, context)` runs the poll's body, so CS opens its
  `Frame::Boundary::Caller` around it as before, with no allocation per poll. `Attached(label)` and `Refused(site, reason)` map to
  `NoteAttach` and `NoteRefusal`. CS maps the set site to "revision", so the census keys are unchanged. Ask's late change and the
  adoption-point check stay in the CS facade, which never left CS.
- **The recipe trace is a `Desc::afterBuild(epoch, graph)` hook**, run on the building thread inside the build, under the build lock
  and the same try as before. CS's hook defers `NoteTrace` with `DeferToAttach`, which lands in the same `attached` list after the
  extensions' entries, as the direct push did.
- **Steady state.** `Desc::isSteadyState` is read at each frame end until true (empty: true at the first). CS passes the world
  window check and `steadyStateName = "first world frame"`, so the log lines, which the shader-reload configs match, are byte-equal.
- **Logging** goes to `Desc::log(level, line)`, formatted with `std::format` only when a log is set. CS sends each level to its
  logger. The texts are unchanged.
- **The executor and lane come from the host** (`Desc::executor`, `Desc::lane`). CS passes the shared executor and
  `FatalOnRefusal({"CS graph builds", Preparation, Cpu})`, as `Frame::JobLane` did.
- **`BuildInputs<State>`** holds the attached value, its serial, the built serial and the first-builds flag. `Next` keeps the
  value when nothing moved, `Settle` takes `settled()` lazily (only once the serial is unbuilt, as before) and returns
  `BuiltAndAttached` for the first builds, so DCLF still publishes the attached state itself (it also attaches the graph
  conditions). DCLF's `buildState` member and its free functions stay; their storage is one namespace-scope `BuildInputs`.
- **No device-free test of `StagedBuilds` itself**: it drives an `org::GraphHost`, which needs a device. `ORGBuildsTests` covers
  the build context and the build inputs.

### p4-bufsets decisions

Decided by the agent, to confirm:

- Payload versions did not merge into `ComposedVersions`/`NamedVersions`. The payload parts are late-bound buffers that the
  frame publishes per version (`LateRefBuffer`), not `VersionedBuffer`s that a recording resolves through
  `IResourceVersions`. Naming them in a set would change how the frame binds them. The chains became OMS `BufferChains`
  over `VersionedBufferBuild` instead: change detection, the journal floor, the held versions, settle and finish, the
  counters. DCLF keeps the part list, the journals' readers and the frame payload.
- `BufferChains` makes no pools and posts no graph work itself. The host passes `makePool` (it gets the library's
  suspension counter as the pool's telemetry), `post` (it gets a `built` hook to run before the version freezes), `wake`
  and `failed`. DCLF's `post` wraps `built` in its `PayloadPartTag` and calls `SceneGraph::PostPayloadPart` as before, so
  p4-misc's scene graph helpers are untouched.
- A failed build calls `failed` every time. DCLF still logs only a part's first failure (its own `reported` flags).
- `PayloadSource` derives from `BufferChainSource`: its fields are unchanged, and it gains the per-part overrides.
  `PayloadPools::PartReport` is an alias of `BufferChains::Report` (same fields).
- `LatchRegions` moved to OMS with the enumerators `PerRun` and `Shared` (OMS style, no `k`). Producer.cpp's two uses
  changed. `Shared` registers one region (a dedicated block), `PerRun` two (a frame-arena block), as before.
- DCLF keeps its own `GrowableSpec` (no shrink or address fields). `SceneBufferSet::Make` adds DCLF's shrink policy and
  `address = true` when it builds the OMS `GrowableBufferSpec`.
- No device-free test covers the `GrowableBufferSet` additions: a seal needs real `org::Buffer`s. They are thin
  forwards of what DCLF did; the in-game runs cover them.

### p4-misc decisions

Decided by the agent, to confirm:

- Scene graph helpers are two OMS headers in the style of `VersionGrowthArtifact.h`: `VersionedBufferPosts` +
  `PostVersionedBuffer` (org::services, beside `RegisterVersionedBufferProducer`; it expects no `wrap` hook) and
  `WindowGates` (org::async: `RegisterWindows`, `RegisterGates`, `PostWindow`, `RetireWindow`, `PostGate`). The gate
  producer captures the `WindowGates` object, as DCLF's captured its gate list, so the graph must shut down first.
  `SceneGraph` keeps its API and member order.
- `IndirectRun` moved to OMS with `vertexLayout` renamed `tag` (a host word bound with the pipeline). `IndirectDraw`, the
  push words and `PulledBucketPipeline` stay in DCLF. `RecordIndirectRuns` is templated on the list type so the test can
  use a fake list; DCLF copies its counters into an `IndirectRunState` and back. After the runs DCLF sets
  `boundVertexLayout` to the last run's tag, which is what the old loop left.
- The read-order check returns structured findings (`ReadOrderFinding`) and counts; CS formats the same messages as
  before, so the log text is unchanged. The owner tests became two predicates: `requiresLocalWriter` (CS stages) and
  `allowsLaterWriter` (fragment sources' stages). Positions are the recipe's stage indices.
- ORG's `Render/ShaderBindings.h` (`org::shader_bindings`) takes the push block helpers, `Latch`/`LatchOf`, and
  `SamplerPreset`/`SamplerDescOf`. CS keeps `CreateSampler`, `SamplerFromD3D11`, the shared preset cache and
  `CreateShaderResourceView`, since they go through CS's runtime and D3D11 types. `RenderGraph/ShaderBindings.h` is a
  forwarding header (about 100 users).
- OMS `shaders/OrgResources.hlsli` keys on `ORG_BINDLESS` and has no SHARED kind. CS's `Common/OrgResources.hlsli`
  (included by about 160 shaders, many FXC ones shared with D3D11) defines `ORG_BINDLESS` under `CS_ORG_BINDLESS`,
  includes `ORGModuleServices/OrgResources.hlsli`, and adds SHARED as a host kind. The FXC token stream is unchanged; this
  needs the DXBC comparison below. A host kind is added by defining `ORG_HANDLE_<kind>` and `ORG_RESOURCE_<kind>`.
- p4-misc check that needs a run: FXC output of the D3D11 shaders that include `Common/OrgResources.hlsli` is identical
  (`tools/verify-shader-refactor.ps1` on Lighting.hlsl, ISHDR.hlsl, Effect.hlsl), and `tools/check-dual-target-shaders.py`
  passes; both need `package/Shaders/ORGModuleServices` staged by a configure first.
