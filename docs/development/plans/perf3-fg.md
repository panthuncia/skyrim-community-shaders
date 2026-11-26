# Round 3 lead `fg`: guest frame generation with ORG's async compute queue (2026-11-25)

CS branch `perf3/fg` (org_integration 8ad13b3b7, then org_integration with perf3/teardown merged). DXVK `perf3/fg` 94f80ed8
on d966e5d1. Streamline `perf3/fg` 0772934 on 156ace6. ORG, BasicRHI and OMS unchanged (ORG 8b7b709, OMS 6fae892 from the
merge). CS commits: c212a5707 (build fix without Tracy), 268dc9f03 (DXVK and Streamline bumps), a24446ad6 (guest takes
DXVK's compute queue, `CS_ORG_QUEUE_TOPOLOGY` removed), eddf1f3af (merge), and these notes.

## The hang (guest+fg with the own compute queue)

Not FFX racing DXVK. ORG's compute batches wait for `streamPoint` at their epoch's entry. DXVK's CS thread records that
signal, but DXVK's submission thread may not have submitted it yet. So the compute queue holds work that waits for a DXVK
submission still in DXVK's queue. Anything that waits for the whole device while holding DXVK's submissions back never
returns. Two things did:

- DXVK's `waitForIdle` at a swapchain rebuild (FG on/off recreates the swapchain). It held DXVK's queue lock through
  `vkDeviceWaitIdle`. Run p3-compute-fg6: render thread in `waitForIdle`, dxvk-submit blocked on that lock, the hang log's
  compute batch #319 waiting for `streamPoint>=3923` (at 3921) with 3922-3931 recorded and pending.
- FFX's frame generation swapchain. `waitForPresents` ends with `vkDeviceWaitIdle`, and it runs inside the present on DXVK's
  submission thread whenever the generation state changes. Runs p3-compute-fg1 and fg3: dxvk-submit in `amd_fidelityfx_vk`
  with the same driver frames as `vkDeviceWaitIdle`, compute batch #371 waiting for `streamPoint>=2646` (at 2644).

Fixes:
- DXVK `waitForIdle` waits for DXVK's own queues (graphics, transfer, sparse). The device-wide wait stays only at device
  destruction, after the interop clients retired.
- sl.fsr_g points the FFX DLL's `vkDeviceWaitIdle` import at a function that does nothing. FFX has already waited for its
  four queues by then. A failed patch disables FSR-FG (logged).
- Guest now takes DXVK's compute queue whenever DXVK exports one. Without one, ORG folds as before. CS stays
  topology-agnostic.

## The device loss at `fg off`

The f-fg runs failed at most 0.5 s after `fg off` (p3-compute-f-fg: `Device fault 0: Memory address: 0x0 (type 2)`, a write
at VA 0). That is the same signature as the known fault from a submission beside DXVK's submission thread
(dclf-architecture.md). Compute's narrow lock (d966e5d1) allowed more of that: ORG's submissions no longer wait for DXVK's
presents. Two paths were unserialised at `fg off`, and one during every FG present:

- FFX's swapchain teardown (inside `vkDestroySwapchainKHR`, on the render thread) waited for and freed on DXVK's graphics
  queue with no lock, beside DXVK's submission thread and ORG's submitters. Now DXVK destroys the swapchain under
  `lockIdleQueues`: it waits for DXVK's queues to idle, then holds the queue lock and the submission lock.
- The CS thread could still record FG inputs (FFX prepare) after the rebuild's idle wait, and CS frees the interpolation
  context right after the rebuild (`SwapchainTornDown`). Now the presenter runs a rebuild proc first: D3D11's swapchain
  synchronises the CS thread, so all recorded work is submitted before the idle wait.
- FFX's composition submits to the graphics queue inside DXVK's present, which does not hold the submission lock. New DXVK
  export `dxvkLockPresentSubmissions` (valid only on the presenting thread); `FfxGameQueueSubmit` takes it around each
  submission, never across FFX's waits.

Not A/B-tested against the old DLLs (`94063a21.bak`): the failure rate is 2 in 4 runs, and telling the rates apart would
take many exclusive runs. The cause comes from reading the stacks and the fault; 7 of 7 guest+fg runs on this branch passed,
FG off included.

## Tests (stage p3-fg; DXVK and sl.fsr_g copied into the stage)

| Config | Label | Result |
|---|---|---|
| guest+fg (own queue, first fix only) | p3-fg-fg1 | passed, compute 2:0 taken, FG presenting x2 |
| guest+fg (+ present lock) | p3-fg-fg2, fg3, fg4 | passed |
| guest+fg, final (teardown merged) | p3-fg-f-fg1, f-fg2, f-fg3 | passed, shots checked by eye |
| guest+quick | p3-fg-f-guest-quick | passed |
| guest-interior | p3-fg-f-guest-interior | passed |
| native-vulkan+quick | p3-fg-f-native-vulkan-quick | passed |
| native-d3d12-quick | p3-fg-f-native-d3d12-quick | passed |

GPU time is not re-measured. Steady frames do the same GPU work: the changes only act at swapchain rebuilds, FG state
changes and FFX's submissions. Compute measured the own queue 0.19 ms (3%) faster than folding (Dev-Fast).

## Decided by the agent, to confirm

- `CS_ORG_QUEUE_TOPOLOGY` is deleted: its only other value brought back the slower fold.
- The FFX import patch replaces a vendor call with a no-op. FFX waits on its own queues just before, so nothing it owns
  goes unwaited.
- `lockIdleQueues` holds the submission lock through FFX's swapchain destroy. That is safe only because DXVK's queues are
  idle first, so no graphics work waits for a compute batch still to be submitted.
- Streamline built from my worktree (premake-regenerated project, packman pull). The deployed sl.fsr_g came from a deleted
  worktree; I assumed its source matches 156ace6.
- c212a5707 fixes a C4189 build break in `ORGPostProcessing.cpp` (outside my boundary, one line).
- SE: untested at runtime; nothing here is runtime specific.

## Open items

- FFX's presenter thread still presents on its own queues beside everything (as in every round so far).
- Native FG with the compute queue was not run.
