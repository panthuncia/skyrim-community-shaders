#ifndef COMMUNITY_SHADERS_RENDER_GRAPH_API_H
#define COMMUNITY_SHADERS_RENDER_GRAPH_API_H

#include <stdint.h>

#if defined(_WIN32)
#define CS_RG_CALL __cdecl
#else
#define CS_RG_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CS_RENDER_GRAPH_API_VERSION_2 2u
#define CS_RENDER_GRAPH_API_CURRENT CS_RENDER_GRAPH_API_VERSION_2

#define CS_RG_ANCHOR_FRAME_BEGIN "cs.frame.begin"
#define CS_RG_ANCHOR_SHADOWS_READY "cs.shadows.ready"
#define CS_RG_ANCHOR_GBUFFER_READY "cs.gbuffer.ready"
#define CS_RG_ANCHOR_DEFERRED_LIGHTING_BEGIN "cs.deferred-lighting.begin"
#define CS_RG_ANCHOR_DEFERRED_LIGHTING_END "cs.deferred-lighting.end"
#define CS_RG_ANCHOR_FRAME_END "cs.frame.end"

typedef uint64_t CSRGRegistrationHandle;
typedef uint64_t CSRGBuildHandle;
typedef uint64_t CSRGGenerationHandle;
typedef uint32_t CSRGBinding;

typedef enum CSRGStatus {
	CS_RG_OK = 0,
	CS_RG_E_INVALID_ARGUMENT = 1,
	CS_RG_E_UNSUPPORTED_VERSION = 2,
	CS_RG_E_RUNTIME_UNAVAILABLE = 3,
	CS_RG_E_STALE_HANDLE = 4,
	CS_RG_E_DUPLICATE_ID = 5,
	CS_RG_E_RESERVED_ID = 6,
	CS_RG_E_MISSING_DEPENDENCY = 7,
	CS_RG_E_CYCLE = 8,
	CS_RG_E_INCOMPATIBLE_RESOURCE = 9,
	CS_RG_E_WRONG_THREAD = 10,
	CS_RG_E_CALLBACK_FAILED = 11,
	CS_RG_E_CLOSED = 12,
	CS_RG_E_NOT_READY = 13,
	CS_RG_E_UNSUPPORTED_CAPABILITY = 14,
	CS_RG_E_DUPLICATE_BINDING = 15,
	CS_RG_E_INTERNAL = 0x7fffffff
} CSRGStatus;

typedef enum CSRGBackend {
	CS_RG_BACKEND_NONE = 0,
	CS_RG_BACKEND_D3D12 = 1,
	CS_RG_BACKEND_VULKAN = 2
} CSRGBackend;

typedef enum CSRGContributorKind {
	CS_RG_CONTRIBUTOR_REQUIRED = 0,
	CS_RG_CONTRIBUTOR_OPTIONAL = 1,
	CS_RG_CONTRIBUTOR_DIAGNOSTIC = 2
} CSRGContributorKind;

typedef enum CSRGRegistrationState {
	CS_RG_REGISTRATION_ACTIVE = 0,
	CS_RG_REGISTRATION_UNREGISTERING = 1,
	CS_RG_REGISTRATION_RETIRED = 2
} CSRGRegistrationState;

typedef enum CSRGPassKind {
	CS_RG_PASS_RENDER = 0,
	CS_RG_PASS_COMPUTE = 1,
	CS_RG_PASS_COPY = 2
} CSRGPassKind;

typedef enum CSRGQueueAssignment {
	CS_RG_QUEUE_AUTOMATIC = 0,
	CS_RG_QUEUE_FORCE_GRAPHICS = 1,
	CS_RG_QUEUE_FORCE_COMPUTE = 2,
	CS_RG_QUEUE_FORCE_COPY = 3
} CSRGQueueAssignment;

typedef enum CSRGResourceLifetime {
	CS_RG_RESOURCE_TRANSIENT = 0,
	CS_RG_RESOURCE_PERSISTENT = 1
} CSRGResourceLifetime;

typedef enum CSRGResourceDimension {
	CS_RG_RESOURCE_BUFFER = 0,
	CS_RG_RESOURCE_TEXTURE_1D = 1,
	CS_RG_RESOURCE_TEXTURE_2D = 2,
	CS_RG_RESOURCE_TEXTURE_3D = 3
} CSRGResourceDimension;

