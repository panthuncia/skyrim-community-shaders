#pragma once

#include "Features/Upscaling/PresentationConfig.h"

#include <memory>
#include <string>

namespace org::services
{
	class VariableSnapshot;
}

/**
* @brief The revision coordinator's side of the frame-wide variables (frame revision plan, step 5b). Revision.cpp and its input
* producers (Revision::RegisterInputProducer) use it, with the render graph. Render thread only: it is the variable set's one
* consumer.
*/
namespace RenderGraph::Frame::Vars::Lane
{
	/** @brief The newest values. The same snapshot, with no allocation, while nothing changed since the last call. */
	std::shared_ptr<const org::services::VariableSnapshot> Take();

	/** @brief The snapshot of the revision adopted last, or null before the first adoption. */
	const std::shared_ptr<const org::services::VariableSnapshot>& Adopted();

	/** @brief Makes a_snapshot the adopted values. Inside the adoption point. */
	void Adopt(std::shared_ptr<const org::services::VariableSnapshot> a_snapshot);

	/** @brief The presentation a_values carries. */
	FrameGen::PresentationConfig PresentationConfigIn(const org::services::VariableSnapshot& a_values);

	/**
	* @brief The names of the variables a_newer changed since a_older, comma separated. Without a_older, the variables written
	* at least once. Empty when none.
	*/
	std::string ChangedNames(const org::services::VariableSnapshot& a_newer, const org::services::VariableSnapshot* a_older);
}
