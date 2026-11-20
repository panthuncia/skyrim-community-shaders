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
#	include <atomic>
#	include <chrono>
#	include <deque>
#	include <map>
#	include <mutex>
#	include <optional>
#	include <string>
#	include <vector>

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
		constexpr std::uint32_t kRevisionSlots = 6;

		// The starvation rule (plan 7.8): recordings dropped and asked for again this often with no new input is a schedule bug.
		constexpr std::uint64_t kRevisionStarvedAfter = 4 * RenderGraphRuntime::kGameFramesInFlight;

		/** @brief A twin's newest size wish, held by its Alive() token since the twin may go first. */
		struct RevisionWish
		{
			std::weak_ptr<const void> alive;
			GraphResources::Resource* twin = nullptr;
			Extent extent{};
			std::uint64_t frame = 0;  // the frame of the first wish since the twin last had its wished size
		};

		/** @brief A buffer's newest contents, waiting for the next revision. */
		struct BufferWish
		{
			std::weak_ptr<org::VersionedBuffer> buffer;
			std::uint32_t elements = 0;
			std::shared_ptr<std::vector<std::byte>> bytes;
			std::shared_ptr<const void> payload;
		};

		/** @brief A texture the caller made, waiting to become its slot's next version (PublishTexture). */
		struct TextureWish
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
			std::shared_ptr<std::vector<std::byte>> bytes;  // kept until the upload completed
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
			std::vector<BufferWish> bufferWishes;
			std::vector<TextureWish> textureWishes;  // kept until adoption, so a superseded revision can give them back
			// The newest variables when it started, and the names of those changed since the revision before.
			std::shared_ptr<const org::services::VariableSnapshot> variables;
			std::string changedVariables;
			// What its variable changes need outside the frame, and the fragment that resolves once all of it is ready.
			std::vector<RevisionInputWait> inputWaits;
			std::shared_ptr<org::async::RevisionFragment> inputWaitsFragment;
			// The stage builds its declared clients' registrations need (Frame/Declare.h).
			std::vector<FrameAnchors::StageBuildNeed> needs;
			std::uint64_t firstWish = 0;
			std::uint64_t planned = 0;  // the frame it was handed to the lane
			std::shared_ptr<org::runtime::IUploadService> uploads;
			org::GraphHost* host = nullptr;
			// Written on the lane, then done.
			std::vector<GraphResources::PreparedVersion> versions;
			std::shared_ptr<const org::services::TextureVersionsMade> made;
			std::vector<std::shared_ptr<org::VersionedTexture>> published;  // the slots of textureWishes still alive
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
			std::uint64_t askedAgain = 0;
			std::uint32_t recordingCount = 0;
			std::uint32_t buildCount = 0;
		};

		struct RevisionStats
		{
			std::uint64_t prepared = 0, adopted = 0, superseded = 0, resized = 0, askedAgain = 0, builds = 0, buffers = 0, variableRevisions = 0;
			std::uint32_t maxMembers = 0, maxRecordings = 0, maxBuilds = 0;
			std::vector<std::uint64_t> wishFrames;
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
			std::map<const void*, BufferWish> bufferWishes;
			std::uint64_t bufferFirstWish = 0;
			// Textures the callers made, from any thread.
			std::mutex textureMutex;
			std::map<const void*, TextureWish> textureWishes;
			std::uint64_t textureFirstWish = 0;
			std::atomic<std::uint64_t> beginFrame{ 0 };  // the frame of the last Begin, for wishes from other threads
			std::map<const void*, std::shared_ptr<const void>> adoptedPayloads;
			// Render thread from here.
			std::map<GraphResources::Resource*, RevisionWish> wishes;
			std::map<GraphResources::Resource*, RevisionWish> resizes;  // wishes no version serves
			std::uint64_t wishSerial = 0;
			std::uint64_t plannedSerial = 0;
			std::uint64_t askedAgainSinceWish = 0;
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

		/**
		* @brief The buffers' versions made and their uploads queued on the uploader's worker path. The fragment resolves once every
		* upload completed, and fails when one is cancelled. Lane.
		*/
		void MakeBufferVersions(PreparedRevision& a_job)
		{
			std::vector<RevisionBuffer> entries;
			std::vector<std::shared_ptr<org::TrackedUploadTicket>> tickets;
			for (auto& wish : a_job.bufferWishes) {
				auto buffer = wish.buffer.lock();
				if (!buffer)
					continue;
				RevisionBuffer entry{ .key = buffer->Key(), .buffer = buffer, .elements = wish.elements, .payload = std::move(wish.payload), .bytes = std::move(wish.bytes) };
				entry.version = buffer->MakeStructured(wish.elements);
				entry.name = entry.version->buffer ? entry.version->buffer->GetName() : std::string("a buffer");
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
			a_job.bufferWishes.clear();
			if (entries.empty())
				return;
			auto buffers = std::make_shared<const RevisionBuffers>(std::move(entries));
			auto fragment = std::make_shared<org::async::RevisionFragment>();
			a_job.buffers = buffers;
			a_job.buffersFragment = fragment;
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
				for (const auto& wish : job->textureWishes) {
					auto texture = wish.texture.lock();
					if (!texture)
						continue;
					org::services::TextureVersionRequest request;
					request.slot = texture;
					request.image = wish.image;
					request.contents = org::services::TextureContents::Undefined;
					request.owner = wish.owner;
					requests.push_back(std::move(request));
					job->publishedNames.push_back(wish.image->GetName());
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

		/** @brief The hook RequestSize calls on every change of a twin's wish. Render thread. */
		void OnSizeWish(GraphResources::Resource& a_twin, const std::optional<Extent>& a_extent)
		{
			auto& coordinator = GetCoordinator();
			++coordinator.wishSerial;
			// A new event: drops asked again before it are not starvation.
			coordinator.askedAgainSinceWish = 0;
			coordinator.resizes.erase(&a_twin);
			if (!a_extent) {
				coordinator.wishes.erase(&a_twin);
				return;
			}
			const auto frame = Driver::Current().frame;
			if (!a_twin.Preparable(*a_extent)) {
				coordinator.wishes.erase(&a_twin);
				coordinator.resizes[&a_twin] = { a_twin.Alive(), &a_twin, *a_extent, frame };
				return;
			}
			auto& wish = coordinator.wishes[&a_twin];
			const bool standing = wish.twin == &a_twin && !wish.alive.expired();
			wish = { a_twin.Alive(), &a_twin, *a_extent, standing ? wish.frame : frame };
		}

		/** @brief Wishes no version serves take today's path: the twin is made again now, inside the adoption point. */
		void ApplyResizes(Coordinator& a_coordinator)
		{
			for (auto& [twin, wish] : std::exchange(a_coordinator.resizes, {})) {
				if (wish.alive.expired() || twin->CurrentExtent() == wish.extent)
					continue;
				twin->ResizeTo(wish.extent);
				if (!*twin)
					FailRevision(fmt::format("{} could not be made again at {} at a frame boundary", twin->Name(), ExtentText(wish.extent)));
				const std::lock_guard lock(a_coordinator.statsMutex);
				++a_coordinator.window.resized;
				++a_coordinator.session.resized;
			}
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
		* @brief Hands the standing inputs to the lane as the next revision: size wishes and buffer contents. With none of those, a
		* staged stage build makes a revision of its own. Only one is prepared at a time.
		*/
		void StartPreparation(Coordinator& a_coordinator, std::uint64_t a_frame)
		{
			if (a_coordinator.preparing)
				return;
			// Without a graph (plain D3D11, or before the graph is up) a revision carries only variables.
			auto* host = RenderGraphRuntime::Get().Host();
			auto job = std::make_shared<PreparedRevision>();
			job->firstWish = UINT64_MAX;
			// The declarations follow the values this revision starts with. Their new extents are wishes, collected below.
			if (host && a_coordinator.variables) {
				job->needs = std::exchange(a_coordinator.carriedNeeds, {});
				auto needs = Declarations::Evaluate(*a_coordinator.variables, [](GraphResources::Resource& a_twin, const std::array<unsigned int, 3>& a_extent) {
					OnSizeWish(a_twin, Extent{ a_extent[0], a_extent[1], a_extent[2] });
				});
				for (auto& need : needs)
					job->needs.push_back(std::move(need));
			}
			if (host && !a_coordinator.wishes.empty() && a_coordinator.plannedSerial != a_coordinator.wishSerial) {
				for (auto it = a_coordinator.wishes.begin(); it != a_coordinator.wishes.end();) {
					auto& [twin, wish] = *it;
					if (wish.alive.expired() || twin->CurrentExtent() == wish.extent) {
						it = a_coordinator.wishes.erase(it);
						continue;
					}
					auto plan = twin->PlanPrepare(wish.extent);
					if (!plan) {
						a_coordinator.resizes[twin] = wish;
						it = a_coordinator.wishes.erase(it);
						continue;
					}
					job->plans.push_back(std::move(plan));
					job->twins.push_back(twin);
					job->alive.push_back(wish.alive);
					job->names.push_back(twin->Name());
					job->from.push_back(twin->CurrentExtent());
					job->to.push_back(wish.extent);
					job->firstWish = (std::min)(job->firstWish, wish.frame);
					++it;
				}
				a_coordinator.plannedSerial = a_coordinator.wishSerial;
			}
			if (host) {
				const std::lock_guard lock(a_coordinator.bufferMutex);
				for (auto& [key, wish] : std::exchange(a_coordinator.bufferWishes, {}))
					job->bufferWishes.push_back(std::move(wish));
				if (!job->bufferWishes.empty())
					job->firstWish = (std::min)(job->firstWish, a_coordinator.bufferFirstWish);
			}
			if (host) {
				const std::lock_guard lock(a_coordinator.textureMutex);
				for (auto& [key, wish] : std::exchange(a_coordinator.textureWishes, {}))
					job->textureWishes.push_back(std::move(wish));
				if (!job->textureWishes.empty())
					job->firstWish = (std::min)(job->firstWish, a_coordinator.textureFirstWish);
			}
			// The variables changed since the last revision started. Every revision carries the newest snapshot.
			const bool variablesChanged = a_coordinator.variables && a_coordinator.variables != a_coordinator.startedVariables;
			if (variablesChanged)
				job->firstWish = (std::min)(job->firstWish, a_coordinator.variablesFirstChange);
			const bool lane = !job->plans.empty() || !job->bufferWishes.empty() || !job->textureWishes.empty();
			if (!lane && !(host && FrameAnchors::StagedStages()) && !variablesChanged && job->needs.empty())
				return;
			if (job->firstWish == UINT64_MAX)
				job->firstWish = a_frame;
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
			for (auto& need : revision->needs)
				a_coordinator.carriedNeeds.push_back(std::move(need));
			a_coordinator.plannedSerial = 0;
			// Its variable changes count again against the adopted values.
			a_coordinator.startedVariables = Vars::Lane::Adopted();
			// Its buffer contents go back, unless newer ones came meanwhile.
			if (revision->buffers) {
				const std::lock_guard lock(a_coordinator.bufferMutex);
				for (const auto& entry : revision->buffers->buffers)
					if (const auto buffer = entry.buffer.lock(); buffer && !a_coordinator.bufferWishes.contains(entry.key))
						a_coordinator.bufferWishes[entry.key] = { buffer, entry.elements, entry.bytes, entry.payload };
			}
			// So do its published textures.
			{
				const std::lock_guard lock(a_coordinator.textureMutex);
				for (auto& wish : revision->textureWishes)
					if (const auto texture = wish.texture.lock(); texture && !a_coordinator.textureWishes.contains(texture->Key()))
						a_coordinator.textureWishes[texture->Key()] = std::move(wish);
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

		/** @brief Adopts a ready revision whole. Render thread, inside the adoption point. */
		void Adopt(Coordinator& a_coordinator, std::uint64_t a_frame)
		{
			auto revision = std::move(a_coordinator.preparing);
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
					const std::lock_guard lock(a_coordinator.bufferMutex);
					for (const auto& entry : buffers->buffers)
						if (const auto buffer = entry.buffer.lock()) {
							buffer->Adopt(entry.version);
							a_coordinator.adoptedPayloads[entry.key] = entry.payload;
						}
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
			// The last revision's versions and recordings go once the GPU is past every frame submitted with them.
			if (a_coordinator.adopted)
				a_coordinator.retiring.emplace_back(DCLF::SubmittedRetirementPoint(), std::move(a_coordinator.adopted));
			for (std::size_t i = 0; i < revision->twins.size(); ++i)
				if (const auto it = a_coordinator.wishes.find(revision->twins[i]); it != a_coordinator.wishes.end() && it->second.extent == revision->to[i])
					a_coordinator.wishes.erase(it);
			const auto members = revision->recordings ? FrameAnchors::RevisionMembers(*revision->recordings) : std::vector<std::string>{};
			const std::uint64_t wishFrames = a_frame > revision->firstWish ? a_frame - revision->firstWish : 0;
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
					stats->wishFrames.push_back(wishFrames);
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
					"members [{}]",
					revision->serial, a_frame, wishFrames, revision->milliseconds, twins, bufferNames, revision->changedVariables, waits, stagedEpochs.size(),
					stages.empty() ? "none" : stages, revision->recordingCount, others.empty() ? "none" : others);
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
					Adopt(a_coordinator, a_frame);
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
				if (!revision.needs.empty())
					FrameAnchors::RequireBuilds(*revision.recordings, revision.needs);
				const std::lock_guard lock(a_coordinator.statsMutex);
				++a_coordinator.window.prepared;
				++a_coordinator.session.prepared;
			} else if (const auto slots = ChangedSlots(revision, held); !slots.empty()) {
				// Builds may change which graphs declare the slots, so the closure is taken again until adoption.
				FrameAnchors::AddRevisionMembers(*revision.recordings, revision.host->EpochsDeclaring(slots), revision.others);
			}
			const auto progress = FrameAnchors::AdvanceRevisionRecordings(*revision.recordings, a_frame);
			// A staged build found first is a new input: drops before it are not starvation.
			if (progress.newBuilds)
				a_coordinator.askedAgainSinceWish = 0;
			revision.askedAgain += progress.askedAgain;
			a_coordinator.askedAgainSinceWish += progress.askedAgain;
			if (progress.askedAgain) {
				const std::lock_guard lock(a_coordinator.statsMutex);
				a_coordinator.window.askedAgain += progress.askedAgain;
				a_coordinator.session.askedAgain += progress.askedAgain;
			}
			if (!progress.ready && a_coordinator.askedAgainSinceWish > kRevisionStarvedAfter) {
				Driver::NoteTapError(Driver::TapError::Starved, static_cast<SegmentId>(progress.missing),
					fmt::format("frame revision {} (inputs from frame {}, {} staged builds) has not been adopted: {} of its recordings were asked for again "
								"after they were dropped with no new input since (last: {}); still missing [{}]",
						revision.serial, revision.firstWish, progress.builds, a_coordinator.askedAgainSinceWish,
						progress.dropReasons.empty() ? "none" : progress.dropReasons, progress.waiting));
				a_coordinator.askedAgainSinceWish = 0;
			}
			if (!progress.ready || !inputsReady)
				return;
			revision.recordingCount = progress.recordings;
			revision.buildCount = progress.builds;
			Adopt(a_coordinator, a_frame);
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
		GraphResources::Resource::SetSizeWishHook(&OnSizeWish);
		// Bound settings raise their variable from here on.
		CS::SettingRegistry::SetRaiseHook(&Vars::RaiseSetting);
#	if !defined(CS_RELEASE_PROFILE)
		Testing::RegisterExitReport([] { LogStats(); });
#	endif
		logger::info("[ORG] frame revision: size wishes, buffer contents, staged stage builds and variable changes are prepared off the render thread "
					 "and adopted whole at a frame's Begin");
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
			coordinator.askedAgainSinceWish = 0;
		}
		ApplyResizes(coordinator);
		if (coordinator.preparing && coordinator.preparing->done.load(std::memory_order_acquire))
			Advance(coordinator, frame);
		// A revision of builds alone needs no lane, so it is advanced in the Begin that starts it.
		const bool idle = !coordinator.preparing;
		StartPreparation(coordinator, frame);
		if (idle && coordinator.preparing && coordinator.preparing->done.load(std::memory_order_acquire))
			Advance(coordinator, frame);
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
		if (coordinator.bufferWishes.empty())
			coordinator.bufferFirstWish = frame;
		coordinator.bufferWishes[a_buffer->Key()] = { a_buffer, a_elements, std::make_shared<std::vector<std::byte>>(std::move(a_bytes)), std::move(a_payload) };
	}

	void PublishTexture(const std::shared_ptr<org::VersionedTexture>& a_texture, std::shared_ptr<org::GloballyIndexedResource> a_image,
		std::shared_ptr<const void> a_owner)
	{
		if (!a_texture || !a_image)
			FailRevision("a texture was published without its slot or its image");
		auto& coordinator = GetCoordinator();
		const auto frame = coordinator.beginFrame.load(std::memory_order_relaxed);
		const std::lock_guard lock(coordinator.textureMutex);
		if (coordinator.textureWishes.empty())
			coordinator.textureFirstWish = frame;
		coordinator.textureWishes[a_texture->Key()] = { a_texture, std::move(a_image), std::move(a_owner) };
	}

	std::shared_ptr<const void> AdoptedPayload(const org::VersionedBuffer& a_buffer)
	{
		auto& coordinator = GetCoordinator();
		const std::lock_guard lock(coordinator.bufferMutex);
		const auto it = coordinator.adoptedPayloads.find(a_buffer.Key());
		return it != coordinator.adoptedPayloads.end() ? it->second : nullptr;
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
					 "per revision (max {}, {} in all), buffers {}, frames from first input to adoption (p50/p95/max) {}; recordings asked again {}, resized "
					 "at Begin without a version {}; session: prepared {}, adopted {}, superseded {}, members (max {}), recordings (max {}), builds (max {}, "
					 "{} in all), buffers {}, input to adoption {}, asked again {}, resized {}; with variable changes {} (session {})",
			w.prepared, w.adopted, w.superseded, w.maxMembers, w.maxRecordings, w.maxBuilds, w.builds, w.buffers, Percentiles(w.wishFrames), w.askedAgain,
			w.resized, s.prepared, s.adopted, s.superseded, s.maxMembers, s.maxRecordings, s.maxBuilds, s.builds, s.buffers, Percentiles(s.wishFrames),
			s.askedAgain, s.resized, w.variableRevisions, s.variableRevisions);
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
	void LogStats() {}
}

#endif
