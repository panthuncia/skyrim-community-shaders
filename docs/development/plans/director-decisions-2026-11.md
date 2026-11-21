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
33. **Batch 2 (6.4/6.5, 6.7, 6.8, 6.12) passed 21 of 21 on nfg**, including `llf-parity`, both interior configs, and
    `shadows-blend` in four modes.
34. **Batch 3 merged and built:** 6.6, 6.9, 6.13, 6.14, 6.16/6.17, 6.21, the wading fix and 6.18. The director's
    integration fixes:
    - The scene catalog generator now accepts `.Get()` in SaveSettings pairs.
    - New `Util::SliderFloat` over a `CS::SettingElement`, a writable element of an array Setting. Together with 6.18's
      `Util::RadioButton`, it keeps COD Bloom's mip factors and SSGI's resolution mode bound in the scene editor. The
      catalog floors would otherwise have failed: 9 controllable entries were lost.
    - A merge between `DeclareProgram` and `DeriveFromSetting` needed two hand-placed braces. The keep-both script only
      handles a split struct; a split function needs a hand fix.
    - `schema.toml` is regenerated for `CS_LLF_LOCAL_SHADOWS`.
35. **6.9 breaks two rules.** It builds the chain, shader compiles included, on the render thread at revision start, and
    it falls back to D3D11 when a chain build fails. The 6.20 lead also moves the chain's programs onto `DeclareProgram`
    and makes a failed build fatal.
36. **To measure:** SSGI keeps display-size partners at every resolution mode (6.6), about 115–180 MB more at 4K.
    LensFlare Ultra keeps all 8 spectra (up to 56 MB). Dynamic Cubemaps and Cloud Shadows make a new cubemap edge fatal
    instead of declaring textures (6.18; the passes are made for 9 mips).
37. **A step 6 follow-ups lead:** `DeclareBuffer` (the LLF cluster buffers, the HistogramAutoExposure buffers),
    ImageSpace depending on `EnabledSSR`, the Volumetric Shadows cascades variable, and the section 8 grep review.
    6.15 (HDR) and 6.23 (Screenshot) wait for step 7b.
38. **6.19 is merged** (the DCLF family clients and Bink). Its calls are in section 9.
39. **7b and 7c start before 7a.** The plan orders them after 7a, but 7a only deletes code that step 6 frees up, and 7b
    and 7c don't depend on those deletions. 4e already replaced `SwitchStaged` with `AttachStaged`. The 7b lead also
    does 6.15 (HDR) and 6.23 (Screenshot), which wait on 7b. 7a and 7d follow once step 6 is fully merged.
40. **`guest+llf-local-shadows` drew no world** (a flat sky colour and the HUD) from its first shot, before any LLF
    change. That config turns DCLF off. `d3d11+llf-local-shadows` passed with Dragonsreach drawn. So guest with DCLF
    off does not draw the world on this branch, and 6.13's graph cache is untested in guest. Open: is guest without DCLF
    supported? If not, the config's guest form needs another route.
41. **6.10/6.11 (Effects11) is merged, with the same rule gaps as 6.9:**
    - FX planning, including per-pass DXC compiles, runs on the render thread at revision start;
    - a failed plan falls back to FX11;
    - toggling the rays shows one frame on D3D11.

    These go to a follow-up after 6.20 lands, using the pattern 6.20 sets for the post chain.
42. **7c is merged.** Q5's answer: a plain pipeline swap at adoption is not safe. Programs read their live pipeline slot
    when they record, and a kept recording can replay after the old pipeline is freed. So a shader reload still builds
    every graph on the render thread, now inside the adoption of the `ShaderGeneration` revision. That is the plan's
    allowed fallback. The real fix is revision-named pipelines:
    - OMS staged sets passed as host data;
    - an ORG `RecordingContext` that holds its pipelines.

    This is listed for after step 10. A shader reload is a development action, so the render-thread build only affects
    developers.
