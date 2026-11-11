#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/ProgramCache.h"

#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/Format.h"

#	include <OpenRenderGraph/GraphHost.h>

#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#		include "RenderGraph/ProgramBuild.h"
#	endif

#	include <condition_variable>
#	include <deque>
#	include <thread>

namespace RenderGraph
{
	namespace
	{
		/** @brief The one program build thread: builds one program at a time, in the order they were asked for. */
		struct Builder
		{
			std::mutex mutex;
			std::condition_variable wake;
			std::deque<std::function<void()>> jobs;
			std::thread thread;
			bool stopping = false;

			void Loop()
			{
				SetThreadDescription(GetCurrentThread(), L"CS ORG program builds");
				for (;;) {
					std::function<void()> job;
					{
						std::unique_lock lock(mutex);
						wake.wait(lock, [&] { return stopping || !jobs.empty(); });
						if (stopping)
							return;
						job = std::move(jobs.front());
						jobs.pop_front();
					}
					job();
				}
			}
		};

		Builder& GetBuilder()
		{
			static Builder s_builder;
			return s_builder;
		}

		std::string DefinesLabel(const std::vector<std::pair<std::wstring, std::wstring>>& a_defines)
		{
			std::string text;
			for (const auto& [define, value] : a_defines)
				text += fmt::format("{}{}{}{}", text.empty() ? "" : " ", Util::WStringToString(define), value.empty() ? "" : "=", Util::WStringToString(value));
			return text;
		}
	}

	namespace ProgramCacheDetail
	{
		void ResolveDefines(RasterProgram::Desc& a_desc)
		{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
			if (!a_desc.commonDefines)
				return;
			if (a_desc.vertexStageDefines.empty())
				a_desc.vertexStageDefines = ProgramBuild::WithCommonDefines(a_desc.defines, L"vs_6_6");
			if (a_desc.pixelStageDefines.empty())
				a_desc.pixelStageDefines = ProgramBuild::WithCommonDefines(a_desc.defines, L"ps_6_6");
#	else
			(void)a_desc;
#	endif
		}

		void ResolveDefines(ComputeProgram::Desc& a_desc)
		{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
			if (!a_desc.commonDefines)
				return;
			a_desc.defines = ProgramBuild::WithCommonDefines(a_desc.defines, L"cs_6_6");
			a_desc.commonDefines = false;
#	else
			(void)a_desc;
#	endif
		}

		std::string Label(const RasterProgram::Desc& a_desc)
		{
			return fmt::format("{} + {} ({}) [{}]", a_desc.vertexSource, a_desc.pixelSource, Util::WStringToString(a_desc.pixelEntry), DefinesLabel(a_desc.defines));
		}

		std::string Label(const ComputeProgram::Desc& a_desc)
		{
			return fmt::format("{} ({}) [{}]", a_desc.source, Util::WStringToString(a_desc.entry), DefinesLabel(a_desc.defines));
		}

		std::shared_ptr<const RasterProgram> Load(const RasterProgram::Desc& a_desc)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			return host ? RasterProgram::Load(host->Device(), a_desc) : nullptr;
		}

		std::shared_ptr<const ComputeProgram> Load(const ComputeProgram::Desc& a_desc)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			return host ? ComputeProgram::Load(host->Device(), a_desc) : nullptr;
		}

		bool Post(std::function<void()> a_job)
		{
			auto& builder = GetBuilder();
			{
				std::scoped_lock lock(builder.mutex);
				if (builder.stopping)
					return false;
				if (!builder.thread.joinable())
					builder.thread = std::thread([&builder] { builder.Loop(); });
				builder.jobs.push_back(std::move(a_job));
			}
			builder.wake.notify_one();
			return true;
		}

		bool Available()
		{
			return RenderGraphRuntime::Get().IsActive() && RenderGraphRuntime::Get().Host();
		}
	}

	void StopProgramBuilds()
	{
		auto& builder = GetBuilder();
		{
			std::scoped_lock lock(builder.mutex);
			builder.stopping = true;
			builder.jobs.clear();
		}
		builder.wake.notify_all();
		if (builder.thread.joinable())
			builder.thread.join();
	}
}

#endif
