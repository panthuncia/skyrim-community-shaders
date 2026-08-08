#ifndef COMMUNITY_SHADERS_DX12_GRAPH_API_H
#define COMMUNITY_SHADERS_DX12_GRAPH_API_H

#include <stdint.h>

#if defined(_WIN32)
#define CS_DX12_GRAPH_CALL __cdecl
#else
#define CS_DX12_GRAPH_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Version 1 was experimental and is intentionally unsupported. */
#define CS_DX12_GRAPH_API_VERSION_2 2u
#define CS_DX12_GRAPH_API_CURRENT CS_DX12_GRAPH_API_VERSION_2

#define CS_DX12_ANCHOR_FRAME_BEGIN "cs.frame.begin"
#define CS_DX12_ANCHOR_SHADOWS_READY "cs.shadows.ready"
#define CS_DX12_ANCHOR_GBUFFER_READY "cs.gbuffer.ready"
#define CS_DX12_ANCHOR_DEFERRED_LIGHTING_BEGIN "cs.deferred-lighting.begin"
#define CS_DX12_ANCHOR_DEFERRED_LIGHTING_END "cs.deferred-lighting.end"
#define CS_DX12_ANCHOR_FRAME_END "cs.frame.end"

typedef uint64_t CSDX12RegistrationHandle;
typedef uint64_t CSDX12BuildHandle;
typedef uint64_t CSDX12ResourceHandle;
typedef uint64_t CSDX12PassHandle;
typedef uint64_t CSDX12GenerationHandle;

typedef enum CSDX12Status {
	CS_DX12_OK = 0,
	CS_DX12_E_INVALID_ARGUMENT = 1,
	CS_DX12_E_UNSUPPORTED_VERSION = 2,
	CS_DX12_E_RUNTIME_UNAVAILABLE = 3,
	CS_DX12_E_STALE_HANDLE = 4,
	CS_DX12_E_DUPLICATE_ID = 5,
	CS_DX12_E_RESERVED_ID = 6,
	CS_DX12_E_MISSING_DEPENDENCY = 7,
	CS_DX12_E_CYCLE = 8,
	CS_DX12_E_INCOMPATIBLE_RESOURCE = 9,
	CS_DX12_E_WRONG_THREAD = 10,
	CS_DX12_E_CALLBACK_FAILED = 11,
	CS_DX12_E_CLOSED = 12,
	CS_DX12_E_NOT_READY = 13,
	CS_DX12_E_UNSUPPORTED_CAPABILITY = 14,
	CS_DX12_E_INTERNAL = 0x7fffffff
} CSDX12Status;

typedef enum CSDX12ContributorKind {
	CS_DX12_CONTRIBUTOR_REQUIRED = 0,
	CS_DX12_CONTRIBUTOR_OPTIONAL = 1,
	CS_DX12_CONTRIBUTOR_DIAGNOSTIC = 2
} CSDX12ContributorKind;

typedef enum CSDX12QueuePolicy {
	CS_DX12_QUEUE_AUTOMATIC = 0,
	CS_DX12_QUEUE_PREFER_GRAPHICS = 1,
	CS_DX12_QUEUE_PREFER_COMPUTE = 2,
	CS_DX12_QUEUE_PREFER_COPY = 3,
	CS_DX12_QUEUE_REQUIRE_GRAPHICS = 4,
	CS_DX12_QUEUE_REQUIRE_COMPUTE = 5,
	CS_DX12_QUEUE_REQUIRE_COPY = 6
} CSDX12QueuePolicy;

typedef enum CSDX12ResourceLifetime {
	CS_DX12_RESOURCE_TRANSIENT = 0,
	CS_DX12_RESOURCE_PERSISTENT = 1,
	CS_DX12_RESOURCE_CS_IMPORTED = 2,
	CS_DX12_RESOURCE_CONTRIBUTOR_IMPORTED = 3
} CSDX12ResourceLifetime;

typedef enum CSDX12ResourceDimension {
	CS_DX12_RESOURCE_BUFFER = 0,
	CS_DX12_RESOURCE_TEXTURE_1D = 1,
	CS_DX12_RESOURCE_TEXTURE_2D = 2,
	CS_DX12_RESOURCE_TEXTURE_3D = 3
} CSDX12ResourceDimension;

typedef enum CSDX12SizingMode {
	CS_DX12_SIZE_ABSOLUTE = 0,
	CS_DX12_SIZE_RENDER_RELATIVE = 1,
	CS_DX12_SIZE_DISPLAY_RELATIVE = 2
} CSDX12SizingMode;