43. **6.20 and 6.22 are merged, so every step 6 migration is in.** 6.20 also fixed 6.9: the chain's programs build on the
    programs lane, and a failed build is fatal. Two D3D11 runs remain:
    - an `Update` hold (Color Grading's readback) runs that frame's chain on D3D11;
    - at session start the chain stays on D3D11 until its first plan is built.

    These are to confirm. A fix lead does the same for Effects11.
44. **The `ibl` config hangs** in native Vulkan and guest. A world recipe finding ("reads a resource before the pass that
    writes it") on `pre-main`'s new build is fatal and opens a message box. Before it, PublishedResources is read on the
    BuildManager thread. A fix lead is on it.
45. **The graph modes require DCLF.** Guest and native now stop at startup with a clear error when DCLF is off for any
    reason, instead of drawing a blank world. `llf-local-shadows` is D3D11 only. Left for the user:
    - **6.13's graph local-shadow cache can't run in any supported setup.** LLF's local shadows need DCLF off, and the
      graph modes need DCLF on. Either delete it, or let the cache follow DCLF's shadow views.
    - **Turning DCLF off after startup** ("Disable at Boot" from saved settings, or the menu toggle) still blanks the
      world silently in graph mode. The toggle should probably be locked there.
46. **7b, 6.15 and 6.23 are merged, and the step 6 follow-ups too** (`DeclareBuffer`, ImageSpace on `EnabledSSR`,
    `ShadowCascades`).
    - **7b moved nothing:** A7 had already made the own epochs into UI-stage steps.
    - **D3D11 frames to confirm:** HDR composes on D3D11 between a new registration and its adoption. Screenshot keeps a
      per-frame check of its key. Both are like the post chain's remaining D3D11 frames (item 43) and are to confirm.
    - **Last section 8 hits in its files:** the capture client in `ORGScreenshot.cpp` (`Scheduled`, plus a mid-frame
      re-registration in `SetSource`).
    - **Still left:** 7a (the deletions) and 7d (the check made fatal), then steps 8 to 10.
47. **Batch 3 result:** 24 of 28 runs passed. The four that did not:
    - `guest+llf-local-shadows`: guest without DCLF, now fatal by design (item 45);
    - `ibl` in two modes: the recipe finding, fixed;
    - `post-effects` in three modes: the configs' `no_log` "missed settings writer" matched the startup line, so the
      pattern is now "missed settings writer:". All 13 shots were taken.

    `native-vulkan+terrain-shadows` now passes with the wading bound.
48. **Merged into batch 4:** 6.10/6.11, 6.19, 6.20, 6.22, 7a, 7b with 6.15 and 6.23, 7c, the step 6 follow-ups, the
    graph modes requiring DCLF, the IBL recipe and published-snapshot fix, and the Effects11 fix (programs lane, fatal
    errors). The director renamed two shadowing locals in Effects11.
    - **Effects11 can't be tested on this machine:** no Effects11 preset (`enbseries/enbeffect.fx`) is installed. The user
      should run `effects11` and `effects11-programs` with a preset.
    - **7a kept, with owners:** the one-shot passes (5 users), the settings shims for fields that aren't `Setting<T>`
      (strings and a few toggles), and two `FrameAnchors::Refresh` calls (ImGuiRenderer for step 9b, and a DCLF stress
      command).
49. **7d is merged.** Late versions and late attaches are now fatal (`TapError::OutsideAdoption`), with two named,
    counted exceptions:
    - `Dclf`, until step 10a;
    - `EngineImports`, until step 9.

    In 6b3 and 6b4 the only late versions were guest's VL accumulation pair, which comes in through engine imports.
50. **The `ibl` config still stopped in 6b4.** The recipe check stops at its first finding. The second was IBL's env SH
    (pre-main) reading `DynamicCubemaps::Env`, which is written in deferred+water. The director added it to
    `kPreviousFrame`: it is Persistent, and D3D11 also reads last frame's.
51. **Step 8 is merged.** Render-size viewports live in the latch, drawn by `FullscreenLatchViewportVS`. `post.dynres` is
    gone. Declaring a texture or buffer on a constants-only variable is a fatal declaration error. Open: Screen Space
    Shadows' program and the Effects11 rays' areas still depend on the render size. `dynres` shows whether a quality
    change re-registers them. The VRAM measurement procedure is in the plan (`dynres-vram`, at 4K).
52. **Steps 9a and 9b run in parallel** with separate leads, as the plan allows. 9a resolves Q4 from the code before it
    writes any. Step 10 is serial afterwards: 10a, 10b and 10c, one lead each, with full runs between them.
53. **9a is merged.** Q4's answer: the engine reads its new targets at once (`ReInit`, `State::Setup`), so `EndSet` is
    not made a boundary. A second `Create` while the graph runs is now an explicit fatal, per the user's 2026-11-18
    decision.
    - **Visible effect:** dragging the window frame in windowed mode stops the game. The user should know this.
    - **Native HDR:** a colour-space-only switch now happens at adoption. With frame generation on, the old wait stays.
    - **Boundary exceptions:** `EngineImports` is narrowed to `JoinSlot` (9b).
54. **9b is merged.**
    - **Import changes:** an import change outside the adoption point is now `TapError::OutsideAdoption`, naming the file
      and line. Inside the adoption point it becomes a pending version.
    - **Removed:** the version-lag branch and the `EngineImports` exception. Only `Dclf` is left, until 10a.
    - **Not audited:** DCLF's `ShareImage`, Scaleform's passes, the skeleton's kMAIN copy and Cloud Shadows. A mid-frame
      import change there would now stop the game.
    - **Merges:** 9a and 9b were merged in that order; in BoundaryCheck the director kept 9b's deletions.
55. **Batch 4: 38 of 44 passed.** The failures:
    - `ibl` in three modes: the second recipe finding, fixed (item 50);
    - `post-effects` in three modes: turning Local Exposure off made a declared transient (aliasable) texture take a
      1x1 extent, which can't be versioned. Since 7a that is fatal ("has no version ... changes its shape class"; the
      message misleads). Fix: `DeclareTexture` promotes a non-exported transient to Persistent.

    Everything else passed in batch 4: DCLF family, Bink, shader reload, Physical Sky, clouds, dynamic cubemaps,
    volumetric shadows, SSS, cloud shadows, post-lens, post-programs and the 7a full set.
56. **Batch 5** is 7d, 8, 9a, 9b and the fixes, built after one shadowing rename in `EngineTargets.cpp`.
57. **10a stopped at correction 5** (docs only, merged). The lead found that targets, shadow format and main resources
    can each still change outside a revision, plus four more blockers (see "As found" under step 10). The director read
    the 44 batch-4 run logs:
    - **No changes in practice:** zero "Main pass targets changed" lines, and one "Main-pass graph resources" line per
      session, so no main-resource remake.
    - **The waits:** every graph run has 2–6 "[DCLF] frame N waited" lines, of 42–67 ms each. All are "a snapshot current
      for the graph as built (build generation N)", after DCLF's own rebuilds. That is the stall 10a removes.

    **The director's calls for 10a-0, to confirm:**
    - A target change or main-resource remake after DCLF first draws becomes a tap error. The plan's proposal to
      register CS features' t16+ buffers is not built, since nothing changes.
    - The shadow format comes from the engine's shadow and occlusion targets when they are made. If that can't be done
      cleanly, the format switch becomes a counted, named exception.
    - **Shader reload while DCLF draws keeps a named, counted render-thread wait** (development only), instead of the
      lead's proposed fatal, until recordings name their pipelines (item 42). Shader reload is a development tool, so a
      wait there is better than losing it.
58. **10a-0 and 10a are merged (`2d44ec319`), not built yet.**
    - **New tap error:** `TapError::DclfInputChanged` for target or main-resource changes after DCLF draws. The shadow
      format is latched from the engine's targets, so no exception was needed.
    - **Staged epochs:** DCLF's epochs are staged, `AdoptSnapshotAtBegin` runs at every render-owner Begin, and the boundary
      exceptions are gone.
    - **Shader reload:** keeps a named wait (`[DCLF] shader-reload wait`).
    - **Open:** nothing flags a DCLF epoch that keeps re-staging and never switches in. A `Starved`-like tap error is
      wanted.
    - **10b:** written code-only in parallel, merged only after 10a's full runs pass.
59. **Batch 5 passed 38 of 38** (7d, 8, 9a, 9b and the fixes), including `ibl` and `post-effects` in three modes each,
    and `dynres` in three modes. The `hdr-2`/`sdr-2` shots, two frames after each switch, are not black in any of the
    four modes (mean luma 109 to 174), so 9a's black-frame acceptance holds. `dynres` showed no Screen Space Shadows
    program build and no Effects11 rays re-registration on a quality change.
60. **10a builds clean.** Its full set (the step 7 list plus `river`, `rain`, `flicker-taa`, `shader-reload`,
    `dclf-toggles`, `native-census` and `native-tour`) runs as batch 6.
61. **10b is written** (`org-native/rev-step10b`, two commits). It merges after 10a's batch 6 passes. Risks to watch in
    its tests:
    - a shader reload while a revision with DCLF epochs prepares can't finish its wait;
    - a revision waiting on DCLF's scene has no starvation rule (a menu with no scene pass would wait forever);
    - the new "binds a version the frame does not read" warning may fire on every scene if late-bound slots are
      listed.

    If any of these shows, a fix lead goes before 10c.
62. **10a's batch 6: 21 of 23 passed.**
    - **Waits:** every run had zero "[DCLF] frame N waited" lines (batch 4 had 2–6 of 42–67 ms per run). The one
      "shader-reload wait" was in the shader-reload run. No tap errors, and no refused staged builds.
    - **Coverage:** `native-census`, `native-tour`, `river`, `rain` and `flicker-taa` all passed.
    - **Failures:** the new `dclf-toggles` config hung in both modes at `dclf:pipeline gpu`, after the CPU pipeline. The
      next frame's adopted snapshot left the forward and first-person epochs uncovered, which is fatal by DCLF's
      coverage rule.

    A pipeline toggle switching mid-session is 10c's job (toggles as variables), so it goes into the 10c brief. It is
    unknown whether this failed before 10a; the config is new.
63. **10b (with its fix) passed its real checks**:
    - zero DCLF waits and zero tap errors in the 21 runs read;
    - `menu-resize` passed, which shows the starvation rule;
    - "declared view mismatch" *warnings* are zero.

    `resize` failed in three modes only because its `no_log` matched the stats line ("0 mismatched"); the pattern is now
    "declared view mismatch:". Those three are rerun with 10c's batch.
64. **10c is merged**: DCLF toggles and graph conditions are variables. The `dclf-toggles` hang was the pipeline fields
    coming from the attached (old) conditions while recordings used the new build state; they now come from
    `next.build->conditions`.

    **Late changes stay counted, not fatal (to confirm).** Most are not DCLF's: one-shot passes and the re-registering
    of `cs.scaleform`, `cs.screenshot` and `cs.deferred`, plus 16 unnamed ones in guest. A follow-up lead moves each
    source into the adoption point and then makes `NoteLateChange` fatal. That is the last item of the frame revision
    plan.
65. **10c passed 24 of 24** (batch 8):
    - `dclf-toggles` in all three modes, including the CPU→GPU pipeline switch that hung under 10a;
    - `resize` in three modes, with the corrected `no_log`;
    - the step 7 set, plus `river`, `rain`, `flicker-taa`, `native-census` and `native-tour`.

    Every run had zero DCLF waits and zero tap errors. Steps 1–10 of the frame revision plan are merged and tested. Only
    the late-changes follow-up (making in-frame build requests fatal) is still in flight.
66. **Late changes are merged and build clean** (`f207e06e9`). In-frame registrations and build requests outside the
    adoption point are now `TapError::LateChange`, keyed by file and line.
    - **One-shot passes:** the `once`/`armed` machinery is deleted. Its users are first-run gates now
      (`RenderGraph::FirstRun`).
    - **Declared clients:** Scaleform, the screenshot capture, the deferred composite, FG inputs and the glints fill
      register as declared clients.
    - **Remaining per-frame D3D11 frames, to confirm:** skeleton copies fall back to D3D11 for a frame whose textures
      changed inside it.
    - **Failure paths now fatal:** paths that unregister mid-frame (Bink, ImGui, Upscaling release, Dynamic Cubemaps'
      give-up) are now tap errors too.
    - **Batch 9** runs the full set. Any guest source the census never named will stop with its file:line.
67. **Late changes passed a targeted set**, cut down from the full 25 at the user's request (2026-11-20):
    - `guest-quick`, `native-quick`, `menu` and `guest+fg`;
    - `terrain-shadows`, `dynamic-cubemaps`, `screenshot-clean` and `shader-reload` in native Vulkan;
    - `guest+dclf-toggles` and `latch-census-short`.

    All 10 passed, with `late changes 0` and no tap errors. The other runs cover paths this change doesn't touch, and
    they passed on the 10c build.

    **Open:** the glints noise logged "filled" once in the shader-reload run, where the lead expected a refill after the
    reload. Either the refill doesn't run or it doesn't log. Check it in glints' `ClearShaderCache` path.

    **The frame revision plan (steps 1–10 plus the late-changes follow-up) is merged and tested on integ/hard-ports.**
    Nothing is pushed. Local-only submodule commits: ORG `bfd13bb`, OMS `d48c924`, and those in item 13.
68. **Cleanup and refactor plan: the user's answers (2026-11-20).** The plan is
    `org-cleanup-refactor-plan.md`, with its survey notes and director helpers in `org-cleanup/`.
    - **D-1:** Light Limit Fix local shadows are reworked so the cache follows DCLF's shadow views. They are
      not deleted.
    - **D-2:** the guest present copy (the back buffer becomes a CS slot) is accepted. Why the copy is
      needed, and whether it can be removed, is an investigation item in the plan's A1.
    - **D-3:** graph mode may require DCLF.
      - **Decided by the director, to confirm:** the DCLF on/off control is locked in graph mode, and a
        saved change applies at the next launch, so the blank world can't be reached.
    - **D-4:** `CS_ORG_LOADING_THREAD` is retired. Adoption runs on the main render thread only.
    - **D-5:** tap errors are fatal in development builds only. In release builds the first of each kind
      is logged in full and raises a warning in the DCLF menu, which tells the player to send their logs
      to the developers. The game keeps running.
69. **Cleanup phase 0 (DCLF lane): the `CS_DCLF_SET_PARITY` switch stays (to confirm).** Only the 17 TEMP records,
    mirrors and shader writer records are deleted. The switch still gates about 15 other parity checks that
    `rx6600-parity` and dclf-validation.md use.
70. **D-4 as built (to confirm): the switch is retired, but adoption follows the thread that owns rendering.**
    `CS_ORG_LOADING_THREAD=1` was the rule that lets a loading screen's or the startup movie's thread own rendering;
    it was already the default and its switch entry was gone. lead-vars made revisions adopt on the main thread only.
    That broke `guest-quick`: the startup sequence (frames 0-166) runs on the Bink thread, rt1 is made again there and
    the skeleton's UI places register again, and the first main menu frame ran without them (a fatal "no open stage
    has a place" tap error). Preparing on the loading thread and adopting on the main thread was tried; it adopted a
    preparation started before the registration, so it still failed. So the thread that owns rendering adopts, as
    before; DCLF scenes still adopt on the main thread only, and only the main thread samples `Interior`. The
    switch and its text are gone. Making loads adopt only on the main thread would need the rendering thread to stop
    changing registrations during a load, which is phase 3's target set (targets are no longer remade).
71. **Phase 3 integration: the set declares targets the engine never makes, from a full plan (to confirm).** Deferred
    reuses six slots the PC engine never creates (kINDIRECT to kRAWINDIRECT_PREVIOUS_DOWNSCALED). A plan with kMAIN's
    size, a format and bind flags makes such a slot known, and the set declares it like any target. A plan that only
    changes the engine's shape (HDR's LDR plans) leaves a target the engine never made unknown, as before.
