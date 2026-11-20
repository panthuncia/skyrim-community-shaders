#include "RenderGraph/Frame/Revision.h"

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

#	include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"
#	include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/Frame/BoundaryCheck.h"
#	include "RenderGraph/Frame/DeclareLane.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/RevisionRecordings.h"
#	include "RenderGraph/Frame/Variables.h"
#	include "RenderGraph/Frame/VariablesLane.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/Setting.h"
#	if !defined(CS_RELEASE_PROFILE)
#		include "Testing/Testing.h"
#	endif

#	include <ORGModuleServices/Async/RevisionAssembly.h>
#	include <ORGModuleServices/Async/SerializedTaskPump.h>
#	include <ORGModuleServices/BackingPool.h>
#	include <ORGModuleServices/ComposedVersions.h>
#	include <ORGModuleServices/GrowableBufferSet.h>
#	include <ORGModuleServices/VersionedBufferBuild.h>
#	include <ORGModuleServices/Variables.h>
#	include <ORGModuleServices/VersionedTextureGrowth.h>
#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/Runtime/IUploadService.h>
#	include <Resources/Buffers/VersionedBuffer.h>
#	include <Resources/VersionedTexture.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <chrono>
#	include <deque>
#	include <map>
#	include <mutex>
#	include <optional>
#	include <span>
#	include <string>
#	include <vector>

namespace DCLF
{
	// DCLF's side of its scene fragment (plan step 10b), in Core/Render/FrameDriver.cpp. Render thread, main thread only.
	// The frame took a_scene from the builder's post, adopted or not.
	void SceneTaken(const std::shared_ptr<const void>& a_scene);
	// Inside the adoption point, after its staged builds attached.
	void InstallScene(const std::shared_ptr<const void>& a_scene);
	// Not adopted. DCLF keeps it to release it on its builder, and makes a new one.
	void DropScene(const std::shared_ptr<const void>& a_scene, RenderGraph::Frame::Revision::SceneDrop a_why, std::string_view a_text,
		std::span<const std::uint32_t> a_epochs);
	// Any thread. The frames whose coverage ran a DCLF epoch, in the session. While it moves, a revision with DCLF epochs in its
	// closure must adopt a scene recorded for it.
	std::uint64_t SceneRecordedFrames();
	// After the scene step of every Begin: the named shader-reload wait (plan step 10a).
	void AfterSceneStep();
}

namespace RenderGraph::Frame::Revision
{
	namespace
	{
		using GraphResources::Extent;

		// The revision's fragments (plan, step 4c). Inputs hold the variables' snapshot (step 5b). Input waits hold the work
		// outside the frame that changed variables need (RegisterInputProducer), such as a swap chain replacement.
		constexpr std::uint32_t kRevisionInputs = 0;
		constexpr std::uint32_t kRevisionTextures = 1;
		constexpr std::uint32_t kRevisionBuffers = 2;
		constexpr std::uint32_t kRevisionBuilds = 3;
		constexpr std::uint32_t kRevisionRecordings = 4;
		constexpr std::uint32_t kRevisionInputWaits = 5;
		// DCLF's scene (kScene, plan step 10b): its publication, draws and payload, and the buffer set its recordings name. They
		// inherit like the others, so a revision of CS stages keeps the adopted scene and a scene alone keeps the rest.
		constexpr std::uint32_t kRevisionScene = 6;
		constexpr std::uint32_t kRevisionSceneBuffers = 7;
		constexpr std::uint32_t kRevisionSlots = 8;

		// The starvation rule (plan 7.8): recordings dropped and asked for again this often with no new input is a schedule bug.
		constexpr std::uint64_t kRevisionStarvedAfter = 4 * RenderGraphRuntime::kGameFramesInFlight;
		// A revision with DCLF epochs in its closure waits for DCLF's scene only while DCLF's epochs ran in one of this many last
		// Begins (plan 7.8). A menu without a scene pass, a load screen or an idle DCLF posts no scene, so then it adopts alone.
		constexpr std::uint64_t kSceneIdleFrames = 4 * RenderGraphRuntime::kGameFramesInFlight;

		/** @brief A declared texture's newest extent, held by its twin's Alive() token since the twin may go first. */
		struct ExtentRequest
		{
			std::weak_ptr<const void> alive;
			GraphResources::Resource* twin = nullptr;
			Extent extent{};
			std::uint64_t frame = 0;  // the frame of the first extent since the twin last had the one asked for
			std::uint64_t renew = 0;  // nonzero: a new version even at the twin's extent (a declaration's renew count changed)
		};

		/**
		* @brief A buffer's newest contents, waiting for the next revision. A declared buffer's (Frame::DeclareBuffer) has no bytes:
		* its new element count needs no upload, and its twin takes the version at adoption.
		*/
		struct BufferRequest
		{
			std::weak_ptr<org::VersionedBuffer> buffer;
			std::uint32_t elements = 0;
			std::shared_ptr<std::vector<std::byte>> bytes;
			std::shared_ptr<const void> payload;
			std::weak_ptr<const void> twinAlive;
			GraphResources::StructuredBuffer* twin = nullptr;
		};

		/** @brief A texture the caller made, waiting to become its slot's next version (PublishTexture). */
		struct TextureRequest
		{
			std::weak_ptr<org::VersionedTexture> texture;
			std::shared_ptr<org::GloballyIndexedResource> image;
			std::shared_ptr<const void> owner;
		};

		/** @brief One buffer of a revision: its pending version and what the version was made for. */
		struct RevisionBuffer
		{
			const void* key = nullptr;  // VersionedBuffer::Key
			std::weak_ptr<org::VersionedBuffer> buffer;
			std::string name;
			std::uint32_t elements = 0;
			std::shared_ptr<const org::BufferVersion> version;
			std::shared_ptr<const void> payload;
			std::shared_ptr<std::vector<std::byte>> bytes;  // kept until the upload completed. Null for a declared buffer
			std::weak_ptr<const void> twinAlive;
			GraphResources::StructuredBuffer* twin = nullptr;  // a declared buffer's twin, which takes the version at adoption
		};

		/** @brief The value of the buffers fragment: names its buffers' pending versions to the recordings made for the revision. */
		class RevisionBuffers final : public org::IResourceVersions
		{
		public:
			explicit RevisionBuffers(std::vector<RevisionBuffer> a_buffers) :
				buffers(std::move(a_buffers)) {}

			std::shared_ptr<const org::BufferVersion> Find(const org::VersionedBuffer& a_buffer) const noexcept override
			{
				for (const auto& entry : buffers)
					if (entry.key == a_buffer.Key())
						return entry.version;
				return nullptr;
			}

			std::vector<RevisionBuffer> buffers;
		};

		/** @brief The value of the inputs fragment: the variables' values the revision started with. */
		struct RevisionInputs
		{
			std::shared_ptr<const org::services::VariableSnapshot> variables;
		};

		/** @brief One input a revision waits for, from a variable's producer. */
		struct RevisionInputWait
		{
			std::string variable;
			InputWait wait;
			bool ready = false;
		};

		/** @brief The value of the input waits fragment: what adoption runs for each input. */
		struct RevisionInputWaits
		{
			std::vector<std::function<void()>> adopt;
		};

		/** @brief The value of the builds fragment: the staged builds the revision attaches. */
		struct RevisionBuilds
		{
			std::vector<std::uint32_t> epochs;
		};

		/** @brief The value of the recordings fragment. */
		struct RevisionMemberRecordings
		{
			std::shared_ptr<FrameAnchors::RevisionRecordings> recordings;
		};

		/**
		* @brief The value of the scene fragment: the DCLF scene the revision runs. It names the scene and DCLF owns it, so nothing of
		* a scene is released on the render thread.
		*/
		struct RevisionScene
		{
			std::uint64_t serial = 0;
			std::uint64_t frameVersions = 0;
		};

		/** @brief The value of the scene buffers fragment: the serial of the DCLF buffer set the scene's recordings name. */
		struct RevisionSceneBuffers
		{
			std::uint64_t serial = 0;
		};

