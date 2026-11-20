#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace org
{
	class GloballyIndexedResource;
	struct IResourceVersions;
	class VersionedBuffer;
	class VersionedTexture;
}

namespace org::services
{
	class VariableSnapshot;
}

/**
* @brief The frame revision coordinator (frame revision plan, step 4).
*
* A frame revision changes everything CS stage recordings bind in one frame. Its inputs:
* - declared textures' new extents (Frame/Declare.h): the next versions are made on the revision lane, off the render
*   thread, born cleared for histories. An extent no version can serve (another shape class) is fatal;
* - declared buffers' new element counts (Frame::DeclareBuffer): new versions made on the lane, contents undefined;
* - CS-owned versioned buffers' new contents (PublishBuffer): uploaded from the lane, ready once the upload completes;
* - textures a caller made as new versions of a versioned texture (PublishTexture), such as ImGui's atlas pages;
* - staged builds of CS stages: every frame slot is recorded on the staged build;
* - DCLF's scene snapshots (PublishScene): a scene recorded against the revision in preparation is adopted with it, and one
*   recorded against the adopted versions is adopted alone (plan step 10b);
* - the frame-wide variables (Frame/Variables.h): every revision carries the newest snapshot it started with, and adoption
*   makes it the frame's. A revision with only variable changes needs no lane work and is adopted in the Begin that starts it,
*   unless a variable it changed has an input producer (RegisterInputProducer). Then it is adopted once those inputs are ready.
* The closure is every epoch whose graph declares a changed slot. The CS stages among it, and every stage with a staged build,
* record every frame slot against the pending versions. Once all of it is ready, one Begin adopts it whole: versions, exports,
* staged builds and every member stage's schedule switch in the same frame. One revision is prepared at a time. Inputs that
* arrive meanwhile wait, latest wins.
*/
namespace RenderGraph::Frame::Revision
{
	/** @brief Starts the coordinator and binds the settings' raise hook. Frame::Driver::Install calls it. */
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
	* @brief Render thread, first in Frame::Driver::OnBegin inside the adoption point. Advances the revision in preparation,
	* adopts it when it is ready, and hands newer inputs to the lane. Never waits.
	*/
	void AdoptLatest();

	/**
	* @brief Any thread. a_bytes become a new version of a_buffer (a structured buffer of a_elements), uploaded from the revision
	* lane and adopted with a revision, together with every stage that declares the buffer. a_payload is what the version was made
	* for (AdoptedPayload). Latest wins per buffer. A cancelled upload is fatal.
	*/
	void PublishBuffer(const std::shared_ptr<org::VersionedBuffer>& a_buffer, std::uint32_t a_elements, std::vector<std::byte> a_bytes,
		std::shared_ptr<const void> a_payload);

	/**
	* @brief Any thread. a_image, a texture the caller made, becomes a new version of a_texture, kept alive with a_owner. The
	* version is made pending on the revision lane and adopted with a revision, together with every stage that declares
	* a_texture. Latest wins per texture. Its contents are what the caller's passes write once it is adopted. a_image must have
	* a_texture's format, mip count and slice count; another is fatal.
	*/
	void PublishTexture(const std::shared_ptr<org::VersionedTexture>& a_texture, std::shared_ptr<org::GloballyIndexedResource> a_image,
		std::shared_ptr<const void> a_owner);

	/** @brief Any thread. The payload of a_buffer's version adopted last. Null until a PublishBuffer of it was adopted. */
	std::shared_ptr<const void> AdoptedPayload(const org::VersionedBuffer& a_buffer);

	/**
	* @brief The frame's texture and buffer versions DCLF's scene recordings name (plan step 10b). While a revision whose closure has
	* DCLF epochs is in preparation, they are its pending versions and serial is that revision's. Otherwise versions is null (the
	* adopted ones) and serial is the last adopted revision that had DCLF epochs in its closure, 0 before the first.
	*/
	struct SceneVersions
	{
		std::uint64_t serial = 0;
		std::shared_ptr<const org::IResourceVersions> versions;
	};

	/** @brief A scene's SceneVersions serial when it has no recordings, so any frame revision can run it. */
	inline constexpr std::uint64_t kAnySceneVersions = UINT64_MAX;

	/** @brief Render thread. The versions DCLF's next scene recordings name. */
	SceneVersions SceneFrameVersions();

	/**
	* @brief DCLF's scene snapshot as a fragment of the frame revision (kScene, plan step 10b). Its builder posts one per scene revision.
	* A scene recorded against a revision in preparation is adopted with that revision. One recorded against the adopted versions is
	* adopted alone, at the first Begin that takes it. Any other is dropped, and its builder makes a new one.
	*/
	struct Scene
	{
		std::shared_ptr<const void> value;  // DCLF's snapshot. DCLF owns it: the fragments name it, and a drop hands it back
		std::uint64_t serial = 0;           // for the logs
		std::uint64_t buffersSerial = 0;    // its buffer set's serial (DCLF's NamedVersions), 0 without one
		std::uint64_t frameVersions = kAnySceneVersions;  // the SceneVersions serial its recordings named
		// The build each DCLF epoch's recordings are of: (epoch, build generation). Each must be the attached or the staged one
		// at its adoption. Staged ones attach with it.
		std::vector<std::pair<std::uint32_t, std::uint64_t>> builds;
	};

	/** @brief Why a posted scene was not adopted. */
	enum class SceneDrop
	{
		Stale,          // recorded against other frame versions
		BuildReplaced,  // an epoch's build it was recorded on is neither attached nor staged any more
		Refused,        // ORG refused its staged builds at the adoption
		Superseded,     // a newer scene for the same revision, or its revision was superseded
	};

	/** @brief Any thread (DCLF's snapshot builder). a_scene replaces any scene posted and not taken yet. Latest wins. */
	void PublishScene(Scene a_scene);

	/**
	* @brief Render thread, inside the adoption point. Takes a posted scene and adopts it if it can stand alone. For DCLF's
	* shader-reload wait, which needs a scene in the Begin it waits in. True when one was adopted.
	*/
	bool AdoptScene();

	/** @brief Logs the "[ORG] frame revision:" census line for the window and resets it. */
	void LogStats();
}