72. **Phase 3 integration: the engine's window views alias kFRAMEBUFFER's field (to confirm).** `RendererWindow`
    keeps the swap chain target's RTV and SRV (`renderView`, `resourceView`) and copies them back into
    kFRAMEBUFFER's field in a movie frame; after `DeclareSet` released the engine's objects they dangled. The set
    now points them at the field's objects with a reference of their own, and `EndSet` puts the set's handles back
    in fields the engine released or replaced at a later set (Deferred's planned targets, kFRAMEBUFFER's swap chain
    views). Every present checks that each colour field still holds its handle (`Targets::CheckFields`, fatal):
    about 100 pointer compares a frame.
73. **Phase 3 integration: CS's own D3D11 present binds unwrap kFRAMEBUFFER (to confirm).** HDR Display's
    present-time `OMSetRenderTargets` of kFRAMEBUFFER (ImGui's target, the overlay, the UI buffer restore) and its
    SDR scene SRV now pass `Tripwire::Unwrap(field)` to DXVK's context. In graph modes ImGui draws on the graph, so
    these binds are leftovers; removing them is a follow-up.
74. **Phase 3 integration: DCLF takes the shadow inputs at data-loaded (to confirm).** Lane D read the Utility
    shader at `PostSetupResources`, but the engine makes it when the game's data loads. The target formats stay at
    setup; `DeclaredTargets::InitShadows` runs from `DrawcallLimitFix::DataLoaded`.
75. **Phase 3 integration: SSGI observes Deferred's forward target (to confirm).** Lane M read
    `forwardRenderTargets[0]` at the key, before any world frame's `StartDeferred` captured it (it read
    kFRAMEBUFFER, which the deferred stage cannot read). It is now an `Observed<uint32_t>`, set by the first world
    frame's Prepass, as Dynamic Cubemaps does. Design 5 row 16's "a constant Deferred sets at setup" would replace
    both.
