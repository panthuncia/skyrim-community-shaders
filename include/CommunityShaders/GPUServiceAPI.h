#ifndef COMMUNITY_SHADERS_GPU_SERVICE_API_H
#define COMMUNITY_SHADERS_GPU_SERVICE_API_H

#include "RenderGraphAPI.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CS_GPU_SERVICE_API_VERSION_1 1u

typedef enum CSGPUCapabilityBits {
	CS_GPU_CAP_SHADER_COMPILATION = 1ull << 2,
	CS_GPU_CAP_SCHEDULED_UPLOADS = 1ull << 4
} CSGPUCapabilityBits;

typedef uint64_t CSGPUShaderHandle;

typedef enum CSGPURequestState {
	CS_GPU_REQUEST_PENDING = 0,
	CS_GPU_REQUEST_READY = 1,
	CS_GPU_REQUEST_FAILED = 2
} CSGPURequestState;

typedef struct CSGPUShaderRequest {
	uint32_t structSize;
	uint32_t apiVersion;
	const char* sourceName;
	const void* source;
	uint64_t sourceSize;
	const char* entryPoint;
	const char* target;
} CSGPUShaderRequest;

typedef struct CSGPUBufferUpload {
	uint32_t structSize;
	uint32_t apiVersion;
	CSRGBinding destination;
	uint64_t destinationOffset;
	const void* data;
	uint64_t dataSize;
} CSGPUBufferUpload;

typedef struct CSGPUServiceAPI {
	uint32_t structSize;
	uint32_t apiVersion;
	uint64_t capabilities;
	CSRGStatus(CS_RG_CALL* RequestShader)(const CSGPUShaderRequest*, CSGPUShaderHandle*);
	CSRGStatus(CS_RG_CALL* GetShaderStatus)(CSGPUShaderHandle, uint32_t*, const void**, uint64_t*);
	CSRGStatus(CS_RG_CALL* ReleaseShader)(CSGPUShaderHandle);
	CSRGStatus(CS_RG_CALL* QueueBufferUpload)(const CSRGExecutionContext*, const CSGPUBufferUpload*);
} CSGPUServiceAPI;

#if defined(_WIN32) && defined(CS_RENDER_GRAPH_EXPORTS)
#define CS_GPU_API __declspec(dllexport)
#elif defined(_WIN32)
#define CS_GPU_API __declspec(dllimport)
#else
#define CS_GPU_API
#endif
CSRGStatus CS_GPU_API CS_RG_CALL CS_GetGPUServiceAPI(uint32_t version, CSGPUServiceAPI* out);

#ifdef __cplusplus
}
#endif
#undef CS_GPU_API

#endif
