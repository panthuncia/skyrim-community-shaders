# Epoch resource synchronization: implementation status

This is an **incomplete implementation**, not a validated performance fix. The
existing epoch entry/exit barriers are still active. Do not remove them until the
resource-aware DXVK handoff and complete CS access declarations are connected and
validated. The final design removes the old broad-barrier path entirely.

## Implemented foundations

- ORG `ExecutionBoundaryManifest` retains concrete backing handles/generations,
  access order, queue slot, buffer extents, image subresources/aspects, and leases.
  `CompileStateUse` and persistent `Declare` accept boundary byte extents/aspects.
  Internal graph state scheduling still conservatively treats a buffer as one
  state cell. Graph replay version 3 preserves the added access metadata and
  continues reading versions 1 and 2.
- Persistent hosts can install a `BoundaryManifestRecorder`. Only hosts using
  that callback allocate manifests during recording. The current CS runtime has
  not installed it.
- BasicRHI can attach resolved native resource accesses to recorded command
  lists and deliver them through `QueueSubmissionHooks::submitResources` with
  their command buffers. Reset clears the manifest. Attachment validates native
  ranges and preserves the previous manifest if validation fails.
- Adopted Vulkan devices using submission hooks emit synchronization2 barriers
  with individual source/destination stages. Resource ranges are preserved;
  independent stage pairs are no longer combined at command scope. Other Vulkan
  devices retain their existing synchronization behavior.
- DXVK's barrier batch accepts scoped buffer barriers and can retain same-layout
  image barriers instead of converting them to global barriers. Current callers
  have not enabled the new scoped-image option.
- `DxvkExternalAccessLedger` implements transactional interval hazard tracking.
  It distinguishes byte/layer intervals and image lanes, preserves outstanding
  writer visibility across readers at different stages, handles partial writes,
  and rejects stale transactions. It is connected to `DxvkContext` for the
  development buffer/image handoff described below. Production activation remains pending.
  Dependency emission now preserves distinct destination stage/access pairs,
  deduplicates identical scopes, and merges ranges only when all synchronization
  requirements match. Historical cells use a monotonic cursor instead of a full
  rescan at every interval split. The randomized ledger suite and the new exact
  destination-scope/adjacent-range tests pass after this refinement.

## Remaining integration

1. Complete the versioned DXVK C ABI with resource manifest submissions and
   require scoped-synchronization capability at CS startup. Stable registration,
   capability negotiation and GPU-retained submissions are implemented; the
   scoped-synchronization capability intentionally remains unset.
2. Finish auditing native DXVK producer/consumer tracking on the CS worker across
   flushes (buffers and GENERAL-layout images are connected). Cover optimized
   graphics, compute, copy, resolve, clear, and input-assembly paths. Prevent
   conflicting initialization/transfer work moving ahead of external epochs.
3. Track final layouts, queue ownership and alias dependencies. Native resource
   destruction now retires ledger identities. The interval ledger handles memory hazards only; callers must
   supply these remaining Vulkan guarantees.
4. Connect the pooled boundary command buffers and ORG manifests to CS through
   BasicRHI's resource-aware hook, preserving epoch boundaries
   in CS batching. Supply manifests for uploads and other nongraph submissions.
5. Complete DCLF bindless/BDA declarations from immutable reachable-resource
   snapshots, including geometry, material textures, descriptor/preprocess
   storage, active shadow slices, and shared LLF resources. Include actual
   outgoing accesses and home-layout postconditions in the handoff contract.
6. Cache manifests and reuse ledger scratch/storage. The current ledger is a
   correctness foundation; its allocations and interval search cost need
   optimization and measurement before hot-path activation.
7. Extend GPU interop tests, run synchronization validation, then deploy matching
   binaries and benchmark with the MO2 runner. No Skyrim performance claim is
   supported by the foundation tests.
8. After validation, delete broad epoch barriers, their switches, and compatibility
   paths; update the operational documentation and final deployed binaries.

## Verification entry points

- `OpenRenderGraphExecutionBoundaryTests`: metadata, backing generation, lease
  lifetime, invalid ranges, replay round-trip, and read-then-write preservation.
- `ExternalAccessLedgerTests` under `tests/OrgDxvkInterop`: RAW/WAR/WAW, disjoint
  ranges/lanes, reader visibility, abandonment, retirement, stale transactions,
  and 2,000 randomized operations against an independent per-byte oracle.