76. **Phase 3 integration: copies and clears follow each frame's versions (to confirm).** An `AnchorPasses::Copy`
    with no extent copies the smaller of the two current versions' extents, and the skeleton's clears take their
    viewport from the slot's current version. Both read it in `Prepare` and key their recordings on it. So a resize
    needs no new registration (the skeleton's re-registration lagged one frame behind the stage builds, a fatal
    "not declared" tap error), and Upscaling's copies stopped assuming full size (D3D12 refused a 1920x1080 copy of
    the 960x540 underwater mask).
77. **Merge train: LLF's retired cache classes are one texel (to confirm).** A cache class for a slot count that is not the
    revision's is 1x1, since an old registration keeps its class alive until its revision retires. It followed a new
    resolution at full size: 32 slots at 4096 (2 GiB of R32F) could not be made on native Vulkan. The LLF test now restores
    the slot count before the resolution; 32 slots at the engine's resolution is still 2 GiB if a user picks it.
78. **Merge train: withheld engine draws and clears with no pass are logged, not tap errors (to confirm).** Residue made
    both tap errors, but its lead could not run the smoke set. Guest runs still withhold the loading screen's 3D model and
    some world draws (Lighting, Effect, DistantTree, Utility through DrawTriShape), and the water targets kUNDERWATER_MASK
    and kRAW_WATER have no clear pass or recipe place. They stay as before residue (skipped draws, D3D11 clears), logged
    once each. `CS_ORG_ENGINE_DRAW_TAP=1` makes them tap errors; the residue census (a game draw outside the latch) stays a
    tap error unless the switch is 0. The switch stays until those sources have places.
