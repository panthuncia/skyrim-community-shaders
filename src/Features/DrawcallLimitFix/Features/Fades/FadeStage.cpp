// Fade passes in the depth epoch. FadeStateCS computes the roots' states for the next frame's cull on the compute queue,
// alongside the depth raster work. Then the write-back events are copied to the host.

#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"  // MakeEventReadbackPass
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct FadeStateBindings
	{
		org::DeclaredViewToken roots, states, frame, log, rootPrograms, animated, events, reported;
		org::DeclaredViewToken queries, results;
		std::array<org::DeclaredViewToken, 2> published;
	};

	struct FadeStatePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		FadeStateConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
		bool queries = false;  // true for the portal query stage, which writes queries instead of reading results
		bool events = false;
	};

	/** @brief Runs each root's main-camera OnVisible once a frame, before the depth culling. */
	class FadeStatePass final : public org::TypedRenderGraphPass<FadeStatePass, FadeStatePrepared, FadeStateBindings>
	{
	public:
		FadeStatePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_queries = false) :
			resources(std::move(a_resources)), fades(Features::Fades::SharedSceneOf(*resources->scene)), segment(a_segment), queryStage(a_queries) {}

		FadeStateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(AsyncComputeQueue());
			const auto& scene = *fades;
			FadeStateBindings bindings{};
			if (queryStage)
				bindings.queries = a_builder.UnorderedAccess(*scene.portalBatch->queries).View();
			else
				bindings.results = a_builder.ShaderResource(*scene.portalBatch->results).View();
			bindings.roots = a_builder.ShaderResource(*scene.fadeRoots).View();
			bindings.states = a_builder.UnorderedAccess(*scene.fadeStates).View();
			for (std::uint32_t h = 0; h < 2; ++h)
				bindings.published[h] = a_builder.UnorderedAccess(*scene.fadeStatesOut[h]).View();
			bindings.frame = a_builder.ShaderResource(scene.fadeFrameBuffer).View();
			// The programs are a per-frame publication filled by an upload in the same execution, so the late-bound buffer must be declared.
			if (queryStage)
				Extensions::DeclareCullBindingPublications(a_builder, *resources->scene, { Api::CullBindingSlot::Programs });
			bindings.rootPrograms = a_builder.ShaderResource(*scene.fadeRootPrograms).View();
			bindings.animated = a_builder.ShaderResource(*scene.fadeAnimated).View();
			bindings.log = a_builder.UnorderedAccess(scene.fadeLog).View();
			if (scene.fadeEvents) {
				bindings.events = a_builder.UnorderedAccess(*scene.fadeEvents).View();
				bindings.reported = a_builder.UnorderedAccess(*scene.fadeReported).View();
			}
			FrameValues::Get().DeclareReads(a_builder, FrameValues::kReadPlacements | FrameValues::kReadPools);
			return bindings;
		}

		// Counts live in the uploaded FadeFrame row, so they do not need to be part of the revision.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(queryStage); }

		FadeStatePrepared Prepare(const FadeStateBindings&, const org::PassPrepareContext& a_preparation) const
		{
			FadeStatePrepared prepared{};
			const auto& scene = *fades;
			const auto frame = CurrentFrame(a_preparation, segment);
			// The smallest root capacity among the per-root buffers.
			std::uint64_t roots = ~0ull;
			for (const auto* buffer : { &scene.fadeRoots, &scene.fadeStates, &scene.fadeStatesOut[0], &scene.fadeStatesOut[1], &scene.fadeRootPrograms, &scene.fadeAnimated })
				roots = std::min(roots, NamedUnitsOf(a_preparation, *buffer));
			if (scene.fadeReported)
				roots = std::min(roots, NamedUnitsOf(a_preparation, scene.fadeReported));
			if (!scene.fadeState || !roots || !frame || !frame->latch)
				return prepared;
			prepared.program = queryStage ? scene.portalQueries : scene.fadeState;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			prepared.queries = queryStage;
			// Bind events only when a staging set can hold the whole list.
			if (const auto events = static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, scene.fadeEvents));
				scene.fadeEvents && scene.fadeWriteBack && scene.fadeWriteBack->SetFor(events)) {
				prepared.events = true;
				constants.eventCapacity = events;
			}
			// Full capacity; the shader stops at the count.
			prepared.groups = static_cast<std::uint32_t>((roots + kFadeStateGroup - 1) / kFadeStateGroup);
			return prepared;
		}

		static void Record(const FadeStateBindings& a_bindings, const FadeStatePrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			constants.rootsIndex = a_recording.Index(a_bindings.roots);
			constants.statesIndex = a_recording.Index(a_bindings.states);
			constants.frameIndex = a_recording.Index(a_bindings.frame);
			constants.logIndex = a_recording.Index(a_bindings.log);
			constants.rootProgramsIndex = a_recording.Index(a_bindings.rootPrograms);
			if (a_frame.queries)
				constants.portalQueriesIndex = a_recording.Index(a_bindings.queries);
			else
				constants.portalResultsIndex = a_recording.Index(a_bindings.results);
			constants.animatedIndex = a_recording.Index(a_bindings.animated);
			for (std::uint32_t h = 0; h < 2; ++h)
				constants.outIndices[h] = a_recording.Index(a_bindings.published[h]);
			if (a_frame.events) {
				constants.eventsIndex = a_recording.Index(a_bindings.events);
				constants.reportedIndex = a_recording.Index(a_bindings.reported);
			}
			SetLatchWords(constants, *a_frame.latch, a_recording.Run());
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kFadeStateConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(std::min(a_frame.groups, 65535u), (a_frame.groups + 65534u) / 65535u, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const Features::Fades::FadeScene> fades;  // never null, the setup requires it
		RenderGraphRuntime::Segment segment;
		bool queryStage = false;
	};
}

