#include "RenderGraph/PipelineVersions.h"

#include <atomic>

namespace RenderGraph::PipelineVersions
{
	namespace
	{
		// The adopted revision's ShaderGeneration, for any thread.
		std::atomic<std::uint64_t> adoptedGeneration{ 0 };
	}

	std::uint64_t Adopted()
	{
		return adoptedGeneration.load(std::memory_order_acquire);
	}
}

#include "RenderGraph/BuildManager.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/Frame/VariablesLane.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "Utils/Logging.h"

#include <ORGModuleServices/PipelineRegistry.h>
#include <OpenRenderGraph/GraphHost.h>
#include <Render/PreparedPass.h>
#include <Render/RenderGraph/RenderGraph.h>

#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace RenderGraph::PipelineVersions
{
	namespace
	{
		namespace Vars = Frame::Vars;
		namespace Revision = Frame::Revision;

		/** @brief The pins of one epoch's builds: the attached one and the staged one. */
		struct EpochPins
		{
			std::uint64_t attachedBuild = 0;  // 0: a host build on the owner thread, which attaches at once
			std::uint64_t attachedPin = 0;
			std::uint64_t stagedBuild = 0;
			std::uint64_t stagedPin = 0;
		};

		struct PinTable
		{
			std::mutex mutex;
			std::map<std::uint32_t, EpochPins> epochs;
		};

		PinTable& Pins()
		{
			static PinTable table;
			return table;
		}

		void NoteAttached(std::uint32_t a_epoch, std::uint64_t a_build, std::uint64_t a_pin)
		{
			auto& table = Pins();
			const std::lock_guard lock(table.mutex);
			auto& pins = table.epochs[a_epoch];
			pins.attachedBuild = a_build;
			pins.attachedPin = a_pin;
		}

		void NoteStaged(std::uint32_t a_epoch, std::uint64_t a_build, std::uint64_t a_pin)
		{
			auto& table = Pins();
			const std::lock_guard lock(table.mutex);
			auto& pins = table.epochs[a_epoch];
			pins.stagedBuild = a_build;
			pins.stagedPin = a_pin;
		}

		/** @brief The pins of a_epoch's attached and staged builds, as the host has them now. 0 for a build the table does not know. */
		std::pair<std::uint64_t, std::uint64_t> PinsNow(org::GraphHost& a_host, std::uint32_t a_epoch)
		{
			auto& table = Pins();
			const std::lock_guard lock(table.mutex);
			const auto it = table.epochs.find(a_epoch);
			if (it == table.epochs.end())
				return { 0, 0 };
			const auto& pins = it->second;
			const std::uint64_t attached = pins.attachedBuild == 0 || pins.attachedBuild == a_host.BuildGeneration(a_epoch) ? pins.attachedPin : 0;
			const std::uint64_t stagedBuild = a_host.StagedGeneration(a_epoch);
			const std::uint64_t staged = stagedBuild != 0 && pins.stagedBuild == stagedBuild ? pins.stagedPin : 0;
			return { attached, staged };
		}

		/**
		* @brief The extension every graph build makes: it pins the registry generation, and its graph's recordings carry the pin
		* (RecordingContext::Tag). It adds no passes.
		*/
		class PinExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			PinExtension(org::GraphHost& a_host, std::shared_ptr<const org::services::PipelinePin> a_pin) :
				host(&a_host), pin(std::move(a_pin)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				a_graph.SetRecordingTag(pin);
				auto* owner = host;
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				const std::uint64_t generation = pin->generation;
				// A build on the build lane counts once it is staged or attached. A dropped one never does.
				if (Builds::DeferStaged([owner, epoch, generation] { NoteStaged(epoch, owner->StagedGeneration(epoch), generation); })) {
					(void)Builds::Defer([owner, epoch, generation] { NoteAttached(epoch, owner->BuildGeneration(epoch), generation); });
					return;
				}
				// The host's own builds on the owner thread attach at once.
				NoteAttached(epoch, 0, generation);
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>&) override {}

		private:
			org::GraphHost* host;
			std::shared_ptr<const org::services::PipelinePin> pin;
		};

		/** @brief The revision's input for a change of ShaderGeneration: every graph rebuilt with the new pipeline versions. Render thread. */
		Revision::InputWait ProduceShaderGeneration(const org::services::VariableSnapshot& a_values, std::uint64_t a_revision)
		{
			const std::uint64_t generation = Vars::Lane::ValueIn(a_values, Vars::ShaderGeneration());
			Revision::InputWait wait;
			wait.what = fmt::format("every graph built with pipeline generation {}", generation);
			wait.pipelineGeneration = generation;
			wait.poll = [generation](std::string& a_why) {
				auto* host = RenderGraphRuntime::Get().Host();
				// Without a graph nothing binds a pipeline version.
				if (!host)
					return Revision::InputState::Ready;
				if (EveryEpochAt(*host, generation, a_why))
					return Revision::InputState::Ready;
				static Util::ReasonLog s_pending("[ORG] shader reload: the revision waits for an epoch's rebuild");
				s_pending.Report(a_why);
				return Revision::InputState::Pending;
			};
			logger::info("[ORG] shader reload: frame revision {} waits for {}; frames keep the adopted pipelines meanwhile", a_revision, wait.what);
			return wait;
		}
	}

	std::shared_ptr<const org::services::PipelineVersion> VersionFor(const org::services::LivePipelineSlot& a_slot, const org::RecordingContext& a_recording)
	{
		// Only PinExtension sets a graph's recording tag, so a tag is a pin.
		if (const auto* pin = static_cast<const org::services::PipelinePin*>(a_recording.Tag()))
			return a_slot.At(pin->generation);
		return a_slot.Current();
	}

	void Attach(org::GraphHost& a_host)
	{
		auto* host = &a_host;
		a_host.AddExtension("cs.org.pipeline-versions", [host]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
			return std::make_unique<PinExtension>(*host, RenderGraphRuntime::PipelineRegistry()->Pin());
		});
	}

	void Install()
	{
		static std::once_flag once;
		std::call_once(once, [] {
			(void)Revision::RegisterInputProducer("ShaderGeneration", &ProduceShaderGeneration);
			Vars::OnAdopted(Vars::ShaderGeneration(), std::function<void(const std::uint64_t&, const std::uint64_t&)>([](const std::uint64_t& a_before, const std::uint64_t& a_now) {
				adoptedGeneration.store(a_now, std::memory_order_release);
				logger::info("[ORG] shader reload: pipeline generation {} adopted (was {}): every graph and recording the frames run from here binds its versions; "
							 "no graph was built on the render thread",
					a_now, a_before);
			}));
		});
	}

	std::uint64_t BuildPin(org::GraphHost& a_host, std::uint32_t a_epoch, std::uint64_t a_build)
	{
		if (a_build != 0 && a_build == a_host.StagedGeneration(a_epoch))
			return PinsNow(a_host, a_epoch).second;
		if (a_build == a_host.BuildGeneration(a_epoch))
			return PinsNow(a_host, a_epoch).first;
		return 0;
	}

	bool EveryEpochAt(org::GraphHost& a_host, std::uint64_t a_generation, std::string& a_why)
	{
		for (std::size_t i = 0; i < a_host.GraphCount(); ++i) {
			const auto epoch = a_host.EpochAt(i);
			// An epoch's first build pins the newest generation.
			if (!a_host.Graph(epoch))
				continue;
			const auto [attached, staged] = PinsNow(a_host, epoch);
			if (attached >= a_generation || staged >= a_generation)
				continue;
			a_why = fmt::format("epoch {} runs pipeline generation {} (staged: {})", epoch, attached, staged);
			return false;
		}
		return true;
	}
}