typedef enum CSRGHeapClass {
	CS_RG_HEAP_DEVICE_LOCAL = 0,
	CS_RG_HEAP_UPLOAD = 1,
	CS_RG_HEAP_READBACK = 2
} CSRGHeapClass;

typedef enum CSRGSizingMode {
	CS_RG_SIZE_ABSOLUTE = 0,
	CS_RG_SIZE_RENDER_RELATIVE = 1,
	CS_RG_SIZE_DISPLAY_RELATIVE = 2
} CSRGSizingMode;

/* Values are the stable, backend-neutral BasicRHI format ordinals. */
typedef enum CSRGFormat {
	CS_RG_FORMAT_UNKNOWN = 0,
	CS_RG_FORMAT_R32G32B32A32_TYPELESS, CS_RG_FORMAT_R32G32B32A32_FLOAT, CS_RG_FORMAT_R32G32B32A32_UINT, CS_RG_FORMAT_R32G32B32A32_SINT,
	CS_RG_FORMAT_R32G32B32_TYPELESS, CS_RG_FORMAT_R32G32B32_FLOAT, CS_RG_FORMAT_R32G32B32_UINT, CS_RG_FORMAT_R32G32B32_SINT,
	CS_RG_FORMAT_R16G16B16A16_TYPELESS, CS_RG_FORMAT_R16G16B16A16_FLOAT, CS_RG_FORMAT_R16G16B16A16_UNORM, CS_RG_FORMAT_R16G16B16A16_UINT, CS_RG_FORMAT_R16G16B16A16_SNORM, CS_RG_FORMAT_R16G16B16A16_SINT,
	CS_RG_FORMAT_R32G32_TYPELESS, CS_RG_FORMAT_R32G32_FLOAT, CS_RG_FORMAT_R32G32_UINT, CS_RG_FORMAT_R32G32_SINT,
	CS_RG_FORMAT_R10G10B10A2_TYPELESS, CS_RG_FORMAT_R10G10B10A2_UNORM, CS_RG_FORMAT_R10G10B10A2_UINT,
	CS_RG_FORMAT_R11G11B10_FLOAT,
	CS_RG_FORMAT_R8G8B8A8_TYPELESS, CS_RG_FORMAT_R8G8B8A8_UNORM, CS_RG_FORMAT_R8G8B8A8_UNORM_SRGB, CS_RG_FORMAT_R8G8B8A8_UINT, CS_RG_FORMAT_R8G8B8A8_SNORM, CS_RG_FORMAT_R8G8B8A8_SINT,
	CS_RG_FORMAT_R16G16_TYPELESS, CS_RG_FORMAT_R16G16_FLOAT, CS_RG_FORMAT_R16G16_UNORM, CS_RG_FORMAT_R16G16_UINT, CS_RG_FORMAT_R16G16_SNORM, CS_RG_FORMAT_R16G16_SINT,
	CS_RG_FORMAT_R32_TYPELESS, CS_RG_FORMAT_D32_FLOAT, CS_RG_FORMAT_R32_FLOAT, CS_RG_FORMAT_R32_UINT, CS_RG_FORMAT_R32_SINT,
	CS_RG_FORMAT_R8G8_TYPELESS, CS_RG_FORMAT_R8G8_UNORM, CS_RG_FORMAT_R8G8_UINT, CS_RG_FORMAT_R8G8_SNORM, CS_RG_FORMAT_R8G8_SINT,
	CS_RG_FORMAT_R16_TYPELESS, CS_RG_FORMAT_R16_FLOAT, CS_RG_FORMAT_R16_UNORM, CS_RG_FORMAT_R16_UINT, CS_RG_FORMAT_R16_SNORM, CS_RG_FORMAT_R16_SINT,
	CS_RG_FORMAT_R8_TYPELESS, CS_RG_FORMAT_R8_UNORM, CS_RG_FORMAT_R8_UINT, CS_RG_FORMAT_R8_SNORM, CS_RG_FORMAT_R8_SINT,
	CS_RG_FORMAT_BC1_TYPELESS, CS_RG_FORMAT_BC1_UNORM, CS_RG_FORMAT_BC1_UNORM_SRGB,
	CS_RG_FORMAT_BC2_TYPELESS, CS_RG_FORMAT_BC2_UNORM, CS_RG_FORMAT_BC2_UNORM_SRGB,
	CS_RG_FORMAT_BC3_TYPELESS, CS_RG_FORMAT_BC3_UNORM, CS_RG_FORMAT_BC3_UNORM_SRGB,
	CS_RG_FORMAT_BC4_TYPELESS, CS_RG_FORMAT_BC4_UNORM, CS_RG_FORMAT_BC4_SNORM,
	CS_RG_FORMAT_BC5_TYPELESS, CS_RG_FORMAT_BC5_UNORM, CS_RG_FORMAT_BC5_SNORM,
	CS_RG_FORMAT_B8G8R8A8_TYPELESS, CS_RG_FORMAT_B8G8R8A8_UNORM, CS_RG_FORMAT_B8G8R8A8_UNORM_SRGB,
	CS_RG_FORMAT_BC6H_TYPELESS, CS_RG_FORMAT_BC6H_UF16, CS_RG_FORMAT_BC6H_SF16,
	CS_RG_FORMAT_BC7_TYPELESS, CS_RG_FORMAT_BC7_UNORM, CS_RG_FORMAT_BC7_UNORM_SRGB,
	CS_RG_FORMAT_COUNT
} CSRGFormat;

