# OpenRenderGraph contributor API

Community Shaders is an OpenRenderGraph host. External modules discover the standard `ORG_GetRenderGraphAPI` export and compile against `OpenRenderGraph/ContributorAPI.h`. Include `CommunityShaders/RenderGraphHost.h` only when targeting Community Shaders identity or its ordered `cs.*` anchors.

ORG owns contributor registration, validation, immutable graph generations, managed resources and views, uploads, diagnostics, activation, and exact-completion retirement. Community Shaders supplies its host metadata, Skyrim frame placement, D3D11 resources, and native deferred-rendering extensions.

## Portable execution surface

Contributors declare resources, passes, usages, views, bindings, and symbolic ordering through the C ABI. They cannot obtain native devices, command lists, queues, descriptor heaps, resources, fences, or timelines. The current lifecycle callbacks deliberately do not expose a command encoder.

Scheduled buffer uploads use `ORGRenderGraphAPI.QueueBufferUpload` and the active graph's `IUploadService`. Shader compilation is optional: query `org.shader-compiler` version 1 with `QueryService` and `ORGShaderCompilerServiceAPI` from `OpenRenderGraph/ShaderCompilerService.h`.

## Community Shaders anchors

- `cs.frame.begin`
- `cs.shadows.ready`
- `cs.gbuffer.ready`
- `cs.deferred-lighting.begin`
- `cs.deferred-lighting.end`
- `cs.frame.end`

The runtime returns these in order through `GetAnchorCount` and `GetAnchor`; it also reports host ID `community-shaders` through `GetHostInfo`.

## Scheduling and lifetime

Registration order and numeric handles have no scheduling meaning. ORG's dependency compiler is authoritative for pass order, cycles, queue assignment, transitions, UAV ordering, and cross-queue synchronization. A replacement generation activates only after successful validation and structural compilation; a failed replacement leaves the previous graph active.

`BeginUnregister` is asynchronous. Keep contributor code loaded until `GetRegistrationState` reports `ORG_RG_REGISTRATION_RETIRED`. Resources, views, callbacks, native extensions, and DLL code remain alive until their exact generation completion value retires.
