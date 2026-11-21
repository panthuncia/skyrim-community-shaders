# Cleanup plan: director log

Progress log for `org-cleanup-refactor-plan.md`. Decisions that need the user's review go in
`director-decisions-2026-11.md` (items 69 on); this file records what ran and when.

## 2026-11-20

- Removed the merged, clean agent worktree `.claude/worktrees/agent-ad7ae842a6310c712`.
- Phase 0 leads started on code-only worktrees under `..\org-port-worktrees\cleanup-*`:
  `p0-core`, `p0-renames`, `p0-ports`, `p0-dclf`. Phase 1 test support (B23) started early as
  `p1-tests`, as the plan's "Where to start" says. A read-only design lead (`p3-design`) writes
  the phase 3 design (guest retarget, native wraps on the lane) and reads the present-copy
  questions.
- Baseline Dev-Fast build of `09b2cd799`: clean.
- Merged `cleanup-p0-dclf` (`71c457535`). Builds clean. Kept the `CS_DCLF_SET_PARITY` switch and
  its non-TEMP checks; deleted only the TEMP records (decision 69). Tests deferred to the
  phase 0/1 batch.
- Started `p3-design` (phase 3 design note, read-only).
- Merged and tested so far: phase 0 (core text, A11, flush dead code, drag fix; DCLF dead code and
  on/off lock; tests B23), phase 1 (B7 port skeleton + fatal ports, engine fatal, B13 variables,
  loading thread per decision 70), phase 2 (FrameClock, Builds::Ask, executor B9, BuildState A5.1,
  ProgramService B8 + parallel builds), A5 step 2 (DCLF on the build lane), phase 3 lane T, phase 4
  B14/B19/B22. Fixes found in integration: setup-time census gate, unity-batch renames, mid-world
  Bink movie (fix-bink), program latency on D3D12 (fix-programs; setup batch 2077 -> 602 ms).
- Passed on the merged builds (one run each): guest/native/native-d3d12 quick, tap-warning, menu
  (vulkan, d3d12), dclf-toggles x3, shader-reload, harness-selftest, resize x3, bink (guest,
  vulkan), hdr, gpu-block, fog, dclf-family, rain, river, dynamic-cubemaps, tripwire self-test x3.
- Phase 3 lanes A and P merged (`ec5c25468`+); integration handed to an integration lead
  (it builds and tests; no other agent builds meanwhile). kFRAMEBUFFER was never declared (the
  engine makes it from the swap chain) - fixed; next failure: Deferred clears target 71.
- Phase 3 T+A+P working (integration lead): guest/native/native-d3d12 quick, menu, latch-census-short,
  hdr, guest+fg, guest+bink, guest+resize, native-vulkan+resize pass. Fixes: the set declares
  Deferred's planned slots, window views alias kFRAMEBUFFER, HDR's present binds unwrap, set fields
  refilled after a later set (decisions 71-73).
- Phase 3 lanes D and M merged (`b0d2085c7`, `71649ced9`). Fixes: fog name clash, DCLF shadow inputs
  at data-loaded, SSGI forward target observed, copies and clears follow each version (decisions
  74-76). Rows D and M pass in guest and native-vulkan (27 runs plus the PP batch, native-census).
- Present copy measured (1080p): 0.03 ms guest and native Vulkan, fps within noise; written into A1.
- Open: a guest window size change (window-resize, hdr-resize, window-drag, screenshot-clean's
  resize step) stops on DCLF's main-pass resources, fixed once DCLF drew (phase 5 item); native
  window-resize passes but the engine makes its targets at the old size (no resize happens).