79. **Merge train: a target remade inside a frame lags its clears (to confirm).** The startup sequence makes depth0 again on
    the Bink thread inside frame 0. Its clear is dropped (ReasonLog) until the next frame start registers it again, as the
    residue plan note says; it was a tap error.
80. **Merge train: residue's known change (to confirm).** Guest D3D11 frames that still bind the engine's per-frame buffer
    (b12) read a stale buffer once the 140 pool constant buffers are answered from CS memory, until p6-holds lands.
81. **Merge train: an ImageSpace step the engine turns on after Begin lags (to confirm).** The first world frame sets the
    fog composite's state (ISSAOCompositeFog, form 2) after Begin. Its draw was `NotInDescription`. A step the live scope
    state has on is now lag, and the next Begin reads it; a step neither Begin's nor the live state has on stays fatal.
82. **Merge train: unbacked volumetric lighting volumes keep their handles at a new size (to confirm).** Phase 3's set
    declares the 3D accumulation volumes once; a VL quality change makes them again at another size, which was fatal
    (guest/native-interior). They have no memory and the port draws its own `VolumetricLighting::Volume`, so the handles
    stay (their desc keeps the first size). A backed 3D target at a new size stays fatal.
83. **Merge train: holds' behaviour change (to confirm).** In graph mode the main menu and menu-scene tonemap frames draw no
    CS post chain or sun rays. Holds also makes FX/Sun Rays missing inputs, Cloud Shadows refusals and a world frame without
    the fog's client fatal, and HDR, Terrain Blending, SSS and Exponential Height Fog never run on D3D11.
