# snap7-devloss6: the render-thread device loss, round 6

Lead `devloss6`, round `snap7`, branch `snap7/devloss6` (base `org_integration` 7ea6d47bb, with snap7-devloss5), stage
`s7-devloss6`. Continues [snap7-devloss5.md](snap7-devloss5.md) and the rounds before it. Dev-Fast throughout. RTX 5080.

Status: **the interim wait (decision 229) stays.** This round made every loss come with a fault record, and the records
changed the picture: the bad reads are descriptor heap indices taken from float data. Holding replaced graphs, the slot
lifetimes, and native queue boundary barriers are ruled out. The cause is not fixed.

## 1. Commits

| Repo | Branch | Commit | What |
|---|---|---|---|
| BasicRHI | snap7/devloss6 | 1ee1993 | fix(vulkan): poll the device fault query after a loss |
| BasicRHI | snap7/devloss6 | 0ed6da6 | feat(vulkan): decode float heap indices in faults |
| CS | snap7/devloss6 | 79ac872be | chore(basicrhi): device fault records (pointer to 0ed6da6) |
| CS | snap7/devloss6 | (this file) | docs: snap7-devloss6 notes |

The BasicRHI changes are on a new branch `snap7/devloss6` in that submodule; 79ac872be moves the pointer. Nothing else in
CS changed. The experiment switches of this round were deleted again (section 4); their patches are in the scratchpad
(`scratchpad/devloss6/experiments-cs.diff`, `experiments-org.diff`).

## 2. Fault records for every loss (plan step 1)

`vkGetDeviceFaultInfoEXT` failed with -13 (VK_ERROR_UNKNOWN) whenever BasicRHI saw the loss through a timeline that read
UINT64_MAX. At that moment the driver has not put the device in the lost state yet: Streamline's validation said so in the
logs ("device has not been found to be in a lost state"). It happened in about 60 runs across the lab's history. Now the
counts query is polled every 10 ms for up to 3 s. In this round's losses the device reached the lost state 20 ms after the
loss was seen, and every loss had a record ("the device reached the lost state 20 ms after the loss was seen (3 queries)").

## 3. What the records show

Each loss has an "instruction pointer (fault)" entry and an invalid read. The instruction pointer entry is the address of the
faulting instruction (VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT), not a freed shader. So the earlier
"instruction-pointer faults" were the same reads, not destroyed pipelines.

The invalid reads are descriptor heap reads with a float word as the index. With the resource heap at 0x14610000 and a
stride of 16, `heap + index * 16`, wrapped in 32 bits, gives every address seen:

| Fault address | Index read as a float | Runs |
|---|---|---|
| 0x5840000 | 0.5710 | 19 (all rounds) |
| 0x5780000, 0x48d9000..0x656f000 | 0.51-0.62 | about 20 |
| 0x3e610000 | 80.0 | 2 |
| 0x38e10000 | 50.0 | 1 |
| 0x56610000 (a-cs3) | 640.0 | 1 |
| 0x64610000 (b-cs1) | 2048.0 | 1 |

Exact round values (80, 50, 640, 2048) cannot be chance. A shader, or the pipeline layout's descriptor mapping
(VK_EXT_descriptor_heap reads texture indices from the frame record), takes a heap index from memory that holds float
constants: a material row's float block, frame constants, or a frame record overwritten by constants. BasicRHI now prints this
decoding in the fault report ("as a heap N index that wrapped in 32 bits ..."). The poison never fired: no released view ref
was involved (14 poison runs, 2 losses with records, no POISON line).

The other cluster, 64-bit garbage addresses (0x8126e019f8eed, 0x8135d0008f5d8), looks like a device address read from the
same kind of wrong data.

## 4. Experiments (all without the interim wait)

| Change | Runs | Result |
|---|---|---|
| Poison, fault retry (a-*) | cloud-shadows 3, cadence 3 | cs3 lost (640.0); cadence 3 passed or failed shots only |
| + slot early-release detector (b-*) | cloud-shadows 1, cadence 1 | cs1 lost (2048.0, `cs.dclf.z.depth`, GPU path); the detector never fired |
| Replaced graphs held 20 s (`ORG_RETIRED_GRAPH_HOLD_MS`, c-*) | cloud-shadows 3, cadence 2 | cadence lost (0x5840000) 2.0 s after the graphs' attach, with no teardown at all |
| Native queue boundary barriers at runtime (d-*) | 2 | invalid: changing the boundary refused staged builds (fatal) |
| Native queue boundary barriers from startup (`CS_NATIVE_QUEUE_BOUNDARY`, e-*) | cadence 2, cloud-shadows 2 | cadence lost at the first coc |

What they rule out:
- **Graph teardown.** Every earlier loss came 2.0 s after "N replaced graph(s) torn down". With the graphs held 20 s, the loss
  still came 2.0 s after the attach of the new graphs. The 2 s is the driver's detection time; the trigger is the attach (the
  first frames of the new graphs), not the teardown. That matches devloss2's 3 s hold (z-graphhold1, lost).