		/** @brief One revision: what the render thread planned, what the lane made of it, and its recordings. */
		struct PreparedRevision
		{
			// Written on the render thread before the lane takes it.
			std::uint64_t serial = 0;
			std::vector<std::shared_ptr<const GraphResources::PreparePlan>> plans;
			std::vector<GraphResources::Resource*> twins;
			std::vector<std::weak_ptr<const void>> alive;
			std::vector<std::string> names;
			std::vector<Extent> from;  // each twin's extent when planned
			std::vector<Extent> to;
			std::vector<std::uint64_t> renews;  // each twin's renew serial when planned
			std::vector<BufferRequest> bufferRequests;
			std::vector<TextureRequest> textureRequests;  // kept until adoption, so a superseded revision can give them back
			// The newest variables when it started, and the names of those changed since the revision before.
			std::shared_ptr<const org::services::VariableSnapshot> variables;
			std::string changedVariables;
			// What its variable changes need outside the frame, and the fragment that resolves once all of it is ready.
			std::vector<RevisionInputWait> inputWaits;
			std::shared_ptr<org::async::RevisionFragment> inputWaitsFragment;
			// The stage builds its declared clients' registrations need (Frame/Declare.h).
			std::vector<FrameAnchors::StageBuildNeed> needs;
			std::uint64_t firstInput = 0;
			std::uint64_t planned = 0;  // the frame it was handed to the lane
			std::shared_ptr<org::runtime::IUploadService> uploads;
			org::GraphHost* host = nullptr;
			// Written on the lane, then done.
			std::vector<GraphResources::PreparedVersion> versions;
			std::shared_ptr<const org::services::TextureVersionsMade> made;
			std::vector<std::shared_ptr<org::VersionedTexture>> published;  // the slots of textureRequests still alive
			std::vector<std::string> publishedNames;
			std::shared_ptr<const RevisionBuffers> buffers;
			std::shared_ptr<org::async::RevisionFragment> buffersFragment;
			std::shared_ptr<const org::async::GpuSubmissionSet> bufferTickets;  // notifies buffersFragment
			std::vector<std::uint32_t> closure;
			std::string error;
			double milliseconds = 0.0;
			std::atomic<bool> done{ false };
			// Render thread, once the lane is done.
			std::shared_ptr<const org::services::ComposedVersions> versionSet;
			std::shared_ptr<FrameAnchors::RevisionRecordings> recordings;
			std::vector<std::uint32_t> others;  // closure epochs that are not CS stages
			// Some of others are DCLF's. Then DCLF records its scene against versionSet, and the scene is adopted with it.
			bool dclfMembers = false;
			std::uint64_t askedAgain = 0;
			std::uint32_t recordingCount = 0;
			std::uint32_t buildCount = 0;
		};

		struct RevisionStats
		{
			std::uint64_t prepared = 0, adopted = 0, superseded = 0, askedAgain = 0, builds = 0, buffers = 0, variableRevisions = 0;
			std::uint32_t maxMembers = 0, maxRecordings = 0, maxBuilds = 0;
			std::vector<std::uint64_t> inputFrames;
			// DCLF's scenes: adopted alone, adopted with a revision, and dropped by SceneDrop.
			std::uint64_t scenesAlone = 0, scenesWithRevision = 0;
			std::array<std::uint64_t, 4> scenesDropped{};
		};

		struct Coordinator
		{
			bool installed = false;
			// The lane's hand-off: the render thread posts one revision, the lane takes it.
			std::mutex laneMutex;
			std::shared_ptr<PreparedRevision> laneJob;
			org::async::SerializedTaskPump* pump = nullptr;
			// Buffer contents, from any thread.
			std::mutex bufferMutex;
			std::map<const void*, BufferRequest> bufferRequests;
			std::uint64_t bufferFirstInput = 0;
			// Textures the callers made, from any thread.
			std::mutex textureMutex;
			std::map<const void*, TextureRequest> textureRequests;
			std::uint64_t textureFirstInput = 0;
			std::atomic<std::uint64_t> beginFrame{ 0 };  // the frame of the last Begin, for extents from other threads
			std::map<const void*, std::shared_ptr<const void>> adoptedPayloads;
			// DCLF's newest scene, from its builder (PublishScene).
			std::mutex sceneMutex;
			std::optional<Scene> postedScene;
			// Render thread from here.
			// A scene recorded against the revision in preparation, adopted with it.
			std::optional<Scene> heldScene;
			// The last adopted revision that had DCLF epochs in its closure: the frame versions a scene adopted alone must name.
			std::uint64_t adoptedSceneVersions = 0;
			// DCLF's SceneRecordedFrames at the last Begin, and the Begin's frame when it last moved (plan 7.8).
			std::uint64_t sceneRecordedSeen = 0, sceneRecordedFrame = 0;
			// Scenes that could not serve a revision waiting for DCLF's scene, since the last new input: posted on other versions,
			// or held and dropped for their builds. The starvation rule (plan 7.8) for the scene.
			std::uint64_t sceneMissesSinceInput = 0;
			std::map<GraphResources::Resource*, ExtentRequest> extents;
			std::uint64_t extentSerial = 0;
			std::uint64_t renewSerial = 0;
			std::uint64_t plannedSerial = 0;
			std::uint64_t askedAgainSinceInput = 0;
			std::uint64_t serials = 0;
			// The variables: the newest snapshot taken at Begin and the one the last started revision carries.
			std::shared_ptr<const org::services::VariableSnapshot> variables;
			std::shared_ptr<const org::services::VariableSnapshot> startedVariables;
			std::uint64_t variablesFirstChange = 0;  // the frame of the first change no revision carries yet
			std::shared_ptr<PreparedRevision> preparing;
			std::shared_ptr<PreparedRevision> adopted;
			// The needs of a superseded revision, carried to the next one.
			std::vector<FrameAnchors::StageBuildNeed> carriedNeeds;
			std::deque<std::pair<std::uint64_t, std::shared_ptr<PreparedRevision>>> retiring;
			std::unique_ptr<org::async::RevisionAssembler> assembler = std::make_unique<org::async::RevisionAssembler>(kRevisionSlots);
			std::uint64_t adoptedLines = 0;
			// The input producers by variable name (RegisterInputProducer).
			std::mutex producerMutex;
			std::map<std::string, InputProducer, std::less<>> producers;
			// The census, read by LogStats on any thread.
			std::mutex statsMutex;
			RevisionStats window;
			RevisionStats session;
		};

		Coordinator& GetCoordinator()
		{
			// Never destroyed: the lane may still finish a preparation during process teardown.
			static auto* coordinator = new Coordinator;
			return *coordinator;
		}

		[[noreturn]] void FailRevision(std::string_view a_why)
		{
			const auto message = fmt::format("Community Shaders: a frame revision failed: {}", a_why);
			logger::critical("[ORG] frame revision: {}", message);
			spdlog::default_logger()->flush();
			stl::report_and_fail(message);
		}

		std::string ExtentText(const Extent& a_extent)
		{
			return a_extent[2] > 1 ? fmt::format("{}x{}x{}", a_extent[0], a_extent[1], a_extent[2]) : fmt::format("{}x{}", a_extent[0], a_extent[1]);
		}

		std::string ErrorText(const std::exception_ptr& a_error)
		{
			try {
				std::rethrow_exception(a_error);
			} catch (const std::exception& e) {
				return e.what();
			} catch (...) {
			}
			return "unknown";
		}

		/** @brief An epoch that is not a CS stage, named by its segment. */
		std::string EpochText(std::uint32_t a_epoch)
		{
			using Segment = RenderGraphRuntime::Segment;
			for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(Segment::Count); ++s)
				if (RenderGraphRuntime::EpochOf(static_cast<Segment>(s)) == a_epoch)
					return RenderGraphRuntime::IsDclfSegment(static_cast<Segment>(s)) ? fmt::format("DCLF epoch {} (segment {})", a_epoch, s) :
					                                                                    fmt::format("epoch {} (segment {})", a_epoch, s);
			return fmt::format("epoch {}", a_epoch);
		}