84. **Merge train: the post chain's graph key sets its effects up (to confirm).** An effect's `SetupResources` ran at its
    first D3D11 draw, which holds removed, so Color Grading's curve ramp (imported by the key) never existed. The key calls
    `EnsureResources` for each effect in the order; effects still make their D3D11 textures as before.
85. **Merge train: texture readbacks take wider rows (to confirm).** `RunReadback::TexturePass` copied one 256-byte row
    at most; the pitch is now 16 bytes a texel aligned to 256, and Color Grading's curve preview part has room for its 256
    texels. Holds moved the preview's readback onto the graph.
86. **Merge train: HDR's composite key reads kFRAMEBUFFER's slot, not its field handle (to confirm).** Guest mode installs the
    handles after holds' setup evaluation, so the key had no back buffer and every HDR frame was fatal.
87. **Merge train: SSGI off has no client (to confirm).** Birth registered an off client with no passes, which the anchors
    refuse (fatal since holds). An off or input-less key is now no client; the outputs read their zero birth.
88. **Merge train: the LLF graph cache keeps its setup slot count (to confirm).** Birth made a second resource under a
    published id fatal, and a version changes only the extent, so a new slot count cannot switch in. It applies after a
    restart; the menu says so (`local_shadow_slots_restart`). Resolution changes still switch in by version. Making the slot
    count a version needs array-size versions in ORG.
89. **Merge train: submodules on combined branches (to confirm).** ORG `cleanup/integ` (`73b7d81`: birth `74950be` + pipeline
    versions `1ffbf46`) and OMS `cleanup/integ` (`5b2d206`: birth `63dfa3a` + `d499195`) merged cleanly; the superproject
    pins them. Not pushed.
90. **Merge train: coverage's routine refusals are logged, not tap errors (to confirm; part reverted).** Coverage made every
    per-draw refusal `TapError::Refused`. Textures still importing or streaming their window in ("a later batch takes it")
    are lag. The rest (the sun query's depth target that is neither kMAIN's nor the Z-prepass copy, menu views) have no
    classification rule yet, so they log as before unless `CS_ORG_ENGINE_DRAW_TAP=1`. Open: a classification-time rule
    for each.
91. **Merge train: a reload fragment skips epochs with no graph (to confirm).** The fragment lists every DCLF epoch; one
    without a graph has no pin, so the shader-reload revision never adopted. `FragmentPinned` skips them as `EveryEpochAt`
    does. The waits now log their reason once.
92. **Merge train: coverage's `FragmentSource::left` stays (to confirm).** The brief preferred never adopting a revision
    without a drawing source's fragment. That would block every DCLF-member revision (a menu resize, an HDR switch) once
    the player is back at the main menu, where DCLF has drawn but posts no fragment. The conflict with residue is gone with
    decision 78 (withheld engine draws log). No test run stopped DCLF (`left` 0 times in latch-census-short, map, toggles,
    reload-resize).
93. **Merge train: p3-resize's native engine size is reverted (to confirm; part reverted).** With `42fbd617c` native display
    resize really resized, and the skeleton's kMAIN copy, recorded on the lane for the resize revision, read the adopted
    (old) extent through `FrameAnchors::Texture::Width()` and copied 1920x1080 into 1280x720 versions: native D3D12 refused
    the list, and Vulkan ran it out of bounds. `bf3e87dcb` reverts it; native window-resize passes again without resizing
    the engine. Needed first: decision 76's copies (and any pass that reads an engine slot's extent in Prepare) must take
    the extent of the revision being prepared. DCLF's main resources following DisplaySize (`76790607c`) stay merged.