typedef enum CSRGResourceUsageBits {
	CS_RG_USAGE_NONE = 0,
	CS_RG_USAGE_SHADER_RESOURCE = 1u << 0,
	CS_RG_USAGE_CONSTANT_BUFFER = 1u << 1,
	CS_RG_USAGE_UNORDERED_ACCESS = 1u << 2,
	CS_RG_USAGE_RENDER_TARGET = 1u << 3,
	CS_RG_USAGE_DEPTH_READ = 1u << 4,
	CS_RG_USAGE_DEPTH_WRITE = 1u << 5,
	CS_RG_USAGE_COPY_SOURCE = 1u << 6,
	CS_RG_USAGE_COPY_DESTINATION = 1u << 7,
	CS_RG_USAGE_INDIRECT_ARGUMENT = 1u << 8,
	CS_RG_USAGE_INDEX_BUFFER = 1u << 9,
	CS_RG_USAGE_LEGACY_INTEROP = 1u << 10
} CSRGResourceUsageBits;

typedef enum CSRGAccessKind {
	CS_RG_ACCESS_NONE = 0,
	CS_RG_ACCESS_SHADER_RESOURCE = 1,
	CS_RG_ACCESS_CONSTANT_BUFFER = 2,
	CS_RG_ACCESS_UNORDERED_ACCESS = 3,
	CS_RG_ACCESS_UNORDERED_ACCESS_CLEAR = 4,
	CS_RG_ACCESS_RENDER_TARGET = 5,
	CS_RG_ACCESS_RENDER_TARGET_CLEAR = 6,
	CS_RG_ACCESS_DEPTH_READ = 7,
	CS_RG_ACCESS_DEPTH_READ_WRITE = 8,
	CS_RG_ACCESS_DEPTH_STENCIL_CLEAR = 9,
	CS_RG_ACCESS_COPY_SOURCE = 10,
	CS_RG_ACCESS_COPY_DESTINATION = 11,
	CS_RG_ACCESS_INDIRECT_ARGUMENT = 12,
	CS_RG_ACCESS_INDEX_BUFFER = 13,
	CS_RG_ACCESS_LEGACY_INTEROP = 14
} CSRGAccessKind;

typedef enum CSRGViewKind {
	CS_RG_VIEW_NONE = 0,
	CS_RG_VIEW_SHADER_RESOURCE = 1,
	CS_RG_VIEW_CONSTANT_BUFFER = 2,
	CS_RG_VIEW_UNORDERED_ACCESS = 3,
	CS_RG_VIEW_RENDER_TARGET = 4,
	CS_RG_VIEW_DEPTH_STENCIL = 5
} CSRGViewKind;

typedef enum CSRGViewDimension {
	CS_RG_VIEW_DIMENSION_DEFAULT = 0,
	CS_RG_VIEW_DIMENSION_BUFFER = 1,
	CS_RG_VIEW_DIMENSION_TEXTURE_1D = 2,
	CS_RG_VIEW_DIMENSION_TEXTURE_1D_ARRAY = 3,
	CS_RG_VIEW_DIMENSION_TEXTURE_2D = 4,
	CS_RG_VIEW_DIMENSION_TEXTURE_2D_ARRAY = 5,
	CS_RG_VIEW_DIMENSION_TEXTURE_3D = 6,
	CS_RG_VIEW_DIMENSION_TEXTURE_CUBE = 7,
	CS_RG_VIEW_DIMENSION_TEXTURE_CUBE_ARRAY = 8
} CSRGViewDimension;