		/** @brief Whether a_epoch is one of DCLF's. */
		bool IsDclfEpoch(std::uint32_t a_epoch)
		{
			using Segment = RenderGraphRuntime::Segment;
			for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(Segment::Count); ++s)
				if (RenderGraphRuntime::IsDclfSegment(static_cast<Segment>(s)) && RenderGraphRuntime::EpochOf(static_cast<Segment>(s)) == a_epoch)
					return true;
			return false;
		}

		/** @brief Marks a revision whose closure has DCLF epochs. Render thread. */
		void NoteDclfMembers(PreparedRevision& a_revision)
		{
			a_revision.dclfMembers = a_revision.dclfMembers || std::ranges::any_of(a_revision.others, IsDclfEpoch);
		}

		/** @brief Hands a scene that is not adopted back to DCLF, and counts it. Render thread. */
		void DropScene(Coordinator& a_coordinator, Scene a_scene, SceneDrop a_why, std::string_view a_text, std::span<const std::uint32_t> a_epochs = {})
		{
			{
				const std::lock_guard lock(a_coordinator.statsMutex);
				++a_coordinator.window.scenesDropped[static_cast<std::size_t>(a_why)];
				++a_coordinator.session.scenesDropped[static_cast<std::size_t>(a_why)];
			}
			DCLF::DropScene(a_scene.value, a_why, a_text, a_epochs);
		}

		/**
		* @brief Takes DCLF's newest posted scene. One recorded against the revision in preparation is held for its adoption, and
		* the one held before is dropped. Any other is returned, to be adopted alone or dropped. Render thread, main thread.
		*/
		std::optional<Scene> TakePostedScene(Coordinator& a_coordinator)
		{
			std::optional<Scene> scene;
			{
				const std::lock_guard lock(a_coordinator.sceneMutex);
				scene = std::move(a_coordinator.postedScene);
				a_coordinator.postedScene.reset();
			}
			if (!scene)
				return std::nullopt;
			DCLF::SceneTaken(scene->value);
			const auto& preparing = a_coordinator.preparing;
			if (scene->frameVersions == kAnySceneVersions || !preparing || !preparing->dclfMembers || scene->frameVersions != preparing->serial) {
				// A scene with recordings on other versions while a revision needs DCLF's scene is a miss for the starvation rule.
				if (scene->frameVersions != kAnySceneVersions && preparing && preparing->dclfMembers)
					++a_coordinator.sceneMissesSinceInput;
				return scene;
			}
			if (a_coordinator.heldScene)
				DropScene(a_coordinator, std::move(*a_coordinator.heldScene), SceneDrop::Superseded, "a newer scene was recorded for the same revision");
			a_coordinator.heldScene = std::move(scene);
			return std::nullopt;
		}

		/** @brief Notes whether DCLF's epochs ran since the last Begin. Render thread, every Begin. */
		void NoteSceneRuns(Coordinator& a_coordinator, std::uint64_t a_frame)
		{
			if (const std::uint64_t runs = DCLF::SceneRecordedFrames(); runs != a_coordinator.sceneRecordedSeen) {
				a_coordinator.sceneRecordedSeen = runs;
				a_coordinator.sceneRecordedFrame = a_frame;
			}
		}

		/**
		* @brief Whether DCLF's epochs ran in one of the last kSceneIdleFrames Begins. Only then does a revision with DCLF epochs in its
		* closure wait for DCLF's scene (plan 7.8): a DCLF that does not draw posts no scene. Render thread.
		*/
		bool DclfDraws(const Coordinator& a_coordinator, std::uint64_t a_frame)
		{
			return a_coordinator.sceneRecordedSeen != 0 && a_frame - a_coordinator.sceneRecordedFrame <= kSceneIdleFrames;
		}

		/**
		* @brief Whether every build a_scene's recordings are of is attached or staged. a_staged gets the staged ones, which attach with
		* it. a_why says which is neither.
		*/
		bool SceneBuildsReady(const Scene& a_scene, std::vector<std::uint32_t>& a_staged, std::string& a_why)
		{
			a_staged.clear();
			if (a_scene.builds.empty())
				return true;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host) {
				a_why = "the graph host is gone";
				return false;
			}
			for (const auto& [epoch, generation] : a_scene.builds) {
				const std::uint64_t attached = host->BuildGeneration(epoch);
				if (generation == attached)
					continue;
				const std::uint64_t staged = host->StagedGeneration(epoch);
				if (generation != 0 && generation == staged) {
					if (std::ranges::find(a_staged, epoch) == a_staged.end())
						a_staged.push_back(epoch);
					continue;
				}
				a_why = fmt::format("{} was recorded on build {}, and its attached build is {}, its staged one {}", EpochText(epoch), generation, attached, staged);
				return false;
			}
			return true;
		}

		/**
		* @brief Attaches the staged builds a_scene was recorded on. False when it cannot be adopted: it is dropped then. Render thread,
		* inside the adoption point.
		*/
		bool AttachSceneBuilds(Coordinator& a_coordinator, Scene& a_scene)
		{
			std::vector<std::uint32_t> staged;
			std::string why;
			if (!SceneBuildsReady(a_scene, staged, why)) {
				DropScene(a_coordinator, std::move(a_scene), SceneDrop::BuildReplaced, why);
				return false;
			}
			if (staged.empty())
				return true;
			const Boundary::Caller caller("dclf scene");
			// DCLF's builds can meet a host configuration change from its own conditions, so a refusal drops the scene and DCLF builds
			// them again (plan step 10a). Nothing of the revision was adopted yet.
			if (const auto refused = RenderGraph::Builds::AttachStaged(staged); !refused.empty()) {
				DropScene(a_coordinator, std::move(a_scene), SceneDrop::Refused, refused, staged);
				return false;
			}
			return true;
		}

		/** @brief Puts a_scene's fragments in a_draft. */
		void SetScene(org::async::RevisionAssembler::Draft& a_draft, const Scene& a_scene)
		{
			auto buffersValue = std::make_shared<RevisionSceneBuffers>();
			buffersValue->serial = a_scene.buffersSerial;
			const auto buffers = org::async::RevisionFragment::MakeReady(std::move(buffersValue));
			a_draft.Set(kRevisionSceneBuffers, buffers);
			auto sceneValue = std::make_shared<RevisionScene>();
			sceneValue->serial = a_scene.serial;
			sceneValue->frameVersions = a_scene.frameVersions;
			a_draft.Set(kRevisionScene, org::async::RevisionFragment::MakeReady(std::move(sceneValue), { { kRevisionSceneBuffers, buffers } }));
		}

		/** @brief Installs a scene the revision assembler selected. Render thread, inside the adoption point. */
		void InstallScene(const Scene& a_scene)
		{
			const Boundary::Caller caller("dclf scene");
			DCLF::InstallScene(a_scene.value);
		}

		/**
		* @brief Adopts a scene that names no revision in preparation: alone, as a revision of the scene fragments only, when it names
		* the adopted frame versions. Otherwise it is dropped. Render thread, inside the adoption point.
		*/
		bool AdoptSceneAlone(Coordinator& a_coordinator, std::optional<Scene>& a_scene)
		{
			if (!a_scene)
				return false;
			auto scene = std::move(*a_scene);
			a_scene.reset();
			if (scene.frameVersions != kAnySceneVersions && scene.frameVersions != a_coordinator.adoptedSceneVersions) {
				const auto text = fmt::format("its recordings name the frame versions of revision {}, and the frame runs those of revision {}", scene.frameVersions,
					a_coordinator.adoptedSceneVersions);
				DropScene(a_coordinator, std::move(scene), SceneDrop::Stale, text);
				return false;
			}
			if (!AttachSceneBuilds(a_coordinator, scene))
				return false;
			auto& assembler = *a_coordinator.assembler;
			auto draft = assembler.Begin();
			SetScene(draft, scene);
			assembler.Seal(std::move(draft));
			if (!assembler.Collect().failures.empty() || assembler.TrySelect() != org::async::RevisionAssembler::Selection::Selected)
				FailRevision(fmt::format("DCLF's scene {} was complete but not selected", scene.serial));
			InstallScene(scene);
			const std::lock_guard lock(a_coordinator.statsMutex);
			++a_coordinator.window.scenesAlone;
			++a_coordinator.session.scenesAlone;
			return true;
		}

		/**
		* @brief The buffers' versions made and their uploads queued on the uploader's worker path. The fragment resolves once every
		* upload completed, and fails when one is cancelled. A declared buffer's version has nothing to upload: its passes write it
		* before they read it. With no upload at all the fragment resolves here. Lane.
		*/
		void MakeBufferVersions(PreparedRevision& a_job)
		{
			std::vector<RevisionBuffer> entries;
			std::vector<std::shared_ptr<org::TrackedUploadTicket>> tickets;
			for (auto& input : a_job.bufferRequests) {
				auto buffer = input.buffer.lock();
				if (!buffer)
					continue;
				RevisionBuffer entry{ .key = buffer->Key(), .buffer = buffer, .elements = input.elements, .payload = std::move(input.payload), .bytes = std::move(input.bytes),
					.twinAlive = input.twinAlive, .twin = input.twin };
				entry.version = buffer->MakeStructured(input.elements);
				entry.name = entry.version->buffer ? entry.version->buffer->GetName() : std::string("a buffer");
				if (!entry.bytes) {
					entries.push_back(std::move(entry));
					continue;
				}
				if (!a_job.uploads)
					throw std::runtime_error(fmt::format("{} has no uploader", entry.name));
				const org::StreamingUploadSegment segment{ entry.bytes->data(), entry.bytes->size() };
				// The version is pending: no queue reads it before a revision adopts it, after this copy.
				auto ticket = a_job.uploads->QueueTrackedStreamingUploadSegments({ &segment, 1 }, segment.size,
					org::WorkerOwnedDestination{ entry.version->buffer, org::WorkerOwnedDestination::Ownership::PendingVersion });
				if (!ticket)
					throw std::runtime_error(fmt::format("the uploader refused {}", entry.name));
				tickets.push_back(std::move(ticket));
				entries.push_back(std::move(entry));
			}
			a_job.bufferRequests.clear();
			if (entries.empty())
				return;
			auto buffers = std::make_shared<const RevisionBuffers>(std::move(entries));
			auto fragment = std::make_shared<org::async::RevisionFragment>();
			a_job.buffers = buffers;
			a_job.buffersFragment = fragment;
			if (tickets.empty()) {
				(void)fragment->Resolve(std::move(buffers));
				return;
			}
			const auto ready = org::services::TokenForTickets(std::move(tickets));
			// Settled from the uploader's notifications, once. The callback holds only weak references, so it ends no cycle.
			ready->subscribe([weakReady = std::weak_ptr<const org::async::GpuSubmissionSet>(ready), weakFragment = std::weak_ptr(fragment),
								 weakBuffers = std::weak_ptr<const RevisionBuffers>(buffers)] {
				const auto token = weakReady.lock();
				const auto settling = weakFragment.lock();
				if (!token || !settling || settling->GetState() != org::async::RevisionFragment::State::Pending)
					return;
				if (token->Failed())
					settling->Fail(std::make_exception_ptr(std::runtime_error("its upload was cancelled: " + token->Failure())));
				else if (token->Complete())
					if (auto value = weakBuffers.lock())
						settling->Resolve(std::move(value));
			});
			// The token lives as long as the revision, so its notifications reach the fragment.
			a_job.bufferTickets = ready;
		}

		/** @brief The lane's work: every version made with its memory, the fills and uploads queued, and the closure. Never on the render thread. */
		void DrainRevisionLane()
		{
			auto& coordinator = GetCoordinator();
			std::shared_ptr<PreparedRevision> job;
			{
				const std::lock_guard lock(coordinator.laneMutex);
				job = std::move(coordinator.laneJob);
			}
			if (!job)
				return;
			ZoneScopedN("CS.Frame.Revision.Prepare");
			const auto start = std::chrono::steady_clock::now();
			try {
				std::vector<org::services::TextureVersionRequest> requests;
				requests.reserve(job->plans.size());
				for (const auto& plan : job->plans) {
					auto prepared = GraphResources::Resource::Prepare(*plan);
					org::services::TextureVersionRequest request;
					request.slot = prepared.slot;
					request.image = prepared.resource;
					request.extent = { prepared.extent[0], prepared.extent[1], prepared.extent[2] };
					request.contents = prepared.contents == GraphResources::Contents::Zero ? org::services::TextureContents::Zero :
					                                                                          org::services::TextureContents::Undefined;
					// An exported version's image lives as long as the version.
					request.owner = prepared.exported;
					requests.push_back(std::move(request));
					job->versions.push_back(std::move(prepared));
				}
				// The callers' textures: their passes write them once adopted, so nothing fills them.
				for (const auto& input : job->textureRequests) {
					auto texture = input.texture.lock();
					if (!texture)
						continue;
					org::services::TextureVersionRequest request;
					request.slot = texture;
					request.image = input.image;
					request.contents = org::services::TextureContents::Undefined;
					request.owner = input.owner;
					requests.push_back(std::move(request));
					job->publishedNames.push_back(input.image->GetName());
					job->published.push_back(std::move(texture));
				}
				if (!requests.empty())
					job->made = std::make_shared<const org::services::TextureVersionsMade>(org::services::MakeTextureVersions(requests, job->uploads));
				MakeBufferVersions(*job);
				std::vector<const org::IVersionedResource*> slots;
				for (const auto& version : job->versions)
					slots.push_back(version.slot.get());
				for (const auto& texture : job->published)
					slots.push_back(texture.get());
				std::vector<std::shared_ptr<org::VersionedBuffer>> held;
				if (job->buffers)
					for (const auto& entry : job->buffers->buffers)
						if (auto buffer = entry.buffer.lock()) {
							slots.push_back(buffer.get());
							held.push_back(std::move(buffer));
						}
				job->closure = job->host->EpochsDeclaring(slots);
			} catch (const std::exception& e) {
				job->error = e.what();
			}
			job->milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			job->done.store(true, std::memory_order_release);
		}

		org::async::SerializedTaskPump& RevisionLane()
		{
			auto& coordinator = GetCoordinator();
			if (!coordinator.pump) {
				// A lane on DCLF's executor, the one the runtime already has (plan Q3). Its preparation pool takes the work.
				static const org::async::Scope scope = DCLF::SceneScheduler::Executor().CreateScope("CS frame revision");
				coordinator.pump = DCLF::SceneScheduler::MakeLanePump(DCLF::SceneScheduler::Executor(), scope, DCLF::SceneExecutor::Preparation,
					org::async::TaskDispatch::Controlled, "CS frame revision", [] { DrainRevisionLane(); },
					[] { logger::critical("[ORG] frame revision: the revision lane stopped (its task was refused)"); });
			}
			return *coordinator.pump;
		}

		/** @brief A declared texture's new extent. a_renew asks for a new version even at its current extent. Render thread. */
		void OnExtent(GraphResources::Resource& a_twin, const Extent& a_extent, bool a_renew)
		{
			auto& coordinator = GetCoordinator();
			++coordinator.extentSerial;
			// A new event: drops asked again before it are not starvation.
			coordinator.askedAgainSinceInput = 0;
			coordinator.sceneMissesSinceInput = 0;
			// A declared texture changes only its extent, so another shape class is a declaration error.
			if (!a_twin.Preparable(a_extent))
				FailRevision(fmt::format("{} has no version at {}: the extent changes its shape class (mips or slices)", a_twin.Name(), ExtentText(a_extent)));
			const auto frame = Driver::Current().frame;
			auto& input = coordinator.extents[&a_twin];
			const bool standing = input.twin == &a_twin && !input.alive.expired();
			// A renew no revision served yet stands through a later extent.
			const std::uint64_t renew = a_renew ? ++coordinator.renewSerial : (standing ? input.renew : 0);
			input = { a_twin.Alive(), &a_twin, a_extent, standing ? input.frame : frame, renew };
		}

		/** @brief A declared buffer's new element count, a version for the next revision. Latest wins. Render thread. */
		void OnBufferElements(Coordinator& a_coordinator, GraphResources::StructuredBuffer& a_buffer, std::uint32_t a_elements, std::uint64_t a_frame)
		{
			const auto& slot = a_buffer.BufferVersions();
			if (!slot)
				FailRevision(fmt::format("{} has a new element count but no versions", a_buffer.Name()));
			const std::lock_guard lock(a_coordinator.bufferMutex);
			if (a_coordinator.bufferRequests.empty())
				a_coordinator.bufferFirstInput = a_frame;
			a_coordinator.bufferRequests[slot->Key()] = { slot, a_elements, nullptr, nullptr, a_buffer.Alive(), &a_buffer };
		}

		/** @brief The variables a_newer changed since a_older. Without a_older, the variables written at least once. */
		std::vector<org::services::VariableIndex> ChangedVariables(const org::services::VariableSnapshot& a_newer, const org::services::VariableSnapshot* a_older)
		{
			if (a_older)
				return a_newer.ChangedSince(*a_older);
			// A variable's first serial is 1, so a higher one was written.
			std::vector<org::services::VariableIndex> changed;
			for (org::services::VariableIndex i = 0; i < a_newer.Size(); ++i)
				if (a_newer.Serial(i) > 1)
					changed.push_back(i);
			return changed;
		}

		/** @brief Starts the work outside the frame that the revision's variable changes need. Render thread. */
		void ProduceInputs(Coordinator& a_coordinator, PreparedRevision& a_job, const org::services::VariableSnapshot& a_values,
			const org::services::VariableSnapshot* a_started)
		{
			for (const auto index : ChangedVariables(a_values, a_started)) {
				InputProducer producer;
				{
					const std::lock_guard lock(a_coordinator.producerMutex);
					const auto it = a_coordinator.producers.find(a_values.Name(index));
					if (it == a_coordinator.producers.end())
						continue;
					producer = it->second;
				}
				auto wait = producer(a_values, a_job.serial);
				if (!wait.poll)
					FailRevision(fmt::format("revision {}'s producer of {} returned no wait", a_job.serial, a_values.Name(index)));
				a_job.inputWaits.push_back({ std::string(a_values.Name(index)), std::move(wait) });
			}
			if (!a_job.inputWaits.empty())
				a_job.inputWaitsFragment = std::make_shared<org::async::RevisionFragment>();
		}

		/**
		* @brief Hands the standing inputs to the lane as the next revision: declared extents and buffer contents. With none of those, a
		* staged stage build makes a revision of its own. Only one is prepared at a time.
		*/
		void StartPreparation(Coordinator& a_coordinator, std::uint64_t a_frame)
		{
			if (a_coordinator.preparing)
				return;
			// The programs the newest values need are built first, off the render thread (Frame/Declare.h, DeclareProgram).
			// Until then no revision starts, and frames keep the adopted one.
			if (a_coordinator.variables && !Declarations::ProgramsReady(*a_coordinator.variables))
				return;
			// Without a graph (plain D3D11, or before the graph is up) a revision carries only variables.
			auto* host = RenderGraphRuntime::Get().Host();
			auto job = std::make_shared<PreparedRevision>();
			job->firstInput = UINT64_MAX;
			// The declarations follow the values this revision starts with. Their new extents are collected below.
			if (host && a_coordinator.variables) {
				job->needs = std::exchange(a_coordinator.carriedNeeds, {});
				auto needs = Declarations::Evaluate(
					*a_coordinator.variables,
					[](GraphResources::Resource& a_twin, const std::array<unsigned int, 3>& a_extent, bool a_renew) {
						OnExtent(a_twin, Extent{ a_extent[0], a_extent[1], a_extent[2] }, a_renew);
					},
					[&a_coordinator, a_frame](GraphResources::StructuredBuffer& a_buffer, std::uint32_t a_elements) {
						OnBufferElements(a_coordinator, a_buffer, a_elements, a_frame);
					});
				for (auto& need : needs)
					job->needs.push_back(std::move(need));
			}
			if (host && !a_coordinator.extents.empty() && a_coordinator.plannedSerial != a_coordinator.extentSerial) {
				for (auto it = a_coordinator.extents.begin(); it != a_coordinator.extents.end();) {
					auto& [twin, input] = *it;
					if (input.alive.expired() || (!input.renew && twin->CurrentExtent() == input.extent)) {
						it = a_coordinator.extents.erase(it);
						continue;
					}
					auto plan = twin->PlanPrepare(input.extent);
					if (!plan)
						FailRevision(fmt::format("{} has no version at {}: it is not a live versioned texture of that shape class", twin->Name(), ExtentText(input.extent)));
					job->plans.push_back(std::move(plan));
					job->twins.push_back(twin);
					job->alive.push_back(input.alive);
					job->names.push_back(twin->Name());
					job->from.push_back(twin->CurrentExtent());
					job->to.push_back(input.extent);
					job->renews.push_back(input.renew);
					job->firstInput = (std::min)(job->firstInput, input.frame);
					++it;
				}
				a_coordinator.plannedSerial = a_coordinator.extentSerial;
			}
			if (host) {
				const std::lock_guard lock(a_coordinator.bufferMutex);
				for (auto& [key, input] : std::exchange(a_coordinator.bufferRequests, {}))
					job->bufferRequests.push_back(std::move(input));
				if (!job->bufferRequests.empty())
					job->firstInput = (std::min)(job->firstInput, a_coordinator.bufferFirstInput);
			}
			if (host) {
				const std::lock_guard lock(a_coordinator.textureMutex);
				for (auto& [key, input] : std::exchange(a_coordinator.textureRequests, {}))
					job->textureRequests.push_back(std::move(input));
				if (!job->textureRequests.empty())
					job->firstInput = (std::min)(job->firstInput, a_coordinator.textureFirstInput);
			}
			// The variables changed since the last revision started. Every revision carries the newest snapshot.
			const bool variablesChanged = a_coordinator.variables && a_coordinator.variables != a_coordinator.startedVariables;
			if (variablesChanged)
				job->firstInput = (std::min)(job->firstInput, a_coordinator.variablesFirstChange);
			const bool lane = !job->plans.empty() || !job->bufferRequests.empty() || !job->textureRequests.empty();
			if (!lane && !(host && FrameAnchors::StagedStages()) && !variablesChanged && job->needs.empty())
				return;
			if (job->firstInput == UINT64_MAX)
				job->firstInput = a_frame;
			if (variablesChanged)
				job->changedVariables = Vars::Lane::ChangedNames(*a_coordinator.variables, a_coordinator.startedVariables.get());
			job->serial = ++a_coordinator.serials;
			if (variablesChanged)
				ProduceInputs(a_coordinator, *job, *a_coordinator.variables, a_coordinator.startedVariables.get());
			job->variables = a_coordinator.variables;
			a_coordinator.startedVariables = a_coordinator.variables;
			job->planned = a_frame;
			job->host = host;
			a_coordinator.preparing = job;
			if (!lane) {
				// Builds or variables only: nothing for the lane to make.
				job->done.store(true, std::memory_order_release);
				return;
			}
			job->uploads = host->RetainUploads();
			{
				const std::lock_guard lock(a_coordinator.laneMutex);
				a_coordinator.laneJob = std::move(job);
			}
			if (!RevisionLane().Notify())
				FailRevision("the revision lane refused its work");
		}

		/** @brief Whether every twin of the revision is still the one it was planned for. Render thread. */
		bool StillPlanned(const PreparedRevision& a_revision, std::string& a_why)
		{
			for (std::size_t i = 0; i < a_revision.twins.size(); ++i) {
				auto* twin = a_revision.twins[i];
				if (a_revision.alive[i].expired()) {
					a_why = fmt::format("{} was destroyed", a_revision.names[i]);
					return false;
				}
				if (twin->Versioned() != a_revision.versions[i].slot || twin->CurrentExtent() != a_revision.from[i]) {
					a_why = fmt::format("{} was made again since", a_revision.names[i]);
					return false;
				}
			}
			return true;
		}

		/** @brief Drops a revision that can never be adopted. Its inputs stand and are prepared again. Supersession is never an error. */
		void Supersede(Coordinator& a_coordinator, std::string_view a_why)
		{
			auto revision = std::move(a_coordinator.preparing);
			logger::info("[ORG] frame revision {}: superseded before adoption ({}); its inputs are prepared again", revision->serial, a_why);
			// DCLF's scene recorded for it can run no other. DCLF records the next one against the adopted versions.
			if (a_coordinator.heldScene) {
				DropScene(a_coordinator, std::move(*a_coordinator.heldScene), SceneDrop::Superseded, "its frame revision was superseded");
				a_coordinator.heldScene.reset();
			}
			a_coordinator.sceneMissesSinceInput = 0;
			for (auto& need : revision->needs)
				a_coordinator.carriedNeeds.push_back(std::move(need));
			a_coordinator.plannedSerial = 0;
			// Its variable changes count again against the adopted values.
			a_coordinator.startedVariables = Vars::Lane::Adopted();
			// Its buffer contents go back, unless newer ones came meanwhile.
			if (revision->buffers) {
				const std::lock_guard lock(a_coordinator.bufferMutex);
				for (const auto& entry : revision->buffers->buffers)
					if (const auto buffer = entry.buffer.lock(); buffer && !a_coordinator.bufferRequests.contains(entry.key))
						a_coordinator.bufferRequests[entry.key] = { buffer, entry.elements, entry.bytes, entry.payload, entry.twinAlive, entry.twin };
			}
			// So do its published textures.
			{
				const std::lock_guard lock(a_coordinator.textureMutex);
				for (auto& input : revision->textureRequests)
					if (const auto texture = input.texture.lock(); texture && !a_coordinator.textureRequests.contains(texture->Key()))
						a_coordinator.textureRequests[texture->Key()] = std::move(input);
			}
			const std::lock_guard lock(a_coordinator.statsMutex);
			++a_coordinator.window.superseded;
			++a_coordinator.session.superseded;
		}

		void ReleaseRetired(Coordinator& a_coordinator)
		{
			const auto completed = DCLF::FrameRetirement()->Completed();
			while (!a_coordinator.retiring.empty() && a_coordinator.retiring.front().first <= completed)
				a_coordinator.retiring.pop_front();
		}

		/**
		* @brief Adopts a ready revision whole, with a_scene, DCLF's scene recorded against it, when it has one. Its staged builds attached
		* already. Render thread, inside the adoption point.
		*/
		void Adopt(Coordinator& a_coordinator, std::uint64_t a_frame, const Scene* a_scene)
		{
			auto revision = std::move(a_coordinator.preparing);
			a_coordinator.sceneMissesSinceInput = 0;
			auto& assembler = *a_coordinator.assembler;
			const auto stagedEpochs = revision->recordings ? FrameAnchors::RevisionStagedEpochs(*revision->recordings) : std::vector<std::uint32_t>{};
			// Sealed once complete, so it is selected in this Begin.
			auto draft = assembler.Begin();
			auto inputsValue = std::make_shared<RevisionInputs>();
			inputsValue->variables = revision->variables;
			// The variables are valid only with the work their changes needed.
			std::vector<org::async::RevisionFragment::Requirement> inputRequirements;
			if (revision->inputWaitsFragment)
				inputRequirements.push_back({ kRevisionInputWaits, revision->inputWaitsFragment });
			draft.Set(kRevisionInputWaits, revision->inputWaitsFragment);
			const auto inputs = org::async::RevisionFragment::MakeReady(std::move(inputsValue), std::move(inputRequirements));
			draft.Set(kRevisionInputs, inputs);
			draft.Set(kRevisionTextures, revision->made ? revision->made->fragment : nullptr);
			draft.Set(kRevisionBuffers, revision->buffersFragment);
			std::shared_ptr<const org::async::RevisionFragment> builds;
			if (!stagedEpochs.empty()) {
				auto value = std::make_shared<RevisionBuilds>();
				value->epochs = stagedEpochs;
				builds = org::async::RevisionFragment::MakeReady(std::move(value));
			}
			draft.Set(kRevisionBuilds, builds);
			std::vector<org::async::RevisionFragment::Requirement> requirements{ { kRevisionInputs, inputs } };
			if (revision->made)
				requirements.push_back({ kRevisionTextures, revision->made->fragment });
			if (revision->buffersFragment)
				requirements.push_back({ kRevisionBuffers, revision->buffersFragment });
			if (builds)
				requirements.push_back({ kRevisionBuilds, builds });
			auto memberValue = std::make_shared<RevisionMemberRecordings>();
			memberValue->recordings = revision->recordings;
			draft.Set(kRevisionRecordings, org::async::RevisionFragment::MakeReady(std::move(memberValue), std::move(requirements)));
			// Without a scene of its own the revision inherits the adopted one.
			if (a_scene)
				SetScene(draft, *a_scene);
			assembler.Seal(std::move(draft));
			const auto collected = assembler.Collect();
			if (!collected.failures.empty())
				FailRevision(fmt::format("revision {} failed to assemble", revision->serial));
			if (assembler.TrySelect() != org::async::RevisionAssembler::Selection::Selected)
				FailRevision(fmt::format("revision {} was complete but not selected", revision->serial));
			const auto lease = assembler.AcquireActive();
			const auto adoptedInputs = lease->Get<RevisionInputs>(kRevisionInputs);
			const auto inputWaits = lease->Get<RevisionInputWaits>(kRevisionInputWaits);
			const auto named = lease->Get<org::services::NamedVersions>(kRevisionTextures);
			const auto buffers = lease->Get<RevisionBuffers>(kRevisionBuffers);
			const auto attaching = lease->Get<RevisionBuilds>(kRevisionBuilds);
			const auto recorded = lease->Get<RevisionMemberRecordings>(kRevisionRecordings);
			if (static_cast<bool>(named) != static_cast<bool>(revision->made) || static_cast<bool>(buffers) != static_cast<bool>(revision->buffers) ||
				static_cast<bool>(attaching) != !stagedEpochs.empty() || !recorded || !adoptedInputs || !adoptedInputs->variables ||
				static_cast<bool>(inputWaits) != static_cast<bool>(revision->inputWaitsFragment))
				FailRevision(fmt::format("revision {} was selected without its inputs, versions, builds or recordings", revision->serial));
			// A new shader generation swaps the staged pipelines in once the rest of the revision is in (plan Q5).
			const std::uint64_t shaderGenerationBefore = Vars::AdoptedShaderGeneration();
			{
				const Boundary::Caller caller("revision");
				// The frame reads the variables of the revision it runs, and takes what their changes made outside the frame.
				Vars::Lane::Adopt(adoptedInputs->variables);
				if (inputWaits)
					for (const auto& adopt : inputWaits->adopt)
						adopt();
				if (named)
					named->AdoptAll();
				for (std::size_t i = 0; i < revision->twins.size(); ++i)
					revision->twins[i]->AdoptPrepared(revision->versions[i]);
				if (buffers) {
					{
						const std::lock_guard lock(a_coordinator.bufferMutex);
						for (const auto& entry : buffers->buffers)
							if (const auto buffer = entry.buffer.lock()) {
								buffer->Adopt(entry.version);
								a_coordinator.adoptedPayloads[entry.key] = entry.payload;
							}
					}
					// A declared buffer's twin makes its D3D11 export of the version and reads its count from it.
					for (const auto& entry : buffers->buffers)
						if (entry.twin && !entry.twinAlive.expired() && !entry.buffer.expired())
							entry.twin->AdoptVersion(entry.version, entry.elements);
				}
			}
			// Per-frame tickets (DCLF's, the anchors') prepared before resolved the old versions. Revision-driven stages need no call.
			if (named || buffers)
				revision->host->NoteNewVersions();
			// The staged builds attach as one. A refusal is fatal: no frame may run a half-adopted revision.
			if (attaching) {
				const Boundary::Caller caller("revision");
				if (const auto refused = RenderGraph::Builds::AttachStaged(attaching->epochs); !refused.empty()) {
					std::string epochs;
					for (const auto epoch : attaching->epochs)
						epochs += fmt::format("{}{}", epochs.empty() ? "" : ", ", EpochText(epoch));
					FailRevision(fmt::format("revision {}'s staged builds of [{}] were refused at its adoption: {}", revision->serial, epochs, refused));
				}
			}
			if (recorded->recordings)
				FrameAnchors::InstallRevisionSchedules(*recorded->recordings, a_frame);
			// DCLF's epochs take their recordings of the same versions in the same frame (plan step 10b).
			if (a_scene)
				InstallScene(*a_scene);
			if (revision->dclfMembers)
				a_coordinator.adoptedSceneVersions = revision->serial;
			// After the staged attach, which a later build would refuse. Every graph is built here with the host's thread held, so
			// every epoch takes the new pipelines in this Begin and no recording made before the swap runs after it.
			if (const std::uint64_t shaderGeneration = Vars::AdoptedShaderGeneration(); shaderGeneration != shaderGenerationBefore) {
				const Boundary::Caller caller("revision");
				RenderGraphRuntime::Get().SwapStagedPipelines(fmt::format("revision {}, shader generation {}", revision->serial, shaderGeneration));
			}
			// The last revision's versions and recordings go once the GPU is past every frame submitted with them.
			if (a_coordinator.adopted)
				a_coordinator.retiring.emplace_back(DCLF::SubmittedRetirementPoint(), std::move(a_coordinator.adopted));
			for (std::size_t i = 0; i < revision->twins.size(); ++i)
				if (const auto it = a_coordinator.extents.find(revision->twins[i]);
					it != a_coordinator.extents.end() && it->second.extent == revision->to[i] && it->second.renew == revision->renews[i])
					a_coordinator.extents.erase(it);
			const auto members = revision->recordings ? FrameAnchors::RevisionMembers(*revision->recordings) : std::vector<std::string>{};
			const std::uint64_t inputFrames = a_frame > revision->firstInput ? a_frame - revision->firstInput : 0;
			const std::uint32_t bufferCount = buffers ? static_cast<std::uint32_t>(buffers->buffers.size()) : 0;
			{
				const std::lock_guard lock(a_coordinator.statsMutex);
				for (auto* stats : { &a_coordinator.window, &a_coordinator.session }) {
					++stats->adopted;
					stats->builds += stagedEpochs.size();
					stats->buffers += bufferCount;
					stats->variableRevisions += revision->changedVariables.empty() ? 0 : 1;
					stats->maxMembers = (std::max)(stats->maxMembers, static_cast<std::uint32_t>(members.size()));
					stats->maxRecordings = (std::max)(stats->maxRecordings, revision->recordingCount);
					stats->maxBuilds = (std::max)(stats->maxBuilds, static_cast<std::uint32_t>(stagedEpochs.size()));
					stats->inputFrames.push_back(inputFrames);
					stats->scenesWithRevision += a_scene ? 1 : 0;
				}
			}
			if (++a_coordinator.adoptedLines <= 64 || a_coordinator.adoptedLines % 100 == 0) {
				std::string twins;
				for (std::size_t i = 0; i < revision->twins.size(); ++i)
					twins += fmt::format("{}{} {} -> {}", twins.empty() ? "" : ", ", revision->names[i], ExtentText(revision->from[i]), ExtentText(revision->to[i]));
				for (const auto& name : revision->publishedNames)
					twins += fmt::format("{}{} (published)", twins.empty() ? "" : ", ", name);
				std::string bufferNames;
				if (buffers)
					for (const auto& entry : buffers->buffers)
						bufferNames += fmt::format("{}{}", bufferNames.empty() ? "" : ", ", entry.name);
				std::string stages;
				for (const auto& member : members)
					stages += fmt::format("{}{}", stages.empty() ? "" : ", ", member);
				std::string others;
				for (const auto epoch : revision->others)
					others += fmt::format("{}{}", others.empty() ? "" : ", ", EpochText(epoch));
				std::string waits;
				for (const auto& input : revision->inputWaits)
					waits += fmt::format("{}{}", waits.empty() ? "" : ", ", input.wait.what);
				logger::info(
					"[ORG] frame revision {}: adopted at frame {} with every member stage, {} frames after its first input (prepared in {:.2f} ms "
					"on the lane); twins [{}]; buffers [{}]; variables [{}]; inputs [{}]; staged builds {}; member stages [{}] with {} recordings; not "
					"members [{}]; DCLF scene {}",
					revision->serial, a_frame, inputFrames, revision->milliseconds, twins, bufferNames, revision->changedVariables, waits, stagedEpochs.size(),
					stages.empty() ? "none" : stages, revision->recordingCount, others.empty() ? "none" : others,
					a_scene ? fmt::format("{} recorded with it", a_scene->serial) : std::string(revision->dclfMembers ? "none (DCLF's epochs did not run)" : "kept"));
			}
			a_coordinator.adopted = std::move(revision);
		}

		/** @brief The slots whose versions the revision changes, for the closure. */
		std::vector<const org::IVersionedResource*> ChangedSlots(const PreparedRevision& a_revision, std::vector<std::shared_ptr<org::VersionedBuffer>>& a_held)
		{
			std::vector<const org::IVersionedResource*> slots;
			for (const auto& version : a_revision.versions)
				slots.push_back(version.slot.get());
			for (const auto& texture : a_revision.published)
				slots.push_back(texture.get());
			if (a_revision.buffers)
				for (const auto& entry : a_revision.buffers->buffers)
					if (auto buffer = entry.buffer.lock()) {
						slots.push_back(buffer.get());
						a_held.push_back(std::move(buffer));
					}
			return slots;
		}

		/** @brief The state of a fragment the revision waits for. Fatal when it failed. */
		bool FragmentReady(const PreparedRevision& a_revision, const org::async::RevisionFragment* a_fragment, std::string_view a_what)
		{
			if (!a_fragment)
				return true;
			if (a_fragment->GetState() == org::async::RevisionFragment::State::Failed)
				FailRevision(fmt::format("revision {}'s {} failed: {}", a_revision.serial, a_what, ErrorText(a_fragment->Error())));
			return a_fragment->GetState() == org::async::RevisionFragment::State::Ready;
		}

		/** @brief Polls the revision's input waits and resolves their fragment once all are ready. Fatal when one failed. Render thread. */
		bool PollInputs(PreparedRevision& a_revision)
		{
			if (!a_revision.inputWaitsFragment)
				return true;
			bool ready = true;
			for (auto& input : a_revision.inputWaits) {
				if (input.ready)
					continue;
				std::string why;
				switch (input.wait.poll(why)) {
				case InputState::Ready:
					input.ready = true;
					break;
				case InputState::Failed:
					a_revision.inputWaitsFragment->Fail(std::make_exception_ptr(std::runtime_error(fmt::format("{} for {}: {}", input.wait.what, input.variable, why))));
					break;
				case InputState::Pending:
					ready = false;
					break;
				}
			}
			if (ready && a_revision.inputWaitsFragment->GetState() == org::async::RevisionFragment::State::Pending) {
				auto value = std::make_shared<RevisionInputWaits>();
				for (const auto& input : a_revision.inputWaits)
					if (input.wait.adopt)
						value->adopt.push_back(input.wait.adopt);
				a_revision.inputWaitsFragment->Resolve(std::move(value));
			}
			return FragmentReady(a_revision, a_revision.inputWaitsFragment.get(), "input waits");
		}

		/** @brief The revision in preparation, once the lane is done: recordings, then adoption. Render thread. */
		void Advance(Coordinator& a_coordinator, std::uint64_t a_frame)
		{
			auto& revision = *a_coordinator.preparing;
			if (!revision.error.empty())
				FailRevision(fmt::format("revision {} could not be prepared: {}", revision.serial, revision.error));
			std::string why;
			if (!StillPlanned(revision, why)) {
				Supersede(a_coordinator, why);
				return;
			}
			// Inputs outside the frame (a swap chain replacement) run meanwhile. Only adoption waits for them.
			const bool inputsReady = PollInputs(revision);
			if (!revision.host) {
				// A revision of variables alone has no recordings to make.
				if (inputsReady)
					Adopt(a_coordinator, a_frame, nullptr);
				return;
			}
			// The zero fills and uploads run on the uploader's queue. Recordings wait for nothing, but the revision is adopted only once they are done.
			const bool texturesReady = FragmentReady(revision, revision.made ? revision.made->fragment.get() : nullptr, "texture versions' fills");
			const bool buffersReady = FragmentReady(revision, revision.buffersFragment.get(), "buffer uploads");
			if (!texturesReady || !buffersReady)
				return;
			std::vector<std::shared_ptr<org::VersionedBuffer>> held;
			if (!revision.recordings) {
				std::shared_ptr<const org::IResourceVersions> textures = revision.made ? revision.made->named : nullptr;
				if (textures || revision.buffers)
					revision.versionSet = std::make_shared<const org::services::ComposedVersions>(
						std::initializer_list<std::shared_ptr<const org::IResourceVersions>>{ textures, revision.buffers });
				revision.recordings = FrameAnchors::MakeRevisionRecordings(revision.closure, revision.versionSet, revision.versionSet ? revision.versionSet->Serial() : 0,
					revision.others);
				// DCLF's epochs among the others record their scene against versionSet from the next build point (SceneFrameVersions).
				NoteDclfMembers(revision);
				if (!revision.needs.empty())
					FrameAnchors::RequireBuilds(*revision.recordings, revision.needs);
				const std::lock_guard lock(a_coordinator.statsMutex);
				++a_coordinator.window.prepared;
				++a_coordinator.session.prepared;
			} else if (const auto slots = ChangedSlots(revision, held); !slots.empty()) {
				// Builds may change which graphs declare the slots, so the closure is taken again until adoption.
				FrameAnchors::AddRevisionMembers(*revision.recordings, revision.host->EpochsDeclaring(slots), revision.others);
				NoteDclfMembers(revision);
			}
			const auto progress = FrameAnchors::AdvanceRevisionRecordings(*revision.recordings, a_frame);
			// A staged build found first is a new input: drops before it are not starvation.
			if (progress.newBuilds)
				a_coordinator.askedAgainSinceInput = 0;
			revision.askedAgain += progress.askedAgain;
			a_coordinator.askedAgainSinceInput += progress.askedAgain;
			if (progress.askedAgain) {
				const std::lock_guard lock(a_coordinator.statsMutex);
				a_coordinator.window.askedAgain += progress.askedAgain;
				a_coordinator.session.askedAgain += progress.askedAgain;
			}
			if (!progress.ready && a_coordinator.askedAgainSinceInput > kRevisionStarvedAfter) {
				Driver::NoteTapError(Driver::TapError::Starved, static_cast<SegmentId>(progress.missing),
					fmt::format("frame revision {} (inputs from frame {}, {} staged builds) has not been adopted: {} of its recordings were asked for again "
								"after they were dropped with no new input since (last: {}); still missing [{}]",
						revision.serial, revision.firstInput, progress.builds, a_coordinator.askedAgainSinceInput,
						progress.dropReasons.empty() ? "none" : progress.dropReasons, progress.waiting));
				a_coordinator.askedAgainSinceInput = 0;
			}
			if (!progress.ready || !inputsReady)
				return;
			// With DCLF epochs in its closure, the revision takes DCLF's scene recorded against its versions, while DCLF's epochs run
			// (plan 7.8: they ran in one of the last kSceneIdleFrames Begins). Until it comes, frames keep the adopted revision and
			// scene. Its staged builds attach first: a refusal drops that scene, and the revision waits for the next one. A DCLF that
			// does not draw posts no scene, so then the revision adopts alone and DCLF's next scene is recorded on it.
			std::optional<Scene> scene;
			if (revision.dclfMembers && (a_coordinator.heldScene || DclfDraws(a_coordinator, a_frame))) {
				if (!a_coordinator.heldScene) {
					// DCLF draws and keeps posting scenes that cannot serve this revision, with no new input: a schedule bug, not a wait.
					if (a_coordinator.sceneMissesSinceInput > kRevisionStarvedAfter) {
						Driver::NoteTapError(Driver::TapError::Starved, SegmentId::Count,
							fmt::format("frame revision {} (inputs from frame {}) has not been adopted: DCLF's epochs draw, and {} of its scenes since the last "
										"new input were recorded on other frame versions or dropped for their builds; none was recorded against revision {}",
								revision.serial, revision.firstInput, a_coordinator.sceneMissesSinceInput, revision.serial));
						a_coordinator.sceneMissesSinceInput = 0;
					}
					return;
				}
				scene = std::move(a_coordinator.heldScene);
				a_coordinator.heldScene.reset();
				if (!AttachSceneBuilds(a_coordinator, *scene)) {
					++a_coordinator.sceneMissesSinceInput;
					return;
				}
			}
			revision.recordingCount = progress.recordings;
			revision.buildCount = progress.builds;
			Adopt(a_coordinator, a_frame, scene ? &*scene : nullptr);
		}

		std::string Percentiles(std::vector<std::uint64_t> a_values)
		{
			if (a_values.empty())
				return "none";
			std::ranges::sort(a_values);
			const auto at = [&](double a_fraction) { return a_values[static_cast<std::size_t>(a_fraction * static_cast<double>(a_values.size() - 1) + 0.5)]; };
			return fmt::format("{}/{}/{}", at(0.5), at(0.95), a_values.back());
		}
	}

	void Install()
	{
		auto& coordinator = GetCoordinator();
		coordinator.installed = true;
		// Bound settings raise their variable from here on.
		CS::SettingRegistry::SetRaiseHook(&Vars::RaiseSetting);
#	if !defined(CS_RELEASE_PROFILE)
		Testing::RegisterExitReport([] { LogStats(); });
#	endif
		logger::info("[ORG] frame revision: declared extents, buffer contents, staged stage builds and variable changes are prepared off the render "
					 "thread and adopted whole at a frame's Begin");
	}

	bool RegisterInputProducer(std::string_view a_variable, InputProducer a_producer)
	{
		auto& coordinator = GetCoordinator();
		const std::lock_guard lock(coordinator.producerMutex);
		if (!coordinator.producers.emplace(std::string(a_variable), std::move(a_producer)).second)
			FailRevision(fmt::format("a second input producer for the variable {}", a_variable));
		logger::info("[ORG] frame revision: a revision that changes {} waits for its producer's input", a_variable);
		return true;
	}

	void AdoptLatest()
	{
		auto& coordinator = GetCoordinator();
		if (!coordinator.installed || !FrameAnchors::IsRenderOwner())
			return;
		ZoneScopedN("CS.Frame.Revision.Adopt");
		const std::uint64_t frame = Driver::Current().frame + 1;
		coordinator.beginFrame.store(frame, std::memory_order_relaxed);
		ReleaseRetired(coordinator);
		// The same snapshot, with no allocation, while no variable changed.
		auto previous = std::exchange(coordinator.variables, Vars::Lane::Take());
		if (coordinator.variables != previous) {
			// The first change no revision carries yet starts the wait.
			if (previous == coordinator.startedVariables)
				coordinator.variablesFirstChange = frame;
			// A new event: drops asked again before it are not starvation.
			coordinator.askedAgainSinceInput = 0;
			coordinator.sceneMissesSinceInput = 0;
		}
		// DCLF's newest scene (plan step 10b). One recorded against the revision in preparation is held for its adoption below. DCLF
		// draws on the main thread only, so a loading screen's thread that owns rendering leaves its scenes to the main thread's Begin.
		const bool mainThread = FrameAnchors::IsMainThread();
		std::optional<Scene> scene;
		if (mainThread)
			scene = TakePostedScene(coordinator);
		// Whether DCLF's epochs still run, so a revision with DCLF epochs knows whether to wait for its scene (plan 7.8).
		NoteSceneRuns(coordinator, frame);
		if (coordinator.preparing && coordinator.preparing->done.load(std::memory_order_acquire))
			Advance(coordinator, frame);
		// A revision of builds alone needs no lane, so it is advanced in the Begin that starts it.
		const bool idle = !coordinator.preparing;
		StartPreparation(coordinator, frame);
		if (idle && coordinator.preparing && coordinator.preparing->done.load(std::memory_order_acquire))
			Advance(coordinator, frame);
		if (mainThread) {
			// A scene of the adopted frame versions is adopted alone, after any revision this Begin adopted. Then the shader-reload
			// wait, after a pipeline swap that revision ran (plan step 10a).
			AdoptSceneAlone(coordinator, scene);
			DCLF::AfterSceneStep();
		}
		// After any adoption, so FirstFrame sees the versions this frame binds.
		Declarations::NoteBegin(frame);
	}

	void PublishBuffer(const std::shared_ptr<org::VersionedBuffer>& a_buffer, std::uint32_t a_elements, std::vector<std::byte> a_bytes,
		std::shared_ptr<const void> a_payload)
	{
		if (!a_buffer)
			return;
		auto& coordinator = GetCoordinator();
		const auto frame = coordinator.beginFrame.load(std::memory_order_relaxed);
		const std::lock_guard lock(coordinator.bufferMutex);
		if (coordinator.bufferRequests.empty())
			coordinator.bufferFirstInput = frame;
		coordinator.bufferRequests[a_buffer->Key()] = { a_buffer, a_elements, std::make_shared<std::vector<std::byte>>(std::move(a_bytes)), std::move(a_payload) };
	}

	void PublishTexture(const std::shared_ptr<org::VersionedTexture>& a_texture, std::shared_ptr<org::GloballyIndexedResource> a_image,
		std::shared_ptr<const void> a_owner)
	{
		if (!a_texture || !a_image)
			FailRevision("a texture was published without its slot or its image");
		auto& coordinator = GetCoordinator();
		const auto frame = coordinator.beginFrame.load(std::memory_order_relaxed);
		const std::lock_guard lock(coordinator.textureMutex);
		if (coordinator.textureRequests.empty())
			coordinator.textureFirstInput = frame;
		coordinator.textureRequests[a_texture->Key()] = { a_texture, std::move(a_image), std::move(a_owner) };
	}

	std::shared_ptr<const void> AdoptedPayload(const org::VersionedBuffer& a_buffer)
	{
		auto& coordinator = GetCoordinator();
		const std::lock_guard lock(coordinator.bufferMutex);
		const auto it = coordinator.adoptedPayloads.find(a_buffer.Key());
		return it != coordinator.adoptedPayloads.end() ? it->second : nullptr;
	}

	SceneVersions SceneFrameVersions()
	{
		const auto& coordinator = GetCoordinator();
		// The revision's pending versions, once its lane made them and its closure has DCLF epochs.
		if (const auto& preparing = coordinator.preparing; preparing && preparing->dclfMembers && preparing->versionSet)
			return { preparing->serial, preparing->versionSet };
		return { coordinator.adoptedSceneVersions, nullptr };
	}

	void PublishScene(Scene a_scene)
	{
		auto& coordinator = GetCoordinator();
		// A replaced scene is released here, on the poster's thread, outside the lock.
		std::optional<Scene> replaced;
		{
			const std::lock_guard lock(coordinator.sceneMutex);
			replaced = std::move(coordinator.postedScene);
			coordinator.postedScene = std::move(a_scene);
		}
	}

	bool AdoptScene()
	{
		auto& coordinator = GetCoordinator();
		if (!coordinator.installed || !FrameAnchors::IsRenderOwner() || !FrameAnchors::IsMainThread())
			return false;
		auto scene = TakePostedScene(coordinator);
		return AdoptSceneAlone(coordinator, scene);
	}

	std::uint64_t SupersedeForSceneWait(std::string_view a_why)
	{
		auto& coordinator = GetCoordinator();
		if (!coordinator.installed || !FrameAnchors::IsRenderOwner() || !FrameAnchors::IsMainThread())
			return 0;
		// Only a revision whose closure has DCLF epochs holds DCLF's scenes. Its lane is done, since Advance marked it.
		if (!coordinator.preparing || !coordinator.preparing->dclfMembers)
			return 0;
		const std::uint64_t serial = coordinator.preparing->serial;
		Supersede(coordinator, a_why);
		return serial;
	}

	void LogStats()
	{
		auto& coordinator = GetCoordinator();
		if (!coordinator.installed)
			return;
		const std::lock_guard lock(coordinator.statsMutex);
		const auto& w = coordinator.window;
		const auto& s = coordinator.session;
		logger::info("[ORG] frame revision: prepared {}, adopted {}, superseded {}, members per revision (max {}), recordings per revision (max {}), builds "
					 "per revision (max {}, {} in all), buffers {}, frames from first input to adoption (p50/p95/max) {}; recordings asked again {}; session: "
					 "prepared {}, adopted {}, superseded {}, members (max {}), recordings (max {}), builds (max {}, {} in all), buffers {}, input to "
					 "adoption {}, asked again {}; with variable changes {} (session {})",
			w.prepared, w.adopted, w.superseded, w.maxMembers, w.maxRecordings, w.maxBuilds, w.builds, w.buffers, Percentiles(w.inputFrames), w.askedAgain,
			s.prepared, s.adopted, s.superseded, s.maxMembers, s.maxRecordings, s.maxBuilds, s.builds, s.buffers, Percentiles(s.inputFrames),
			s.askedAgain, w.variableRevisions, s.variableRevisions);
		logger::info("[ORG] frame revision scenes: DCLF scenes adopted alone {}, with a revision {}; dropped as stale {}, on a replaced build {}, refused {}, "
					 "superseded {}; session: alone {}, with a revision {}, dropped {}/{}/{}/{}",
			w.scenesAlone, w.scenesWithRevision, w.scenesDropped[0], w.scenesDropped[1], w.scenesDropped[2], w.scenesDropped[3], s.scenesAlone,
			s.scenesWithRevision, s.scenesDropped[0], s.scenesDropped[1], s.scenesDropped[2], s.scenesDropped[3]);
		coordinator.window = {};
		Vars::LogStats();
	}
}

#else

namespace RenderGraph::Frame::Revision
{
	void Install() {}
	bool RegisterInputProducer(std::string_view, InputProducer) { return false; }
	void AdoptLatest() {}
	void PublishBuffer(const std::shared_ptr<org::VersionedBuffer>&, std::uint32_t, std::vector<std::byte>, std::shared_ptr<const void>) {}
	void PublishTexture(const std::shared_ptr<org::VersionedTexture>&, std::shared_ptr<org::GloballyIndexedResource>, std::shared_ptr<const void>) {}
	std::shared_ptr<const void> AdoptedPayload(const org::VersionedBuffer&) { return nullptr; }
	SceneVersions SceneFrameVersions() { return {}; }
	void PublishScene(Scene) {}
	bool AdoptScene() { return false; }
	std::uint64_t SupersedeForSceneWait(std::string_view) { return 0; }
	void LogStats() {}
}

#endif