94. **Merge train: guest window size changes still stop (open).** With p3-resize's DCLF part, guest window-resize now stops
    on a skeleton "host skipped" tap error: in the resize frame DCLF ran its main opaque epoch without its Z-prepass, where
    the main depth clear is hosted. The resize lead's "main resources ... adopted with a scene made for them" handover and
    the skeleton's host rule need to agree on transition frames. The foliage record is a diagnostic switch, off by default
    (`CS_DCLF_FOLIAGE_PARITY`), so its buffer is never written in normal runs.
95. **Phase 7: guest window size changes work (decision 94 closed).** DCLF's Z-prepass compared kMAIN's depth with the
    object the previous frame's main pass bound. In guest mode that is DXVK's object, retargeted at adoption, so the
    resize frame skipped the Z-prepass that hosts the depth clear. The main pass now records whether it binds kMAIN
    (`mainPassBindsMainDepth`), and the Z-prepass reads that (`423e04bc1`). Guest window-resize, hdr-resize,
    window-drag, resize-drag, dclf-toggles and native-vulkan window-resize pass. Open: a main pass that really binds
    another depth still skips the Z-prepass with a warning, and the skeleton names the wrong cause.
96. **Phase 7: native resize feeds the engine size again (decision 93 closed, to confirm).** Passes prepared on a lane
    take their size from the version the revision binds (`FrameAnchors::Texture::PreparedSize`; a slot with no version
    is fatal). Copies with size 0 and draws with a zero viewport mean "the smaller bound version" and "the whole bound
    target". The extent left the copies' and clears' revision hashes, since kept recordings already check versions.
    `42fbd617c` is back (`58f579e19`). ImageSpace's copy back and frozen-background draw also needed it (D3D12 refused a
    1920x1080 copy into 1280x720). Native vulkan/d3d12 window-resize, resize, hdr-resize, vulkan window-drag and
    resize-drag, guest resize, window-resize, hdr-resize and mode quick runs pass. Open: `RegisterCopy`'s size check
    still compares adopted sizes; ImageSpace's chain is built for the old render size in the resize revision and
    follows one revision later.
97. **Library extraction plan written** (`docs/development/plans/org-library-extraction-plan.md`). Phases: small
    async containers (OMS), services with clean cuts, the pipeline-stack decision then programs and GPU primitives,
    build and upload services, and coordinators only once a second user exists.
98. **Simplification before extraction (user, 2026-11-20).** `org-simplification-plan.md` runs before the extraction
    plan. User answers:
    - Native Vulkan, native D3D12 and guest stay; plain D3D11 goes at RC; the no-ORG build goes now.
    - Both DCLF pipelines stay.
    - `cleanup/integ` libraries are CS-only (BR/SARP re-adopt later); library APIs may change freely.
    - Release stays soft.
    - Diagnostics are cut aggressively, but capture parity, the compatibility monitor, the diagnostic capture and the
      telemetry tallies stay.
    - DCLF class toggles go; engine-only casters go (logged refusal).
    - Absent-pass build keys stay.
    - The guest context becomes like native (last phase).
    - The DXVK guest-device exports go; the DCLF Api boundary stays.
    - LoadHold re-entry is verified, then cut.

## Simplification plan

99. **Simplification: capture parity still compares in graph mode.** guest-quick with `CS_DCLF_CAPTURE_PARITY=1` and
    `CS_DCLF_PARITY_BOTH=1` at `7be943267`: 31,369 engine main-pass lighting draws, 31,348 checked against the tables,
    0 mismatched; face capture parity 44,659 checked, 0 differ. `PARITY_BOTH` lets the engine draws through, so capture
    parity needs no new engine-state capture. Its sub-parities for draws, light data, permutations and skin checked 0
    draws on that run.
100. **Simplification 4.3: refused casters' draws are skipped today.** `CS_DCLF_TEST_REFUSE=Tree,Rock` with the residue
    census: refused geometry "stays the engine's ... the engine culls its root for shadows", and the flush latch logs
    each such engine draw as unvouched and skipped. The engine-only caster subset only costs an engine cull.
101. **Simplification 4.9: LoadHold re-entry fires only for the map's camera cut (to confirm).** latch-census-short
    (save, load, `coc`, map, menus): re-entry fired on map open and on map close; the shadow and occlusion re-entries
    (graph rebuild, shadow epoch not recorded, occlusion map not drawable) never fired. So the map re-entry stays; the
    shadow and occlusion re-entries become tap errors and the per-frame `ImportShadowDepth` goes.
