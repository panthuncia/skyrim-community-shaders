// The CPU-driven pipeline's draws as graph passes (Sites.h): each draw site's epoch ends with a late pass (Api::PassDef::recordAtExecution,
// ORG's RecordAtExecution), which the site's execution records on DXVK's CS thread into DXVK's command buffer, once the site's culls are in
// (SiteFrame.h, RecordExecutingSite). It declares the targets its site draws, so the graph orders, times and names the draws with the
// epoch's other passes; its revision is constant (what it draws is its run's, not the recording's), so a kept recording records it again
// in each execution (PreparedPass::RecordAgain).

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "RenderGraph/BufferRefs.h"

#include <RenderPasses/Base/TypedRenderGraphPass.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace DCLF::Direct
{
	namespace
	{
		struct SiteBindings
		{
			Site site = Site::Depth;
			RenderGraph::DeclaredRefs sequences;  // the view batch sequences, which the recorder pushes refs into
		};

		/** @brief A draw site's late pass: its targets declared, its recording the executing run's draws. */
		class SitePass final : public org::TypedRenderGraphPass<SitePass, org::EmptyPassFrameData, SiteBindings>
		{
		public:
			using Textures = std::vector<std::shared_ptr<org::ExternalTextureResource>>;

			// a_reads: in a GPU-driven graph, what its draws read: there the barriers are ORG's alone, an external epoch's being full.
			using Reads = std::function<void(org::PassBuilder&)>;
			SitePass(Site a_site, Textures a_colours, Textures a_depths, Reads a_reads) :
				site(a_site), colours(std::move(a_colours)), depths(std::move(a_depths)), reads(std::move(a_reads)) {}

			SiteBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				for (const auto& colour : colours)
					(void)a_builder.RenderTarget(colour);
				for (const auto& depth : depths)
					(void)a_builder.DepthReadWrite(depth);
				if (reads)
					reads(a_builder);
				// The site's draws read the frame's placements, palettes, shading and fade seeds through the frame record (t117, t121-t123).
				FrameValues::Get().DeclareReads(a_builder);
				// And the view batch set's sequences, through the ref every draw's push words carry (ViewBatchSet::sequencesRef).
				SiteBindings bindings{ site };
				Draws::DeclareViewBatchReads(a_builder, bindings.sequences);
				// The set's sequences' rows are offsets from the payload's row parts (ViewBatchSet::pipelineRowsRef...).
				Draws::DeclarePayloadReads(a_builder);
				return bindings;
			}

			org::EmptyPassFrameData Prepare(const SiteBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				// The set's ref is latched when the frame takes it (PublishViewBatches). Phase 4 records with the declaration's.
				RenderGraph::CheckDeclaredRefs(a_bindings.sequences, a_preparation, "a CPU draw site");
				return {};
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(static_cast<std::uint64_t>(site));
			}

			static void Record(const SiteBindings& a_bindings, org::PassRecordContext& a_recording)
			{
				(void)detail::RecordExecutingSite(a_bindings.site, a_recording.Commands());
			}

		private:
			Site site;
			Textures colours, depths;
			Reads reads;
		};

		/** @brief A site's pass, or null when it would declare no target (a pass that writes nothing would be culled). */
		std::shared_ptr<org::RenderPass> MakeSitePass(Site a_site, SitePass::Textures a_colours, SitePass::Textures a_depths, SitePass::Reads a_reads = nullptr)
		{
			std::erase(a_colours, nullptr);
			std::erase(a_depths, nullptr);
			if (a_colours.empty() && a_depths.empty())
				return nullptr;
			return std::make_shared<SitePass>(a_site, std::move(a_colours), std::move(a_depths), std::move(a_reads));
		}

		/** @brief What the forward family's draws read of the main resources, declared in a GPU-driven graph. */
		void DeclareMainReads(org::PassBuilder& a_builder, const Draws::Resources& a_main)
		{
			const std::span<const org::SrvView> noViews{};
			(void)a_builder.IndexBuffer(*a_main.pool->indices);
			if (a_main.scene->vertexStream)
				a_builder.ShaderResource(*a_main.scene->vertexStream, noViews);
			Draws::DeclareFrameBuffers(a_builder, a_main);
			a_builder.ShaderResource(a_main.frameConstants, noViews);
		}

		/**
		 * @brief What a main site's draws read and write by buffer view ref, declared in every graph: the main frame constants (the frame
		 * record's slots, after the latched copies), and with the foliage record (Policy::RecordFoliagePixels) the depth draws' owners
		 * (a_depth) or the colour draws' ids and colours. An external epoch's full barriers cover them too; the declarations order them.
		 */
		void DeclareFrameReads(org::PassBuilder& a_builder, const Draws::Resources& a_main, bool a_depth)
		{
			a_builder.ShaderResource(a_main.frameConstants, std::span<const org::SrvView>{});
			const auto& foliage = a_main.foliage;
			if (!foliage)
				return;
			const std::span<const org::UavView> noUavs{};
			if (a_depth) {
				a_builder.UnorderedAccess(foliage->owners, noUavs);
				return;
			}
			for (std::uint32_t h = 0; h < 2; ++h) {
				a_builder.UnorderedAccess(foliage->ids[h], noUavs);
				a_builder.UnorderedAccess(foliage->colours[h], noUavs);
			}
		}

		SitePass::Textures MainColours(const Draws::Resources& a_resources)
		{
			return { a_resources.native.begin(), a_resources.native.begin() + a_resources.targetCount };
		}

		// main.depth: main depth, the families' depth and the hosted views' (the reflection's faces into their own targets, first person's
		// depth into the main depth).
		std::shared_ptr<org::RenderPass> DepthSite(const Api::PassContext& a_context)
		{
			const auto& main = *a_context.Main();
			std::vector<std::shared_ptr<Draws::ViewResources>> hosted(a_context.hosted.begin(), a_context.hosted.end());
			return MakeSitePass(Site::Depth, {}, { main.nativeDepth },
				[resources = a_context.Main(), hosted = std::move(hosted)](org::PassBuilder& a_builder) {
					DeclareFrameReads(a_builder, *resources, true);
					// The hosted views' draws (RecordHostedDepthViews) read each view's blocks, and its own frame constants when it has
					// them, through the view's push words. Their latched copies are this epoch's, so the graph has them registered.
					const std::span<const org::SrvView> noViews{};
					for (const auto& view : hosted) {
						if (view->frameConstants)
							a_builder.ShaderResource(view->frameConstants, noViews);
						if (view->blocks)
							a_builder.ShaderResource(view->blocks, noViews);
					}
				});
		}

		// main.colour: main colour, the decals, the families' colour, the second pass.
		std::shared_ptr<org::RenderPass> ColourSite(const Api::PassContext& a_context)
		{
			const auto& main = *a_context.Main();
			return MakeSitePass(Site::Colour, MainColours(main), { main.nativeDepth },
				[resources = a_context.Main()](org::PassBuilder& a_builder) { DeclareFrameReads(a_builder, *resources, false); });
		}

		// forward: the forward range's draws and the refraction normals. In the GPU-driven graph too, under GraphConditions::forwardDirect.
		std::shared_ptr<org::RenderPass> ForwardSite(const Api::PassContext& a_context)
		{
			const auto& conditions = Draws::CurrentGraphConditions();
			const auto& main = *a_context.Main();
			if (!conditions.cpuPipeline && (!conditions.forwardDirect || !main.pool))
				return nullptr;
			auto colours = MainColours(main);
			colours.push_back(main.refractionNormals);
			SitePass::Reads reads;
			if (!conditions.cpuPipeline)
				reads = [resources = a_context.Main()](org::PassBuilder& a_builder) { DeclareMainReads(a_builder, *resources); };
			else
				reads = [resources = a_context.Main()](org::PassBuilder& a_builder) {
					a_builder.ShaderResource(resources->frameConstants, std::span<const org::SrvView>{});
				};
			return MakeSitePass(Site::Forward, std::move(colours), { main.nativeDepth }, std::move(reads));
		}

		/**
		 * @brief What the shadow site's draws read, declared in every graph: the shadow constants (the views' frame record) and the view
		 * blocks, by the addresses and refs in each view's push words (ShadowFrameView::push), and the index pool's indices (the adopted
		 * version, bound as the index buffer), as the GPU-driven shadow views pass declares them. An external
		 * epoch's full barriers cover them too; the declarations order them, and Phase 4's in-pass recording needs them.
		 */
		void DeclareShadowReads(org::PassBuilder& a_builder, const Draws::ShadowResources& a_shadow)
		{
			const std::span<const org::SrvView> noViews{};
			if (a_shadow.constants)
				a_builder.ShaderResource(a_shadow.constants, noViews);
			if (a_shadow.viewBlocks.buffer)
				a_builder.ShaderResource(*a_shadow.viewBlocks.buffer, noViews);
			if (a_shadow.pool && a_shadow.pool->indices)
				(void)a_builder.IndexBuffer(*a_shadow.pool->indices);
		}

		// shadow: every shadow view and the occlusion maps, into the imported maps.
		std::shared_ptr<org::RenderPass> ShadowSite(const Api::PassContext& a_context)
		{
			const auto& shadow = *a_context.Shadow();
			return MakeSitePass(Site::Shadow, {}, { shadow.depth.begin(), shadow.depth.end() },
				[resources = a_context.Shadow()](org::PassBuilder& a_builder) { DeclareShadowReads(a_builder, *resources); });
		}

		// fp.colour: first person's colour and forward part, into the main targets against its depth (kPOST_ZPREPASS_COPY).
		std::shared_ptr<org::RenderPass> FirstPersonSite(const Api::PassContext& a_context)
		{
			const auto& conditions = Draws::CurrentGraphConditions();
			const auto& view = *a_context.View();
			if (!view.main || (!conditions.cpuPipeline && (!conditions.firstPersonForwardDirect || !view.main->pool)))
				return nullptr;
			SitePass::Reads reads;
			if (!conditions.cpuPipeline)
				reads = [resources = a_context.View()](org::PassBuilder& a_builder) {
					a_builder.ShaderResource(resources->blocks, std::span<const org::SrvView>{});
					DeclareMainReads(a_builder, *resources->main);
				};
			else
				reads = [resources = a_context.View()](org::PassBuilder& a_builder) {
					a_builder.ShaderResource(resources->main->frameConstants, std::span<const org::SrvView>{});
				};
			return MakeSitePass(Site::FirstPerson, MainColours(*view.main), { view.target }, std::move(reads));
		}

		constexpr Api::PassDef kSitePasses[] = {
			{ .key = "core.z.direct", .passName = "cs.dclf.z.direct", .kind = Api::PassKind::Render, .make = &DepthSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "core.direct", .passName = "cs.dclf.direct", .kind = Api::PassKind::Render, .make = &ColourSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "core.forward-direct", .passName = "cs.dclf.forward.direct", .kind = Api::PassKind::Render, .make = &ForwardSite, .cpuPipeline = true, .recordAtExecution = true, .gpuLate = true },
			{ .key = "core.shadow-direct", .passName = "cs.dclf.shadow.direct", .kind = Api::PassKind::Render, .make = &ShadowSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "view.direct", .passName = "{view}.direct", .kind = Api::PassKind::Render, .make = &FirstPersonSite, .cpuPipeline = true, .recordAtExecution = true, .gpuLate = true },
		};
	}

	std::span<const Api::PassDef> SitePassDefs()
	{
		return kSitePasses;
	}
}
