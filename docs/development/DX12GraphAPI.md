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

## D3D11/D3D12 texture interop

Prefer a single producer-owned NT-shared allocation. A D3D11 producer should create its texture with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` (and the actual RTV/SRV/UAV bind flags it needs), export it through `IDXGIResource1::CreateSharedHandle`, and let CS open that allocation on D3D12. A D3D12 producer should use a shared heap and `D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS`, then let D3D11 open the exported NT handle. Do not create two independently updated textures merely to cross the API boundary when the producer allocation can be shared.

Every imported texture read must be declared on the consuming ORG pass. CS attaches the current D3D11-readiness timeline value to the first consuming batch at execution time. ORG applies that queue wait before its transitions and command list, and the D3D12-to-D3D11 completion timeline is signaled only after all contributing queues complete. Contributors must never cache a concrete per-frame fence value in a compiled graph, issue their own queue waits, or rely on pass vector order.

Use a mirror only when the original engine allocation cannot be shared (unsupported flags, samples, format, or ownership). Queue the D3D11 copy before the readiness signal and import the mirror as a normal declared resource. For D3D12 output, prefer a D3D12-owned shared texture and perform the minimum D3D11 handoff needed by Skyrim. The deferred path, for example, preserves unsupported pixels in Skyrim's main target and selectively overwrites promoted pixels instead of round-tripping the entire compatibility image.