102. **Simplification 3.10: no engine draw reads an export.** latch-census-short's residue census: all 259 engine draw
    sites, menus, map and loading screens included, are skipped by the latch. Exports stay only for CS's own D3D11
    readers.
103. **Simplification 5.6: native no longer reaches End/GetData or engine pool constant buffers.** native-census's op
    census: by-kind lists only Map write (discard), Unmap upload, UpdateSubresource, CopyResource and
    ClearRenderTargetView; every mapped buffer is CS-owned (State, TreeWind, GrassCollision, LLF, Deferred,
    VolumetricLighting). Both shrink to stubs. native-tour also logged a failed `Dispatch` on the native immediate
    context from CS code (`CommunityShaders.dll+0x2fbe738`); open.
104. **Simplification 6.11 is checked in phase 6.** The guest A/B for the native-only host options needs a temporary
    patch, since `RenderGraphRuntime.cpp` gates them on `state->native`. Phase 1 fixes the CS switches as constants
    for native only.
105. **Simplification phase 1: a missing ID3D11DeviceContext1 stays a warning.** p1-ports made it fatal as "impossible";
    native's context has none, so every native run stopped at setup. Reverted (`aa85119d8`).
106. **Simplification phase 0: a buffer's first watched frame is not a draw-input defect (to confirm).** DCLF's mirror
    sees only writes after `Watch`, so the first frame it watches a frame buffer reads empty; p0's DrawInputs tap error
    stopped every run there (t98). `Watch` returns true on that frame and Uploads.cpp skips the error. Deferred also
    writes t98 every frame from the last sun record, so a frame without a sun shadow light still fills it.
107. **Simplification phase 2: outside-ledgers work and undeclared access are logged, not tap errors (to confirm).**
    p2-org-diag made both fatal in dev. Native stops at once: loading-screen and menu uploads (`UpdateSubresource` on
    `Native::Buffer`), CloudShadows layer and engine target clears run outside the ledgers, and Scaleform's menu draw
    reads a GFx texture written since creation (`Passes.cpp:692`); TextureFeedback reads a buffer by handle
    (`TextureFeedback.cpp:55`). Each is now logged once per operation (or site) and resource, with no switch. Making
    them tap errors needs those producers on the ledgers first.
108. **Simplification phase 5: native query End/GetData are silent stubs (to confirm).** p5-modes made End a tap
    error; CS's own D3D11 timers end queries at setup (frame 0, main menu), which decision 103's census did not see.
    End does nothing; GetData answers at once (zeros, disjoint timestamps, events done).
109. **Simplification: the incremental Dev-Fast PDB reached 6.4 GB and failed with LNK1318.** Deleted with the rotated
    `old.*.pdb` copies. If it recurs, delete `build/Dev-Fast/CommunityShaders.pdb` and `.ilk` before linking.
110. **Simplification: ImageSpace draws before their group registers, or with no held build, are lag (to confirm).** After
    p3-declare2 a key first met at setup registers a revision later. The first loading screen's menu-scene HDR and the
    post chain's luminance steps then hit "no CS pass" tap errors. `MenuStepNow` no longer needs a registered group;
    `TakeMenuStep` and the chain's luminance steps note lag when the group is unregistered or nothing is held.
111. **Simplification 4.9: the occlusion re-entry stays (decision 101 corrected).** p4-paths made an undrawable
    occlusion map a tap error; rain starts the precipitation occlusion map with a new layout, which DCLF cannot draw
    for a few frames, and the run stopped. The re-entry with its streak limit is restored (`3ad529e74`). The shadow
    re-entries stay tap errors.
112. **Simplification: post-effects needed the profile's Post Processing mode.** The configs enabled the feature but
    the saved mode was Vanilla, so the chain was bypassed. Five PP configs now set `"Post Processing Mode"` through
    `saved_settings`; PostProcessingMode logs the chosen mode once.
113. **Simplification 6.11: guest runs with every native-only ORG host option.** A variant with `reuseLiveRecordings`,
    `coalesceSubmissions`, `mergeHandoffs`, `uploadsWithBatches` and `keepGatedInline` on in guest passed guest-quick,
    guest-interior, guest+fg, guest+dclf-toggles, guest+resize, latch-census-short and guest+shader-reload; perf-quick
    51.6 fps against 52.1 without (Dev-Fast /Od, one sample each). CS sets all five in every mode. ORG dropping their
    off paths (the `GraphHost::Desc` flags) moves to the library extraction plan.
114. **`integ/hard-ports` squash-merged into `org_integration` (user, 2026-11-20).** One commit holds the hard ports,
    the cleanup plan and the simplification plan. `integ/hard-ports` stays as the detailed history; the hashes in this
    file and the director log point into it. New work, starting with the library extraction plan, branches from
    `org_integration`. Nothing is pushed.
