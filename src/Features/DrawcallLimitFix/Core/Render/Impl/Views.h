#pragma once

// IndirectDraws::Impl's per-view frame state. Included by Impl.h, before the struct.

namespace DCLF
{
	struct ImplViews
	{
		// The reflection's faces use the cube's format with DCLF's D24S8 depth. First person uses kMAIN's format, motion vectors and kPOST_ZPREPASS_COPY.
		std::array<ViewEpochState, kViews> viewStates;
		ViewEpochState& View(ViewKind a_kind) { return viewStates[ViewIndex(a_kind)]; }
		const ViewEpochState& View(ViewKind a_kind) const { return viewStates[ViewIndex(a_kind)]; }
		/** @brief A view's last coverage decision, else its initial coverage. */
		bool ViewCovered(ViewKind a_kind) const
		{
			const auto& view = View(a_kind);
			return view.coverageDecided ? view.covered : Extensions::ViewEntryOf(a_kind).coveredInitially;
		}
		void SetViewCovered(ViewKind a_kind, bool a_covered)
		{
			auto& view = View(a_kind);
			view.covered = a_covered;
			view.coverageDecided = true;
		}
		/** @brief The depth range the view's work set, else its shape's default. */
		float DepthRangeOf(ViewKind a_kind) const
		{
			const float range = View(a_kind).depthRange;
			return range > 0.0f ? range : Extensions::ViewEntryOf(a_kind).shape.depthRange;
		}
		// Hosted view epochs commit into the depth epoch's commit. hostedWork is the view work the depth commit runs inside its execution.
		// hostedCommit is the host's commit while that work runs, and records which hosted epochs joined it.
		const std::function<void()>* hostedWork = nullptr;
		struct HostedCommit
		{
			std::uint32_t host = kNoEpoch;
			std::vector<std::shared_ptr<const void>>* owners = nullptr;
			const MainPayload* payload = nullptr;
			std::uint32_t drawn = 0;
		};
		HostedCommit hostedCommit;
		std::array<std::vector<rhi::PipelineHandle>, kViews> viewRevisionPipelines;
		std::array<rhi::PipelineHandle, Extensions::kDrawFamilies> viewRevisionFaces{};
		std::vector<std::uint8_t> viewForwardSlots;
	};
}
