// The render graph's test commands (owner "org"), moved out of DCLF's test harness. Not in Release builds.
#include "RenderGraph/Testing/OrgCommands.h"

#if !defined(CS_RELEASE_PROFILE)

#	include "RenderGraph/Testing/GraphFrames.h"
#	include "Testing/Testing.h"

#	include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#	include "Features/RemoteControl/GpuCounters.h"
#	include "GpuEventTimers.h"
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/PipelineVersions.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <ORGModuleServices/PipelineRegistry.h>
#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RhiExecution.h>
#	include <rhi.h>

#	include <algorithm>
#	include <functional>
#	include <cstdlib>
#	include <mutex>
#	include <sstream>
#	include <string>
#	include <utility>

namespace RenderGraph::TestSupport
{
	namespace
	{
		using OrgCommand = std::function<bool(const std::string& a_arg, std::string& a_error)>;

		void AddOrgCommand(const char* a_name, OrgCommand a_command)
		{
			::Testing::RegisterFeatureCommand("org", a_name, std::move(a_command));
		}

		/** @brief The last reload asked for and the last NvPerf capture started, for the waits. Render thread. */
		struct OrgHarnessState
		{
			bool reloadAsked = false;
			std::uint64_t reloadSerial = 0;  // the registry's before the reload was asked
			std::uint64_t nvperfCapture = 0;                         // 0 for none started
		};

		OrgHarnessState& OrgHarness()
		{
			static OrgHarnessState state;
			return state;
		}

		/** @brief The next builds read a diagnostic switch: every graph is built again at the next adoption point. */
		void OrgRebuildForSwitch(std::string_view a_command)
		{
			RenderGraph::Builds::Ask({ .all = true }, fmt::format("test command org:{}", a_command));
		}

		std::uint32_t OrgParseCount(const std::string& a_arg)
		{
			return static_cast<std::uint32_t>(std::strtoul(a_arg.c_str(), nullptr, 10));
		}

		bool OrgStartNvPerf(const std::string& a_arg, std::string& a_error)
		{
			std::string value = a_arg;
			::GpuCounters::CaptureConfiguration configuration;
			configuration.samples = 3;
			// "@<metric>[:<kind>],..." replaces the default metrics.
			if (const auto at = value.rfind('@'); at != std::string::npos) {
				std::stringstream list(value.substr(at + 1));
				for (std::string metric; std::getline(list, metric, ',');) {
					::GpuCounters::MetricRequest request;
					std::string error;
					if (!::GpuCounters::ParseMetric(metric, request, error)) {
						a_error = fmt::format("NvPerf metric '{}': {}", metric, error);
						return false;
					}
					configuration.metrics.push_back(std::move(request));
				}
				value.resize(at);
			}
			if (const auto hash = value.rfind('#'); hash != std::string::npos) {
				configuration.samples = std::max(1u, static_cast<std::uint32_t>(std::strtoul(value.c_str() + hash + 1, nullptr, 10)));
				value.resize(hash);
			}
			for (std::size_t start = 0; start <= value.size();) {
				const auto bar = value.find('|', start);
				auto range = value.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
				if (!range.empty())
					configuration.ranges.push_back(std::move(range));
				if (bar == std::string::npos)
					break;
				start = bar + 1;
			}
			std::uint64_t id = 0;
			std::string error;
			if (!::GpuCounters::RequestCapture(configuration, id, error)) {
				a_error = fmt::format("no NvPerf capture: {}", error);
				return false;
			}
			OrgHarness().nvperfCapture = id;
			logger::info("[ORG] test command: NvPerf capture {} of {} ranges, {} samples", id, configuration.ranges.size(), configuration.samples);
			return true;
		}

