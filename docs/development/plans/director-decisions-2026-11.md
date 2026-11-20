# Decisions to confirm (2026-11-19 onward)

Decisions taken while the user was away, during the fix round, the port completion and the two plans
(`native-frame-generation-plan.md`, then `frame-revision-plan.md`). Each says what was decided and why. Nothing here is
final until the user confirms it.

## Fix round and ports

1. **Physical Sky phase 4 (volumetric clouds) gets its own lead agent.** The phase is about 12,000 shader lines, 45 programs
   and about 60 resources, too large for the agent that did phases 2, 5 and 6. Until it lands, guest runs the AP shadow, the
   fog scattering and the sky tail on D3D11 on frames with clouds on, and native has no clouds or lightning.
2. **Physical Sky open questions 9-15** (`physical-sky-port.md`, section 8) were decided by the port agent and are marked
   "to confirm". The ones that most need a look: 9 (depth without Dynamic Cubemaps), 10 (when the AP composite runs), 13 (D3D11
   inputs in guest mode), 14 (the sky tail waiting on ViewAtmosphere).
3. **cs-test configs with a setting change one frame after a shot.** On the graph a run's first shot needs a rebuild to
   add its copy, so a change one frame later lands first. `lens.toml` was fixed (60 frames). `quality-burst.toml`,
   `resize.toml` and `upscale-burst.toml` have the same pattern and were left alone, since they are burst tests. A
   longer-term fix: cs-test's `previous` anchor after a `shot` waits for the shot's read back.
4. **The PublishedResources off-thread read** (DCLF's `DeclarePublishedRead` during an async build) is recorded in
   `dclf-open-defects.md` and left for later. It needs the build snapshot to carry each entry's state and view.
5. **Native frame generation started while the clouds port runs.** The fix round is done (post chain, lens, guest fog, Physical
   Sky phases 2 and 6 all pass in every mode); only Physical Sky's clouds (phase 4) remain, with a lead agent on them. Native
   frame generation's parts A (step 1, A7) and C (steps 4-5, swap chains a presentation layer may wrap, and native presentation
   replacement) touch other code, so they started in parallel rather than waiting hours. Parts B (steps 2-3), D (step 6), E
   (step 7) and F (step 8, DLSS-G) follow as A and C land.
6. **DLSS-G in native (step 8) is implemented but not testable here.** The plan needs the RTX 5080; the test machine has the
   RX 6600. It lands behind its probe, with its tests left for the user.
7. **Frame generation steps 1, 4 and 5 merged** (`e1c469e09`; quick runs in three modes, `hdr` in all four, `menu` and guest
   `fg` pass). Their leads' calls are in `native-frame-generation-plan.md` section 6, marked "to confirm". The ones with visible
   effects:
   - HDR's frame end moves to HDR Display's own present. Under HDR, a DCLF load-hold present now holds.
   - Every native colour-space change is a full swap chain destroy and create.
   - `SetSwapChainColorSpace` now reports HDR10 support, not that the change applied.
   - The `Placed` epochs and the late-epoch hook stay, for EngineCubeClear and guest's separator.
8. **Washed-out exteriors in today's runs** are Physical Sky on without Linear Lighting (the open upstream question), not a
   regression. Physical Sky's textures are now staged with the test build, so the feature runs where it did not before. Runs that
   compare brightness set `features.PhysicalSky=false`.
