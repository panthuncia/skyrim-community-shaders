#pragma once

// The shapes an epoch's recording is prepared for, as its passes read them. A feature's pass reads its own epoch's shapes.
// Self-contained because features include it.

// volk must precede every Vulkan header in this translation unit (Main/Resources.h first).
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Main/FrameLayout.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include <RenderPasses/Base/TypedRenderGraphPass.h>

#include <array>
#include <cstdint>
#include <memory>

namespace DCLF::Draws
{
	struct ShadowFrame;
	struct ViewFrame;
	/**
	 * @brief The shapes an epoch's recording is prepared for, read from the preparation's host data.
	 * A preparation without them draws nothing, because every DCLF epoch is submitted from a revision.
	 */
	struct RevisionShapes
	{
		std::array<std::shared_ptr<const PassFrame>, 2> main;  // kDepthShape, kColourShape
		std::shared_ptr<const ShadowFrame> shadow;
		// In a host epoch's recording, a hosted view's shape is null in the variants where that view draws nothing.
		std::array<std::shared_ptr<const ViewFrame>, kViews> views;
		// The depth epoch's variant whose index pool pass records nothing. Used when the depth commit has no range to upload.
		bool depthWithoutIndexPool = false;
	};
	inline const RevisionShapes* RevisionShapesOf(const org::PassPrepareContext& a_preparation)
	{
		return a_preparation.preparationData ? a_preparation.preparationData->Get<RevisionShapes>() : nullptr;
	}
	/** @brief The units of the a_buffer version the revision names, or 0. Passes size their dispatches by this. */
	inline std::uint64_t NamedUnitsOf(const org::PassPrepareContext& a_preparation, const Versioned& a_buffer)
	{
		const auto* sealed = a_preparation.preparationData ? a_preparation.preparationData->Get<NamedVersions>() : nullptr;
		return sealed ? NamedUnits(*sealed, a_buffer) : 0;
	}
	/** @brief The main shape for a segment, or null when there is no revision or the segment does not draw. */
	inline std::shared_ptr<const PassFrame> CurrentFrame(const org::PassPrepareContext& a_preparation, RenderGraphRuntime::Segment a_segment)
	{
		if (const auto* shapes = RevisionShapesOf(a_preparation)) {
			if (a_segment == RenderGraphRuntime::Segment::ZPrepass)
				return shapes->main[kDepthShape];
			if (a_segment == RenderGraphRuntime::Segment::MainOpaque)
				return shapes->main[kColourShape];
		}
		return nullptr;
	}

	static_assert(DrawPipelines::kMaxPipelines == 4096, "BuildDrawsCS.hlsl's kSortKeys");

	/** @brief Whether BuildDraws runs in the segment. Keep in step with BuildDrawsPass::Prepare. */
	inline bool BuildsDraws(const org::PassPrepareContext& a_preparation, const Resources& a_resources, RenderGraphRuntime::Segment a_segment)
	{
		const auto frame = a_resources.buildDraws && a_resources.dispatchSignature ? CurrentFrame(a_preparation, a_segment) : nullptr;
		return frame && frame->latch;
	}
}
