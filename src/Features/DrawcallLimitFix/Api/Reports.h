#pragma once

// A feature's lines in DCLF's periodic report (CS_DCLF_STATS), placed at a named point.

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace DCLF::Api
{
	/** @brief Where in the report a feature's lines go. Lines at one point run in registration order. */
	enum class ReportPoint : std::uint8_t
	{
		// Render thread, every frame. The lines must check Reports::Due themselves. Logged.
		RenderThread,
		// Primary cull lines. Only on report frames whose window had cut frames. Logged.
		PrimaryCull,
		// Shadow source cull lines, on the same frames as PrimaryCull. Logged.
		PrimaryCullSources,
		// Primary cull parity lines, on the same frames. Logged.
		PrimaryCullParity,
		// The primary cull's last lines, on the same frames. Logged.
		PrimaryCullLast,
		// Write-back lines in the draws' async report, every report. Text.
		AsyncWorkWriteBack,
		// View lines in the draws' async report, every report. Text.
		AsyncWorkViews,
		// Accumulated write lines, only when the scene has the visibility readback. Text.
		AccumulatedWrites,
		// Forward epoch lines, when the forward epoch ran since the last report or its toggle is on. Text.
		AsyncWorkForward,
		// The draws' async report, every report. Text.
		AsyncWork,
		// Draw family lines in the draws' async report, every report. Text.
		AsyncWorkFamilies,
		// Render thread scene lines, every report. Text.
		SceneFrame,
		// LOD gate releases, once per report window. Logged.
		GateReleases,
		// Coordinator gate lines, on the scene lane. Logged.
		SceneCoordinatorGates,
		// Coordinator set lines, on the scene lane, right after the "DCLF set:" line. Logged.
		SceneCoordinatorSet,
		// Coordinator scene lines, on the scene lane. Text.
		SceneCoordinatorSegments,
		// Coordinator lines after the material records, on the scene lane. Text.
		SceneCoordinatorMaterials,
		// Fragments appended inside the coordinator's delta line. Each starts with "; " and has no newline. Only when the line prints.
		SceneCoordinatorDelta,
		// Coordinator statistics for its last pass, on the scene lane, under CS_DCLF_STATS. Logged.
		SceneCoordinatorLastPass,
		// Shadow view statistics on report frames with CS_DCLF_STATS and shadows on. Logged.
		ShadowViews,
		// The last statistics on report frames with CS_DCLF_STATS.
		Statistics,
		// Diagnostic capture census, on the scene lane. One JSON document per registration. Not part of the periodic report.
		CaptureCensus,
		// Diagnostic capture growth census, on the scene lane. Lines of "<name> <value>". Not part of the periodic report.
		CaptureGrowth,
	};

	/** @brief Whether a_point composes its lines into the report's text instead of logging them. */
	constexpr bool ReportPointComposesText(ReportPoint a_point)
	{
		return a_point == ReportPoint::AsyncWorkWriteBack || a_point == ReportPoint::AsyncWorkViews || a_point == ReportPoint::AccumulatedWrites || a_point == ReportPoint::AsyncWorkForward || a_point == ReportPoint::AsyncWork ||
		       a_point == ReportPoint::AsyncWorkFamilies || a_point == ReportPoint::SceneFrame || a_point == ReportPoint::SceneCoordinatorSegments ||
		       a_point == ReportPoint::SceneCoordinatorMaterials || a_point == ReportPoint::SceneCoordinatorDelta || a_point == ReportPoint::CaptureCensus ||
		       a_point == ReportPoint::CaptureGrowth;
	}

	/** @brief Whether a_point belongs to the coordinator and so accepts a coordinator component's lines. */
	constexpr bool ReportPointTakesComponents(ReportPoint a_point)
	{
		return a_point == ReportPoint::SceneCoordinatorGates || a_point == ReportPoint::SceneCoordinatorSet || a_point == ReportPoint::SceneCoordinatorSegments ||
		       a_point == ReportPoint::SceneCoordinatorMaterials || a_point == ReportPoint::SceneCoordinatorDelta ||
		       a_point == ReportPoint::SceneCoordinatorLastPass || a_point == ReportPoint::CaptureCensus || a_point == ReportPoint::CaptureGrowth;
	}

	/**
	 * @brief A feature's report lines. Set `lines` for a point that logs, or `text` for one that composes text.
	 * componentText replaces either at a coordinator point, and must be registered after its component.
	 */
	struct ReportLines
	{
		std::string_view name;
		ReportPoint point = ReportPoint::PrimaryCull;
		void (*lines)(std::uint32_t a_frame) = nullptr;
		void (*text)(std::string& a_text) = nullptr;
		CoordinatorComponentId component;
		void (*componentText)(const CoordinatorComponentContext& a_context, std::string& a_text) = nullptr;
	};
}
