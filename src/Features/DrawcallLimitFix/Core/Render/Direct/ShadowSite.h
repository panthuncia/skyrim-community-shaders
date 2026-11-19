#pragma once

// The CPU-driven pipeline's shadow site. It draws the shadow views and occlusion maps at shadow-maps.after from the shadow
// epoch's commit. Each latched view is culled on DCLF's pool and drawn into its map's slice with direct draws, after the
// site's pre-work. The pre-work's copies are read by the frame's other sites too.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"

#include <Render/RunRef.h>

#include <cstdint>
#include <memory>
#include <span>

namespace org::services
{
	class NamedVersions;
}

namespace DCLF::Draws
{
	struct ShadowFrame;
	struct ShadowResources;
}

namespace DCLF::Direct
{
	/** @brief One view the shadow commit latched: its list, slot, view in the shape and latch. */
	struct ShadowSiteView
	{
		ViewType type = ViewType::Sun;
		std::uint32_t slot = 0;  // the CullView's viewIndex
		std::uint32_t index = 0;  // into ShadowFrame::views
		Draws::BuildDrawsLatch latch{};
	};

	/** @brief The shadow commit's inputs to its site. */
	struct ShadowCommitInputs
	{
		std::shared_ptr<const Draws::ShadowFrame> shape;  // the shadow shape the commit wrote against
		std::shared_ptr<Draws::ShadowResources> resources;
		const org::services::NamedVersions* adopted = nullptr;  // held by the execution's owner
		std::span<const ShadowSiteView> views;
		std::span<const Draws::SunEntryProcess> sunEntry;  // clamped
		org::RunRef latchRun{};  // a published copy's, or the external run
	};

	/**
	 * @brief Render thread, inside the shadow epoch's commit: opens the shadow site with a view per latched map the shadow
	 * resources import. The views are culled once the frame's cull frame is in.
	 */
	void ShadowCommitted(const ShadowCommitInputs& a_inputs);

}
