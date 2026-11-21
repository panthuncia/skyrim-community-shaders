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
	/** @brief Takes a declared texture's next extent, for the coordinator's version path. a_renew asks for a new version even at the same extent. */
	using TextureExtentFn = std::function<void(GraphResources::Resource&, const std::array<unsigned int, 3>&, bool a_renew)>;

	/**
	* @brief Takes a declared buffer's next element count, for the coordinator's buffer path. a_zero: the version is born zero
	* (Frame::Birth), so the lane uploads zeros into it.
	*/
	using BufferElementsFn = std::function<void(GraphResources::StructuredBuffer&, std::uint32_t a_elements, bool a_zero)>;

	/** @brief Whether a declaration was made or invalidated since the last Evaluate, so a revision must start. */
	bool Dirty();

	/**
	* @brief When a revision starts with a_values, on the render owner (fatal elsewhere). Evaluates every declaration whose
	* dependencies changed since its last evaluation, or that is new or invalidated. Textures with a new extent or renew count go
	* to a_textures, buffers with a new element count to a_buffers. Clients with a new key are registered again here. Returns the
	* stage builds the revision must install for them.
	*/
	std::vector<FrameAnchors::StageBuildNeed> Evaluate(const org::services::VariableSnapshot& a_values, const TextureExtentFn& a_textures,
		const BufferElementsFn& a_buffers);

	/**
	* @brief Before a revision starts with a_values. Starts the builds of declared programs whose key a_values changed to one not
	* built yet. True when every declared program has a build for a_values, so the revision may start. Never waits. Fatal when a
	* build failed.
	*/
	bool ProgramsReady(const org::services::VariableSnapshot& a_values);

	/**
	* @brief Render owner, at setup's build point, before any frame. Starts the builds of the declared programs for a_values and
	* waits for them (a setup wait, as Programs::JoinSetup). Fatal when one failed.
	*/
	void JoinProgramsAtSetup(const org::services::VariableSnapshot& a_values);

	/**
	* @brief Render owner, at setup's build point, after Evaluate and before the setup build. The clients registered there are the
	* frame's from the first frame, since the setup build holds them, so their states are adopted now. Returns how many were.
	*/
	std::size_t AdoptAtSetup();

	/**
	* @brief At the end of every Begin, frame a_frame. Notes the textures and buffers that bind a new version from this frame on, and the
	* programs of the adopted values.
	*/
	void NoteBegin(std::uint64_t a_frame);
}
