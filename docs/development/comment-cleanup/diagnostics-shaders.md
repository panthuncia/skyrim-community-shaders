# Comment cleanup notes: diagnostics-shaders

Slice: every `.cpp`, `.h` and `.inl` under `src/Features/DrawcallLimitFix/Diagnostics/`, and every `.hlsl` and `.hlsli` under
`features/Drawcall Limit Fix/` outside `Common/`. Comments only. Each touched file was checked mechanically: with comments and
whitespace removed it matches HEAD (134 files, 0 differences).

## 1. Simplification or documentation targets

- **Diagnostics/Engine/RegistrationCensus.cpp, classification of untracked geometry (the "live approximation").** The census
  rebuilds the coordinator's Classify walk on the render thread: the static verdict, then the parents, the ordered-node rule, the
  billboard rule and the forward-epoch exception. The comments were the only description of this mirror, and the two can drift
  apart. Suggestion: expose one classifier that both the coordinator and the census call, so the census holds no copy of the rules.
- **RegistrationCensus.h / .cpp and Scene/Census.h, SceneCensus.cpp: the async question-and-answer.** Collect pins geometries and
  posts them to the next scene pass. The pass answers and sends the pins back so they are released on the render thread. Replies
  age out after kReplyCollects, and a request that waits a whole window is dropped. The 17-line doxygen in RegistrationCensus.h
  was the only explanation. Suggestion: run the census only with the scene pass inline, which removes the protocol. Otherwise
  describe it in dclf-validation.md, "The registration census".
- **Diagnostics/Hooks.h, the Notify, Subscribe and Policy split.** A 16-line block was the one statement of the rules: the main path
  reaches diagnostics only through Observe(), Policy and Notify. The registry is frozen after Install, so Notify takes no lock.
  Observers must not change the main path. I kept a shorter top comment, because nothing else enforces these rules. Suggestion:
  make them a short section in the DCLF architecture doc, and enforce "observers must not change the main path" in code where
  possible, for example with const event payloads.
- **Draws/SetParity.cpp, SetClassParity.cpp and BuildParity.cpp, together with BuildDrawsCS.hlsl's visibility word and state
  classes.** Understanding these checks needs the full GPU visibility model: stamp and verdict bits, the frustum stamp with its
  fade bit in bit 31, provisional against final rejections, class sub-ranges with zero-count decal fillers, and plain buckets. The
  long comments explained that model again at every site. Suggestion: describe the visibility word and the class and bucket layout
  once in dclf-architecture.md, "BuildDraws", and cut the parity code down to references to it.
- **Engine/Accumulated.h and VisibilityEventsCS.hlsl: what kAccumulated means.** Who reads the engine's flag bit 26, what DCLF writes
  instead, and why HZB occlusion is left out of it, was explained only in these comments. The engine readers are animation updates,
  dynamic light receivers and the tree clock. Suggestion: keep it in dclf-architecture.md, "What kAccumulated means", which the old
  comments cited, and check that the doc holds everything that was cut.
- **Features/Fades/FadeStateCS.hlsl and TreeWindCS.hlsl: ports that must match CPU code instruction for instruction.** These are
  FadeState.cpp, MergeFadeSeed and MergeTreeSeed, each kept in step by hand, with "keep the two the same" in the comments.
  Suggestion: share one header between HLSL and C++, or add a parity test, so the rule does not rest on a comment.
- **FadeStateCS.hlsl FrustumInView: the actor pre-cull interaction.** The engine's BSFadeNodeCuller sets bit 12 and kNotVisible on
  actor roots. The flags in the seed are stale, so DCLF makes the pre-cull's frustum test itself. This is surprising and was
  explained only here. Original: "A preprocessed node (an actor's root: the actor pre-cull, BSFadeNodeCuller, sets bit 12 and, off
  screen, kNotVisible every frame in Main::Draw's camera setup) passes in the engine unless kNotVisible, whatever its room; the
  seed's copy of bit 20 is the listing frame's, so DCLF makes the pre-cull's test itself: the main camera's frustum on the root's
  bound." Suggestion: document it in the engine notes.
- **ForwardSortCS.hlsl: the engine's blended draw order.** The order is set by FUN_1414b39d0, FUN_1414e8fc0, the qsort at
  0x1414f55e0 and FUN_1414f5330: ordered-node groups are merged into the blended list. The comment was the only record. I kept a
  shorter version with the addresses. Suggestion: copy it into skyrim-engine-notes.md.
- **Diagnostics/Scene/MirrorWatch.h and .cpp: the CS_DCLF_MIRROR_WATCH values.** The list of watch modes and engine offsets lived
  only in doxygen. I kept the offsets. Suggestion: one table of modes in dclf-validation.md.

## 2. Hazards kept

- PortalPrograms.hlsli: on AMD (RX 6600, driver 2.0.353) the short-circuit interpreter culled about 900 visible objects a frame, so
  AMD uses the uniform scan.
- PortalPrograms.hlsli: `a_program == kPortalNoProgram || a_program >= count` was miscompiled on the RX 6600 (driver 26.10.1). The
  code uses one unsigned compare instead.
- PortalPrograms.hlsli, PortalEvaluators.hlsli: the loop bound is written with min, and op ranges are tested with range compares,
  never equality guards.