		void OrgRegisterGraphCommands()
		{
			// Builds, kept until the next adoption point (Builds::Ask). Nothing is rebuilt inside a frame.
			AddOrgCommand("rebuild", [](const std::string&, std::string& a_error) {
				if (!RenderGraphRuntime::Get().Host()) {
					a_error = "no render graph host to rebuild";
					return false;
				}
				RenderGraph::Builds::Ask({ .all = true }, "test command org:rebuild");
				return true;
			});
			AddOrgCommand("refresh", [](const std::string& a_arg, std::string& a_error) {
				if (!RenderGraphRuntime::Get().Host()) {
					a_error = "no render graph host to refresh";
					return false;
				}
				// An empty id builds every epoch.
				RenderGraph::Builds::Wanted wanted;
				if (a_arg.empty())
					wanted.all = true;
				else
					wanted.ids.push_back(a_arg);
				RenderGraph::Builds::Ask(std::move(wanted), fmt::format("test command org:refresh={}", a_arg));
				return true;
			});
			// Shader hot reload, served at the next present. Plain reloads every live pipeline and keeps the unchanged ones.
			// "all" rebuilds and publishes every one even when unchanged. "<file>" reloads those built from that file, or all for an include.
			AddOrgCommand("reload-shaders", [](const std::string& a_arg, std::string&) {
				if (const auto* registry = RenderGraphRuntime::PipelineRegistry()) {
					auto& harness = OrgHarness();
					harness.reloadAsked = true;
					harness.reloadSerial = registry->LastReload().serial;
				}
				if (a_arg == "all")
					RenderGraphRuntime::RequestShaderReload({}, true);
				else
					RenderGraphRuntime::RequestShaderReload(a_arg);
				return true;
			});
			// The GPU event timers from the next frame on, as one window. "-" is not logged. Needs CS_GPU_EVENT_TIMERS.
			AddOrgCommand("gpu-window", [](const std::string& a_arg, std::string&) {
				if (!::GpuEventTimers::Requested())
					logger::warn("[ORG] test command org:gpu-window={}: the GPU event timers are off (CS_GPU_EVENT_TIMERS)", a_arg);
				::GpuEventTimers::StartWindow(a_arg);
				return true;
			});
			// "<range>|<range>...[#<samples>][@<metric>,...]": those ranges with NvPerf from the next frame, 3 samples by default.
			AddOrgCommand("nvperf", OrgStartNvPerf);
			AddOrgCommand("ts-dump", [](const std::string& a_arg, std::string&) {
				RenderGraphRuntime::RequestTimestampDump(OrgParseCount(a_arg));
				return true;
			});
			// What the next builds record.
			AddOrgCommand("async-compute", [](const std::string& a_arg, std::string&) {
				::DCLF::Draws::asyncComputeOverride.store(a_arg == "default" ? -1 : a_arg != "0", std::memory_order_relaxed);
				OrgRebuildForSwitch("async-compute");
				return true;
			});
			AddOrgCommand("pass-queries", [](const std::string& a_arg, std::string&) {
				org::diagnostics::DiagnosticSkipPassQueries().store(a_arg == "0", std::memory_order_relaxed);
				OrgRebuildForSwitch("pass-queries");
				return true;
			});
			AddOrgCommand("heap-dedupe", [](const std::string& a_arg, std::string&) {
				rhi::DiagnosticDedupeHeapBinds().store(a_arg == "1", std::memory_order_relaxed);
				OrgRebuildForSwitch("heap-dedupe");
				return true;
			});
			AddOrgCommand("list-heaps", [](const std::string& a_arg, std::string&) {
				org::diagnostics::DiagnosticSkipListHeapBinds().store(a_arg == "0", std::memory_order_relaxed);
				OrgRebuildForSwitch("list-heaps");
				return true;
			});
			AddOrgCommand("list-barriers", [](const std::string& a_arg, std::string&) {
				org::diagnostics::DiagnosticSkipListBarriers().store(a_arg == "external" ? 1 : a_arg == "all" ? 2 : 0, std::memory_order_relaxed);
				OrgRebuildForSwitch("list-barriers");
				return true;
			});
			AddOrgCommand("rhi-globals", [](const std::string& a_arg, std::string&) {
				rhi::DiagnosticSkipGlobalBarriers().store(a_arg == "0", std::memory_order_relaxed);
				OrgRebuildForSwitch("rhi-globals");
				return true;
			});
			AddOrgCommand("queue-boundary", [](const std::string& a_arg, std::string& a_error) {
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host) {
					a_error = "no render graph host";
					return false;
				}
				const bool on = a_arg != "0";
				host->SetQueueBoundary({ .entry = on, .exit = on });
				return true;
			});
			// "reload" waits until the last reload finished and a frame revision adopted its pipeline generation.
			::Testing::RegisterWait("reload", [] {
				auto& harness = OrgHarness();
				const auto* registry = RenderGraphRuntime::PipelineRegistry();
				if (!registry || !harness.reloadAsked)
					return true;
				const auto reload = registry->LastReload();
				return reload.serial > harness.reloadSerial && (reload.rebuilt == 0 || RenderGraph::PipelineVersions::Adopted() >= reload.generation);
			});
			// "nvperf" waits until the last capture is done or failed.
			::Testing::RegisterWait("nvperf", [] {
				auto& harness = OrgHarness();
				if (!harness.nvperfCapture)
					return true;
				const auto latest = ::GpuCounters::LatestResult();
				return latest && latest->id >= harness.nvperfCapture;
			});
		}
	}

	void Install()
	{
		static std::once_flag once;
		std::call_once(once, [] {
			// "org:<name>" is the command org.<name> of the feature registry, so a step may name it directly.
			::Testing::RegisterCommandPrefix("org:", [](const std::string& a_name, const std::string& a_arg, std::string& a_error) {
				return ::Testing::RunFeatureCommand("org", std::string_view(a_name).substr(4), a_arg, a_error);
			});
			OrgRegisterGraphCommands();
			::Testing::RegisterExitReport([] { GraphFrames::Report(); });
		});
	}
}

#endif
