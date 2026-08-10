#define CS_RENDER_GRAPH_EXPORTS
#include <CommunityShaders/GPUServiceAPI.h>

#include "RenderGraphExecutionContext.h"
#include "RenderGraphRuntime.h"

#if defined(CS_HAS_ORG_MODULE_SERVICES)
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#include <ORGModuleServices/ShaderCompiler.h>
#endif
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	struct ShaderJob {
		std::shared_ptr<std::vector<std::byte>> source;
		std::shared_future<org::services::ShaderArtifact> future;
		std::shared_ptr<org::services::ShaderArtifact> artifact;
		bool released{};
	};
	std::mutex shaderMutex;
	std::unordered_map<uint64_t, ShaderJob> shaderJobs;
	std::atomic_uint64_t nextShaderHandle{ 1 };

	std::wstring Widen(const char* value)
	{
		std::wstring result;
		if (value) while (*value) result.push_back(static_cast<unsigned char>(*value++));
		return result;
	}

	CSRGStatus CS_RG_CALL RequestShader(const CSGPUShaderRequest* request, CSGPUShaderHandle* out)
	{
		if (!request || request->structSize < sizeof(*request) || request->apiVersion != CS_GPU_SERVICE_API_VERSION_1 ||
			!out || !request->sourceName || !request->source || !request->sourceSize || !request->entryPoint || !request->target)
			return CS_RG_E_INVALID_ARGUMENT;
		if (request->sourceSize > SIZE_MAX) return CS_RG_E_INVALID_ARGUMENT;
		{
			std::scoped_lock lock(shaderMutex);
			std::erase_if(shaderJobs, [](const auto& item) {
				return item.second.released && item.second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			});
		}
		auto* compiler = RenderGraphRuntime::Get().GetShaderCompiler();
		if (!compiler || !compiler->Available()) return CS_RG_E_UNSUPPORTED_CAPABILITY;
		try {
			auto source = std::make_shared<std::vector<std::byte>>(request->sourceSize);
			std::memcpy(source->data(), request->source, request->sourceSize);
			org::services::ShaderCompileRequest compile{};
			compile.sourceName = request->sourceName;
			compile.source = *source;
			compile.entryPoint = Widen(request->entryPoint);
			compile.target = Widen(request->target);
			const auto handle = nextShaderHandle.fetch_add(1);
			ShaderJob job{ source, compiler->CompileAsync(std::move(compile)) };
			std::scoped_lock lock(shaderMutex);
			shaderJobs.emplace(handle, std::move(job));
			*out = handle;
			return CS_RG_OK;
		} catch (...) { return CS_RG_E_INTERNAL; }
	}

	CSRGStatus CS_RG_CALL ShaderStatus(CSGPUShaderHandle handle, uint32_t* state, const void** bytes, uint64_t* size)
	{
		if (!state || !bytes || !size) return CS_RG_E_INVALID_ARGUMENT;
		std::scoped_lock lock(shaderMutex);
		const auto it = shaderJobs.find(handle);
		if (it == shaderJobs.end()) return CS_RG_E_STALE_HANDLE;
		auto& job = it->second;
		if (!job.artifact && job.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			try { job.artifact = std::make_shared<org::services::ShaderArtifact>(job.future.get()); }
			catch (...) { job.artifact = std::make_shared<org::services::ShaderArtifact>(); }
		}
		if (!job.artifact) { *state = CS_GPU_REQUEST_PENDING; *bytes = nullptr; *size = 0; return CS_RG_OK; }
		*state = *job.artifact ? CS_GPU_REQUEST_READY : CS_GPU_REQUEST_FAILED;
		*bytes = job.artifact->binary.empty() ? nullptr : job.artifact->binary.data();
		*size = job.artifact->binary.size();
		return CS_RG_OK;
	}

	CSRGStatus CS_RG_CALL ReleaseShader(CSGPUShaderHandle handle)
	{
		std::scoped_lock lock(shaderMutex);
		const auto it = shaderJobs.find(handle);
		if (it == shaderJobs.end()) return CS_RG_E_STALE_HANDLE;
		if (it->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) shaderJobs.erase(it);
		else it->second.released = true;
		return CS_RG_OK;
	}
#endif
	RenderGraphExecutionContextHost* Host(const CSRGExecutionContext* context) noexcept
	{
		if (!context || !context->hostContext) return nullptr;
		auto* host = const_cast<RenderGraphExecutionContextHost*>(
			static_cast<const RenderGraphExecutionContextHost*>(context->hostContext));
		return host->magic == RenderGraphExecutionContextHost::kMagic ? host : nullptr;
	}

	CSRGStatus CS_RG_CALL QueueBufferUpload(const CSRGExecutionContext* context, const CSGPUBufferUpload* upload)
	{
		if (!upload || upload->structSize < sizeof(*upload) || upload->apiVersion != CS_GPU_SERVICE_API_VERSION_1 ||
			!upload->data || !upload->dataSize) return CS_RG_E_INVALID_ARGUMENT;
		auto* host = Host(context);
		if (!host || !host->queueBufferUpload) return CS_RG_E_UNSUPPORTED_CAPABILITY;
		return host->queueBufferUpload(host->uploadUser, upload->destination, upload->destinationOffset,
			upload->data, upload->dataSize);
	}
}

extern "C" CSRGStatus CS_RG_CALL CS_GetGPUServiceAPI(uint32_t version, CSGPUServiceAPI* out)
{
	if (!out || out->structSize < sizeof(*out)) return CS_RG_E_INVALID_ARGUMENT;
	if (version != CS_GPU_SERVICE_API_VERSION_1) return CS_RG_E_UNSUPPORTED_VERSION;
	uint64_t capabilities = CS_GPU_CAP_SCHEDULED_UPLOADS;
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	capabilities |= CS_GPU_CAP_SHADER_COMPILATION;
	*out = { sizeof(*out), version, capabilities,
		&RequestShader, &ShaderStatus, &ReleaseShader, &QueueBufferUpload };
#else
	*out = { sizeof(*out), version, capabilities,
		nullptr, nullptr, nullptr, &QueueBufferUpload };
#endif
	return CS_RG_OK;
}