- `BasicRHIVulkanAdoptionSmoke resources`: GPU readback plus native manifest
  delivery, range validation, and interception of the actual synchronization2
  call to verify independent stages and byte ranges. Existing `submit` and
  default lock-mode variants remain available.
- `tools/build-dxvk.ps1 -Required`: builds the modified DXVK barrier code.

Local test build directories are `build/codex-org-tests`,
`build/codex-ledger-tests`, and `build/codex-basicrhi-tests`. Logs are under
`build/codex-gpu-investigation/scoped-*.log`. The standalone ORG test build uses
the existing release vcpkg prefix and a local CMake alias for `flecs::flecs_static`.

Verified on 2026-09-26: full CS `cmake --build --preset Dev`, required DXVK build,
all three BasicRHI adoption variants, the three ORG boundary/persistent/async
suites, and the external ledger suite passed. The ORG suites were rebuilt cleanly
after the declaration API changed. GPU adoption tests used the RTX 3090 Ti.
These are foundation checks; synchronization-layer coverage and Skyrim
performance acceptance remain outstanding.

The earlier GPU investigation and its measurements remain in
`build/codex-gpu-investigation/investigation.md`. No new binaries have been
deployed to Skyrim as part of this foundation work.

## GPU integration validation (2026-09-26)

Added `OrgDxvkInteropTest.resources`: eight real graph executions on DXVK's
Vulkan device attach the compiled boundary manifest through BasicRHI and check
native buffer identity/range and compute-write scopes at submission. Every
execution is read back through D3D11. Nongraph upload manifests remain incomplete
and this test does not silently treat them as complete.

Added `OrgDxvkInteropTest.scoped`: disables the graph's blanket entry/exit
barriers and records ledger-derived buffer dependencies into DXVK command
buffers with the existing ordered callback interface. The controlled workload
explicitly declares its transfer producer, compute epoch and transfer consumer.
It verifies eight D3D11 readbacks and that both scoped callbacks ran each time.
This is an isolated handoff test, **not production native-access discovery** and
not a replacement for the planned resource-aware submission ABI. It does not
cover asynchronous closed epochs, dynamic bindless/BDA sets or images.

Added `RunSynchronizationValidation.cmake`, enabled with
`ORG_DXVK_TEST_SYNCHRONIZATION_VALIDATION=ON`. It runs the scoped test with
`DXVK_DEBUG=validation` and `VK_LAYER_VALIDATE_SYNC=1`, requires positive evidence
that the Khronos layer loaded, and fails on validation errors or skipped GPU
execution. Logs are isolated in `build/codex-interop-tests/scoped-validation`.

Verified locally on the RTX 3090 Ti: lock, stream, resources and scoped GPU modes,
the explicit scoped synchronization-validation test, the interval ledger suite,
and the existing ORG boundary/persistent/async suites pass. BasicRHI resources
mode also passes with synchronization validation enabled. No validation errors
were reported. The CS production path still uses broad epoch barriers; Skyrim
performance acceptance has **not** been performed and no binaries were deployed.

## Native registration implementation (2026-09-26)

DXVK now exports `dxvkRegisterInteropResource` and
`dxvkUnregisterInteropResource`, with matched declarations in the CS header.
The independently versioned registration returns a client lease token, a
canonical backing token, precise resource/view metadata, graphics queue family,
and legal stage/access scopes for conservative first-registration bootstrap.
It does not yet advertise a resource-aware submission capability.

Registrations of the same native image or buffer handle share a backing token,
including views and buffers suballocated from one VkBuffer. Each registration
retains its own native resource and extent. Tokens are process-wide monotonic;
retiring the last registration and registering the handle again creates a new
backing identity. Duplicate/unknown retirement and incompatible versions fail.
D3D11 device ownership is checked before implementation-specific casts.
Registration performs stabilization/COM resolution; submission is not connected
yet. Imported Vulkan allocations must additionally retain their actual owner
(e.g. the ORG/RHI backing lease); a DXVK wrapper does not own that memory.

DXVK rebuilt successfully. Interop GPU tests now exercise repeated registration,
partial/complete retirement, new generations, image/view canonicalization,
exact mip/layer/aspect ranges, and native retention after D3D11 references are
released. Production CS has not enabled the API. Submission-owned retirement,
ordered bootstrap into the worker ledger, and capability negotiation for the
complete handoff remain required before activation.

## Submission retention and capability interface (2026-09-26)