typedef enum CSDX12Access {
	CS_DX12_ACCESS_NONE = 0,
	CS_DX12_ACCESS_SHADER_READ = 1u << 0,
	CS_DX12_ACCESS_CONSTANT_BUFFER = 1u << 1,
	CS_DX12_ACCESS_UNORDERED_WRITE = 1u << 2,
	CS_DX12_ACCESS_RENDER_TARGET = 1u << 3,
	CS_DX12_ACCESS_DEPTH_READ = 1u << 4,
	CS_DX12_ACCESS_DEPTH_WRITE = 1u << 5,
	CS_DX12_ACCESS_COPY_SOURCE = 1u << 6,
	CS_DX12_ACCESS_COPY_DESTINATION = 1u << 7,
	CS_DX12_ACCESS_INDIRECT_ARGUMENT = 1u << 8
} CSDX12Access;

typedef enum CSDX12PassFlags {
	CS_DX12_PASS_NONE = 0,
	CS_DX12_PASS_PARALLEL_RECORDING_SAFE = 1u << 0,
	CS_DX12_PASS_DISABLE_STATISTICS = 1u << 1
} CSDX12PassFlags;

typedef enum CSDX12Capability {
	CS_DX12_CAP_ASYNC_COMPUTE = 1ull << 0,
	CS_DX12_CAP_COPY_QUEUE = 1ull << 1,
	CS_DX12_CAP_MANAGED_RESOURCES = 1ull << 2,
	CS_DX12_CAP_FRAME_UPLOADS = 1ull << 3,
	CS_DX12_CAP_NATIVE_ESCAPE_HATCH = 1ull << 4,
	CS_DX12_CAP_MODULE_SERVICES = 1ull << 5
} CSDX12Capability;

typedef uint32_t CSDX12Format; /* Values intentionally equal DXGI_FORMAT. */

typedef struct CSDX12RuntimeInfo {
	uint32_t structSize;
	uint32_t apiVersion;
	uint32_t available;
	uint32_t deviceRemoved;
	uint64_t capabilities;
	uint64_t activeGeneration;
	uint64_t lastSubmittedCompletion;
	uint64_t completedD3D12Fence;
	uint32_t framesInFlight;
	uint32_t reserved;
} CSDX12RuntimeInfo;

typedef struct CSDX12FrameInfo {
	uint32_t structSize;
	uint32_t apiVersion;
	uint64_t frameIndex;
	uint64_t generation;
	uint64_t completionValue;
	uint32_t frameSlot;
	uint32_t framesInFlight;
	uint32_t width;
	uint32_t height;
} CSDX12FrameInfo;

typedef struct CSDX12SubresourceRange {
	uint32_t firstMip;
	uint32_t mipCount;
	uint32_t firstArraySlice;
	uint32_t arraySize;
} CSDX12SubresourceRange;

typedef struct CSDX12ResourceDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* id;
	CSDX12ResourceLifetime lifetime;
	CSDX12ResourceDimension dimension;
	CSDX12SizingMode sizing;
	CSDX12Format format;
	uint64_t byteSize;
	uint32_t structureByteStride;
	uint32_t width;
	uint32_t height;
	uint32_t depthOrArraySize;
	uint32_t mipLevels;
	uint32_t sampleCount;
	uint32_t allowedAccess;
	uint32_t initialAccess;
	uint32_t finalAccess;
	void* borrowedNativeResource;
} CSDX12ResourceDesc;

typedef struct CSDX12ResourceAccessDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	CSDX12ResourceHandle resource;
	uint32_t access;
	CSDX12SubresourceRange range;
} CSDX12ResourceAccessDesc;

typedef struct CSDX12UploadAllocation {
	void* cpuAddress;
	uint64_t gpuAddress;
	uint64_t size;
	void* borrowedNativeResource;
} CSDX12UploadAllocation;

typedef struct CSDX12DescriptorAllocation {
	uint64_t cpuHandle;
	uint64_t gpuHandle;
	uint32_t descriptorSize;
	uint32_t count;
	void* borrowedNativeHeap;
} CSDX12DescriptorAllocation;

struct CSDX12ExecutionContext;
typedef CSDX12Status(CS_DX12_GRAPH_CALL* CSDX12GetResourceFn)(const struct CSDX12ExecutionContext*, CSDX12ResourceHandle, void**);
typedef CSDX12Status(CS_DX12_GRAPH_CALL* CSDX12AllocateUploadFn)(const struct CSDX12ExecutionContext*, uint64_t, uint64_t, CSDX12UploadAllocation*);
typedef CSDX12Status(CS_DX12_GRAPH_CALL* CSDX12AllocateDescriptorsFn)(const struct CSDX12ExecutionContext*, uint32_t, uint32_t, CSDX12DescriptorAllocation*);

