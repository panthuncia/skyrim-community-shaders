#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace org
{
	class VersionedBuffer;
}

/**
* @brief The frame revision coordinator (frame revision plan, step 4).
*
* A frame revision changes everything CS stage recordings bind in one frame. Its inputs:
* - twins' size wishes (GraphResources::Texture2D/Texture3D::RequestSize): the next versions are made on the revision lane, off
*   the render thread, born cleared for histories;
* - CS-owned versioned buffers' new contents (PublishBuffer): uploaded from the lane, ready once the upload completes;
* - staged builds of CS stages: every frame slot is recorded on the staged build.
* The closure is every epoch whose graph declares a changed slot. The CS stages among it, and every stage with a staged build,
* record every frame slot against the pending versions. Once all of it is ready, one Begin adopts it whole: versions, exports,
* staged builds and every member stage's schedule switch in the same frame. One revision is prepared at a time. Inputs that
* arrive meanwhile wait, latest wins. A wish no version can serve (another shape class) takes today's path: a resize at the
* next Begin.
*/
namespace RenderGraph::Frame::Revision
{
	/** @brief Takes the twins' size wishes. Frame::Driver::Install calls it. Does nothing without the render graph. */
	void Install();

	/**
	* @brief Render thread, first in Frame::Driver::OnBegin inside the adoption point. Applies wishes no version serves, advances
	* the revision in preparation, adopts it when it is ready, and hands newer inputs to the lane. Never waits.
	*/
	void AdoptLatest();

	/**
	* @brief Any thread. a_bytes become a new version of a_buffer (a structured buffer of a_elements), uploaded from the revision
	* lane and adopted with a revision, together with every stage that declares the buffer. a_payload is what the version was made
	* for (AdoptedPayload). Latest wins per buffer. A cancelled upload is fatal.
	*/
	void PublishBuffer(const std::shared_ptr<org::VersionedBuffer>& a_buffer, std::uint32_t a_elements, std::vector<std::byte> a_bytes,
		std::shared_ptr<const void> a_payload);

	/** @brief Any thread. The payload of a_buffer's version adopted last. Null until a PublishBuffer of it was adopted. */
	std::shared_ptr<const void> AdoptedPayload(const org::VersionedBuffer& a_buffer);

	/** @brief Logs the "[ORG] frame revision:" census line for the window and resets it. */
	void LogStats();
}
