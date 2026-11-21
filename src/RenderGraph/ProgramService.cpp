// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/ProgramService.h"

#include "RenderGraph/Async/Lanes.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <ORGModuleServices/PipelineRegistry.h>
#include <ORGModuleServices/Programs/ProgramBuild.h>

#include <atomic>
#include <string>
#include <string_view>

#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#	include "RenderGraph/PipelineVersions.h"
#	include "RenderGraph/ProgramBuild.h"
#endif

namespace RenderGraph::Programs
{
	void InstallEnvironment()
	{
		org::services::ProgramEnvironment environment;
		environment.shaderRoot = RenderGraphRuntime::kShaderDirectory;
		environment.backend = [] { return Backend::Active(); };
		environment.compiler = [] { return RenderGraphRuntime::Get().ShaderCompiler(); };
		environment.registry = [] { return RenderGraphRuntime::PipelineRegistry(); };
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
		environment.commonDefines = [](const org::services::ProgramDefines& a_own, std::wstring_view a_target) {
			const std::wstring target(a_target);
			return ProgramBuild::WithCommonDefines(a_own, target.c_str());
		};
		environment.versionFor = [](const org::services::LivePipelineSlot& a_slot, const org::RecordingContext& a_recording) {
			return PipelineVersions::VersionFor(a_slot, a_recording);
		};
#endif
		environment.requestDefines = [] {
			// Dual-target shaders take their bindless declarations.
			org::services::ProgramDefines defines{ { L"CS_ORG_BINDLESS", L"1" } };
			// For AMD shader compiler workarounds, such as PortalPrograms.hlsli's interpreter.
			if (RenderGraphRuntime::VendorId() == 0x1002)
				defines.emplace_back(L"CS_GPU_AMD", L"1");
			return defines;
		};
		environment.labelPipelines = [](const char* a_label) { rhi::vulkan::set_pipeline_statistics_label(a_label); };
		environment.log = [](org::services::ProgramLogLevel a_level, std::string_view a_message) {
			switch (a_level) {
			case org::services::ProgramLogLevel::Info:
				logger::info("[ORG] {}", a_message);
				break;
			case org::services::ProgramLogLevel::Warning:
				logger::warn("[ORG] {}", a_message);
				break;
			case org::services::ProgramLogLevel::Error:
				logger::error("[ORG] {}", a_message);
				break;
			}
		};
		org::services::SetProgramEnvironment(std::move(environment));
	}

	namespace
	{
		std::atomic<org::services::ProgramService*> s_madeService{ nullptr };
	}

	org::services::ProgramService& Service()
	{
		// Never destroyed: Stop stops it before the device goes.
		static auto* s_service = [] {
			auto* made = new org::services::ProgramService({
				.executor = &Frame::SharedExecutor(),
				.taskClass = Frame::Executor::Preparation,
				// DCLF's build pump runs the builds' compiles on the preparation pool.
				.compileDomain = Frame::Executor::Preparation.domain,
				.fallback = Frame::Executor::Streaming,
				.laneName = "CS programs",
				.host = [] { return static_cast<const org::GraphHost*>(RenderGraphRuntime::Get().Host()); },
				.available = [] { return Available(); },
				.onError = RenderGraph::FatalHandler("Programs"),
			});
			s_madeService.store(made, std::memory_order_release);
			return made;
		}();
		return *s_service;
	}

	bool Available()
	{
		return RenderGraphRuntime::Get().IsActive() && RenderGraphRuntime::Get().Host();
	}

	void Stop()
	{
		// A service never made has nothing to stop.
		if (auto* service = s_madeService.load(std::memory_order_acquire))
			service->Stop();
	}
}
