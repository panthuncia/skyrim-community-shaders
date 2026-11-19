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
