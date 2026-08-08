# DX12 graph contributor API v2

Community Shaders owns the D3D12 device, queues, interop timelines, and OpenRenderGraph runtime. A module obtains `CS_GetDX12GraphAPI` with `GetProcAddress` and compiles against only `include/CommunityShaders/DX12GraphAPI.h`. Version 1 was experimental and is rejected.

## Build and execution

Registration and rebuild requests may originate on any thread. CS snapshots registrations and calls build callbacks serially on Skyrim's render thread at the deferred boundary. Build handles and resource lookup handles are valid only during that callback. Strings and descriptors are copied before it returns.

Execution callbacks receive a borrowed `CSDX12ExecutionContext`. Callbacks are serialized unless a pass declares `CS_DX12_PASS_PARALLEL_RECORDING_SAFE`; that flag is a promise that the callback and its `userData` are thread-safe. A failure status aborts ORG recording and is never treated as a diagnostic-only result.

Modules must not retain, release, submit, signal, or wait on borrowed D3D12 objects. Upload and descriptor allocations belong to the current submission and remain valid until its exact completion value retires. Normal frame execution uses GPU queue waits and performs no CPU fence wait.

## Scheduling

The public anchors are real structural ORG passes:

- `cs.frame.begin`
- `cs.shadows.ready`
- `cs.gbuffer.ready`
- `cs.deferred-lighting.begin`
- `cs.deferred-lighting.end`
- `cs.frame.end`

Pass `after` and `before` declarations become structural DAG edges. ORG is authoritative for queue assignment, barriers, and cross-queue signals/waits. `AUTOMATIC`, `PREFER_*`, and `REQUIRE_*` are distinct policies; required queue failure rejects a required contributor or omits an optional one.

## Generations and DLL lifetime

Candidate ORG graphs compile separately from the active graph. Activation is atomic after successful validation and compile. The previous compiled graph and all frame allocations remain alive until their recorded completion values pass. Generation activation and retirement callbacks delimit code/data lifetime.

`UnregisterContributor` begins asynchronous removal. A plugin must remain loaded until `IsRegistrationRetired` reports true; unloading sooner can leave callbacks referenced by an in-flight generation. Device loss stops DX12 submission, reports diagnostics once, and leaves Skyrim's D3D11 fallback active.

## Resources and optional services

The ABI distinguishes transient, runtime-persistent, CS-imported, and contributor-imported resources. A native import is the advanced escape hatch: it must provide state and ownership metadata and still participate in the graph. Contributors never submit their own command lists.

`ORGModuleServices` is optional and independent of OpenRenderGraph. When installed, CS advertises frame uploads and fence-retired descriptor allocation. Shader and pipeline service capabilities must be queried before use. A CS build without the package retains core scheduling and native contributors; the clustered-culling proof is disabled because it requires frame-safe allocations.
