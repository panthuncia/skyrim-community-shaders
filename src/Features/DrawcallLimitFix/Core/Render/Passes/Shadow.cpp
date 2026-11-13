// The shadow views' and occlusion maps' graph extension: their resources registered, and the shadow epoch's passes gathered from the
// schedule (shadows.*: Features/Shadows/Engine's); and the shadow epoch's index pool copies (Core's IndexPoolPass, as the depth epoch's),
// which that feature's shadows.index-pool makes.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/IndexPoolPass.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"

namespace DCLF::Draws
{
	class ShadowExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit ShadowExtension(std::shared_ptr<const BuildState> a_build) :
			build(std::move(a_build)), resources(build->shadow), slots(build->shadowSlots) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.constants"), resources->constants);
			RegisterSceneBuffers(a_graph, *resources->scene);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.visibility"), resources->visibility);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.view-blocks"), resources->viewBlocks.buffer);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.caster-rows"), resources->casterRows);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.caster-firsts"), resources->casterFirsts);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.caster-rows-used"), resources->casterRowsUsed);
			// The slots and shadow maps as the build state has them, never the live vectors the render thread grows.
			for (std::size_t s = 0; s < slots->drawArgs.size(); ++s) {
				if (slots->OwnsDrawArgs(s))
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-args{}", s)), slots->drawArgs[s]);
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-count{}", s)), slots->count[s]);
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.bucket-counts{}", s)), slots->bucketCounts[s]);
			}
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (slots->depth[i])
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.depth{}", i)), slots->depth[i]);
			}
		}

		void GatherStructuralPasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			// The shadow epoch's passes (Schedule/Frame.cpp): the graph's epoch's.
			Api::PassContext context{};
			context.shadow = &build->shadow;
			context.build = build.get();
			GatherScheduled(Api::EpochShape::Shadow, {}, context, a_graph.PassEpochFilter(), a_out);
		}

	private:
		std::shared_ptr<const BuildState> build;  // what the graph is built from; held for the build
		std::shared_ptr<ShadowResources> resources;
		std::shared_ptr<const ShadowSlotBuffers> slots;
	};

	std::shared_ptr<org::RenderPass> MakeShadowIndexPoolPass(const std::shared_ptr<ShadowResources>& a_resources)
	{
		return std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(a_resources->pool, [shadow = a_resources](const org::PassPrepareContext& a_preparation) {
			const auto frame = CurrentShadowFrame(a_preparation);
			return IndexPoolPass::LatchOf{ frame ? frame->latch : nullptr, frame ? frame->latchLayout.PoolOffset() : 0u };
		}));
	}

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<const BuildState> a_build)
	{
		if (!a_build || !a_build->shadow || !a_build->shadowSlots)
			return nullptr;
		return std::make_unique<ShadowExtension>(std::move(a_build));
	}
}