Implemented `dxvkGetResourceInteropInterface` with versioned capability bits and
borrowed context/function pointers. Its enqueue path performs no COM queries or
resource-description calls. `DxvkOrgInteropLeasedSubmission` copies batch data,
validates all registration tokens before enqueue, and retains native resource
entries independently of client unregister calls. Unsupported submission
extensions, protected/device-group flags and invalid tokens are rejected before
acceptance. The interface advertises registration and retained submission only;
`DXVK_ORG_CAP_SCOPED_SYNCHRONIZATION` remains unset.

DXVK appends a signal of its existing graphics completion timeline to the final
VkSubmitInfo2 in the existing vkQueueSubmit2 call. Its existing finish worker
retires resource leases and invokes onCompleted after that timeline completes.
No additional queue submission or render-thread GPU wait is added. The CS-worker
closure moves its submission into the queue instead of keeping a second lease
copy until command-stream chunk reuse. Managed submission failures become
terminal queue errors; the new error path avoids calling the queue's own
waitForIdle from its submit/finish workers.

The resource registry now removes backing lookup entries on actual last-owner
retirement, including completion-worker retirement. The gated test found a bug
in the initial unregister code: backing shared_ptr use_count did not count
multiple owners of the enclosing resource entry. This has been fixed; a weak
registry link removes expired identities without retaining the device or growing
the lookup indefinitely. Imported ORG-owned allocations still need their own
RHI owner leases in addition to these DXVK wrapper leases.

`OrgDxvkInteropTest.leased` gates GPU work with a timeline semaphore, unregisters
the client lease immediately after enqueue, verifies identity retention while
work is blocked, releases the gate, verifies readback/completion notification,
and checks generation replacement after retirement. It also tests transactional
rejection of mixed valid/invalid tokens and unsupported flags. The validation
runner accepts a test mode and now also runs `leased-validation`. All eight
interop/ledger tests pass, including both required synchronization-validation
runs. DXVK rebuilt from the modified source. No Skyrim binaries were deployed.

Native tracking audit: `commitGraphicsState` can bypass `checkGraphicsHazards`
entirely for ordinary read-only draws. `checkResourceHazards` also skips clean
read-only bindings and resources without graphics stores; `trackUniformBufferBinding`,
`trackBufferViewBinding`, and `trackImageViewBinding` can omit access recording.
`trackDrawBuffer` only lifetime-tracks indirect arguments/counts. Consequently,
adding checks only to `acquireResources`/`releaseResources` is insufficient.
External consumer resolution must run before beginRenderPass, with active shader
binding stage metadata and vertex/index/indirect ranges, while native producer
history must survive endCurrentCommands/flushBarriers. The buffer implementation
below now covers those checks; the production runtime continues to use broad
epoch barriers until the complete contract is implemented.

## Native buffer handoff and manifest integration (2026-09-26)

The development `dxvkEnqueueBufferHandoff` interface accepts one epoch submission
with registered buffer intervals and exact stage/access masks. It validates and
copies the manifest before enqueueing, retains the registration leases through
GPU completion, and performs no COM queries, description queries or GPU waits in
submission. This is not the final general resource interface; scoped capability
remains unset and CS has not enabled it.

On the CS worker, `DxvkContext` keeps a separate interval ledger across native
command-list flushes. New resources bootstrap from their legal producer scopes;
overlapping aliases preserve existing outstanding accesses. Native buffer accesses
update this ledger. Entry dependencies are resolved after the preceding native
flush and recorded into small pooled synchronization2 command buffers, prepended
to the existing external submission. Pool leases retire on the graphics timeline.
This adds command buffers, not queue submissions. There is no unconditional
external exit barrier in this development path.

Native transfer acquisition and shader-state commitment resolve return hazards.
Shader checks include clean descriptors and read-only graphics pipelines, using
actual binding stage metadata. Vertex/index inputs, indirect arguments/counts,
and transform-feedback buffers/counters are included. Conflicting graphics
dependencies suspend rendering before emission. Registered backing handles are
excluded from early initialization/SDMA transfer promotion. This exclusion is
currently conservative at whole-backing granularity.

