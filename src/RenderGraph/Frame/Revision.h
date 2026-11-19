#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace org
{
	class VersionedBuffer;
}

namespace org::services
{
	class VariableSnapshot;
}

/**
* @brief The frame revision coordinator (frame revision plan, step 4).
*
* A frame revision changes everything CS stage recordings bind in one frame. Its inputs:
* - twins' size wishes (GraphResources::Texture2D/Texture3D::RequestSize): the next versions are made on the revision lane, off
*   the render thread, born cleared for histories;
* - CS-owned versioned buffers' new contents (PublishBuffer): uploaded from the lane, ready once the upload completes;
* - staged builds of CS stages: every frame slot is recorded on the staged build;
* - the frame-wide variables (Frame/Variables.h): every revision carries the newest snapshot it started with, and adoption
*   makes it the frame's. A revision with only variable changes needs no lane work and is adopted in the Begin that starts it,
*   unless a variable it changed has an input producer (RegisterInputProducer). Then it is adopted once those inputs are ready.
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

	/** @brief Where an input a revision waits for stands. */
	enum class InputState
	{
		Pending,
		Ready,
		Failed,
	};

	/** @brief Work outside the frame that a revision's change of a variable needs before the revision may be adopted. */
	struct InputWait
	{
		std::string what;  // for the logs, as "the swap chain replacement (request 3)"
		// Render thread, at each Begin until it is not Pending. Failed is fatal and names a_why.
		std::function<InputState(std::string& a_why)> poll;
		// Render thread, inside the adoption point of the revision. May be empty.
		std::function<void()> adopt;
	};

	/**
	* @brief Render thread, once a_values carries a change of the variable. Starts the work the change needs and returns how to
	* wait for it. a_revision is the revision's serial.
	*/
	using InputProducer = std::function<InputWait(const org::services::VariableSnapshot& a_values, std::uint64_t a_revision)>;

	/**
	* @brief Any thread, before the variable's first change. A revision that starts with a change of a_variable (its name in
	* Frame/Variables.h) calls a_producer, and the revision is adopted only once the input it returns is ready. Its other
	* members prepare meanwhile. A superseded revision's inputs are produced again for the revision that replaces it, so a
	* producer should reuse work already started for the same value. One producer per variable.
	* @return False without the render graph. There are no revisions then, and the caller does the work itself.
	*/
	bool RegisterInputProducer(std::string_view a_variable, InputProducer a_producer);

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