9. **Physical Sky's clouds and lightning are merged** (`267d413f2`), with these items left open:
   - The graph clouds are about 6% darker than D3D11.
   - Lightning, storms and the other cloud layers are untested.
   - The lead's calls are open questions 16-24 in `physical-sky-port.md`.
   - It added handle tables to `AnchorPasses` (`Dispatch::table`/`Draw::table`, `Bind::Table`), since the clouds need about
     60 resources against the 32-word push block. That also serves the deferred composite later.
   - The merge moves `extern/OpenRenderGraph` to `7c5d080` (`org-port/exempt-exit-commit`, "never commit an exempt backing's
     exit step"). It is a descendant of the previous pointer, is only local, and needs pushing to the ORG remote with the
     rest.
10. **Native FSR-FG, steps 6 and 7, merged** (native Vulkan 46 rendered / 92 presented per second, native D3D12 37 / 74,
    HDR10 too, no device loss, D3D12 InfoQueue clean). The leads' calls are questions 23-36 in the plan. Ones with visible
    effects:
    - Native loads `sl.fsr_g` at boot, which costs one extra swap chain replacement (about 90 ms) with frame generation off.
    - The controller's wanted method no longer waits for the surface (27). This also changes guest: a surface stuck in the
      scRGB fallback stays wrapped and passes frames through.
    - A failed wrap is fatal instead of falling back (30, 32).

    The foreground eye check (HUD ghosting, pacing, menu and loading passthrough) needs the user, on both backends.
    `fg+hdr` cannot test frame generation under HDR (step lists replace whole); `fg-hdr.toml` does.
11. **DLSS-G in native (frame generation step 8) is merged untested** (`2fcb06441`). The lead's calls are plan decisions
    37-42: load at `slInit` and unload before the first swap chain; all tags `eOnlyValidNow`; native sets the DLSS-G constants
    itself; a native optical-flow queue in BasicRHI, with a fallback to interop; the probe checks the native adapter's LUID.
    Guest's DLSS-G tags also moved to the UI-stage inputs pass. The RTX 5080 procedure is in the plan's step 8, "user to run",
    and includes `guest+fg-dlss`.
12. **Frame revision step 1 is merged** (`[ORG] boundary:` census, counting only). The lead's calls are in the plan's
    section 9, "Decided by the agent (step 1)". Steps 2 and 3 run in parallel after it.
13. **Submodule commits that exist only locally** and need pushing with the rest:
    - OpenRenderGraph `2b22711` (`rev-step1-adoption-observer`), on top of `7c5d080`.
    - BasicRHI `021308d` (`org-native/fg-dlssg-native`), on top of `5099de3`.
    - Streamline `156ace6` (`org-native/fg-native-d3d12`), which has the Vulkan branch in it.
    - The Streamline `sl.fsr_g.dll` in the `CS AB nfg` mod is a local build of `156ace6`; the original is kept as
      `sl.fsr_g.dll.orig`.
14. **The shared MO2 profile was broken twice by leads** (staging without the base AIO, and a retry script deleting a live
    stage folder). cs-test's stage now refuses a mod without TerrainHelper.esp. A guard that stops a stage folder being
    deleted while it is live is not built.
15. **Frame revision step 2 is merged** (named texture versions). The lead's calls are in the plan's section 9, marked "to
    confirm":
    - Misses count only the set's own kind, because DCLF's named parity check would warn otherwise.
    - Texture sets hold textures weakly.
    - The CS recording argument carries a serial.

    ORGModuleServices `9b85a9f` (`rev-step2-named-versions`, on `0185120`) is local only. Note that `extern/ORGModuleServices`
    has `update = none` in `.git/config`, so `git submodule update` skips it: check it out by hand after a bump.
16. **`C:/Modding/MO2/mods/CS AB rv2` is left in place** for the user to delete. The permission check refused the lead's
    delete, and the director did not do it in its place.
17. **Frame revision step 3 is merged** (`7707964b9`): ORG `AttachStagedEpochGraphs` and `EpochsDeclaring`, plus refusal
    counts. The lead's calls are in the plan's section 9 (step 3), marked "to confirm". OpenRenderGraph `0f2efbf`
    (`rev-step3-staged-attach`) is local only. Two ORG test failures already on the base are left alone:
    - `PersistentVulkanHostTests leases` fails with "Synchronous frame capacity exhausted", then hangs.
    - `ExternalResourceTests` exits early.
18. **Frame revision step 4 has one lead** that gives the sub-steps 4a-4e to its own workers, at most two at a time, as the
    plan's serial order allows. That keeps the user's "one subagent per part" rule: step 4 is one part with five sub-steps.
19. **Frame revision step 4 is merged** (`f3f292c80`; on nfg the standard set, native Vulkan resize, guest resize-drag, native D3D12 HDR and latch-census-short pass): the prepared set, the revision lane and staged builds joining
    revisions. The lead's calls are in the plan's section 9 (step 4), marked "to confirm". OpenRenderGraph `bfd13bb` and
    ORGModuleServices `d2ec1c6` (both on branch `rev-step4`) are local only. Open from step 4:
    - In native, adoption still waits in `WrapTexture` for each exported twin: 19-75 ms on Vulkan, 41-102 ms on D3D12
      (Dev-Fast). The proposed fix is to zero-fill exported versions on the lane and skip the wrap's initialize.
    - A change of shape class (for example the mip count) still goes through `ResizeTo` and a structural rebuild
      (`Revision.cpp:ApplyResizes`). No run hit it.
    - The prepared part of the graph-resource self-test is untested in native, because the self-test's older part already
      fails there on the base.
    - The 4e files were not built with `-DSC_UNITY_BUILD=OFF`, and the `CS_ORG=0` stubs were not built.
    - `ORGBufferBuildTests` in OMS doesn't compile (a `ProcessDeferredReleases` signature mismatch). This was already so before
      step 4.
    - `981aef49e` is typed `feat(org)`, where the plan prefers `refactor`. It's left as is, because rewording it would mean
      rewriting merged history.
20. **Frame revision step 5 has one lead**, which also does native framegen step 9 (`PresentationConfig` as a variable), as
    both plans say. It reuses the step 4 worktree on `org-native/rev-step5`.
21. **Frame revision step 5 and native framegen step 9 are merged** (`925add6a1`). On nfg, 9 of 9 runs passed: the standard set, native Vulkan resize, guest HDR, menu, native Vulkan FG with HDR, and latch-census-short. None stuck at load. The lead's calls are in both plans,
    marked "to confirm". ORGModuleServices `d48c924` (`rev-step5`) is local only. Open from step 5:
    - Twice, the first launch after a new staged DLL stuck at the loading screen: the main thread logged nothing after
      `load-start`. It is unexplained, and no dump was taken. `rv1` saw it six times on the same day, before step 5, so it
      is probably older than step 5.
    - The `hdr-frames` config does not exist yet. It is needed to judge the 1 to 2 old-content frames native may present
      into a new swap chain.
    - Upscaler options are not yet read from the adopted revision. This moved to migration 6.14.
    - The Dev-Fast PDB hit LNK1140 (the program database size limit) after many incremental links. Deleting the `.pdb` and
      `.ilk` fixes it.
22. **Step 6 runs one lead per migration.** 6.1 (EHF) goes alone first, because it lands the declaration API. After it, up to
    six migrations run at once, on disjoint files (the user raised this from the plan's three on 2026-11-19). To stay near
    the cap of about 8 agents building at once, each migration lead does its own build and uses at most one worker. Runs
    still go one at a time through the cs-test lock, so test queues get longer.
23. **From migration 6.2 on, leads only write code** (the user's instruction, 2026-11-19). A lead doesn't build, stage or
    run the game. It commits on its branch and lists the tests its change needs. The director merges each branch into
    integ/hard-ports, builds once in the main checkout, fixes build and test failures, and runs the listed tests. This
    replaces the per-lead build cap in item 22. Up to six migration leads still run at once. The 6.1 lead was started under
    the old brief and finishes under it.
24. **Migration 6.1 is merged** (`ce2edfbb0`): EHF, and the declaration API in `src/RenderGraph/Frame/Declare.h`. The
    lead's calls are in the plan's section 9 (step 6.1). The lead saw the guest fog go black on the `nfg` stage (the old
    non-finite kMAIN defect), while its own branch was clean. That means the defect is still in integ/hard-ports before
    6.1, even though `dclf-open-defects.md` lists guest fog as closed. It is to be checked in the 6.1 integration run.
25. **The first six code-only migration leads** (worktrees `org-port-worktrees/mig-*`, branches
    `org-native/rev-step6-N`):
    - 6.2 (ImGui pages);
    - 6.3 (Terrain Shadows);
    - 6.4 and 6.5 together, because Screen Space Shadows' client depends on Terrain Blending. This lead also designs
      `DeclareProgram`, which 6.1 left to 6.4;
    - 6.7 (ImageSpace);
    - 6.8 (LLF clusters);
    - 6.12 (Volumetric Lighting).

    SSGI (6.6) and Sun Rays (6.10) wait for `DeclareProgram`.
26. **6.1 integration passed on nfg** (9 of 9 runs), and guest fog now matches native in the `fog` shots. The black guest fog
    the 6.1 lead saw came from an older nfg stage. Merged since: 6.2 (`PublishTexture` added to `Revision.h`), 6.3 (a
    `HeightMap` variable, plus the new `terrain-shadows` config, which assumes Dragonborn and terrain shadows on) and 6.8
    (the LLF grid as a latch value, which now always applies the adopted ratios, even under `dynamicResolutionLock`). The
    plan doc's per-step "Decided by the agent" blocks conflict at each merge; the director keeps every block.
27. **The 6.8 lead changed the shared git config.** In the main checkout's `.git/config`, `submodule.extern/*.url` now points
    at the main checkout's own `extern/*`, because the cs-integration clones lacked local-only commits (BasicRHI `021308d`).
    The change is kept while worktrees need the local-only commits. Restore the upstream URLs before any push.
28. **Next migration leads:** 6.13 (LLF local shadows, which may add `DeclareBuffer`), 6.14 (Upscaling, together with the
    upscaler options from the adopted revision) and 6.16 with 6.17 (IBL and Skylighting).
29. **6.2 and 6.3 integration:** `menu` passed in all four modes. `terrain-shadows` passed in guest and native D3D12, and
    the quick runs passed. Native Vulkan `terrain-shadows` stopped on a fatal schedule mismatch after `coc Riverwood`:
    "ISWaterWadingHeightmap instance 4 of the frame is past the start stage's 4 instances". A code-only fix lead
    (`org-native/fix-wading-instances`) is on it. The mismatch is a gap in the native schedule, not something 6.3 did.
30. **Merged and built together:** 6.4 with 6.5 (Screen Space Shadows, Terrain Blending, and `DeclareProgram`), 6.7
    (ImageSpace, which adds an `EngineImageSpace` variable), 6.8 and 6.12. The director fixed three integration errors:
    - a missing `Frame/Declare.h` include in `ORGTerrainBlending.cpp`;
    - the scene settings generator didn't handle a struct wrapped in `CS::Setting` (VL's `TextureSize`); it now reaches
      the struct's members through `Get()`;
    - a merge that dropped a closing brace in `Variables.h`.

    Every lead adds frame variables at the same spots in `Variables.h`/`.cpp` and `Declare.cpp`. Each merge keeps both
    sides.
31. **Leads now running:** 6.6 (SSGI, the second `DeclareProgram` user), 6.9, 6.13, 6.14 and 6.16+6.17, plus the wading
    fix. Still to come: 6.10 and 6.11 (Effects11, with programs), 6.15 (HDR, which depends on 7b), 6.18, 6.19, 6.20,
    6.21, 6.22 and 6.23 (which moves with 7b).
32. **6.13 and 6.16/6.17 are merged (not yet built).**
    - 6.13 adds `CS_LLF_LOCAL_SHADOWS` (schema to regenerate) and an optional `adopted` callback on `DeclareClient`.
    - 6.17 adds a `RenewFn` to the texture declarations: a reset gives a new version at the same extent.
    - **To measure before keeping:** a Skylighting reset makes about 120 MB of new volumes at every loading screen, briefly
      240 MB during preparation. If the RX 6600 can't afford it, go back to a gate-only reset.
    - IBL's client depends on `DALCMode`, not on `EnableIBL`/`UseStaticIBL`, because Effects 11 can turn IBL on while
      `EnableIBL` is 0. The lead decided this; it is to confirm.
    - 6.18 (the setup-only features) started in the free slot.
