# snap7-thread2: the render thread's open items (phase 7, after snap7-thread)

Lead `thread2`, round `snap7`, branch `snap7/thread2` (base `org_integration` 551b76323), stage `s7-thread2`. Continues
[snap7-thread.md](snap7-thread.md) section 8. Dev-Fast throughout.

Status: items 3 (in part) and 4 landed. Item 1 (resize-stress device loss) is **not fixed**; this file records what was ruled out
and where the evidence points. Items 2, 6 and 7 are recorded, not changed.

## 1. Commits

| Commit | What |
|---|---|
| 040a66efb | fix(native): halves own claims, render slots (items 3 and 4) |

## 2. Item 4: render-serial slots (done)

- `FrameView::RenderSerial` is atomic. It moves once per render (`StepRender`).
- The fade states (`FadeScene::FadeStatesRead`) and the tree wind rows (`TreeWindScene::WindReadBuffer`) take a render serial, not a
  scene step. Readers use the current render's serial and read slot `(serial + 1) & 1`.
- The writers no longer pick their slot from the scene frame's parity. The commit writes the slot into the frame row:
  `FadeFrame::outSlot` (the old `reserved0`, `OutSlot` in `FadeStateCS.hlsl` and `VisibilityEventsCS.hlsl`) and
  `TreeWindFrameRow::windSlot` (the old unused `objectCount`, `WindSlot` in `TreeWindCS.hlsl`).
- So a render always reads what the render before it wrote, whatever steps the game halves took in between.
- The snapshot builder's ahead selects (shadow records) also read the current serial. They were a prediction before too.

## 3. Item 3: state both halves touched

Done:
- **FrameView claims state.** The render side's `StopDrawing` (a revision left DCLF's scene) now only drops the render's claims and
  counts a stop (`stopsAsked`, atomic). The game half applies counted stops at the start of its hand-over (`ApplyAskedStops`: withdraw
  the claims, PassCapture's set, PrimaryCull's notes). Guest, and a render side with no adopted frame, apply at once as before.
- The render side's `NotDrawingYet` reads its adopted `InstalledFrame` (`notDrawing`, `stops`): a stop asked after that frame's game
  half counts at once. `drawingStarted` is atomic; `EverDrew` reads it from any thread.
- **Coverage paired with stops.** `FrameDriver::coveredLast` is now `1 + stopsAsked` of the last covered render (0: not covered). The
  game half reinstalls claims after a stop only when that value equals `1 + stopsApplied`. Before, a render covered just before a stop
  could let the game half reinstall the claims right after it (the two stores in `source.left` were not atomic as a pair).
- **Toggles::InstallFrame.** Two frame words: native's game half writes and reads `gameFrame`, everyone else `frame`. The render side
  sets `frame` from its adopted `InstalledFrame::toggles` (copied from the installed publication at the game half's end).

Benign, left (decided by the agent, to confirm):
- `PassCapture::PublishSet`: its only shared work is the admission statistics, already under `admissionLock`. Now called only by the
  game half in native anyway.
- `LatePoses` plan keys: raw pointers used as keys only.
- `PortalViews::Build`'s step stamp: tried stamping the main walk with `NextSceneStep()`. It broke the portal atlas: point-light contexts
  were rejected ("portal atlas: mixed context frames", 4096 occurrences a run), so it was reverted. The atlas needs the main walk and the
  light walks to carry one frame number. A real fix restamps the main context when the game half takes its step. Open.
- The scene-thread ahead build reading the sun board's working copy: not touched.

## 4. Item 1: window-resize-stress-taa loses the device (not fixed)

Reproduces often: base thread build 2 of 2, the director's suite 2 of 2, the final commit 1 of 3. The fault is always a DCLF pass on the graphics queue
reading an address no buffer holds: `cs.dclf.main-opaque` (4 runs), `cs.dclf.reflection.faces`, `cs.dclf.z.depth`. The read addresses are
low (0x5840000, 0x5aaa000, 0x6335000, 0x7dbd7000, 0xd5936000): not buffers, not descriptor heaps, so most likely freed image memory.
BasicRHI's attribution does not track images.

Timing: in the runs with a full log the GPU stops shortly after DCLF's `main resources for WxH: adopted with a scene made for them`
(the HZB and dispatch signatures of a new display size switch in), or 1-4 s later. Present lines keep coming for a while because Present
does not wait for the GPU. The first or a later resize can trigger it; it is not specific to the second shrink.

Ruled out:
- **Game-half calls into DCLF's render state.** A temporary probe on all 151 `IndirectDraws` / `Impl` functions in `Core/Render`
  logged which the game thread calls while the render thread runs: only `WriteAccumulated`, `AccumulatedVerdictOf` (visibility
  readback into engine flags, game side by design), `DecideDrawFamilies`, `PublicationOf`, `NoteEventAdoption`. None touches the main
  resources, views or build state.
- **The claims race** (section 3): no "left DCLF's scene" stop happened in any failing run.
- **The old main resources freed while frames still read them**: holding the replaced `Resources` until the frame's submitted point
  retired did not help (2 of 2 failed). Reverted.
- **Deferring frame revisions past repeated renders** (adopt only at a Begin with a new snapshot): 1 of 2 failed. Reverted.

Pointer: with the render thread made to render only new snapshots (no repeats, a diagnostic build), the run passed (1 of 1). Repeats raise
the render rate from 60 to about 170 a second in this scene, so this does not tell repeats from rate. Next steps for a successor:
1. Repeat the no-repeat run two more times. If it holds, look for render-side state keyed to a ticket that a repeat reuses across a
   main resources swap or a target revision: the ticket's `FrameGlobals` (its `technique.shadowMask` view), the installed frame's
   lookups, the light set's views, first person's and the reflection view's imports.
2. `ImportViewDepthTarget` / `ImportViewTarget` keep their import while the field handle is the same pointer (handles are stable for
   the session), and the log shows `first-person depth imported: 1920x1080` once only. This looked like a stale image, but `ImportImage`
   goes through `FrameAnchors::ShareImage`, which maps an engine target's image to its versioned slot, so the import should follow the
   versions. Checked by reading only; worth a look at the view's cached `width`/`height` and its own depth (`r.depth`, made once at the
   first size), which do not follow a resize.
