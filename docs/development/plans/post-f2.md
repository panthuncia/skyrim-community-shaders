# post-f2: broken repeat renders (F2)

Lead `post-f2`, branch `post/f2` (base `org_integration` 2cd36d312, post-small merged), stage `post-f2`. RTX 5080, Dev-Fast.

Follows `post-small.md` item 4.

## Cause

Dragonsreach is an interior, so DCLF draws it through the CPU pipeline ("graph conditions: interior cell, ... CPU pipeline").
The cull runs on the CPU (`Direct/Cull.cpp`), not in BuildDrawsCS. post-small's experiments changed the GPU cull
(`MainFrustumRejected`, the portal batch's queue), which Dragonsreach does not run. That is why they changed nothing.

A per-render trace of the CPU depth cull (scratch patch `scratchpad/post-f2/f2-trace.diff`) showed it at once (run t1):

- normal render: 2635 lanes, 2124 in the frustum, 937 rejected by the portal test, 1187 kept;
- both broken frames: the same lanes and frustum, but 1744 rejected by the portal test and 380 kept;
- camera, latch, cull frame, view batch set and recording run were identical on both. 43 renders of about 67,000 had 380.

The portal test reads the frame's portal programs. The CPU fade stage's kick (`FadeCpuStage.cpp`, render thread) copied them
from `PortalViews::Encoded()`. That vector belongs to the game thread's portal walk: `Main::Update` calls `PortalViews::Build`,
which re-encodes it in place for the next update. A repeat render runs while the game thread runs its next update, so the
render copied half-written programs about once in 1500 renders. First renders mostly did not overlap the walk.

The GPU path was already safe: its programs publication reads the adopted light set's portal context.

Not stable latch regions (snap7-devloss7b's lead) and not the device loss: this is CPU memory only.

## Fix (62a6be020)

- The game half puts its main portal context (`PortalViews::MainContext`) in the frame ticket, as `TicketPart::Portals`.
- `PortalViews::FrameContext()` is the walk the caller's frame reads: on native's render side the ticket's, otherwise the
  last walk's (guest and native's main-thread frames build and read on one thread, as before). `FramePrograms()` gives its
  words. The CPU pipeline's culls and set parity use it.
- The room map (`roomIndex`, room -> graph room index) was the same kind of race: the render side's root-program judging
  (`ProgramOf`, `ProgramsVersion`, from FadeCommit and the CPU stage) read it while a graph change cleared and refilled it on
  the game thread. It is now an immutable map replaced whole on a graph change, carried in the context, and read through
  `FrameContext()`.
- Check: `PortalViews::Encoded()` and `MainContext()` log an error once each when read on another thread than the walk's
  (one thread id compare). It would have caught this bug.
- `dragonsreach-bursts`: each step after a burst waits up to 180 s for its PNGs. With 60 s a busy machine failed a run
  ("the previous shot was not saved after 60 s").

No new switch.

## Sweep for the same shape (director's ask)

Render-side reads of CS state the game half rewrites:

| What | Reader | State |
|---|---|---|
| `PortalViews::Encoded()` | CPU fade stage kick, set parity | fixed (ticket) |
| `PortalViews` room map, `graphVersion` | root-program judging (FadeCommit, CPU stage, light context gates) | fixed (context) |
| `PortalViews::Encoded()` fallback in the GPU programs publication | render thread, when the adopted set has no boards | left: boards are there on routed renders; the new check logs if not |
| `PortalViews::VisibleUnbound()` | `Handoff/SceneLists.cpp` | atomic bool; a one-frame answer, left |
| fades, first person, sky, water, animated fades | render side | already ticket parts |
| `FrameView` sun candidates, light set boards, light selection | render side | already render-side copies (`RenderSide()`, `AdoptedBoards`) |

Found nothing else of this shape by reading. A general check (a generation or thread assert on every game-half container) is
not added: the containers differ too much. The PortalViews check covers the one found.

## Runs (stage post-f2)

| Config | Label | Result |
|---|---|---|
| native-vulkan+dragonsreach-bursts, frustum, trace | post-f2-t1 (before) | failed: 2 F2 (spike 155.7, 156.8); both portal 1744 / kept 380 |
| the same with the fix, trace | post-f2-fix1 | failed on the burst save timeout only; 25,000 renders, no 380 render |

| native-vulkan+dragonsreach-bursts, frustum | post-f2-vkf1, vkf2 | passed both (worst spike 1.8, 0 flickering tiles) |
| native-vulkan+dragonsreach-bursts, occlusion | post-f2-vko1, vko2 | passed both |
| native-d3d12+dragonsreach-bursts, frustum | post-f2-dxf1 | passed |
| native-d3d12+dragonsreach-bursts, occlusion | post-f2-dxo1 | passed |
| native-vulkan+quick | post-f2-nvq | passed |
| native-vulkan+cadence-drop-cells | post-f2-cdc | passed |
| native-interior | post-f2-int | passed |
| native-vulkan+flicker-scenes | post-f2-fs | passed |
| native-vulkan+update-cap30 | post-f2-c30 | passed |
| native-vulkan+update-cap240 | post-f2-c240 | passed |
| guest+quick | post-f2-gq | passed |

Before the fix, about one break per occlusion run and several per frustum run (post-small). After it: 48 bursts over six
runs, 0 breaks. The cross-thread check logged nothing in any run.

Skipped: native-d3d12-quick (no ORG, BasicRHI or device change; D3D12 ran the two burst runs).

## Open items

- `StampFrustum` (CPU and BuildDrawsCS) takes "in view last frame" as `previous stamp == stamp - 1`. Render steps share one counter
  with the game half, so consecutive renders often differ by 2. Then a fade-tested lane past its fade distance whose root has no
  serviced verdict is dropped as "not in view last frame". It only touches far, fade-tested lanes. Not changed here.
- The portal programs publication (GPU path) still falls back to `Encoded()`; it could read `FramePrograms()` too. Left so as not
  to change the GPU path without a run of its own.

## Decisions, to confirm

- Decided by the agent, to confirm: native's render side reads the ticket's portal context also on main-thread frames (load
  hold), like FadeState's camera. Same rule as the fades, so the programs always match the camera the stage culls with.
- Decided by the agent, to confirm: the cross-thread check only logs (once per accessor), always on.

Proposed decision-log entry: "Native's render side reads the main portal walk from its ticket (TicketPart::Portals). The CPU
pipeline's culls copied the game thread's portal programs while its next walk rewrote them, so about one repeat render in 1500
culled most of an interior's near scene (F2). The room map travels in the same context; reading the walk's own state on another
thread logs."
