#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBatchLayout.h"

namespace DCLF::Features::Portals
{
	namespace
	{
		struct Constants
		{
			std::uint32_t queries, results, programs, traces, queryCount;
			std::uint32_t latchIndex, latchOffset, aux;  // the depth latch, whose portalProgramsIndex names the programs
			std::uint32_t traceFrame;                    // CS_DCLF_PORTAL_TRACE: the scene frame the pass prepared for
			std::uint32_t padding[3];
		};
		static_assert(sizeof(Constants) == 48);
		constexpr std::uint32_t kConstantWords = sizeof(Constants) / 4;
		struct Bindings
		{
			org::DeclaredViewToken queries, results, traces;
		};
		struct Prepared
		{
			std::shared_ptr<const Portals::Batch> batch;
			std::shared_ptr<const org::LatchBlock> latch;
			Constants constants{};
		};

		class BatchPass final : public org::TypedRenderGraphPass<BatchPass, Prepared, Bindings>
		{
		public:
			BatchPass(std::shared_ptr<Draws::Resources> a_resources, std::shared_ptr<const Batch> a_batch, RenderGraphRuntime::Segment a_segment) :
				resources(std::move(a_resources)), batch(std::move(a_batch)), segment(a_segment) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(Draws::AsyncComputeQueue());
				const auto& b = *batch;
				Bindings bindings{};
				bindings.queries = a_builder.ShaderResource(*b.queries).View();
				bindings.results = a_builder.UnorderedAccess(*b.results).View();
				// The programs are declared through their late-bound buffer, because the pass may run on the compute queue and the depth commit's
				// upload fills the backing in this execution.
				Extensions::DeclareCullBindingPublications(a_builder, *resources->scene, { Api::CullBindingSlot::Programs });
				if (b.trace)
					bindings.traces = a_builder.UnorderedAccess(*b.traces).View();
				return bindings;
			}

			// Opts into kept-recording reuse. Record depends on nothing beyond its bindings and the prepared constants.
			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(batch->trace); }

			Prepared Prepare(const Bindings&, const org::PassPrepareContext& a_preparation) const
			{
				Prepared prepared{};
				const auto frame = Draws::CurrentFrame(a_preparation, segment);
				if (!frame || !frame->latch)
					return prepared;
				prepared.batch = batch;
				prepared.latch = frame->latch;
				auto& c = prepared.constants;
				if (batch->trace)
					c.traceFrame = Scene::FrameView::Get().SceneStep();
				c.queryCount = static_cast<std::uint32_t>(Draws::NamedUnitsOf(a_preparation, batch->queries));
				return prepared;
			}

			static void Record(const Bindings& a_bindings, const Prepared& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.batch)
					return;
				auto c = a_prepared.constants;
				const auto shape = DCLF::Portals::DispatchFor(c.queryCount, 64u);
				if (!shape.x)
					return;
				// The stage's declared views (none declared: 0).
				const auto index = [&](const org::DeclaredViewToken& token) { return token.layout ? a_recording.Index(token) : 0u; };
				c.queries = index(a_bindings.queries);
				c.results = index(a_bindings.results);
				c.traces = index(a_bindings.traces);
				Draws::SetLatchWords(c, *a_prepared.latch, a_recording.Run());
				const auto& program = *a_prepared.batch->program;
				auto& commands = a_recording.Commands();
				commands.BindLayout(program.layout->GetHandle());
				commands.BindPipeline(program.Pipeline(a_recording));
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&c));
				commands.Dispatch(shape.x, shape.y, 1);
			}

		private:
			std::shared_ptr<Draws::Resources> resources;
			std::shared_ptr<const Batch> batch;
			RenderGraphRuntime::Segment segment;
		};
	}

	std::shared_ptr<org::RenderPass> MakeBatchPass(std::shared_ptr<Draws::Resources> a_resources, std::shared_ptr<const Batch> a_batch,
		RenderGraphRuntime::Segment a_segment)
	{
		if (!a_batch)
			return nullptr;
		return std::make_shared<BatchPass>(std::move(a_resources), std::move(a_batch), a_segment);
	}
}
