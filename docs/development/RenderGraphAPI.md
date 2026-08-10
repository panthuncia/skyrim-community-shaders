# Render graph contributor API v2

Community Shaders owns the BasicRHI device, queues, interoperability timelines, descriptors, uploads, and OpenRenderGraph runtime. External modules query `CS_GetRenderGraphAPI` and compile against `CommunityShaders/RenderGraphAPI.h`; no backend SDK header is required.

## Contributor model

Native CS contributors install real ORG extensions, resource providers, and render/compute/copy pass objects. External DLL contributors declare the equivalent structural resources, passes, usages, views, bindings, and ordering through the C ABI. CS adapts those declarations into ORG proxy passes and compiles both frontends into one immutable graph candidate.

Registration order and numeric handles have no scheduling meaning. Resource IDs and explicit ordering constraints are resolved globally. A candidate is activated only after validation, ORG structural compilation, setup, and contributor preparation succeed.

## Portable execution surface

The former queried D3D12 extension has been removed. Contributors cannot obtain a device, command list, queue, native resource, descriptor heap, CPU descriptor handle, fence, or timeline.

The portable surface consists of:

- exact declarative ORG resource usages and views;
- binding metadata and shader-visible descriptor indices;
- frame and generation metadata;
- host-owned scheduled uploads through `CS_GetGPUServiceAPI`;
- opaque shader-service artifacts when that optional capability is installed.

C contributors may create transient or persistent managed resources. Engine and interoperability resources are supplied by native host resource providers and consumed by symbolic ID. A DLL cannot inject a backend-native resource. This keeps ownership, state refresh, and retirement in the host.

The v2 DLL execution callback currently has no portable command encoder. It is suitable for validation and lifecycle participation but cannot record backend commands. A future command-recording ABI must use opaque pipelines and backend-neutral commands; it must not reintroduce borrowed native objects. Native modules that need GPU recording use ORG pass execution and BasicRHI directly.

## Scheduling and synchronization

The public anchors are structural ORG passes:

- `cs.frame.begin`
- `cs.shadows.ready`
- `cs.gbuffer.ready`
- `cs.deferred-lighting.begin`
- `cs.deferred-lighting.end`
- `cs.frame.end`

ORG owns queue assignment, transitions, UAV ordering, cross-queue dependencies, and final completion. Contributors never submit work or issue queue waits and signals. D3D11 readiness and the final D3D11 compatibility handoff are native host boundaries.

## Generations and DLL lifetime

Registration and rebuild requests may originate off the render thread. CS snapshots registrations and invokes structural build callbacks at a render boundary. Build handles are callback-scoped; declarations are copied by the host.

`BeginUnregister` starts asynchronous removal. A DLL must remain loaded until `GetRegistrationState` returns `CS_RG_REGISTRATION_RETIRED`. Pass cleanup and contributor retirement occur only after the generation's exact graph completion value retires. Device loss disables the modern path while preserving Skyrim's D3D11 compatibility result.

## Optional services

`ORGModuleServices` remains optional. Scheduled uploads are implemented by ORG and do not expose staging allocations. Shader compilation is capability-queried. Raw upload memory, descriptor allocation, native heaps, GPU addresses, and native pipeline recipes are intentionally absent from the public ABI.
