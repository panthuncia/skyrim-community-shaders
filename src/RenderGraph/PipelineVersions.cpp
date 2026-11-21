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

#include <ORGModuleServices/PipelinePins.h>
#include <ORGModuleServices/PipelineRegistry.h>
#include <OpenRenderGraph/GraphHost.h>

#include <functional>
#include <mutex>
#include <utility>

namespace RenderGraph::PipelineVersions
{
	namespace
	{
		namespace Vars = Frame::Vars;
		namespace Revision = Frame::Revision;

		/** @brief Every graph build's pipeline pin. Its builds on the build lane count once staged or attached. */
		org::services::PipelinePins& Pins()
		{
			static org::services::PipelinePins pins({
				.pin = [] { return RenderGraphRuntime::PipelineRegistry()->Pin(); },
				.defer = [](std::function<void()> a_staged, std::function<void()> a_attached) {
					if (!Builds::DeferStaged(std::move(a_staged)))
						return false;
					(void)Builds::Defer(std::move(a_attached));
					return true;
				},
			});
			return pins;
		}

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
		return org::services::VersionFor(a_slot, a_recording);
	}

	void Attach(org::GraphHost& a_host)
	{
		Pins().Attach(a_host, "cs.org.pipeline-versions");
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
		return Pins().BuildPin(a_host, a_epoch, a_build);
	}

	bool EveryEpochAt(org::GraphHost& a_host, std::uint64_t a_generation, std::string& a_why)
	{
		return Pins().EveryEpochAt(a_host, a_generation, a_why);
	}
}
