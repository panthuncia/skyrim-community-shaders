#pragma once

#include "RenderGraph/Frame/RevisionRecordings.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace org::services
{
	class VariableSnapshot;
}

namespace GraphResources
{
	class Resource;
}

/**
* @brief The revision coordinator's side of the declarations (Frame/Declare.h). Revision.cpp calls it on the render thread, when
* a revision starts and at every Begin.
*/
namespace RenderGraph::Frame::Declarations
{
	/** @brief A twin's next extent, for the coordinator's version path. */
	using Wish = std::function<void(GraphResources::Resource&, const std::array<unsigned int, 3>&)>;

	/** @brief Whether a declaration was made or invalidated since the last Evaluate, so a revision must start. */
	bool Dirty();

	/**
	* @brief When a revision starts with a_values. Evaluates every declaration whose dependencies changed since its last
	* evaluation, or that is new or invalidated. Textures with a new extent go to a_wish. Clients with a new key are registered
	* again here. Returns the stage builds the revision must install for them.
	*/
	std::vector<FrameAnchors::StageBuildNeed> Evaluate(const org::services::VariableSnapshot& a_values, const Wish& a_wish);

	/** @brief At the end of every Begin, frame a_frame. Notes the textures that bind a new version from this frame on. */
	void NoteBegin(std::uint64_t a_frame);
}