3. Image attribution: `vkGetImageViewAddressNVX` (VK_NVX_image_view_handle) could put image views into BasicRHI's fault attribution.

The single-thread org_integration 1a07653be passed once (snap7-thread s2 runs); 0a609eca also lost the device once. So there is very
likely a pre-existing rate-dependent fault that the render thread exposes.

## 5. Item 2: the crash at a load's held frame

Not reproduced as a CPU crash in this round: facegen twice, native-interior once, resize-stress nine times (section 7). The director's suite hit a *GPU* loss at native-interior's held frame
(`1127-075856-dir-native-vulkan-native-interior`): 0.9 s after "render thread: started", fault in `cs.dclf.forward.direct`, invalid read
at 0x104b2e000. Same signature as item 1 (a DCLF pass reading memory no buffer holds), right when the load's revisions settle. Likely
the same cause. The two CPU crashes of snap7-thread (facegen s2h, resize s2h) stay unexplained.

## 6. Items 6 and 7

- **Item 6, game update dips.** The resize runs show `game 30.6 updates/s` against a cap of 60 for a whole 5 s window while rendering
  168 fps. Hypothesis, not measured: the render thread holds `Renderer::Lock` (AE 77243) for its whole render half, nearly all the time
  at 170 fps; any engine code on the game thread that takes the same lock waits for a render to end. Measure with a Tracy zone or the
  render-thread sampler on the game thread's lock waits. A fix would move the render thread's Lock to only the parts that need it (the
  engine's Begin/End bodies), or port them (TH-b).
- **Item 7, frame-start reads 2.0.** Not changed. Passing the values through the ticket means the game half reads the renderer's target
  table, which the render thread's resize rewrites (`Renderer::Begin`'s body): a new race of the item 1 kind. Decided by the agent, to
  confirm: take them from CS's engine target set instead (`Targets::ViewFormat(Depth(kPOST_ZPREPASS_COPY, DSV))` for the format; the
  field handle or the target's slot for the import).

- Measured on 040a66efb (native quick, detect): frame start is now **3.01** per world frame, not 2.0. The third site is
  `GrassMirror.cpp:84` (ShaderManagerState, 1.00), which came with snap7-thread's TH-p (the draw families' mirrors drained in the render
  half's `BeginFrame`). Render half (record) game-mutated reads stay 0.00; engine render calls 0. The grass read should become a ticket
  value with the family decision, which stays in the game half.

Out of scope, open: PCL simulation id with frame generation (snap7-prep section 5); non-world frame kinds on their own thread.

## 7. Runs (stage s7-thread2)

| Label | Build | Config | Result |
|---|---|---|---|
| a-resize | base (551b76323) | window-resize-stress-taa | device lost after small2 (main-opaque) |
| b-resize-markers | + items 4, 1M marker history | window-resize-stress-taa | device lost before large0 (reflection faces) |
| c-resize-probe | + item 3, game-thread probes | window-resize-stress-taa | device lost (main-opaque); probe list in section 4 |
| d-resize-norepeat | + no repeats (diagnostic) | window-resize-stress-taa | passed, 13 shots |
| e-resize-defer1/2 | + revisions only at new snapshots | window-resize-stress-taa | lost, passed |
| f-resize-retire1/2 | + replaced main resources retired | window-resize-stress-taa | lost (z.depth), lost (main-opaque) |
| g-nvk-quick-detect | 040a66efb | native-vulkan+quick, detect | passed; record game-mutated 0.00, frame start 3.01, engine render calls 0 |
| g-nvk-drop-cells | 040a66efb | native-vulkan+cadence-drop-cells | passed, 3 shots |
| g-guest-quick | 040a66efb | guest+quick | passed |
| g-nvk-facegen, g-nvk-facegen2 | 040a66efb | native-vulkan+facegen | passed, passed (item 2 not reproduced) |
| g-nvk-native-interior | 040a66efb | native-vulkan+native-interior | passed, 4 shots |
| g-resize, g-resize2, g-resize3 | 040a66efb | window-resize-stress-taa | passed, passed, device lost (invalid read 0x5840000) |

## 8. Decisions to confirm

- T2-a. A render-side stop is counted and the game half applies it; coverage counts only after the last stop (section 3).
- T2-b. Toggles keep one frame word per half.
- T2-c. Fade and wind slots key on the render serial; the builder's ahead selects use the current serial.
- T2-d. Item 7 through CS's target set rather than the ticket.
