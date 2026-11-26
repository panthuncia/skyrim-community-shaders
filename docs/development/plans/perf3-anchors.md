# Round 3 lead `anchors`: frame-anchor client commits, ORG's per-epoch remainder

CS branch `perf3/anchors` on org_integration 600b56386. No ORG, BasicRHI, DXVK or OMS change. All timings are Dev-Fast (/Od),
guest mode, the frozen river scene (`trun.sh guest+quick ... --set env.CS_ENGINE_ZONES=1`), per frame.

## Attribution

A temporary table (QPC around each client's commit and around `GraphFrames::NoteGraph`, logged every 1000 frames; removed) in
the first capture (`p3-anchors-base1`):

| | us per frame |
|---|---|
| commit bodies, 43.5 commits | 118 |
| `GraphFrames::NoteGraph` (a mutex and a string-keyed map lookup per commit) | 26 |
| `cs.pp` (post chain, 7 steps) | 17.0 |
| `cs.scaleform` | 12.2 |
| `cs.sky.faces` (2 calls) | 7.0 |
| `cs.cloud-shadows.layers` (3 calls) | 6.8 |
| `cs.engine.imagespace.world.scopes` (4 calls) | 5.5 |
| `cs.view-atmosphere.reflection`, `cs.screen-space-shadows`, `cs.dyncubemaps`, `cs.sky.main`, `cs.view-atmosphere` | 4.4-5.1 each |
| 30 other clients | 0.1-3.9 each |

NoteGraph was the largest fixed cost per commit, more than any client but two. The bodies themselves are real per-frame work
(light directions, dispatch sizes, layer ops, HUD lists): no repeated work stood out beyond what round 2 took.

## What changed

1. **Graph frame counts by handle** (`Testing/GraphFrames.*`). A registration finds its id's counts once (`CountsOf`, kept in
   `Client::frames`); a commit bumps them through the pointer (relaxed atomics, render owner only). No mutex or map lookup per
   commit. Release (`CS_RELEASE_PROFILE`) still compiles it out. Counts are now made at registration, so the report skips ids
   that never committed, which keeps it as before.
2. **The tap's committed list** is a fixed array of indices into the stage's clients (at most `kSegmentClients` can be on), not
   a vector of shared pointers.
3. **Runs list only active clients.** Nothing in `Run` or `TapDeclared` reads an inactive client (the tap commits only found
   active ones; the shared path's checks and errors look at active ones only), so inactive clients are no longer copied in. The
   `active` flag and the unread `serial` of `RunClient` are gone; a dropped segment-only client is erased.

## Measured

`p3-anchors-base2` is the unchanged branch (base1 carried the temporary table and is not used for the comparison).
`after1` has changes 1 and 2, `after2` all three (the final build). Engine self, which no change touches, read 0.549 / 0.561 / 0.547 ms, so
the base2 and after2 captures ran on an equally loaded machine.

| | base2 | after1 | after2 |
|---|---|---|---|
| Window | 2.619 | 2.631 | 2.544 |
| `CS.FrameAnchors.Run` inclusive | 0.250 | 0.226 | 0.212 |
| `CS.Anchors.ClientCommit` self | 0.124 | 0.103 | 0.101 |
| `CS.FrameAnchors.Run` self | 0.057 | 0.059 | 0.048 |
| `CS.Anchors.Tap` self (in Run) | 0.035 | 0.030 | 0.030 |
| `CS.Anchors.SharedConstants` | 0.022 | 0.022 | 0.022 |

About 0.04 ms a frame Dev-Fast in the anchors' runs. The window dropped 0.075 ms, of which the rest is noise.

## Item 2: ORG's per-epoch remainder, not changed

At after1, 14.5 epochs a frame: `ExecuteEpoch` self 2.5 us an epoch, `SubmitEpoch` self 1-3, `CommitTicket` self 3-9 (deferred
releases, retirement, three timing laps, the commit record), `TakePendingUploads` 9-10 for the DCLF epochs that upload,
`EnqueueTicket` 4-6 (the submission handoff, excluded), and the stages' `CS.Stage.CopyLatch` 4.4 / `MoveSlots` 2-7 /
`CopyGates` 1.5 (already copies only what was written since the last clear). Everything else inside the epochs is DCLF's inputs
(Z-prepass 0.22 ms, main opaque 0.10, shadow view 0.08), which belong to the DCLF leads. Each ORG step was cut in round 2
(`perf2-ticket3.md`) and is now 1-9 us of real work; nothing here was a contained cut outside queue and submission code.

## Decided by the agent, to confirm

- Commits are not skipped by a change stamp. A stage's staging run is zeroed and its gates closed at every frame's first tap
  (`OpenInputs`), and the slot copies rotate, so a skipped commit would need its bytes and gate entries kept per slot and the
  clear made selective, for every one of 45 clients, each with a stamp covering all its inputs. Most commits carry camera or
  frame values that change every frame in play, so the frozen scene would overstate the gain. The fixed per-commit cost was
  cut instead.
- SE: nothing here is runtime specific. Untested on SE at runtime (the game folder is AE 1.6.1170).

## Tests (final build)

| Config | Label | Result |
|---|---|---|
| guest+quick, trun capture | p3-anchors-base1 | ran to its exit step (attribution table) |
| guest+quick, trun capture | p3-anchors-base2 | ran to its exit step (baseline, unchanged branch) |
| guest+quick, trun capture | p3-anchors-after1 | ran to its exit step (items 1-2) |
| guest+quick, trun capture | p3-anchors-after2 | ran to its exit step (final) |
| guest+quick | p3-anchors-v-guest-quick | passed, shot looks right; graph frames report as before |
| guest+hdr | p3-anchors-v-guest-hdr | passed, 6 shots |
| guest-interior | p3-anchors-v-guest-interior | passed, 4 shots |
| native-vulkan+quick | p3-anchors-v-native-vulkan-quick | passed, shot looks right |

Native D3D12 not run (no ORG or BasicRHI change).

Captures and bucket files: `scratchpad/anchors/perf/`.

## Commits

CS `perf3/anchors`: 7f953369e (graph frames by handle, committed array), 06a57eacb (active clients only), then these notes.

## Open

- The commit bodies (0.10 ms) are 40-odd clients' real per-frame work; the largest are the post chain and the HUD.
- `CS.Anchors.SharedConstants` (23.5 commits, 0.022 ms): four block writes into each step's latch. Packing them once per stage
  instead of once per step would need the steps to share a region.
- `CS.Stage.CopyLatch` (0.037 ms over 8.5 stages) takes a lock per live block.