typedef struct CSDX12ExecutionContext {
	uint32_t structSize;
	uint32_t apiVersion;
	const CSDX12FrameInfo* frame;
	void* borrowedD3D12GraphicsCommandList;
	void* hostContext;
	CSDX12GetResourceFn GetResource;
	CSDX12AllocateUploadFn AllocateUpload;
	CSDX12AllocateDescriptorsFn AllocateDescriptors;
} CSDX12ExecutionContext;

typedef CSDX12Status(CS_DX12_GRAPH_CALL* CSDX12ExecuteCallback)(void* userData, const CSDX12ExecutionContext* context);

typedef struct CSDX12PassDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* id;
	CSDX12QueuePolicy queuePolicy;
	uint32_t flags;
	const char* const* after;
	uint32_t afterCount;
	const char* const* before;
	uint32_t beforeCount;
	const CSDX12ResourceAccessDesc* accesses;
	uint32_t accessCount;
	CSDX12ExecuteCallback execute;
} CSDX12PassDesc;

typedef CSDX12Status(CS_DX12_GRAPH_CALL* CSDX12BuildCallback)(void*, CSDX12BuildHandle);
typedef void(CS_DX12_GRAPH_CALL* CSDX12GenerationCallback)(void*, CSDX12GenerationHandle);
typedef void(CS_DX12_GRAPH_CALL* CSDX12DeviceLostCallback)(void*, uint32_t);
typedef void(CS_DX12_GRAPH_CALL* CSDX12ShutdownCallback)(void*);

typedef struct CSDX12ContributorDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* id;
	CSDX12ContributorKind kind;
	void* userData;
	CSDX12BuildCallback build;
	CSDX12GenerationCallback generationActivated;
	CSDX12GenerationCallback generationRetired;
	CSDX12DeviceLostCallback deviceLost;
	CSDX12ShutdownCallback shutdown;
} CSDX12ContributorDesc;

typedef struct CSDX12Diagnostic {
	uint32_t structSize;
	uint32_t apiVersion;
	CSDX12Status status;
	uint64_t sequence;
	CSDX12RegistrationHandle contributor;
	char message[512];
} CSDX12Diagnostic;

typedef struct CSDX12GraphAPI {
	uint32_t structSize;
	uint32_t apiVersion;
	CSDX12Status(CS_DX12_GRAPH_CALL* GetRuntimeInfo)(CSDX12RuntimeInfo*);
	CSDX12Status(CS_DX12_GRAPH_CALL* GetBorrowedD3D12Device)(void**);
	CSDX12Status(CS_DX12_GRAPH_CALL* RegisterContributor)(const CSDX12ContributorDesc*, CSDX12RegistrationHandle*);
	CSDX12Status(CS_DX12_GRAPH_CALL* UnregisterContributor)(CSDX12RegistrationHandle);
	CSDX12Status(CS_DX12_GRAPH_CALL* DeclareResource)(CSDX12BuildHandle, const CSDX12ResourceDesc*, CSDX12ResourceHandle*);
	CSDX12Status(CS_DX12_GRAPH_CALL* FindResource)(CSDX12BuildHandle, const char*, CSDX12ResourceHandle*);
	CSDX12Status(CS_DX12_GRAPH_CALL* DeclarePass)(CSDX12BuildHandle, const CSDX12PassDesc*, CSDX12PassHandle*);
	CSDX12Status(CS_DX12_GRAPH_CALL* RequestRebuild)(CSDX12RegistrationHandle);
	CSDX12Status(CS_DX12_GRAPH_CALL* IsRegistrationRetired)(CSDX12RegistrationHandle, uint32_t*);
	CSDX12Status(CS_DX12_GRAPH_CALL* GetLastDiagnostic)(CSDX12RegistrationHandle, CSDX12Diagnostic*);
	const char*(CS_DX12_GRAPH_CALL* StatusString)(CSDX12Status);
} CSDX12GraphAPI;

#if defined(_WIN32) && defined(CS_DX12_GRAPH_EXPORTS)
#define CS_DX12_GRAPH_API __declspec(dllexport)
#elif defined(_WIN32)
#define CS_DX12_GRAPH_API __declspec(dllimport)
#else
#define CS_DX12_GRAPH_API
#endif
CSDX12Status CS_DX12_GRAPH_API CS_DX12_GRAPH_CALL CS_GetDX12GraphAPI(uint32_t, CSDX12GraphAPI*);

#ifdef __cplusplus
}
#endif
#undef CS_DX12_GRAPH_API
#endif
