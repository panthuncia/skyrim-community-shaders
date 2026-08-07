# DX12 graph contributor API

External plugins obtain `CS_GetDX12GraphAPI` with `GetProcAddress` from
`CommunityShaders.dll`. They compile against only
`include/CommunityShaders/DX12GraphAPI.h`; BasicRHI and OpenRenderGraph are
private implementation dependencies.

Registration may occur on any thread. Registration changes are queued as a
rebuild request. Build callbacks are invoked serially on Skyrim's render thread
at the next safe deferred boundary. Descriptors and strings are copied during
the callback; build handles expire when it returns. Execution callbacks may be
scheduled concurrently in later API versions and therefore must be thread-safe.

Identifiers must be UTF-8 and namespaced. External plugins own identifiers
beneath their contributor ID. The `cs.*` namespace and these anchors are
reserved to Community Shaders:

- `cs.frame.begin`
- `cs.shadows.ready`
- `cs.gbuffer.ready`
- `cs.deferred-lighting.begin`
- `cs.deferred-lighting.end`
- `cs.frame.end`

Native device, command-list, and resource pointers are borrowed. Callers must
not retain or release them, submit the command list, or signal/wait host fences.
An unsuccessful required contributor build rejects the candidate generation;
optional and diagnostic contributors are omitted. The previous generation stays
active until a candidate validates successfully.
