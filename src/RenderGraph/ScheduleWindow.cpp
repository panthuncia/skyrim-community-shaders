// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/ScheduleWindow.h"

#include "I18n/I18n.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <DebugUI/ScheduleInspector.h>
#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>

#include <atomic>

namespace RenderGraphSchedule
{
	namespace
	{
		std::atomic<bool> g_windowWants{ false };
		std::atomic<bool> g_devBenchWants{ false };
		bool g_windowOpen = false;  // UI thread
		org::debugui::ScheduleInspectorState g_inspector;  // UI thread


		org::RenderGraph* Graph()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			return host ? host->Graph() : nullptr;
		}

		bool Wanted()
		{
			return g_windowWants.load(std::memory_order_acquire) || g_devBenchWants.load(std::memory_order_acquire);
		}

		// Applied again on every read, so a graph made again after a device reset follows. The inspector shows the first epoch's graph.
		org::RenderGraph* Apply()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (host && host->ScheduleTraceEnabled() != Wanted())
				host->SetScheduleTraceEnabled(Wanted());
			return Graph();
		}
	}

	void SetTracing(Requester a_requester, bool a_enabled)
	{
		(a_requester == Requester::Window ? g_windowWants : g_devBenchWants).store(a_enabled, std::memory_order_release);
		Apply();
	}

	bool TracingEnabled()
	{
		auto* graph = Graph();
		return graph && graph->ScheduleTraceEnabled();
	}

	std::shared_ptr<const org::PersistentScheduleSnapshot> Snapshot()
	{
		auto* graph = Apply();
		return graph ? graph->GetPersistentScheduleSnapshot() : nullptr;
	}

	void OpenWindow()
	{
		g_windowOpen = true;
		SetTracing(Requester::Window, true);
	}


	void DrawWindow()
	{
		if (!g_windowOpen)
			return;
		ImGui::SetNextWindowSize(ImVec2(1200.0f, 650.0f), ImGuiCond_FirstUseEver);
		bool open = true;
		if (ImGui::Begin("Render graph schedule", &open)) {
			if (const auto snapshot = Snapshot())
				org::debugui::ShowScheduleInspector(*snapshot, g_inspector);
			else if (!Graph())
				ImGui::TextWrapped(T("ui.render_graph_schedule.not_running", "The render graph is not running: %s"), RenderGraphRuntime::Get().GetDisabledReason().c_str());
			else
				ImGui::TextWrapped("%s", T("ui.render_graph_schedule.no_schedule", "No schedule yet: the graph traces its executables from the next frame it compiles or runs."));
		}
		ImGui::End();
		if (!open) {
			g_windowOpen = false;
			SetTracing(Requester::Window, false);
		}
	}
}