typedef enum CSRGViewFlags {
	CS_RG_VIEW_FLAG_NONE = 0,
	CS_RG_VIEW_FLAG_RAW_BUFFER = 1u << 0,
	CS_RG_VIEW_FLAG_READ_ONLY_DEPTH = 1u << 1,
	CS_RG_VIEW_FLAG_READ_ONLY_STENCIL = 1u << 2
} CSRGViewFlags;

typedef enum CSRGPassFlags {
	CS_RG_PASS_NONE = 0,
	CS_RG_PASS_PARALLEL_RECORDING_SAFE = 1u << 0,
	CS_RG_PASS_DISABLE_STATISTICS = 1u << 1,
	CS_RG_PASS_GEOMETRY = 1u << 2
} CSRGPassFlags;

typedef enum CSRGCapabilityBits {
	CS_RG_CAP_RENDER_PASSES = 1ull << 0,
	CS_RG_CAP_COMPUTE_PASSES = 1ull << 1,
	CS_RG_CAP_COPY_PASSES = 1ull << 2,
	CS_RG_CAP_ASYNC_COMPUTE = 1ull << 3,
	CS_RG_CAP_COPY_QUEUE = 1ull << 4,
	CS_RG_CAP_MANAGED_RESOURCES = 1ull << 5,
	CS_RG_CAP_RESOURCE_ALIASING = 1ull << 6,
	CS_RG_CAP_GPU_SERVICES = 1ull << 8
} CSRGCapabilityBits;

typedef struct CSRGSubresourceRange {
	uint32_t firstMip;
	uint32_t mipCount;       /* UINT32_MAX means all remaining mips. */
	uint32_t firstArraySlice;
	uint32_t arraySize;      /* UINT32_MAX means all remaining slices. */
} CSRGSubresourceRange;

typedef struct CSRGResourceDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* id;
	CSRGResourceLifetime lifetime;
	CSRGResourceDimension dimension;
	CSRGHeapClass heapClass;
	CSRGSizingMode sizing;
	CSRGFormat format;
	uint64_t byteSize;
	uint32_t structureByteStride;
	uint32_t width;
	uint32_t height;
	uint32_t depthOrArraySize;
	uint32_t mipLevels;
	uint32_t sampleCount;
	float widthScale;
	float heightScale;
	uint32_t allowedUsages;
	uint32_t allowAlias;
	uint64_t aliasingPool;
} CSRGResourceDesc;

typedef struct CSRGResourceAccessDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* resourceId;
	CSRGBinding binding;
	CSRGAccessKind access;
	CSRGSubresourceRange range;
	CSRGViewKind viewKind;
	CSRGViewDimension viewDimension;
	CSRGFormat viewFormat;       /* UNKNOWN inherits the resource format. */
	uint32_t viewFlags;
	uint64_t firstElement;
	uint32_t elementCount;       /* UINT32_MAX means all remaining elements. */
	uint32_t structureByteStride;
	CSRGBinding counterBinding;  /* 0 means no associated UAV counter. */
} CSRGResourceAccessDesc;

typedef struct CSRGBindingInfo {
	uint32_t structSize;
	uint32_t apiVersion;
	CSRGBinding binding;
	CSRGAccessKind access;
	CSRGViewKind viewKind;
	CSRGResourceDimension dimension;
	CSRGFormat resourceFormat;
	CSRGFormat viewFormat;
	uint32_t width;
	uint32_t height;
	uint32_t depthOrArraySize;
	uint32_t mipLevels;
	uint64_t byteSize;
	uint32_t descriptorIndex;    /* UINT32_MAX when the binding has no shader-visible view. */
	uint32_t shaderVisible;
} CSRGBindingInfo;

typedef struct CSRGFrameInfo {
	uint32_t structSize;
	uint32_t apiVersion;
	uint64_t frameIndex;
	CSRGGenerationHandle generation;
	uint64_t completionValue;
	uint32_t frameSlot;
	uint32_t framesInFlight;
	uint32_t renderWidth;
	uint32_t renderHeight;
	uint32_t displayWidth;
	uint32_t displayHeight;
} CSRGFrameInfo;

typedef struct CSRGExecutionContext {
	uint32_t structSize;
	uint32_t apiVersion;
	CSRGBackend backend;
	uint32_t reserved;
	const CSRGFrameInfo* frame;
	const void* hostContext;
} CSRGExecutionContext;

