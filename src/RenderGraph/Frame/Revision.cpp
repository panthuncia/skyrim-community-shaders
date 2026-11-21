#include "RenderGraph/Frame/Revision.h"

#include "RenderGraph/Async/Lanes.h"
#include "RenderGraph/Async/LatestSlot.h"
#include "RenderGraph/Async/Retirement.h"
#include "RenderGraph/BuildManager.h"
#include "RenderGraph/D3D11Wrapper.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/BoundaryCheck.h"
#include "RenderGraph/Frame/DeclareLane.h"
#include "RenderGraph/Frame/EpochRecordings.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/RevisionRecordings.h"
#include "RenderGraph/Frame/Starvation.h"
#include "Utils/Logging.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/Frame/VariablesLane.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameClock.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/PipelineVersions.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "Utils/Setting.h"
#if !defined(CS_RELEASE_PROFILE)
#	include "Testing/Testing.h"
#endif

#include <ORGModuleServices/Async/RevisionAssembly.h>
#include <ORGModuleServices/Async/SerializedTaskPump.h>
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/ComposedVersions.h>
#include <ORGModuleServices/GrowableBufferSet.h>
#include <ORGModuleServices/VersionedBufferBuild.h>
#include <ORGModuleServices/Variables.h>
#include <ORGModuleServices/VersionedTextureGrowth.h>
#include <OpenRenderGraph/GraphHost.h>
#include <Render/Runtime/IUploadService.h>
#include <Resources/Buffers/VersionedBuffer.h>
#include <Resources/VersionedTexture.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <fmt/ranges.h>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace RenderGraph::Frame::Revision
{
	namespace
	{
		using GraphResources::Extent;

		/** @brief OnVersionsAdopted's listeners. Added before the first frame, read at each adoption. */
		struct VersionsAdoptedListeners
		{
			std::mutex mutex;
			std::vector<std::function<void()>> listeners;
		};

		VersionsAdoptedListeners& VersionsAdopted()
		{
			static VersionsAdoptedListeners s_listeners;
			return s_listeners;
		}

		// The revision's fragments (plan, step 4c). Inputs hold the variables' snapshot (step 5b). Input waits hold the work
		// outside the frame that changed variables need (RegisterInputProducer), such as a swap chain replacement.
		constexpr std::uint32_t kRevisionInputs = 0;
		constexpr std::uint32_t kRevisionTextures = 1;
		constexpr std::uint32_t kRevisionBuffers = 2;
		constexpr std::uint32_t kRevisionBuilds = 3;
		constexpr std::uint32_t kRevisionRecordings = 4;
		constexpr std::uint32_t kRevisionInputWaits = 5;
		// Each fragment source's fragments (cleanup plan A5, step 4): its fragment, then the buffer set its recordings name. They
		// inherit like the others, so a revision of CS stages keeps a source's adopted fragment and a fragment alone keeps the rest.
		constexpr std::uint32_t kRevisionFragmentSlots = 6;
		constexpr std::uint32_t kRevisionSlots = kRevisionFragmentSlots + 2 * kMaxFragmentSources;

		constexpr std::uint32_t FragmentSlot(FragmentSourceId a_source) { return kRevisionFragmentSlots + 2 * a_source; }
		constexpr std::uint32_t FragmentBuffersSlot(FragmentSourceId a_source) { return kRevisionFragmentSlots + 2 * a_source + 1; }

		// Segment values a source may own. RenderGraphRuntime::Segment has fewer.
		constexpr std::uint32_t kMaxSegments = 256;

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

		/** @brief The value of a source's fragment slot. It names the fragment, and the source owns it. */
		struct RevisionSourceFragment
		{
			std::uint64_t serial = 0;
			std::uint64_t frameVersions = 0;
		};

		/** @brief The value of a source's buffers slot: the serial of the source's buffer set its recordings name. */
		struct RevisionSourceBuffers
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
			// The newest pipeline generation an input wait needs every graph at (InputWait::pipelineGeneration), 0 for none.
			std::uint64_t pipelineGeneration = 0;
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
			// A bit per fragment source with epochs among others. Each records a fragment against versionSet, adopted with it.
			std::uint32_t sourceMembers = 0;
			std::uint64_t askedAgain = 0;
			std::uint32_t recordingCount = 0;
			std::uint32_t buildCount = 0;
		};

		struct RevisionStats
		{
			std::uint64_t prepared = 0, adopted = 0, superseded = 0, askedAgain = 0, builds = 0, buffers = 0, variableRevisions = 0;
			std::uint32_t maxMembers = 0, maxRecordings = 0, maxBuilds = 0;
			std::vector<std::uint64_t> inputFrames;
			// Fragments: adopted alone, adopted with a revision, and dropped by FragmentDrop.
			std::uint64_t fragmentsAlone = 0, fragmentsWithRevision = 0;
			std::array<std::uint64_t, 4> fragmentsDropped{};
		};

		/** @brief A registered fragment source and where its fragments stand. */
		struct SourceState
		{
			explicit SourceState(FragmentSource a_source) :
				source(std::move(a_source)), watch(source.name + "'s fragment for a revision") {}

			FragmentSource source;
			// Its newest fragment, from its builder (PublishFragment). A replaced one is released on the poster's thread.
			LatestSlot<Fragment> posted;
			// Render thread from here. A fragment recorded against the revision in preparation, adopted with it.
			std::optional<Fragment> held;
			// The last adopted revision that had its epochs in the closure: the frame versions a fragment adopted alone must name.
			std::uint64_t adoptedVersions = 0;
			// Whether its epochs ran in the last frame (FragmentSource::drawing), sampled at each Begin.
			bool drawing = false;
			// Fragments that could not serve a revision waiting for this source, since the last new input (cleanup plan B10).
			StarvationWatch watch;
		};

		std::array<std::uint8_t, kMaxSegments> MakeSegmentSources()
		{
			std::array<std::uint8_t, kMaxSegments> sources{};
			sources.fill(static_cast<std::uint8_t>(kMaxFragmentSources));
			return sources;
		}

		struct Coordinator
		{
			bool installed = false;
			// The lane's hand-off: the render thread posts one revision, the lane takes it.
			LatestSlot<std::shared_ptr<PreparedRevision>> laneJob;
			// Buffer contents, from any thread.
			std::mutex bufferMutex;
			std::map<const void*, BufferRequest> bufferRequests;
			std::uint64_t bufferFirstInput = 0;
			// Textures the callers made, from any thread.
			std::mutex textureMutex;
			std::map<const void*, TextureRequest> textureRequests;
			std::uint64_t textureFirstInput = 0;
			std::map<const void*, std::shared_ptr<const void>> adoptedPayloads;
			// The fragment sources, registered before the first frame and read without a lock after.
			std::array<std::unique_ptr<SourceState>, kMaxFragmentSources> sources;
			std::uint32_t sourceCount = 0;
			// The source of each segment, kMaxFragmentSources for none.
			std::array<std::uint8_t, kMaxSegments> segmentSource = MakeSegmentSources();
			// Render thread from here.
			std::map<GraphResources::Resource*, ExtentRequest> extents;
			std::uint64_t extentSerial = 0;
			std::uint64_t renewSerial = 0;
			std::uint64_t plannedSerial = 0;
			// The starvation rule (cleanup plan B10) for the revision's member recordings.
			StarvationWatch revisionWatch{ "the frame revision's member recordings" };
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
			// Mark's values, any thread.
			std::atomic<std::uint64_t> markRevision{ 0 }, markAdoptedVariables{ 0 }, markTakenVariables{ 0 };
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

		/** @brief The source that owns a_segment, or kMaxFragmentSources. */
		FragmentSourceId SourceOfSegment(const Coordinator& a_coordinator, std::uint32_t a_segment)
		{
			return a_segment < kMaxSegments ? a_coordinator.segmentSource[a_segment] : kMaxFragmentSources;
		}

		/** @brief The source whose segment runs as a_epoch, or kMaxFragmentSources. */
		FragmentSourceId SourceOfEpoch(const Coordinator& a_coordinator, std::uint32_t a_epoch)
		{
			using Segment = RenderGraphRuntime::Segment;
			for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(Segment::Count) && s < kMaxSegments; ++s)
				if (a_coordinator.segmentSource[s] != kMaxFragmentSources && RenderGraphRuntime::EpochOf(static_cast<Segment>(s)) == a_epoch)
					return a_coordinator.segmentSource[s];
			return kMaxFragmentSources;
		}

		/** @brief An epoch that is not a CS stage, named by its segment and its source. */
		std::string EpochText(std::uint32_t a_epoch)
		{
			using Segment = RenderGraphRuntime::Segment;
			const auto& coordinator = GetCoordinator();
			for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(Segment::Count); ++s)
				if (RenderGraphRuntime::EpochOf(static_cast<Segment>(s)) == a_epoch) {
					const auto source = SourceOfSegment(coordinator, s);
					return source != kMaxFragmentSources ? fmt::format("{} epoch {} (segment {})", coordinator.sources[source]->source.name, a_epoch, s) :
					                                       fmt::format("epoch {} (segment {})", a_epoch, s);
				}
			return fmt::format("epoch {}", a_epoch);
		}

		/** @brief Marks the sources with epochs in the revision's closure. Render thread. */
		void NoteSourceMembers(const Coordinator& a_coordinator, PreparedRevision& a_revision)
		{
			for (const auto epoch : a_revision.others)
				if (const auto source = SourceOfEpoch(a_coordinator, epoch); source != kMaxFragmentSources)
					a_revision.sourceMembers |= 1u << source;
		}

		/** @brief Whether a_revision's closure has a_source's epochs. */
		bool HasSourceMember(const PreparedRevision* a_revision, FragmentSourceId a_source)
		{
			return a_revision && ((a_revision->sourceMembers >> a_source) & 1u);
		}

		/** @brief Hands a fragment that is not adopted back to its source, and counts it. Render thread. */
		void DropFragment(Coordinator& a_coordinator, SourceState& a_state, Fragment a_fragment, FragmentDrop a_why, std::string_view a_text,
			std::span<const std::uint32_t> a_epochs = {})
		{
			{
				const std::lock_guard lock(a_coordinator.statsMutex);
				++a_coordinator.window.fragmentsDropped[static_cast<std::size_t>(a_why)];
				++a_coordinator.session.fragmentsDropped[static_cast<std::size_t>(a_why)];
			}
			a_state.source.drop(a_fragment.value, a_why, a_text, a_epochs);
		}

		/**
		* @brief Takes a source's newest posted fragment. One recorded against the revision in preparation is held for its adoption,
		* and the one held before is dropped. Any other is returned, to be adopted alone or dropped. Render thread.
		*/
		std::optional<Fragment> TakePosted(Coordinator& a_coordinator, FragmentSourceId a_source)
		{
			auto& state = *a_coordinator.sources[a_source];
			std::optional<Fragment> fragment;
			if (auto posted = state.posted.Take())
				fragment = std::move(*posted);
			if (!fragment)
				return std::nullopt;
			if (state.source.taken)
				state.source.taken(fragment->value);
			const auto* preparing = a_coordinator.preparing.get();
			const bool member = HasSourceMember(preparing, a_source);
			if (fragment->frameVersions == kAnyFrameVersions || !member || fragment->frameVersions != preparing->serial) {
				// A fragment with recordings on other versions while a revision waits for this source is a miss.
				if (fragment->frameVersions != kAnyFrameVersions && member)
					state.watch.NoteMiss(fmt::format("fragment {} names the versions of revision {}, and revision {} waits", fragment->serial,
						fragment->frameVersions, preparing->serial));
				return fragment;
			}
			if (state.held)
				DropFragment(a_coordinator, state, std::move(*state.held), FragmentDrop::Superseded, "a newer fragment was recorded for the same revision");
			state.held = std::move(fragment);
			return std::nullopt;
		}

		/**
		* @brief Whether every build a_fragment's recordings are of is attached or staged. a_staged gets the staged ones, which attach
		* with it. a_why says which is neither.
		*/
		bool FragmentBuildsReady(const Fragment& a_fragment, std::vector<std::uint32_t>& a_staged, std::string& a_why)
		{
			a_staged.clear();
			if (a_fragment.builds.empty())
				return true;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host) {
				a_why = "the graph host is gone";
				return false;
			}
			for (const auto& [epoch, generation] : a_fragment.builds) {
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
		* @brief Attaches the staged builds a_fragment was recorded on. False when it cannot be adopted: it is dropped then. Render
		* thread, inside the adoption point.
		*/
		bool AttachFragmentBuilds(Coordinator& a_coordinator, SourceState& a_state, Fragment& a_fragment)
		{
			std::vector<std::uint32_t> staged;
			std::string why;
			if (!FragmentBuildsReady(a_fragment, staged, why)) {
				DropFragment(a_coordinator, a_state, std::move(a_fragment), FragmentDrop::BuildReplaced, why);
				return false;
			}
			if (staged.empty())
				return true;
			const Boundary::Caller caller("fragment source");
			// A refusal goes to the source, which stops the game (cleanup plan A3, step 10). Nothing of the revision was adopted yet.
			auto* manager = RenderGraphRuntime::Get().Builds();
			if (const auto refused = manager ? manager->AttachStaged(staged) : std::string("the build manager is gone"); !refused.empty()) {
				DropFragment(a_coordinator, a_state, std::move(a_fragment), FragmentDrop::Refused, refused, staged);
				return false;
			}
			return true;
		}

		/** @brief Puts a source's fragment in a_draft. */
		void SetFragment(org::async::RevisionAssembler::Draft& a_draft, FragmentSourceId a_source, const Fragment& a_fragment)
		{
			auto buffersValue = std::make_shared<RevisionSourceBuffers>();
			buffersValue->serial = a_fragment.buffersSerial;
			const auto buffers = org::async::RevisionFragment::MakeReady(std::move(buffersValue));
			a_draft.Set(FragmentBuffersSlot(a_source), buffers);
			auto value = std::make_shared<RevisionSourceFragment>();
			value->serial = a_fragment.serial;
			value->frameVersions = a_fragment.frameVersions;
			a_draft.Set(FragmentSlot(a_source), org::async::RevisionFragment::MakeReady(std::move(value), { { FragmentBuffersSlot(a_source), buffers } }));
		}

		/** @brief Installs a fragment the revision assembler selected. Render thread, inside the adoption point. */
		void InstallFragment(SourceState& a_state, const Fragment& a_fragment)
		{
			const Boundary::Caller caller("fragment source");
			a_state.source.adopt(a_fragment.value);
		}

		/**
		* @brief Adopts a fragment that names no revision in preparation: alone, as a revision of its source's slots only, when it
		* names the adopted frame versions. Otherwise it is dropped. Render thread, inside the adoption point.
		*/
		bool AdoptAlone(Coordinator& a_coordinator, FragmentSourceId a_source, std::optional<Fragment>& a_fragment)
		{
			if (!a_fragment)
				return false;
			auto& state = *a_coordinator.sources[a_source];
			auto fragment = std::move(*a_fragment);
			a_fragment.reset();
			if (fragment.frameVersions != kAnyFrameVersions && fragment.frameVersions != state.adoptedVersions) {
				const auto text = fmt::format("its recordings name the frame versions of revision {}, and the frame runs those of revision {}", fragment.frameVersions,
					state.adoptedVersions);
				DropFragment(a_coordinator, state, std::move(fragment), FragmentDrop::Stale, text);
				return false;
			}
			if (!AttachFragmentBuilds(a_coordinator, state, fragment))
				return false;
			auto& assembler = *a_coordinator.assembler;
			auto draft = assembler.Begin();
			SetFragment(draft, a_source, fragment);
			assembler.Seal(std::move(draft));
			if (!assembler.Collect().failures.empty() || assembler.TrySelect() != org::async::RevisionAssembler::Selection::Selected)
				RenderGraph::Fatal("frame revision", fmt::format("{} fragment {} was complete but not selected", state.source.name, fragment.serial));
			InstallFragment(state, fragment);
			const std::lock_guard lock(a_coordinator.statsMutex);
			++a_coordinator.window.fragmentsAlone;
			++a_coordinator.session.fragmentsAlone;
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
			const auto posted = coordinator.laneJob.Take();
			if (!posted)
				return;
			std::shared_ptr<PreparedRevision> job = std::move(*posted);
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
					// The version's birth (Frame/Birth.h): zeros or contents on the uploader, ready before the revision is.
					switch (prepared.contents) {
					case GraphResources::Contents::Zero:
						request.contents = org::services::TextureContents::Zero;
						break;
					case GraphResources::Contents::Fill:
						request.contents = org::services::TextureContents::Fill;
						request.fill = prepared.fill;
						break;
					default:
						request.contents = org::services::TextureContents::Undefined;
						break;
					}
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

		/** @brief The revision lane, on the shared executor's preparation pool. */
		org::async::SerializedTaskPump& RevisionLane()
		{
			static auto& lane = MakeLane({ "CS frame revision", Executor::Preparation, org::async::TaskDispatch::Controlled }, [] { DrainRevisionLane(); });
			return lane;
		}

		/** @brief A new input of the revision: misses before it are not starvation. Render thread. */
		void NoteNewInput(Coordinator& a_coordinator)
		{
			a_coordinator.revisionWatch.NoteInput();
			for (std::uint32_t s = 0; s < a_coordinator.sourceCount; ++s)
				a_coordinator.sources[s]->watch.NoteInput();
		}

		/** @brief Drops every fragment held for the revision in preparation. Render thread. */
		void DropHeldFragments(Coordinator& a_coordinator, std::string_view a_why)
		{
			for (std::uint32_t s = 0; s < a_coordinator.sourceCount; ++s)
				if (auto& state = *a_coordinator.sources[s]; state.held) {
					DropFragment(a_coordinator, state, std::move(*state.held), FragmentDrop::Superseded, a_why);
					state.held.reset();
				}
		}

		/** @brief A declared texture's new extent. a_renew asks for a new version even at its current extent. Render thread. */
		void OnExtent(GraphResources::Resource& a_twin, const Extent& a_extent, bool a_renew)
		{
			auto& coordinator = GetCoordinator();
			++coordinator.extentSerial;
			// A new event: drops asked again before it are not starvation.
			NoteNewInput(coordinator);
			// A declared texture changes only its extent, so another shape class is a declaration error.
			if (const auto why = a_twin.WhyNotPreparable(a_extent))
				RenderGraph::Fatal("frame revision", fmt::format("{} has no version at {}: {}", a_twin.Name(), ExtentText(a_extent), *why));
			const auto frame = FrameClock::BeginFrame();
			auto& input = coordinator.extents[&a_twin];
			const bool standing = input.twin == &a_twin && !input.alive.expired();
			// A renew no revision served yet stands through a later extent.
			const std::uint64_t renew = a_renew ? ++coordinator.renewSerial : (standing ? input.renew : 0);
			input = { a_twin.Alive(), &a_twin, a_extent, standing ? input.frame : frame, renew };
		}

		/**
		* @brief A declared buffer's new element count, a version for the next revision. Latest wins. A buffer born Zero (a_zero) is
		* uploaded as zeros from the lane, so its version is ready only once they are written. Render thread.
		*/
		void OnBufferElements(Coordinator& a_coordinator, GraphResources::StructuredBuffer& a_buffer, std::uint32_t a_elements, bool a_zero,
			std::uint64_t a_frame)
		{
			const auto& slot = a_buffer.BufferVersions();
			if (!slot)
				RenderGraph::Fatal("frame revision", fmt::format("{} has a new element count but no versions", a_buffer.Name()));
			auto zeros = a_zero ? std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(a_elements) * a_buffer.Stride()) : nullptr;
			const std::lock_guard lock(a_coordinator.bufferMutex);
			if (a_coordinator.bufferRequests.empty())
				a_coordinator.bufferFirstInput = a_frame;
			a_coordinator.bufferRequests[slot->Key()] = { slot, a_elements, std::move(zeros), nullptr, a_buffer.Alive(), &a_buffer };
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
				// No poll: this change needs no work outside the frame.
				if (!wait.poll) {
					if (wait.adopt)
						RenderGraph::Fatal("frame revision", fmt::format("revision {}'s producer of {} returned an adoption step with no wait", a_job.serial, a_values.Name(index)));
					continue;
				}
				a_job.pipelineGeneration = (std::max)(a_job.pipelineGeneration, wait.pipelineGeneration);
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
					[&a_coordinator, a_frame](GraphResources::StructuredBuffer& a_buffer, std::uint32_t a_elements, bool a_zero) {
						OnBufferElements(a_coordinator, a_buffer, a_elements, a_zero, a_frame);
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
						RenderGraph::Fatal("frame revision", fmt::format("{} has no version at {}: {}", twin->Name(), ExtentText(input.extent),
							twin->WhyNotPreparable(input.extent).value_or("the plan could not be made")));
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
			a_coordinator.laneJob.Post(std::make_unique<std::shared_ptr<PreparedRevision>>(std::move(job)));
			if (!RevisionLane().Notify())
				RenderGraph::Fatal("frame revision", "the revision lane refused its work");
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
			// A fragment recorded for it can run no other. Its source records the next one against the adopted versions.
			DropHeldFragments(a_coordinator, "its frame revision was superseded");
			NoteNewInput(a_coordinator);
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
			const auto completed = Retirement::Clock()->Completed();
			while (!a_coordinator.retiring.empty() && a_coordinator.retiring.front().first <= completed)
				a_coordinator.retiring.pop_front();
		}

		/**
		* @brief Adopts a ready revision whole, with the fragments recorded against it (a_fragments, by source). Their staged builds
		* attached already. Render thread, inside the adoption point.
		*/
		void Adopt(Coordinator& a_coordinator, std::uint64_t a_frame, std::span<const std::optional<Fragment>> a_fragments)
		{
			auto revision = std::move(a_coordinator.preparing);
			NoteNewInput(a_coordinator);
			std::uint32_t fragmentCount = 0;
			for (const auto& fragment : a_fragments)
				fragmentCount += fragment ? 1 : 0;
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
			// Without a fragment of its own the revision inherits a source's adopted one.
			for (std::uint32_t s = 0; s < a_fragments.size(); ++s)
				if (a_fragments[s])
					SetFragment(draft, s, *a_fragments[s]);
			assembler.Seal(std::move(draft));
			const auto collected = assembler.Collect();
			if (!collected.failures.empty())
				RenderGraph::Fatal("frame revision", fmt::format("revision {} failed to assemble", revision->serial));
			if (assembler.TrySelect() != org::async::RevisionAssembler::Selection::Selected)
				RenderGraph::Fatal("frame revision", fmt::format("revision {} was complete but not selected", revision->serial));
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
				RenderGraph::Fatal("frame revision", fmt::format("revision {} was selected without its inputs, versions, builds or recordings", revision->serial));
			{
				const Boundary::Caller caller("revision");
				// The frame reads the variables of the revision it runs, and takes what their changes made outside the frame.
				Vars::Lane::Adopt(adoptedInputs->variables);
				a_coordinator.markAdoptedVariables.store(adoptedInputs->variables->Serial(), std::memory_order_release);
				a_coordinator.markRevision.store(revision->serial, std::memory_order_release);
				if (inputWaits)
					for (const auto& adopt : inputWaits->adopt)
						adopt();
				if (named)
					named->AdoptAll();
				for (std::size_t i = 0; i < revision->twins.size(); ++i)
					revision->twins[i]->AdoptPrepared(revision->versions[i]);
				// The features' D3D11 wrappers follow the versions the twins just took.
				GraphResources::RetargetWrappers();
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
				// Holders of D3D11 handles over declared textures point them at the versions adopted now.
				std::vector<std::function<void()>> listeners;
				{
					auto& adopted = VersionsAdopted();
					const std::lock_guard lock(adopted.mutex);
					listeners = adopted.listeners;
				}
				for (const auto& listener : listeners)
					listener();
			}
			// Per-frame tickets (the sources', the anchors') prepared before resolved the old versions. Revision-driven stages need no call.
			if (named || buffers)
				revision->host->NoteNewVersions();
			// The staged builds attach as one. A refusal is fatal: no frame may run a half-adopted revision.
			if (attaching) {
				const Boundary::Caller caller("revision");
				auto* manager = RenderGraphRuntime::Get().Builds();
				if (const auto refused = manager ? manager->AttachStaged(attaching->epochs) : std::string("the build manager is gone"); !refused.empty()) {
					std::string epochs;
					for (const auto epoch : attaching->epochs)
						epochs += fmt::format("{}{}", epochs.empty() ? "" : ", ", EpochText(epoch));
					RenderGraph::Fatal("frame revision", fmt::format("revision {}'s staged builds of [{}] were refused at its adoption: {}", revision->serial, epochs, refused));
				}
			}
			if (recorded->recordings)
				FrameAnchors::InstallRevisionSchedules(*recorded->recordings, a_frame);
			// The sources' epochs take their recordings of the same versions in the same frame.
			for (std::uint32_t s = 0; s < a_fragments.size(); ++s)
				if (a_fragments[s])
					InstallFragment(*a_coordinator.sources[s], *a_fragments[s]);
			for (std::uint32_t s = 0; s < a_coordinator.sourceCount; ++s)
				if (HasSourceMember(revision.get(), s))
					a_coordinator.sources[s]->adoptedVersions = revision->serial;
			// A member source without a fragment was not drawing. It stops drawing now, at the adoption point, not in a frame.
			for (std::uint32_t s = 0; s < a_coordinator.sourceCount; ++s)
				if (HasSourceMember(revision.get(), s) && !(s < a_fragments.size() && a_fragments[s]) && a_coordinator.sources[s]->source.left) {
					const Boundary::Caller caller("fragment source");
					a_coordinator.sources[s]->source.left(revision->serial);
				}
			// The last revision's versions and recordings go once the GPU is past every frame submitted with them.
			if (a_coordinator.adopted)
				a_coordinator.retiring.emplace_back(Retirement::SubmittedPoint(), std::move(a_coordinator.adopted));
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
					stats->fragmentsWithRevision += fragmentCount;
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
					"members [{}]; fragments {}",
					revision->serial, a_frame, inputFrames, revision->milliseconds, twins, bufferNames, revision->changedVariables, waits, stagedEpochs.size(),
					stages.empty() ? "none" : stages, revision->recordingCount, others.empty() ? "none" : others,
					fragmentCount ? fmt::format("{} recorded with it", fragmentCount) :
									std::string(revision->sourceMembers ? "none (the sources' epochs did not run)" : "kept"));
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

		/** @brief Whether every build a_fragment's recordings are of pins pipeline generation a_generation or a newer one. */
		bool FragmentPinned(const Fragment& a_fragment, std::uint64_t a_generation)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return true;
			for (const auto& [epoch, build] : a_fragment.builds) {
				// An epoch with no graph binds no pipeline, as EveryEpochAt skips it.
				if (!host->Graph(epoch))
					continue;
				if (PipelineVersions::BuildPin(*host, epoch, build) < a_generation)
					return false;
			}
			return true;
		}

		/** @brief The state of a fragment the revision waits for. Fatal when it failed. */
		bool FragmentReady(const PreparedRevision& a_revision, const org::async::RevisionFragment* a_fragment, std::string_view a_what)
		{
			if (!a_fragment)
				return true;
			if (a_fragment->GetState() == org::async::RevisionFragment::State::Failed)
				RenderGraph::Fatal("frame revision", fmt::format("revision {}'s {} failed: {}", a_revision.serial, a_what, ErrorText(a_fragment->Error())));
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
				RenderGraph::Fatal("frame revision", fmt::format("revision {} could not be prepared: {}", revision.serial, revision.error));
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
					Adopt(a_coordinator, a_frame, {});
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
				// Fragment sources' epochs among the others record against versionSet from their next build (FrameVersionsFor).
				NoteSourceMembers(a_coordinator, revision);
				// A change that reaches every graph (a shader reload) makes every source a member: each records its next fragment for it.
				if (revision.pipelineGeneration)
					for (std::uint32_t s = 0; s < a_coordinator.sourceCount; ++s)
						revision.sourceMembers |= 1u << s;
				if (!revision.needs.empty())
					FrameAnchors::RequireBuilds(*revision.recordings, revision.needs);
				const std::lock_guard lock(a_coordinator.statsMutex);
				++a_coordinator.window.prepared;
				++a_coordinator.session.prepared;
			} else if (const auto slots = ChangedSlots(revision, held); !slots.empty()) {
				// Builds may change which graphs declare the slots, so the closure is taken again until adoption.
				FrameAnchors::AddRevisionMembers(*revision.recordings, revision.host->EpochsDeclaring(slots), revision.others);
				NoteSourceMembers(a_coordinator, revision);
			}
			const auto progress = FrameAnchors::AdvanceRevisionRecordings(*revision.recordings, a_frame);
			// A staged build found first is a new input: drops before it are not starvation.
			if (progress.newBuilds)
				a_coordinator.revisionWatch.NoteInput();
			revision.askedAgain += progress.askedAgain;
			if (progress.askedAgain) {
				const std::lock_guard lock(a_coordinator.statsMutex);
				a_coordinator.window.askedAgain += progress.askedAgain;
				a_coordinator.session.askedAgain += progress.askedAgain;
			}
			// Recordings dropped and asked again with no new input since are the revision's misses (cleanup plan B10).
			if (!progress.ready)
				for (std::uint32_t i = 0; i < progress.askedAgain; ++i)
					if (a_coordinator.revisionWatch.NoteMiss(fmt::format("revision {} (inputs from frame {}, {} staged builds) asked again for its recordings ({}); "
																		 "still missing [{}]",
																 revision.serial, revision.firstInput, progress.builds,
																 progress.dropReasons.empty() ? "none" : progress.dropReasons, progress.waiting),
							static_cast<SegmentId>(progress.missing)))
						break;
			if (!progress.ready || !inputsReady)
				return;
			// With a source's epochs in its closure, the revision takes the source's fragment recorded against its versions while the
			// source draws (its epochs ran last frame). Until it comes, frames keep the adopted revision and fragments. Its staged builds
			// attach first: a failure drops that fragment, and the revision waits for the next one. A source that does not draw posts
			// no fragment, so then the revision adopts without one and the source's next fragment is recorded on it.
			for (std::uint32_t s = 0; s < a_coordinator.sourceCount; ++s) {
				const auto& state = *a_coordinator.sources[s];
				// A source that draws and keeps posting fragments that cannot serve this revision is a schedule bug: its watch counts it.
				if (HasSourceMember(&revision, s) && state.drawing && !state.held) {
					static Util::ReasonLog s_noFragment("[ORG] frame revision: a revision waits for a drawing source's fragment");
					s_noFragment.Report(fmt::format("source {}{}", s, revision.pipelineGeneration ? " (shader reload)" : ""));
					return;
				}
				// After a shader reload the fragment must be on the rebuilt graphs. The source records one on them once they are
				// staged, and it replaces this one. Not a miss: the builds are a new input still to come.
				if (revision.pipelineGeneration && HasSourceMember(&revision, s) && state.held && !FragmentPinned(*state.held, revision.pipelineGeneration)) {
					static Util::ReasonLog s_unpinned("[ORG] shader reload: the revision waits for a fragment on the rebuilt graphs");
					s_unpinned.Report(fmt::format("source {}", s));
					return;
				}
			}
			std::array<std::optional<Fragment>, kMaxFragmentSources> fragments;
			for (std::uint32_t s = 0; s < a_coordinator.sourceCount; ++s) {
				auto& state = *a_coordinator.sources[s];
				if (!HasSourceMember(&revision, s) || !state.held)
					continue;
				fragments[s] = std::move(state.held);
				state.held.reset();
				if (!AttachFragmentBuilds(a_coordinator, state, *fragments[s])) {
					state.watch.NoteMiss(fmt::format("a fragment for revision {} was dropped for its builds", revision.serial));
					// The others wait with the revision again.
					for (std::uint32_t o = 0; o < a_coordinator.sourceCount; ++o)
						if (o != s && fragments[o])
							a_coordinator.sources[o]->held = std::move(fragments[o]);
					return;
				}
			}
			revision.recordingCount = progress.recordings;
			revision.buildCount = progress.builds;
			Adopt(a_coordinator, a_frame, std::span<const std::optional<Fragment>>(fragments.data(), a_coordinator.sourceCount));
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
#if !defined(CS_RELEASE_PROFILE)
		Testing::RegisterExitReport([] { LogStats(); });
#endif
		logger::info("[ORG] frame revision: declared extents, buffer contents, staged stage builds and variable changes are prepared off the render "
					 "thread and adopted whole at a frame's Begin");
	}

	bool RegisterInputProducer(std::string_view a_variable, InputProducer a_producer)
	{
		auto& coordinator = GetCoordinator();
		const std::lock_guard lock(coordinator.producerMutex);
		if (!coordinator.producers.emplace(std::string(a_variable), std::move(a_producer)).second)
			RenderGraph::Fatal("frame revision", fmt::format("a second input producer for the variable {}", a_variable));
		logger::info("[ORG] frame revision: a revision that changes {} waits for its producer's input", a_variable);
		return true;
	}

	namespace
	{
		std::vector<std::function<void()>>& SetupObservers()
		{
			static std::vector<std::function<void()>> s_observers;
			return s_observers;
		}
	}

	void OnSetupBuildPoint(std::function<void()> a_observe)
	{
		if (a_observe)
			SetupObservers().push_back(std::move(a_observe));
	}

	void EvaluateAtSetup(std::string_view a_point)
	{
		auto& coordinator = GetCoordinator();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!coordinator.installed || !host)
			return;
		if (!FrameAnchors::IsRenderOwner())
			RenderGraph::Fatal("frame revision", fmt::format("{} evaluated the declarations on a thread that does not own rendering", a_point));
		// Copied, since an observer may add another.
		const auto observers = SetupObservers();
		for (const auto& observe : observers)
			observe();
		const auto values = Vars::Lane::Take();
		if (!values)
			return;
		const auto frame = FrameClock::BeginFrame();
		Declarations::JoinProgramsAtSetup(*values);
		const auto needs = Declarations::Evaluate(
			*values,
			[](GraphResources::Resource& a_twin, const std::array<unsigned int, 3>& a_extent, bool a_renew) {
				OnExtent(a_twin, Extent{ a_extent[0], a_extent[1], a_extent[2] }, a_renew);
			},
			[&coordinator, frame](GraphResources::StructuredBuffer& a_buffer, std::uint32_t a_elements, bool a_zero) {
				OnBufferElements(coordinator, a_buffer, a_elements, a_zero, frame);
			});
		// The setup build that follows (FrameAnchors::BuildPending) builds every stage these changed, inline, so no revision waits for them.
		const std::size_t changes = needs.size();
		const auto adopted = Declarations::AdoptAtSetup();
		logger::info("[ORG] frame revision: the declarations are evaluated at {}: {} client changes go into the setup build, {} clients adopted",
			a_point, changes, adopted);
	}

	void OnVersionsAdopted(std::function<void()> a_listener)
	{
		auto& adopted = VersionsAdopted();
		const std::lock_guard lock(adopted.mutex);
		adopted.listeners.push_back(std::move(a_listener));
	}

	void AdoptLatest()
	{
		auto& coordinator = GetCoordinator();
		// The thread that owns rendering adopts (decision 70).
		if (!coordinator.installed || !FrameAnchors::IsRenderOwner())
			return;
		ZoneScopedN("CS.Frame.Revision.Adopt");
		// The frame clock moved before this Begin, so this is the frame it starts.
		const std::uint64_t frame = FrameClock::BeginFrame();
		ReleaseRetired(coordinator);
		// The same snapshot, with no allocation, while no variable changed.
		auto previous = std::exchange(coordinator.variables, Vars::Lane::Take());
		if (coordinator.variables)
			coordinator.markTakenVariables.store(coordinator.variables->Serial(), std::memory_order_release);
		if (coordinator.variables != previous) {
			// The first change no revision carries yet starts the wait.
			if (previous == coordinator.startedVariables)
				coordinator.variablesFirstChange = frame;
			// A new event: drops asked again before it are not starvation.
			NoteNewInput(coordinator);
		}
		// Each source's newest fragment. One recorded against the revision in preparation is held for its adoption below. A source
		// that adopts on the main thread only leaves its fragments to the main thread's Begin while a loading screen's thread renders.
		const bool mainThread = FrameAnchors::IsMainThread();
		std::array<std::optional<Fragment>, kMaxFragmentSources> taken;
		for (std::uint32_t s = 0; s < coordinator.sourceCount; ++s) {
			auto& state = *coordinator.sources[s];
			if (state.source.mainThreadOnly && !mainThread)
				continue;
			taken[s] = TakePosted(coordinator, s);
			// Whether its epochs ran in the last frame, so a revision with its epochs knows whether to wait for its fragment.
			state.drawing = !state.source.drawing || state.source.drawing();
		}
		if (coordinator.preparing && coordinator.preparing->done.load(std::memory_order_acquire))
			Advance(coordinator, frame);
		// A revision of builds alone needs no lane, so it is advanced in the Begin that starts it.
		const bool idle = !coordinator.preparing;
		StartPreparation(coordinator, frame);
		if (idle && coordinator.preparing && coordinator.preparing->done.load(std::memory_order_acquire))
			Advance(coordinator, frame);
		// A fragment of the adopted frame versions is adopted alone, after any revision this Begin adopted. Then each source's step
		// after the adoption point.
		for (std::uint32_t s = 0; s < coordinator.sourceCount; ++s) {
			auto& state = *coordinator.sources[s];
			if (state.source.mainThreadOnly && !mainThread)
				continue;
			AdoptAlone(coordinator, s, taken[s]);
			if (state.source.afterAdoption)
				state.source.afterAdoption();
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
		const auto frame = FrameClock::BeginFrame();
		const std::lock_guard lock(coordinator.bufferMutex);
		if (coordinator.bufferRequests.empty())
			coordinator.bufferFirstInput = frame;
		coordinator.bufferRequests[a_buffer->Key()] = { a_buffer, a_elements, std::make_shared<std::vector<std::byte>>(std::move(a_bytes)), std::move(a_payload) };
	}

	void PublishTexture(const std::shared_ptr<org::VersionedTexture>& a_texture, std::shared_ptr<org::GloballyIndexedResource> a_image,
		std::shared_ptr<const void> a_owner)
	{
		if (!a_texture || !a_image)
			RenderGraph::Fatal("frame revision", "a texture was published without its slot or its image");
		auto& coordinator = GetCoordinator();
		const auto frame = FrameClock::BeginFrame();
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

	FragmentSourceId RegisterFragmentSource(FragmentSource a_source)
	{
		auto& coordinator = GetCoordinator();
		if (coordinator.sourceCount >= kMaxFragmentSources)
			RenderGraph::Fatal("frame revision", fmt::format("more than {} fragment sources ({})", kMaxFragmentSources, a_source.name));
		if (!a_source.adopt || !a_source.drop)
			RenderGraph::Fatal("frame revision", fmt::format("the fragment source {} has no adopt or drop", a_source.name));
		const FragmentSourceId id = coordinator.sourceCount;
		for (const auto segment : a_source.segments) {
			if (segment >= kMaxSegments || coordinator.segmentSource[segment] != kMaxFragmentSources)
				RenderGraph::Fatal("frame revision", fmt::format("the fragment source {} names segment {}, which is out of range or another source's", a_source.name, segment));
			coordinator.segmentSource[segment] = static_cast<std::uint8_t>(id);
		}
		logger::info("[ORG] frame revision: fragment source {} registered for segments [{}]; a revision with its epochs adopts its fragment recorded "
					 "against it",
			a_source.name, fmt::join(a_source.segments, ","));
		coordinator.sources[id] = std::make_unique<SourceState>(std::move(a_source));
		++coordinator.sourceCount;
		return id;
	}

	bool IsFragmentSegment(std::uint32_t a_segment)
	{
		return SourceOfSegment(GetCoordinator(), a_segment) != kMaxFragmentSources;
	}

	std::vector<std::uint32_t> FragmentSegments()
	{
		const auto& coordinator = GetCoordinator();
		std::vector<std::uint32_t> segments;
		for (std::uint32_t s = 0; s < coordinator.sourceCount; ++s)
			segments.insert(segments.end(), coordinator.sources[s]->source.segments.begin(), coordinator.sources[s]->source.segments.end());
		return segments;
	}

	const char* FragmentSegmentLabel(std::uint32_t a_segment, bool a_inputs)
	{
		const auto& coordinator = GetCoordinator();
		const auto source = SourceOfSegment(coordinator, a_segment);
		if (source == kMaxFragmentSources)
			return nullptr;
		const auto& state = *coordinator.sources[source];
		const auto& segments = state.source.segments;
		const auto& labels = state.source.labels;
		for (std::size_t i = 0; i < segments.size() && i < labels.size(); ++i)
			if (segments[i] == a_segment) {
				const auto& label = a_inputs ? labels[i].second : labels[i].first;
				return label.empty() ? nullptr : label.c_str();
			}
		return nullptr;
	}

	bool FragmentEpochFailed(std::uint32_t a_segment, std::string_view a_label, std::string_view a_error)
	{
		auto& coordinator = GetCoordinator();
		const auto source = SourceOfSegment(coordinator, a_segment);
		if (source == kMaxFragmentSources)
			return false;
		if (const auto& failed = coordinator.sources[source]->source.epochFailed)
			failed(a_label, a_error);
		return true;
	}

	void FragmentFrameStart(std::uint64_t a_frame)
	{
		auto& coordinator = GetCoordinator();
		for (std::uint32_t s = 0; s < coordinator.sourceCount; ++s)
			if (const auto& start = coordinator.sources[s]->source.frameStart)
				start(a_frame);
	}

	bool FragmentsDrawWorld()
	{
		auto& coordinator = GetCoordinator();
		for (std::uint32_t s = 0; s < coordinator.sourceCount; ++s)
			if (const auto& draws = coordinator.sources[s]->source.drawsWorld; draws && draws())
				return true;
		return false;
	}

	FragmentVersions FrameVersionsFor(FragmentSourceId a_source)
	{
		const auto& coordinator = GetCoordinator();
		if (a_source >= coordinator.sourceCount)
			return {};
		// The revision's pending versions, once its lane made them and the source is a member. A member revision that changes no
		// version (a shader reload) names the adopted ones, under its serial.
		if (const auto& preparing = coordinator.preparing; HasSourceMember(preparing.get(), a_source))
			return { preparing->serial, preparing->versionSet };
		return { coordinator.sources[a_source]->adoptedVersions, nullptr };
	}

	void PublishFragment(FragmentSourceId a_source, Fragment a_fragment)
	{
		auto& coordinator = GetCoordinator();
		if (a_source >= coordinator.sourceCount)
			RenderGraph::Fatal("frame revision", fmt::format("a fragment was published for source {}, which is not registered", a_source));
		// A replaced fragment is released here, on the poster's thread.
		coordinator.sources[a_source]->posted.Post(std::make_unique<Fragment>(std::move(a_fragment)));
	}

	AdoptionMark Mark()
	{
		auto& coordinator = GetCoordinator();
		AdoptionMark mark;
		mark.running = coordinator.installed;
		mark.revision = coordinator.markRevision.load(std::memory_order_acquire);
		mark.adoptedVariables = coordinator.markAdoptedVariables.load(std::memory_order_acquire);
		mark.takenVariables = coordinator.markTakenVariables.load(std::memory_order_acquire);
		return mark;
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
		logger::info("[ORG] frame revision fragments: adopted alone {}, with a revision {}; dropped as stale {}, on a replaced build {}, refused {}, "
					 "superseded {}; session: alone {}, with a revision {}, dropped {}/{}/{}/{}",
			w.fragmentsAlone, w.fragmentsWithRevision, w.fragmentsDropped[0], w.fragmentsDropped[1], w.fragmentsDropped[2], w.fragmentsDropped[3], s.fragmentsAlone,
			s.fragmentsWithRevision, s.fragmentsDropped[0], s.fragmentsDropped[1], s.fragmentsDropped[2], s.fragmentsDropped[3]);
		coordinator.window = {};
		const auto recordings = EpochRecordings::Read();
		logger::info("[ORG] epoch recordings: requested {}, ready {}, dropped {} (session)", recordings.requested, recordings.ready, recordings.dropped);
		Vars::LogStats();
	}
}

