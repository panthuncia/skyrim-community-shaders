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

#define CS_DX12_GRAPH_API_VERSION_1 1u
#define CS_DX12_GRAPH_API_CURRENT CS_DX12_GRAPH_API_VERSION_1

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
	CS_DX12_E_INTERNAL = 0x7fffffff
} CSDX12Status;

typedef enum CSDX12ContributorKind {
	CS_DX12_CONTRIBUTOR_REQUIRED = 0,
	CS_DX12_CONTRIBUTOR_OPTIONAL = 1,
	CS_DX12_CONTRIBUTOR_DIAGNOSTIC = 2
} CSDX12ContributorKind;

typedef enum CSDX12QueueKind {
	CS_DX12_QUEUE_GRAPHICS = 0,
	CS_DX12_QUEUE_COMPUTE = 1,
	CS_DX12_QUEUE_COPY = 2
} CSDX12QueueKind;

typedef enum CSDX12ResourceLifetime {
	CS_DX12_RESOURCE_TRANSIENT = 0,
	CS_DX12_RESOURCE_PERSISTENT = 1,
	CS_DX12_RESOURCE_HOST_IMPORTED = 2
} CSDX12ResourceLifetime;

typedef enum CSDX12Access {
	CS_DX12_ACCESS_NONE = 0,
	CS_DX12_ACCESS_SHADER_READ = 1u << 0,
	CS_DX12_ACCESS_UNORDERED_WRITE = 1u << 1,
	CS_DX12_ACCESS_RENDER_TARGET = 1u << 2,
	CS_DX12_ACCESS_DEPTH_READ = 1u << 3,
	CS_DX12_ACCESS_DEPTH_WRITE = 1u << 4,
	CS_DX12_ACCESS_COPY_SOURCE = 1u << 5,
	CS_DX12_ACCESS_COPY_DESTINATION = 1u << 6
} CSDX12Access;

/* Values intentionally equal DXGI_FORMAT. The header does not require DXGI headers. */
typedef uint32_t CSDX12Format;

typedef struct CSDX12RuntimeInfo {
	uint32_t structSize;
	uint32_t apiVersion;
	uint32_t available;
	uint32_t deviceRemoved;
	uint64_t activeGeneration;
	uint64_t completedD3D12Fence;
	uint32_t supportsAsyncCompute;
	uint32_t supportsCopyQueue;
} CSDX12RuntimeInfo;

typedef struct CSDX12FrameInfo {
	uint32_t structSize;
	uint32_t apiVersion;
	uint64_t frameIndex;
	uint64_t generation;
	uint32_t width;
	uint32_t height;
} CSDX12FrameInfo;

typedef struct CSDX12ResourceDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* id;
	CSDX12ResourceLifetime lifetime;
	CSDX12Format format;
	uint32_t width;
	uint32_t height;
	uint32_t depthOrArraySize;
	uint32_t mipLevels;
	uint32_t allowedAccess;
} CSDX12ResourceDesc;

typedef struct CSDX12ResourceAccessDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* resourceId;
	uint32_t access;
} CSDX12ResourceAccessDesc;

typedef CSDX12Status(CS_DX12_GRAPH_CALL* CSDX12ExecuteCallback)(
	void* userData,
	void* borrowedD3D12GraphicsCommandList,
	const CSDX12FrameInfo* frameInfo);

typedef struct CSDX12PassDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* id;
	CSDX12QueueKind queue;
	const char* const* after;
	uint32_t afterCount;
	const char* const* before;
	uint32_t beforeCount;
	const CSDX12ResourceAccessDesc* accesses;
	uint32_t accessCount;
	CSDX12ExecuteCallback execute;
} CSDX12PassDesc;

typedef CSDX12Status(CS_DX12_GRAPH_CALL* CSDX12BuildCallback)(void* userData, CSDX12BuildHandle build);
typedef void(CS_DX12_GRAPH_CALL* CSDX12ShutdownCallback)(void* userData, CSDX12GenerationHandle lastGeneration);

typedef struct CSDX12ContributorDesc {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* id;
	CSDX12ContributorKind kind;
	void* userData;
	CSDX12BuildCallback build;
	CSDX12ShutdownCallback shutdown;
} CSDX12ContributorDesc;

typedef struct CSDX12Diagnostic {
	uint32_t structSize;
	uint32_t apiVersion;
	CSDX12Status status;
	uint64_t sequence;
	char message[512];
} CSDX12Diagnostic;

typedef struct CSDX12GraphAPI {
	uint32_t structSize;
	uint32_t apiVersion;
	CSDX12Status(CS_DX12_GRAPH_CALL* GetRuntimeInfo)(CSDX12RuntimeInfo* outInfo);
	CSDX12Status(CS_DX12_GRAPH_CALL* GetBorrowedD3D12Device)(void** outDevice);
	CSDX12Status(CS_DX12_GRAPH_CALL* RegisterContributor)(const CSDX12ContributorDesc* desc, CSDX12RegistrationHandle* outHandle);
	CSDX12Status(CS_DX12_GRAPH_CALL* UnregisterContributor)(CSDX12RegistrationHandle handle);
	CSDX12Status(CS_DX12_GRAPH_CALL* DeclareResource)(CSDX12BuildHandle build, const CSDX12ResourceDesc* desc, CSDX12ResourceHandle* outHandle);
	CSDX12Status(CS_DX12_GRAPH_CALL* DeclarePass)(CSDX12BuildHandle build, const CSDX12PassDesc* desc, CSDX12PassHandle* outHandle);
	CSDX12Status(CS_DX12_GRAPH_CALL* RequestRebuild)(CSDX12RegistrationHandle handle);
	CSDX12Status(CS_DX12_GRAPH_CALL* GetLastDiagnostic)(CSDX12Diagnostic* outDiagnostic);
} CSDX12GraphAPI;

#if defined(_WIN32) && defined(CS_DX12_GRAPH_EXPORTS)
#define CS_DX12_GRAPH_API __declspec(dllexport)
#elif defined(_WIN32)
#define CS_DX12_GRAPH_API __declspec(dllimport)
#else
#define CS_DX12_GRAPH_API
#endif
CSDX12Status CS_DX12_GRAPH_API CS_DX12_GRAPH_CALL CS_GetDX12GraphAPI(uint32_t requestedVersion, CSDX12GraphAPI* outApi);

#ifdef __cplusplus
}
#endif
#undef CS_DX12_GRAPH_API
#endif
