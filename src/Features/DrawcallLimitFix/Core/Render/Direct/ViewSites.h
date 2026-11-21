#pragma once

// The CPU-driven pipeline's view sites. The views that have epochs of their own in the GPU path are drawn here with
// direct draws at the same triggers.
// - The depth site hosts the reflection faces and first-person depth. Their commits run inside the depth commit, the
//   site attaches them when it opens, and it draws them after main depth and the families, each into its own target.
// - The first-person colour site draws what this frame's first-person depth culled, then its forward part back to front.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"

#include <rhi.h>

#include <cstdint>
#include <memory>
#include <span>

namespace DCLF::Draws
{
	struct RevisionShapes;
	struct ViewFrame;
	struct ViewResources;
}

namespace DCLF::Direct
{
	/** @brief A face's draw family draw, such as the reflection's tree LOD. */
	struct HostedFaceDraw
	{
		std::uint32_t family = 0;  // Api::DrawFamily
		std::uint32_t face = 0;
		RenderGraph::BufferRef row;
		rhi::ResourceHandle list{};  // the adopted set's version, culled in the site's pre-work
		rhi::PipelineHandle pipeline{};
		rhi::CommandSignatureHandle signature{};
	};

	/** @brief A hosted view's commit inside the depth commit, which its site draws from. */
	struct HostedViewCommit
	{
		ViewType type = ViewType::Reflection;  // Reflection or FirstPerson
		std::shared_ptr<const Draws::ViewFrame> shape;  // the revision's shape it committed against
		std::shared_ptr<Draws::ViewResources> resources;
		std::span<const CullView> faces;  // per drawn face, from the latch its commit wrote
		std::span<const HostedFaceDraw> families;
		std::shared_ptr<const void> keep;  // keeps the families' list versions alive
	};

	/** @brief Render thread, inside the depth commit: posts the view for the depth site to take when it opens. */
	void HostedViewCommitted(const HostedViewCommit& a_commit);

	/** @brief Render thread, inside the first-person colour epoch: opens its site from this frame's first-person depth. False without one. */
	bool FirstPersonColourReached();
}

namespace DCLF::Direct::detail
{
	struct FrameSlot;
	struct SiteRun;

	/**
	 * @brief Render thread, after NewRun and before the depth run's pre-work and culls: attaches the hosted views this depth
	 * commit committed to the run, so their copies, face culls and draws ride along with it.
	 */
	void AttachHostedViews(FrameSlot& a_slot, SiteRun& a_depth, Draws::RevisionShapes& a_shapes);
}
