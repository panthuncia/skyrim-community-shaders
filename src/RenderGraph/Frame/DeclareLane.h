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
	class StructuredBuffer;
}

/**
* @brief The revision coordinator's side of the declarations (Frame/Declare.h). Revision.cpp calls it on the render thread, when
* a revision starts and at every Begin.
*/
namespace RenderGraph::Frame::Declarations
{
	/** @brief A twin's next extent, for the coordinator's version path. a_renew asks for a new version even at the same extent. */
	using Wish = std::function<void(GraphResources::Resource&, const std::array<unsigned int, 3>&, bool a_renew)>;

	/** @brief A declared buffer's next element count, for the coordinator's buffer path. */
	using BufferWish = std::function<void(GraphResources::StructuredBuffer&, std::uint32_t a_elements)>;

	/** @brief Whether a declaration was made or invalidated since the last Evaluate, so a revision must start. */
	bool Dirty();

	/**
	* @brief When a revision starts with a_values. Evaluates every declaration whose dependencies changed since its last
	* evaluation, or that is new or invalidated. Textures with a new extent or renew count go to a_wish, buffers with a new element
	* count to a_bufferWish. Clients with a new key are registered again here. Returns the stage builds the revision must install
	* for them.
	*/
	std::vector<FrameAnchors::StageBuildNeed> Evaluate(const org::services::VariableSnapshot& a_values, const Wish& a_wish, const BufferWish& a_bufferWish);

	/**
	* @brief Before a revision starts with a_values. Starts the builds of declared programs whose key a_values changed to one not
	* built yet. True when every declared program has a build for a_values, so the revision may start. Never waits. Fatal when a
	* build failed.
	*/
	bool ProgramsReady(const org::services::VariableSnapshot& a_values);

	/**
	* @brief At the end of every Begin, frame a_frame. Notes the textures and buffers that bind a new version from this frame on, and the
	* programs of the adopted values.
	*/
	void NoteBegin(std::uint64_t a_frame);
}
