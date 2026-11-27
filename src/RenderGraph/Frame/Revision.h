#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
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
* - fragments of registered sources (RegisterFragmentSource, PublishFragment): one recorded against the
*   revision in preparation is adopted with it, and one recorded against the adopted versions is adopted alone;
* - the frame-wide variables (Frame/Variables.h): every revision carries the newest snapshot it started with, and adoption
*   makes it the frame's. A revision with only variable changes needs no lane work and is adopted in the Begin that starts it,
*   unless a variable it changed has an input producer (RegisterInputProducer). Then it is adopted once those inputs are ready.
*   Listeners that only want the adopted value (Vars::OnAdopted) are no producers and never hold a revision back.
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
		// Nonzero: the change reaches every graph (a shader reload's pipeline versions, RenderGraph/PipelineVersions.h). Every
		// fragment source is then a member of the revision, and a drawing source's fragment must be recorded on builds that pin this
		// generation or a newer one.
		std::uint64_t pipelineGeneration = 0;
	};

	/**
	* @brief Render thread, once a_values carries a change of the variable. Starts the work the change needs and returns how to
	* wait for it. a_revision is the revision's serial. A wait with no poll means the change needs no work this time.
	*/
	using InputProducer = std::function<InputWait(const org::services::VariableSnapshot& a_values, std::uint64_t a_revision)>;

	/**
	* @brief Any thread, before the variable's first change. A revision that starts with a change of a_variable (its name in
	* Frame/Variables.h) calls a_producer, and the revision is adopted only once the input it returns is ready. Its other
	* members prepare meanwhile. A superseded revision's inputs are produced again for the revision that replaces it, so a
	* producer should reuse work already started for the same value. One producer per variable, for work outside the frame only:
	* code that only wants a callback at the adoption uses Vars::OnAdopted.
	* @return False without the render graph. There are no revisions then, and the caller does the work itself.
	*/
	bool RegisterInputProducer(std::string_view a_variable, InputProducer a_producer);

	/**
	* @brief Main thread, first in Frame::Driver::OnBegin inside the adoption point. Advances the revision in preparation,
	* adopts it when it is ready, and hands newer inputs to the lane. Never waits. Another thread's Begin adopts nothing (D-4).
	*/
	void AdoptLatest();

	/**
	* @brief Render owner, at setup, before Engine::PostSetup. a_observe runs once at setup's build point, before the declarations
	* are evaluated there. For a port whose client key reads state the frames would observe (an import), so the setup build already
	* holds its client.
	*/
	void OnSetupBuildPoint(std::function<void()> a_observe);

	/**
	* @brief Render owner, at setup's build point (Engine::PostSetup), after the setup batch of programs and before the setup build
	* (FrameAnchors::BuildPending). Builds the declared programs for the newest values, waiting as the setup batch does, and
	* evaluates every declaration. The clients registered here are in the setup build, so they run from the first frame and their
	* states are adopted now. So no frame kind waits for a first registration. Does nothing without the render graph.
	*/
	void EvaluateAtSetup(std::string_view a_point);

	/**
	* @brief Any thread, before the first frame. a_listener runs on the render owner inside the adoption point of every revision,
	* after its texture and buffer versions are adopted and before its builds attach. For holders of D3D11 handles over declared
	* textures (the engine target set's retarget). Listeners run in the order they were added.
	*/
	void OnVersionsAdopted(std::function<void()> a_listener);

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

	/** @brief A registered fragment source (RegisterFragmentSource). */
	using FragmentSourceId = std::uint32_t;

	/** @brief The most fragment sources. Each has two slots in a revision. */
	inline constexpr std::uint32_t kMaxFragmentSources = 4;

	/** @brief A fragment's frameVersions when it has no recordings, so any frame revision can run it. */
	inline constexpr std::uint64_t kAnyFrameVersions = UINT64_MAX;

	/**
	* @brief The frame's texture and buffer versions a source's next recordings name (cleanup plan A5, step 4). While a revision whose
	* closure has the source's epochs is in preparation, they are its pending versions and serial is that revision's. Otherwise
	* versions is null (the adopted ones) and serial is the last adopted revision that had the source's epochs, 0 before the first.
	*/
	struct FragmentVersions
	{
		std::uint64_t serial = 0;
		std::shared_ptr<const org::IResourceVersions> versions;
		// The frame variables of the revision those versions are: the preparing one's, else the adopted ones. Null before the first
		// adoption. Read with Vars::Lane::ValueIn: sizes the recordings derive (a viewport) must follow the versions they draw into.
		std::shared_ptr<const org::services::VariableSnapshot> variables;
	};

	/**
	* @brief One fragment a source posts: recordings of its epochs, made off the render thread. A fragment recorded against a revision
	* in preparation is adopted with that revision. One recorded against the adopted versions is adopted alone, at the first Begin
	* that takes it. Any other is dropped, and the source makes a new one.
	*/
	struct Fragment
	{
		std::shared_ptr<const void> value;  // the source's. The source owns it: a drop hands it back
		std::uint64_t serial = 0;           // for the logs
		std::uint64_t buffersSerial = 0;    // the serial of the source's own buffer set its recordings name, 0 without one
		std::uint64_t frameVersions = kAnyFrameVersions;  // the FragmentVersions serial its recordings named
		// The build each of its epochs' recordings is of: (epoch, build generation). Each must be the attached or the staged one at
		// its adoption. Staged ones attach with it.
		std::vector<std::pair<std::uint32_t, std::uint64_t>> builds;
	};

	/** @brief Why a posted fragment was not adopted. */
	enum class FragmentDrop
	{
		Stale,          // recorded against other frame versions
		BuildReplaced,  // an epoch's build it was recorded on is neither attached nor staged any more
		Refused,        // ORG refused its staged builds at the adoption
		Superseded,     // a newer fragment for the same revision, or its revision was superseded
	};

	/** @brief What a Begin takes of a fragment source (FragmentSource::take). */
	enum class FragmentTake : std::uint8_t
	{
		Newest,  // the newest posted fragment
		Given,   // the fragment the source hands
		Skip,    // nothing; the source's steps wait for a later Begin
	};

	/**
	* @brief A client whose epochs are recorded off the render thread and adopted as fragments of the frame revision (cleanup plan
	* A5, step 4). Every callback runs on the render owner unless it says otherwise. Only adopt and drop are required.
	*/
	struct FragmentSource
	{
		std::string name;  // for the logs
		// Its epochs, as RenderGraphRuntime::Segment values. They are lane-only build epochs: staged, built on the build lane only.
		std::vector<std::uint32_t> segments;
		// Each segment's label and inputs label, in segments' order ("CS DCLF: Z-prepass", "CS DCLF: Z-prepass inputs"). The GPU
		// traces and the census name the epochs by them. Missing ones fall back to the runtime's.
		std::vector<std::pair<std::string, std::string>> labels;
		// The frame took a fragment from the post, adopted or not.
		std::function<void(const std::shared_ptr<const void>& a_value)> taken;
		// Inside the adoption point, after its staged builds attached.
		std::function<void(const std::shared_ptr<const void>& a_value)> adopt;
		// Not adopted. The source keeps it to release it off the render thread, and makes a new one.
		std::function<void(const std::shared_ptr<const void>& a_value, FragmentDrop a_why, std::string_view a_text, std::span<const std::uint32_t> a_epochs)> drop;
		// Once per Begin, before taking: what this Begin takes of the source. Newest: its newest posted fragment. Given: the fragment the
		// source hands in a_given (empty for none), taken out of the post earlier by a game-side taker (TakePostedFragment). Skip: nothing,
		// and the source's adoption steps are left to a later Begin (a loading screen's thread leaves them). Empty: always Newest.
		std::function<FragmentTake(std::optional<Fragment>& a_given)> take;
		// Once per Begin, before adoption: whether the source draws (it started at an adoption and has not stopped) and its epochs ran
		// in the last frame. Only then does a revision with its epochs in the closure wait for its fragment. Empty: always.
		std::function<bool()> drawing;
		// Inside the adoption point: a revision with the source as a member was adopted without its fragment, because it was not
		// drawing. Its adopted fragment binds the old versions now, so the source stops drawing until its next fragment is adopted.
		// a_revision is the revision's serial. May be empty.
		std::function<void(std::uint64_t a_revision)> left;
		// At each Begin, after the adoption point's revision work. May be empty.
		std::function<void()> afterAdoption;
		// Native mode, at a world frame's Begin, before the schedule is chosen. It may only stage builds. May be empty.
		std::function<void(std::uint64_t a_frame)> frameStart;
		// Whether it draws the world in the frame Begin describes (Description::sceneDraws). May be empty.
		std::function<bool()> drawsWorld;
		// Owner thread: one of its epochs failed (RenderGraphRuntime::OnEpochFailed). May be empty.
		std::function<void(std::string_view a_label, std::string_view a_error)> epochFailed;
	};

	/**
	* @brief Main thread, before the render graph initializes (its segments become lane-only build epochs then). Registers a fragment
	* source. A segment belongs to one source. More than kMaxFragmentSources is fatal.
	*/
	FragmentSourceId RegisterFragmentSource(FragmentSource a_source);

	/** @brief Any thread, after registration. Whether a_segment (a RenderGraphRuntime::Segment value) is a fragment source's. */
	bool IsFragmentSegment(std::uint32_t a_segment);

	/** @brief Any thread, after registration. Every fragment source's segments. */
	std::vector<std::uint32_t> FragmentSegments();

	/**
	* @brief Any thread, after registration. The label its source gave a_segment (a RenderGraphRuntime::Segment value), or with a_inputs
	* its inputs label. Null when no source owns it or gave none. Valid for the session.
	*/
	const char* FragmentSegmentLabel(std::uint32_t a_segment, bool a_inputs);

	/** @brief Owner thread. Hands a failed epoch to the source that owns a_segment. False when no source does. */
	bool FragmentEpochFailed(std::uint32_t a_segment, std::string_view a_label, std::string_view a_error);

	/** @brief Render owner. Native mode's frame start for every source (FragmentSource::frameStart). */
	void FragmentFrameStart(std::uint64_t a_frame);

	/** @brief Render owner. Whether any source draws the world in the frame Begin describes. */
	bool FragmentsDrawWorld();

	/**
	* @brief Render thread. The versions a_source's next recordings name. While a revision with the source as a member is in
	* preparation, its serial, with its pending versions or null when it changes none.
	*/
	FragmentVersions FrameVersionsFor(FragmentSourceId a_source);

	/** @brief Any thread (the source's builder). a_fragment replaces any fragment of a_source posted and not taken yet. Latest wins. */
	void PublishFragment(FragmentSourceId a_source, Fragment a_fragment);

	/**
	* @brief Takes a source's newest posted fragment out of the post, for a source whose take callback hands it to a later Begin (Given).
	* The source's one game-side taker. Empty when nothing new was posted.
	*/
	std::optional<Fragment> TakePostedFragment(FragmentSourceId a_source);

	/** @brief Where adoption stands, for the test runner's "adopted" await. */
	struct AdoptionMark
	{
		bool running = false;               // the coordinator is installed, so revisions adopt
		std::uint64_t revision = 0;         // the last adopted revision's serial, 0 before the first
		std::uint64_t adoptedVariables = 0; // the change serial of the variables that revision carried
		std::uint64_t takenVariables = 0;   // the change serial of the newest variables a Begin took
	};

	/**
	* @brief Any thread. A write a Begin took is adopted once adoptedVariables reaches the takenVariables read after that Begin.
	* Everything is 0 without the render graph.
	*/
	AdoptionMark Mark();

	/** @brief Logs the "[ORG] frame revision:" census line for the window and resets it. */
	void LogStats();
}