- **CPU pipeline slot lifetimes** (plan step 3's `CloseForwardSlot` and the CPU frames' slots): a detector compared each
  slot's release with the exact GPU point of everything submitted by its frame's end; it never fired, also in the loss runs.
- **Cross-epoch barriers.** Native runs without the entry and exit full barriers guest has. With them on from startup the
  device was still lost.

## 5. Where the losses happen

Every loss this round sits at a DCLF graph switch: a coc that changes the graph conditions (exterior GPU-driven to interior
CPU pipeline, or back), or a load-hold re-entry ("an occlusion map DCLF cannot draw this frame ... the graph rebuilds").
In the cadence and cloud-shadows runs, the conditions change while the main thread owns rendering during the load hold;
the render thread takes ownership back and attaches the staged DCLF graphs in the same frame, and the GPU stops within that
frame or the next (the loss is reported 2.0 s later). Losses come in both pipelines: `cs.dclf.direct`, `z.direct`,
`forward.direct` (CPU) and `cs.dclf.z.depth` (GPU path).

## 6. Runs (stage s7-devloss6)

| Labels | Build | Result |
|---|---|---|
| a-cs1..3 (cloud-shadows) | poison, fault retry, no wait | passed 2, lost 1 (record: 640.0) |
| a-cad1..3 (cadence-drop-cells) | same | passed 2, failed 1 (shots only) |
| b-cs1, b-cad1 | + slot detector | cs1 lost (2048.0), cad1 failed (shots only) |
| c-cs1..3, c-cad1..2 | 20 s graph hold | cs passed 2, failed 1 (a shot not saved in 60 s); cad failed 1 (shots), lost 1 (0x5840000) |
| d-qbcs1, d-qbcad1..2 | runtime queue boundary | hang (refused builds), passed 1, lost 1 |
| e-qbcad1..2, e-qbcs1..2 | queue boundary from startup | cad passed 1, lost 1; cs passed 2 |
| f-* | final branch, wait kept | section 7 |

Without the wait, this round lost the device in 5 of 17 valid world runs, the same rate as before.

The cadence "failed" runs failed on the dragonsreach shot only: the shot is outdoors (the `coc WhiterunDragonsreach` did not
take) with a flat brown patch of missing land. That is not this item (post-land's missing land, or the save's coc).

## 7. Final runs (branch 79ac872be, wait kept)

| Label | Config | Result |
|---|---|---|
| f-native-vulkan+cadence-drop-cells | native-vulkan+cadence-drop-cells | passed |
| f-native-vulkan+cloud-shadows | native-vulkan+cloud-shadows | passed |
| f-native-vulkan+quick | native-vulkan+quick | passed |
| f-native-d3d12-quick | native-d3d12-quick | passed (BasicRHI pointer moved; the change is Vulkan only) |

Skipped: window-resize-stress-taa, native-interior, native-tour, guest+quick, guest resize and the perf configs. The branch only
changes BasicRHI's Vulkan loss report, which runs after a loss; nothing on a frame's path changed, and the wait stays.

## 8. Decisions to confirm

- DV6-a. The interim wait (DL-a, decision 229) stays: without it 5 of 17 world runs still lost the device.
- DV6-b. BasicRHI polls `vkGetDeviceFaultInfoEXT` for up to 3 s after a loss seen through a timeline. Decided by the agent:
  the device is dead, and a record is worth 20 ms of a crash path.
- DV6-c. The fault report decodes addresses that fit a float word used as a heap index. Always on (only in loss reports).
- DV6-d. No experiment switch was kept (rule: no switch whose other value brings back an older path). The patches are in the
  scratchpad.

## 9. Open items

- The device loss without the wait. Next steps:
  1. Find which word is the float. The candidates are the heap index reads a DCLF draw makes: the pipeline layout's texture
     indices from the frame record (`kDCLFRecordTextures`, read by VK_EXT_descriptor_heap's mapping), the material row's
     texture indices (`DCLFMaterialTextureIndex`, a row at a relative or absolute offset), the animated rows redirect
     (`DCLFAnimatedRows + animatedRow * 1024`), and ref slots read from sequences or the frame record's backings. A way to
     tell: write a different marker float into each candidate's neighbouring float block (or fill freshly made DCLF buffers
     with a pattern) and read which comes back in the decoded index.
  2. Check what the first frames after a DCLF graph attach read: the new graphs' frame record and frame constants, a set
     whose relative row offsets were made against another payload version, or buffers grown "as graph work" in that frame
     before their first write. The switch happens in the frame the render thread takes ownership back from the load hold.
  3. The interim wait is per thread (`t_lastRender` is thread_local), so the render thread's first render after the main
     thread rendered the load hold does not wait for those renders. Worth knowing if the fix is a wait at the handover.
- The cadence dragonsreach shot lands outdoors with missing land in about half the runs (section 6).
- c-cs3 failed because a shot was not saved in 60 s while frames kept presenting (not investigated).