- HzbTest.hlsli: `min(Index, Mips) < 1u` instead of an `||` of equality tests. Otherwise the taps read heap slot 0.
- TextureTransformsCS.hlsl: the null-ref guard is written without an early return, after the `||` form read heap slot 0 and caused
  a device fault. `k >= animated` replaces an equality `||` that stored entries about 4 TB out of range.
- FadeStateCS.hlsl: `min(EventsIndex, ReportedIndex)` (the RX 6600 dropped every event behind the `||` form), and the
  `min(Frame ^ SceneFrame, abs(LodAdjust))` single compare.
- BuildDrawsCS.hlsl LodPartitions: one compare for every null guard. FoliageParityCS.hlsl: kVanished and kAppeared tested as one compare.
- PixelRowsCS.hlsl: three float4 stores instead of a struct store, because AMD's Polaris driver 2.0.279 crashes compiling the struct store.
- BuildDrawsCS.hlsl, ForwardSortCS.hlsl, GrassArgsCS.hlsl: D3D12's SV_InstanceID does not count the first instance, hence the extra push word.
- BuildDrawsCS.hlsl, VisibilityEventsCS.hlsl: on D3D12, GetDimensions on a structured view gives elements, not bytes.
- CullEye.hlsli, BuildDrawsCS.hlsl: project eye-relative in one precise expression, so large terms do not cancel in float.
- HzbTest.hlsli: the depth margin, because the native pass and DCLF use different viewport depth ranges.
- HzbCS.hlsl: raw NDC with MAX reduction, padding loads as 1.0 and out-of-domain loads as 0.0. Above 4096 texels the chain splits.
- Engine/RegistrationCensus.cpp: 0x14338c820 is the first-person camera, not an accumulator. Reading it as one crashed at the main menu.
- Engine/ReleaseGuard.h / .cpp: engine destructors belong on engine threads, because a node removes its collision object from
  Havok. DeleteThis is the same 16 bytes on SE and AE.
- Scene/MirrorWatch.cpp: a watchpoint the disarm missed must be cleared in the handler, or the game crashes.
- Scene/CaptureParity.cpp / .h: DXVK keeps dynamic buffers mapped, so a late VPOSOffset write after Unmap is seen by the draw.
- Scene/CaptureParity.cpp: TreeWind's WindTimers.y depends on native draw order (SetupGeometry AE 1414dd040, case 0xc).
  FUN_1414e00c0 writes only ProjectedUVParams2.xy. Subsurface Scattering leaves IsBeastRace set from the last face.
- Draws/ShadowChecks.cpp: the renderer's viewport depth range is per draw. Camera state is at 0x14202b130 and the bias step at 0x143286a80.
- Scene/ScenePrecisionCensus.cpp: geometries and bones are unpinned, so reads are fault-guarded.
- Draws/FrameValuesParity.cpp: never wait for the producer in the shading check, since waiting holds every lease.
- Draws/CullCounters.cpp: the readback stays on the graphics queue, because a copy-queue wait would split DXVK's command list.
- Feature/TargetDump.cpp: engine target fields can be tripwire handles, so they are unwrapped before copying.
- Zones.h: a BasicTelemetry scope is not opened outside a phase, because its constructor takes a lock.
- Reports/Schedule.h: kQuietInterval exists because always-on lines would otherwise grow a player's log by about 100 MB an hour.

## 3. Possible bugs or dead code noticed

- Diagnostics/TestHarness.cpp, before HarnessState: a garbled comment held a stray `/**` followed by the commented-out text
  `namespace DCLF { namespace {`, which looks like a bad merge. It was comment text only, so I replaced it with the brief it
  wrapped. Worth checking that the namespaces in that file are what was meant.
- Scene/CaptureParity.h: a comment about the bone palettes at VS b9 and b10 sat above `treeSamples`, which it does not describe.
  The fields it described may have moved or been deleted. I removed the comment.
- PlayerParity.cpp DrawPanel: uses `ImGui::Button("Clear")`, but the project convention is `Util::Button`.
- Several `TEMP` diagnostics remain: DrawsState.h SetParityFrame (payloadTables, listedBy, inputsStaging, writersStaging), SetParity.cpp,
  and BuildDrawsCS.hlsl RecordWriter. The TEMP marker is kept. Remove them once the set parity investigation is closed.
- BuildDrawsCS.hlsl: the unbucketed fallback draw path ("Any other unbucketed draw") has no callers today, per its own comment,
  so it may be dead.
- Policy.h: CheckAncestorKeys has no "normal path" answer in its brief, unlike every other question.

## 4. TODO/FIXME lines found

None in the slice. The TODO lines in `features/Drawcall Limit Fix/Shaders/DrawcallLimitFix/FidelityFX/ffx_a.h` are in AMD's vendored
`.h` file, which is not one of the slice's file types. I left it untouched.

## 5. Coverage

All files in the slice are done: 113 C++ files and 21 of 24 shaders were changed. The other 3 shaders had nothing to cut
(ParabolicPrograms.hlsli, PortalContexts.hlsli, PortalQueries.hlsli).
Lines containing a comment, by script count:
- C++: 2393 before, about 1737 after.
- Shaders: 1064 before, 753 after.
- Total: 3457 before, about 2490 after.

The git diff for the slice is 3003 lines deleted and 2036 inserted. Multi-line blocks were cut hardest. Many one-line comments were
reworded rather than deleted, so the drop in line count understates how much shorter the comments got.