`OrgDxvkInteropTest.native-buffer` connects the ORG compiled boundary manifest to
BasicRHI's native-access hook and the DXVK manifest API, with graph entry/exit
barriers disabled and no manually inserted native barriers. It rejects incomplete
or undeclared command-list manifests. Eight frames exercise native compute
production, ORG read/write, read-only pixel-shader consumption with repeated
draws, repeated native compute read/write dispatches, and copy/readback. The ORG
shader depends on the input sentinel, so overwriting stale data cannot mask a
missing incoming dependency. The alias-bootstrap unit test checks that
registration does not erase preexisting producer scopes.

DXVK and the interop executable rebuilt successfully. All ten interop/ledger
tests pass, including scoped, leased and native-buffer synchronization-validation
runs. The validation runner requires the Khronos layer and fails on validation
errors. A test-variable shadowing bug that accidentally retained a persistent
lease in every mode was fixed. No Skyrim binaries were deployed, and no frame-time
or CPU-performance acceptance claim is made.

Limitations at the buffer milestone (superseded where noted in the following section):

- Images, layouts, aspect/mip/layer hazards, attachment/deferred-clear/resolve
  integration, cross-queue ownership and aliasing are not connected to this API.
- The development worker registry retains native buffer owners until context
  destruction. Ordered retirement and allocation-generation handling must be
  finished; this retention is unsuitable for streaming production workloads.
- Ledger transactions and shader-binding collection still allocate scratch
  storage. Cache membership and reuse capacity before performance acceptance.
- Batched epoch boundaries, nongraph uploads, complete BDA/bindless reachable
  resource sets, generated-command storage and CS shared-feature declarations
  remain outstanding. Native vertex/index/indirect/transform-feedback tracking
  has code coverage but still needs dedicated GPU readback tests.
- Accepted worker-side failures need full device-error propagation coverage;
  fault-injection tests are still pending.

## Image handoff, batching and resource retirement (2026-09-26)

`dxvkEnqueueResourceHandoff` uses resource-handoff ABI version 2. A batch has one
explicitly complete manifest per VkSubmitInfo2; incomplete manifests, mismatched
counts, invalid lease tokens and ranges outside registered views are rejected
before acceptance. Each manifest contains registered buffer intervals and image
aspect/mip/layer ranges. This version requires GENERAL throughout external image
use and rejects incompatible layouts rather than selecting a broad fallback.
General scoped-synchronization capability is still unset.

Image dependencies use separate aspect/mip lanes and layer intervals. Entry
barriers retain individual stage/access scopes and exact subresources in the
pooled boundary command buffer for each submission. Native deferred clears are
materialized before the existing native flush. Native image shader reads/writes,
resource acquisition, attachment acquisition and transfer paths consult the
ledger. Registered images cannot promote transfers or attachment transitions
ahead of their epoch. Mixed native layouts are split for memory dependencies;
layout-transition ownership outside GENERAL external epochs remains unfinished.

Native resources now own destruction-notification tokens instead of the context
retaining their allocations indefinitely. Final resource destruction queues a
preallocated notice; the CS worker drains notices and retires ledger identities.
There is no resident-resource scan or allocation in the destruction callback.
Image cookies reject recycled native identity state. Multiple live buffer owners
of a shared VkBuffer keep the backing ledger until the last tracked owner dies.
This does not replace the client's allocation leases for externally owned memory.
Shader access collection reuses its vectors; interval transactions still need
allocation/performance optimization.

Accepted preparation failures now enter the managed submission queue's terminal
error path, preventing subsequent GPU work from using partially updated ledger
state. The earlier materialization stage communicates failures to that same path.
Fault-injection coverage for these branches is still required.

`ImageHandoffTests` exercises native deferred clears, external image-to-buffer
readback followed by a clear, native image and buffer copy/readback, and repeated
native compute shader reads with clean descriptors. It alternates single epochs
and two epochs in one batch, verifies the second epoch sees the first one's
writes, rejects out-of-view/incomplete declarations, and repeats the test with
three resource generations. These image tests use explicit Vulkan command buffers;
the existing buffer test covers ORG compiler-to-BasicRHI manifest delivery.

The expanded suite contains twelve tests, including four required Vulkan
synchronization-validation runs; all twelve pass. Both DXVK and the CS Dev/AIO
package rebuilt successfully from the updated source with deployment disabled.
Production activation is **not complete**:
CS's manifests, private-resource registration, upload submissions, immutable
BDA/bindless resource sets, generated-command accesses and cross-queue/alias
contracts still need connection. Dedicated depth/stencil, resolve, graphics-image,
vertex/index/indirect and failure-injection cases remain necessary. No Skyrim
deployment or performance acceptance has been performed for this path.