typedef CSRGStatus(CS_RG_CALL* CSRGExecuteCallback)(void*, const CSRGExecutionContext*);
typedef CSRGStatus(CS_RG_CALL* CSRGPrepareCallback)(void*, const CSRGExecutionContext*);
typedef CSRGStatus(CS_RG_CALL* CSRGUpdateCallback)(void*, const CSRGExecutionContext*);
typedef void(CS_RG_CALL* CSRGCleanupCallback)(void*, CSRGGenerationHandle);

typedef struct CSRGPassDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* id;
	CSRGPassKind kind;
	CSRGQueueAssignment queue;
	uint32_t flags;
	int32_t priority;
	const char* techniquePath;
	const char* const* featureDomains;
	uint32_t featureDomainCount;
	const char* const* after;
	uint32_t afterCount;
	const char* const* before;
	uint32_t beforeCount;
	const CSRGResourceAccessDesc* accesses;
	uint32_t accessCount;
	CSRGPrepareCallback prepare;
	CSRGUpdateCallback update;
	CSRGExecuteCallback execute;
	CSRGCleanupCallback cleanup;
} CSRGPassDesc;

typedef CSRGStatus(CS_RG_CALL* CSRGBuildCallback)(void*, CSRGBuildHandle);
typedef void(CS_RG_CALL* CSRGGenerationCallback)(void*, CSRGGenerationHandle);
typedef void(CS_RG_CALL* CSRGDeviceLostCallback)(void*, uint32_t);
typedef void(CS_RG_CALL* CSRGShutdownCallback)(void*);

typedef struct CSRGContributorDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* id;
	CSRGContributorKind kind;
	void* userData;
	CSRGBuildCallback build;
	CSRGGenerationCallback generationActivated;
	CSRGGenerationCallback generationRetired;
	CSRGDeviceLostCallback deviceLost;
	CSRGShutdownCallback shutdown;
} CSRGContributorDesc;

typedef struct CSRGRuntimeInfo {
	uint32_t structSize;
	uint32_t apiVersion;
	uint32_t available;
	CSRGBackend backend;
	uint64_t capabilities;
	CSRGGenerationHandle activeGeneration;
	uint64_t lastSubmittedCompletion;
	uint64_t completedFence;
	uint32_t framesInFlight;
	uint32_t reserved;
} CSRGRuntimeInfo;

typedef struct CSRGDiagnostic {
	uint32_t structSize;
	uint32_t apiVersion;
	CSRGStatus status;
	uint32_t phase;
	uint64_t sequence;
	CSRGGenerationHandle generation;
	CSRGRegistrationHandle contributor;
	char message[512];
} CSRGDiagnostic;

typedef struct CSRenderGraphAPI {
	uint32_t structSize;
	uint32_t apiVersion;
	CSRGStatus(CS_RG_CALL* GetRuntimeInfo)(CSRGRuntimeInfo*);
	CSRGStatus(CS_RG_CALL* RegisterContributor)(const CSRGContributorDesc*, CSRGRegistrationHandle*);
	CSRGStatus(CS_RG_CALL* BeginUnregister)(CSRGRegistrationHandle);
	CSRGStatus(CS_RG_CALL* GetRegistrationState)(CSRGRegistrationHandle, CSRGRegistrationState*);
	CSRGStatus(CS_RG_CALL* DeclareResource)(CSRGBuildHandle, const CSRGResourceDesc*);
	CSRGStatus(CS_RG_CALL* DeclarePass)(CSRGBuildHandle, const CSRGPassDesc*);
	CSRGStatus(CS_RG_CALL* RequestRebuild)(CSRGRegistrationHandle);
	CSRGStatus(CS_RG_CALL* GetDiagnostic)(CSRGRegistrationHandle, CSRGDiagnostic*);
	CSRGStatus(CS_RG_CALL* GetBindingInfo)(const CSRGExecutionContext*, CSRGBinding, CSRGBindingInfo*);
	const char*(CS_RG_CALL* StatusString)(CSRGStatus);
} CSRenderGraphAPI;

#if defined(_WIN32) && defined(CS_RENDER_GRAPH_EXPORTS)
#define CS_RG_API __declspec(dllexport)
#elif defined(_WIN32)
#define CS_RG_API __declspec(dllimport)
#else
#define CS_RG_API
#endif
CSRGStatus CS_RG_API CS_RG_CALL CS_GetRenderGraphAPI(uint32_t version, CSRenderGraphAPI* out);

#ifdef __cplusplus
}
#endif
#undef CS_RG_API

#endif
