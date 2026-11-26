# snap3-texinit: zeroed native textures, guest face tint

Lead: `texinit`, branch `snap3/texinit` (base `org_integration` 2abd9a070), stage `s7-texinit`. Carries out decision 158 (a) and (b),
the two open items of [snap3-facenoise.md](snap3-facenoise.md).

## (a) The native device zeroes textures made without contents

`src/RenderGraph/Engine/Native/NativeDevice.cpp`, Vulkan and D3D12 alike (the code is BasicRHI calls only). No BasicRHI or ORG change.

- Every texture the native D3D11 emulation makes already gets one submission at creation: a tracked (graph-owned) texture's
  layout move through the external client (`InitializeTracked`), a host-made one's waited initialisation (`Initialize`). Without
  initial data that same submission now also zeroes it (`PrepareZero` + `RecordZero`). No extra submission, no wait added.
- How (`ZeroWay`): depth by a depth+stencil clear, render targets by an RTV clear, UAV textures by a UAV clear, everything else
  (shader-only, block-compressed, 1D) by copies from one 4 MB upload buffer of zeros (`Native::Zeros`, made on first use; a
  larger subresource is copied in bands of rows, as ORG's copy-queue zero fill does). The texture is moved from its resting state
  (Common) into the clear's state and back inside the submission, so the ledger's state for a tracked texture is unchanged.
- Covered: `MakeTexture` (both paths, DYNAMIC included), `WrapTexture` and `WrapGraphTexture` (non-deferred). Not covered:
  staging textures (CPU buffers), the GenerateMips scratch (written before read), deferred wraps (the graph births them).
- `ZeroWrapped`, which zeroed clearable wraps in a submission of its own after the initialisation, is gone: its two calls are covered.
- Face tint: the head's texture now starts as zeros, so snap3-facenoise's render-side `ClearHead` and its `FaceTint::Zero` texture
  are dead and removed (separate commit, `src/Snapshot/FaceTint.cpp`; RenderFrame lost only the clear loop).
- A log line counts the work at 64 textures and each doubling: `[ORG native] zeroed N textures made without contents (...)`.

### Cost (Dev-Fast, native Vulkan, 1920x1080)

| When | Textures | Depth / colour / UAV clears | By copy (copies) | Bytes |
|---|---|---|---|---|
| startup to the main menu | 256 (logged at 64, 128, 256 within 0.4 s) | 21 / 184 / 41 | 10 (120) | 1603 MB |
| the tour's two cell loads (Whiterun, Dragonsreach) and the cadence-drop cells run | fewer than 256 more (the 512 line never came) | | | |

The numbers are identical on native D3D12. Nearly all the bytes are render targets and depth, which are clears (fast clears on current GPUs, no memory traffic for most). The
copy path, which reads the upload heap over PCIe, is about 4% of the textures. The work is folded into submissions that existed
already, so it adds no submit and no CPU wait. GPU time was not measured per texture.

## (b) Guest: DCLF captures the face material again after the tint swap

`src/Features/DrawcallLimitFix/Core/Shading/MaterialWrites.cpp`, with `Snapshot::FaceTint::QueuedHeads()` added (append only).

- Cause (decision 156): the engine's UpdatePending gives each queued head NiSourceTexture a new renderer texture (+0x48) and
  releases the empty one. The face material keeps the same NiSourceTexture, so no material write hook fires, and DCLF keeps the
  view it captured at attach: the empty, zeroed texture. Guest's race menu drew a flat dark face.
- Fix: outside native, DCLF detours FaceGen::UpdatePending (SE 26455, AE 27041; Detours stacks it with Upscaling's detour). Before
  the call it reads the queued jobs' heads under the queue's lock. After it, a main-thread SKSE task walks the player's third- and
  first-person 3D, runs `MaterialPort::Capture` on each geometry's material and, for a material with a texture field naming one of
  the heads, calls `NoteWritten` (the existing recapture path). The scene work then sees the new view (`SnapshotChange::Fields`)
  and rewrites the record. The task is needed because the loading loop calls UpdatePending on another thread.
- General part: `NoteTexturesReplaced(root, textures)` works for any material class and texture field the capture knows. Only the
  face tint swap feeds it today.
- Native is unchanged: both UpdatePending calls are removed there (decision 129), and the hook is not installed (`Native::Requested()`).

## Runs (stage `s7-texinit`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| guest+facegen | s7-texinit-a-guest-facegen | passed; race menu face has the real skin tint (was flat dark grey); 4 swaps, each found the player's face material |
| native-vulkan+facegen | s7-texinit-b-nvk-facegen | passed; no noise, 4 bakes done (quarter-frame framing regression, snap3-quarter, ignored) |
| guest+quick | s7-texinit-c-guest-quick | passed |
| native-vulkan+quick | s7-texinit-c-nvk-quick | passed |
| native-d3d12-quick | s7-texinit-c-d3d12-quick | passed; same zero counts as Vulkan |
| native-vulkan+native-tour | s7-texinit-c-nvk-tour | passed (the menu-over-world shot shows snap3-quarter's quarter frame) |
| native-vulkan+cadence-drop-cells | s7-texinit-c-nvk-dropcells | passed, first try |

Detector: no engine render call added or removed in native (the hook is guest only, the zeroing is device work).

## Decisions (agent, to confirm)

- **TI-a.** The zero goes into the texture's existing creation submission rather than a batch flushed later. Why: it costs no
  submit and keeps ordering trivially right; a deferred batch would need its own ordering against the ledger and the context.
- **TI-b.** Render targets, depth and UAV textures are cleared; only textures without such a view are copied from a zero buffer.
  Why: clears avoid PCIe reads of hundreds of MB at startup.
- **TI-c.** The guest refresh finds face materials by walking the player's 3D. Why: a tint job only names its head texture, there
  is no back reference to materials, and DCLF's held snapshots age out after 8 frames. NPC tint jobs (actors without FaceGen data)
  are not refreshed; the log line says how many materials were found per swap.
- **TI-d.** FaceTint's render-side head clear is removed as dead after (a). Separate commit, so it can be dropped if it conflicts
  with snap3-jobhang.

## Open items

- NPC tint jobs in guest (runtime FaceGen for actors without FaceGen files) would need the actor; not covered.
- Staging textures made without contents are still not zeroed (CPU buffers; D3D11 leaves them undefined too).
