# snap7-devloss4: the resize transition, round 4

Lead `devloss4`, round `snap7`, branch `snap7/devloss4` (base `org_integration` 7fe8f4a45), stage `s7-devloss4`. Continues
[snap7-devloss3.md](snap7-devloss3.md), [snap7-devloss2.md](snap7-devloss2.md) and [snap7-devloss.md](snap7-devloss.md).
Dev-Fast throughout. RTX 5080.

Status: **guest's second-resize device loss is fixed. Native's loss without the interim wait is not, so the wait stays.**

## 1. Commits

| Repo | Branch | Commit | What |
|---|---|---|---|
| BasicRHI | snap7/devloss4 | 07e856e | fix(vulkan): clip render area to attachments |
| CS | snap7/devloss4 | 78e5d0560 | fix(native): BasicRHI clips render areas (pointer to 07e856e) |
| CS | snap7/devloss4 | (this file) | docs: snap7-devloss4 notes |

## 2. Guest's loss at the second resize (fixed)

Cause: passes drew with a render area larger than their attachments. Right after a resize the passes still use the size they
were made or latched for, while their targets are already the other size. Vulkan leaves rendering past an attachment undefined.
The GPU wrote past the image: DXVK's fault report named an invalid write at a real address (0x2353041000) 13 ms into the
second resize.

The BasicRHI change logs every clip. In guest runs it named, all 1920x1080 on 1280x720 targets:
- `DCLF depth` (MainViewPasses: the frame's width from DCLF's main resources), 30 per run;
- `Sky` (the latched engine viewport), 21-22;
- `ShadowMask` (latched viewport), 1; `DCLF direct forward` (Direct site shape), 1;
- `cs.upscaling.copy-to-main`, `cs.pp.output.main`, `cs.pp.output.main-copy` (fixed viewports), 4 each.

In native only the last three show (8 each per run). They are the recordings round 3's validation saw.

Fix: Vulkan `BeginPass` clips the render area and scissor to the smallest attachment at its view's mip and keeps the viewport
as asked. That is what D3D12 does: it clips to the render targets. So both backends now render the same thing in those frames.
Decided by the agent, to confirm (DV4-a): a clip in BasicRHI rather than a fix at each pass. Each pass draws with a correct
size a frame or two later anyway, and a clip covers passes not found yet. The log stays (first 64 clips, then every 4096th) so
a pass that keeps drawing at the wrong size is visible.

## 3. Native without the interim wait (not fixed)

The wait (decision 229) is still in `CommitMainPayload`. Without it, window-resize-stress-taa loses the device in about one
run in three to one in two, as in round 3. What this round learned:

- Losses come after DCLF adopts its new main resources ("main resources for WxH: adopted with a scene made for them") and its
  five replaced graphs are torn down, mostly at a shrink. Checkpoints: `cs.dclf.z.depth`, `cs.dclf.reflection.faces`,
  `cs.dclf.first-person.depth`. Faults: invalid reads at 0x48d9000..0x656f000 (0x5840000 again), or instruction-pointer faults.
- The low fault addresses look like a byte offset with no base. One reading that fits: a load through a buffer view ref whose
  pair holds the null descriptor (VA 0, size 0), plus the ref's offset. The pulled Z-prepass reads its draw's sequence that way
  (`DCLFSequencesAddress` + instance * 136; 0x5840000 is sequence 680k). Not proven (section 4 row 4).
- The director's cloud-shadows sample (1127-173821, coc into WhiterunDragonsreach) looks like the same family: two frame
  revisions with replaced graphs (8 torn down), then instruction-pointer faults and a garbage read in `cs.dclf.forward.direct`.

## 4. Tried and dropped (each without the interim wait unless named)

| Change | Result |
|---|---|
| Render-area clip alone (a-clip1) | lost 1 of 1; only the three fixed-viewport passes clip in native |
| BuildDraws drops draws with any field below every allocation (b-low) | no loss in 3, but it dropped almost every draw: vertex views are refs (slot, offset), not addresses. Not usable |
| The same for row and index addresses only (c-low) | passed 1, lost 1, 1 failed on a shot size (large5 at 1280x720) |
| CPU wait for the last render at each DCLF graph switch (d-swwait) | passed 3, lost 1 |
| Freed view ref pairs keep their descriptor and wait 3 s before reuse (f-pairdelay) | passed 3, lost 1 (the loss had no fault address) |
| CPU wait for the last render on the first 60 renders after each DCLF adoption (g-adopt60) | passed 2, lost 2 |

So the loss is not only overlap right at the switch, not render-area overruns, not zero row addresses in the sequences, and not
view ref pair reuse alone.

## 5. Runs (stage s7-devloss4, native-vulkan+window-resize-stress-taa unless named)

| Labels | Build | Result |
|---|---|---|
| a-clip1 | clip, no wait | lost |
| b-low1..3 | + draw guard (all fields) | failed 3 (draws missing), no loss |
| c-low1..3 | + draw guard (addresses) | passed 1, lost 1, failed 1 |
| d-swwait1..4 | clip + switch wait | passed 3, lost 1 |
| e-guestclip1..2 | guest+window-resize-stress-taa, clip | passed 2 |
| f-pairdelay1..4 | clip + pair delay | passed 3, lost 1 |
| g-adopt601..4 | clip + 60 waits after adoption | passed 2, lost 2 |
| h-* | final branch (wait kept) | section 6 |

## 6. Final runs (branch 78e5d0560, interim wait kept)

| Label | Config | Result |
|---|---|---|
| h-guest1..3 | guest+window-resize-stress-taa | passed 3 (lost the device at the second resize in every run before) |
| h-native1..3 | native-vulkan+window-resize-stress-taa | passed 2, lost 1 |
| h-nvq1 | native-vulkan+quick | passed |
| h-gq1 | guest+quick | passed |
| h-d3d1 | native-d3d12-quick | passed (BasicRHI touched; the change is Vulkan only) |
| h-cad1 | native-vulkan+cadence-drop-cells | passed |
| h-int1 | native-interior | passed |

h-native2 lost the device with the interim wait in place, about 0.5 s after the TAA switch and before any resize: checkpoint
`cs.dclf.main-opaque`, invalid read at 0xe7fbe000. So the wait lowers the rate but does not remove it (round 3 saw the same:
r-final 4 of 5, j-final and l-clockonly lost with the wait). Frame rates were not measured: the wait stays, so this is the wait
build plus a clip that only acts in resize frames.

## 7. Open items

- Native's device loss without the interim wait (section 3). Next steps: make the null view ref pair point at a small poison
  buffer at a known address, so a load through a released ref faults at poison + offset and the fault names it; and log, per
  render after a DCLF adoption, which sequences version and view slot the Z-prepass pushes against the version adopted.
- The source passes in section 2 should draw at their target's size; the clip makes it harmless but they still draw a frame or
  two at the wrong size.
- c-low3 failed on shot large5 being 1280x720: a shot taken before its revision's size. Possibly snap7-resizeui's area.

## 8. Decisions to confirm

- DV4-a. BasicRHI clips a Vulkan pass's render area and scissor to its attachments (D3D12 parity) and logs each clip.
- DV4-b. The interim wait (DL-a) stays.