- Merge train (integration lead, one build at a time in the main checkout):
  - `cleanup-p6-llf` (`7d7e6e01d`) + fix `c76bc198e` (decision 77): llf-local-shadows guest/native-vulkan/d3d11,
    guest+upscalers+llf-parity, llf-local-shadows-fog guest/native-vulkan, native-census pass.
  - `cleanup-p6-residue` (`a9b3775f5`) + fixes `82f149969`, `1e721b109`, `e8912d201`, and the no-pass clears (decisions
    78-80): guest/native/native-d3d12 quick, menu, latch-census-short, native-census, bink (guest, vulkan), guest boot,
    guest map, resize (guest, vulkan), guest upscalers, guest cloud-shadows, guest lens pass. hdr-resize left to p3-resize.
  - `cleanup-p4-held` (`12f16047f`; seven port conflicts with lane M resolved by keeping lane M's Observed values and stable
    slots on held's `DeclaredClientOf`/`Held`/`Adopted`) and `cleanup-p4-imagespace` (`65598c7ed`) + fixes `e5e7358b1`,
    the unbacked VL volumes (decisions 81-82): guest/native/native-d3d12 quick, menu, latch-census-short, resize (guest,
    vulkan), guest bink, guest hdr, guest physical-sky, vulkan clouds, guest view-atmosphere, vulkan screenshot-clean,
    guest+fg, native-fg, guest dclf-family, vulkan cloud-shadows, PP batch (guest post-effects, post-programs, vulkan
    post-effects), imagespace-transitions (guest, vulkan), guest map, native-census, guest-river, native-river,
    guest river-sgtm, guest-interior, native-interior pass. effects11 has no preset here.
  - `cleanup-p6-holds` (`8e25341fb`; seven port conflicts with lane M resolved as for held) + fixes `633e30a50` (program
    builds never woke holds' setup join: every mode stopped at startup), `61af09588`, `4f4178075`, `660b02214`, i18n
    regen (decisions 83-86): guest/native/native-d3d12 quick, menu, latch-census-short, PP batch (guest post-effects,
    post-lens, post-programs), guest fog, native-vulkan fog+resize, guest cloud-shadows, hdr in 3 modes, guest
    shadows-blend, guest sss, native-vulkan terrain-shadows pass.
  - `cleanup-p4-birth` (`84884aebc`; 15 conflicted ports resolved onto held/holds/imagespace; superproject pinned to the
    birth submodule tips) + fixes `a060559fa` (SSGI) and `9f0deed41` (LLF) (decisions 87-88): guest/native/native-d3d12
    quick (native: "prepared its version ... filled/zeroed" x48), guest view-atmosphere, skylighting-reset, ssgi, vulkan
    dynamic-cubemaps, guest fog, guest physical-sky, vulkan clouds, guest clouds-off-long, vulkan sky-river-org1, guest
    hdr, resize (guest, vulkan), menu-resize, guest upscalers, guest fg, native-fg, vulkan screenshot-clean, guest sss,
    llf-local-shadows (guest, vulkan), PP (guest post-effects, vulkan post-lens) pass. native-vulkan shader-reload failed
    (Cloud Shadows' stage absent in the reload frame); rechecked after p5-reload.
  - `cleanup-p5-coverage` (with fragments and reload; `f105acb9b`; submodules on the combined `cleanup/integ` branches) +
    fixes `e7c279180` (compile), `eb2f338c9` (refusals), `8bf02968a` (reload) (decisions 89-92): guest/native/native-d3d12
    quick, shader-reload in 3 modes, shader-reload-resize (guest, vulkan), dclf-toggles x3, latch-census-short,
    native-census, guest map, resize (guest, vulkan), guest-river, guest rain, native-flicker-taa, native-tour,
    guest-interior, menu, perf-quick (one sample, 52 fps, Dev-Fast /Od) pass. Birth's shader-reload failure is fixed too.
  - `cleanup-p6-misc` (`fbe57838f`; FrameAnchors' PrimeStage takes misc's ask-only form): guest/native/native-d3d12 quick,
    menu, resize x3, native-vulkan window-resize/window-drag/resize-drag (no host wait), guest+fg, native-fg,
    native-d3d12-fg, guest physical-sky, vulkan clouds, guest clouds-long, latch-census-short pass.
  - `cleanup-p3-resize` (`225d7a5ca`) + `06464ab50` (compile); `42fbd617c` reverted in `bf3e87dcb` (decisions 93-94):
    native-vulkan and native-d3d12 window-resize, guest/native/native-d3d12 quick, guest+dclf-toggles, guest resize pass.
    Guest window-resize fails (host skipped, decision 94); its worktree and branch stay for that work.
- Phase 7 (2026-11-20): `cleanup-p7-guest-resize` (`423e04bc1`) and `cleanup-p7-native-extent` (`38b9d4850`, reapplied
  `42fbd617c` as `58f579e19`) merged, plus the ImageSpace copy fix. Decisions 95-96. Test runs listed there pass.
  Native d3d12 window-resize first hung on a refused ImageSpace copy; fixed in the main checkout.
- Survey and extraction plan: three read-only surveys (DCLF, graph core, shared systems) combined into
  `org-library-extraction-plan.md` (decision 97). Worktrees for phase 7 removed.
- Simplification (2026-11-20):
  - Phase 0 + 1: `simplify-p1-ports` (`926dadd4c`), `simplify-p0-defects` (`ff9adc29d`, + `e825cd83f` luminance ids from the
    held build), `simplify-p1-dclf` (`e07ecdcb6`), `simplify-p1-core`, revert of the Context1 fatal (decision 105), t98 and
    first-watch fixes (decision 106), `simplify-p2-harness`. Passed: guest/native/native-d3d12 quick, menu x2,
    dclf-toggles x3, resize x3, native window-resize, guest+fg, native-fg, latch-census-short, map, menu-resize,
    hdr-resize, bink x2, stats-window, guest-interior, native-interior, d3d11+quick, imagespace-transitions, perf-quick
    (51.6 fps, Dev-Fast /Od).
  - Train 2 (`simplify-train2`, fast-forwarded): p2-org-diag, p5-modes (with p5-dxvk; DXVK rebuilt), p3-anchors,
    p3-resources, p3-declare, p3-declare2; compile fixes; decisions 107-108. Mode smoke x3 pass.
  - Train 3 (`simplify-train3`): p2-dclf-diag, p3-rest, p6-libs; compile and link fixes; ImageSpace lag fixes
    (decision 110); pp-fix (decision 112). 25 of 27 pass; dclf-parity reports two open DCLF defects (sun cascade test
    GPU vs CPU, a missed detach); native-vulkan+resize stalled once and passed on rerun.
  - Train 4 (`simplify-train4`): p4-state, p4-install, p4-paths, p2-registry, p6-pipes, p6-staging (ORG
    `simplify/p6-integ` merges both ORG branches), p7-guest; compile fixes. Switch schema regenerated from the game's
    dump (178 switches). 37 of 39 pass; rain hung on the occlusion re-entry (decision 111, fixed); perf-quick 51.7 fps
    (Dev-Fast /Od, one sample).
  - Cleanups: p1-noorg (no-ORG build), p8-backbuffer, p8-cs, p8-lib (ORG `simplify/p8-lib`, OMS `simplify/p8-lib`).
    Smoke x3 and rain pass on `ae9f9a32a`.
  - Director scripts added: `runset.sh` (runs configs, stops after two failures in a row), `bstage.sh` (build and
    stage only on success), `build-wt.cmd` (Dev-Fast build of a worktree).
  - Final (2026-11-20): p9-final, p9-i18n, the host options in every mode (decision 113), the OMS pin fix. Cleanup round
    25 of 27 (effects11 needs ENB files on this machine; native-d3d12-fg missed live shots once and passed on rerun),
    perf-quick 52.1 fps (Dev-Fast /Od, one sample). Final round: smoke x3, effects11-programs, native-d3d12-fg,
    native-vulkan+dclf-toggles, guest+menu pass. Simplification plan done; worktrees removed. Next:
    `org-library-extraction-plan.md` ("Handoff: start here").
- Squash (2026-11-20): `integ/hard-ports` squash-merged into `org_integration` (decision 114). `mkwt.sh` now bases
  worktrees on `org_integration`.
- Library extraction (2026-11-21, `org-native/extract-integ`, submodules on `extract/integ`): parts p0-lib, p1-async,
  p1-frame, p2-executor, p2-retire, p2-recordings, p2-birth, p2-cpu, p2-reflect, p3-gates, p3-kernels, p3-latched,
  p3-pins, p3-programs, p4-build, p4-uploads, p4-bufsets, p4-misc, p4-imgui, cleanup (decisions 115-124). Compile fixes:
  the starvation segment tag, the cull level name. Checkpoint `ck2` 16 of 17 (native-d3d12+shader-reload's uncovered
  epoch once, two reruns pass); perf-quick unchanged. Library tests 75 of 78 (Leases* older than the extraction).
  Staged as `CS AB extract` because the water session used `nfg`.