namespace DCLF::Features::Fades
{
	void RegisterPortalBatchPasses(Api::InstallContext& a_context);
	namespace
	{
		// Null keeps the pass out of the graph.
		std::shared_ptr<const void> SetUpFadeState(const Api::PassSetup& a_setup)
		{
			auto fades = SharedSceneOf(a_setup.scene);
			if (!fades || !fades->fadeState || !fades->fadeRoots)
				return nullptr;
			return fades;
		}

		std::shared_ptr<org::RenderPass> MakeFadeStateStage(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::FadeStatePass>(a_context.Main(), a_context.segment);
		}
		std::shared_ptr<org::RenderPass> MakeFadeQueryStage(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::FadeStatePass>(a_context.Main(), a_context.segment, true);
		}

		std::shared_ptr<const void> SetUpFadeEvents(const Api::PassSetup& a_setup)
		{
			auto fades = SharedSceneOf(a_setup.scene);
			if (!fades || !fades->fadeState || !fades->fadeRoots || !fades->fadeEvents || !fades->fadeWriteBack)
				return nullptr;
			return fades;
		}

		std::shared_ptr<org::RenderPass> MakeFadeEventsStage(const Api::PassContext& a_context)
		{
			const auto fades = a_context.State<FadeScene>();
			return Draws::MakeEventReadbackPass(a_context.Main()->scene, fades->fadeEvents, fades->fadeWriteBack);
		}
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		RegisterPortalBatchPasses(a_context);
		a_context.RegisterPass({ .key = "fades.portal-queries", .passName = "cs.dclf.z.fade-portal-queries", .queue = Api::PassQueue::Compute, .make = &MakeFadeQueryStage, .setup = &SetUpFadeState });
		a_context.RegisterPass({ .key = "fades.state", .passName = "cs.dclf.z.fade-state", .queue = Api::PassQueue::Compute, .make = &MakeFadeStateStage, .setup = &SetUpFadeState });
		a_context.RegisterPass({ .key = "fades.events", .passName = "cs.dclf.z.fade-events", .kind = Api::PassKind::Copy, .make = &MakeFadeEventsStage, .setup = &SetUpFadeEvents });
	}
}
