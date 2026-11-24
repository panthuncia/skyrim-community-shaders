// The CPU-driven pipeline's draws as graph passes. Each draw site's epoch ends with a late pass recorded at execution,
// after the commit opened the site's run. In guest it records into DXVK's command buffer on DXVK's CS thread, in native
// inline into the epoch's own list. It declares its site's targets so the graph orders, times and names the draws.
// Its revision is constant because what it draws comes from its run, so a kept recording records it again each execution.

// Main/Resources.h goes first because volk must come before every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "RenderGraph/BufferRefs.h"

#include <RenderPasses/Base/TypedRenderGraphPass.h>

#include <algorithm>
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
			RenderGraph::DeclaredRefs sequences;  // the recorder pushes refs into these
			// The declared colours, then depths, which the recording resolves to the images this frame's graph binds.
			std::vector<org::DeclaredResourceUse> colours, depths;
			std::vector<std::uint32_t> order;  // SitePass's a_order
		};

		/** @brief A draw site's late pass, recording the executing run's draws. */
		class SitePass final : public org::TypedRenderGraphPass<SitePass, org::EmptyPassFrameData, SiteBindings>
		{
		public:
			using Textures = std::vector<FrameAnchors::Texture>;

			// Only in a GPU-driven graph, where ORG alone places the barriers. An external epoch's barriers are full anyway.
			using Reads = std::function<void(org::PassBuilder&)>;
			// a_order: the recording's colours as indices into a_colours, for a site that draws one of several target sets sharing
			// targets (the water sites). Empty: a_colours in order.
			SitePass(Site a_site, Textures a_colours, Textures a_depths, Reads a_reads, std::vector<std::uint32_t> a_order = {}) :
				site(a_site), colours(std::move(a_colours)), depths(std::move(a_depths)), reads(std::move(a_reads)), order(std::move(a_order)) {}

			SiteBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				SiteBindings bindings{ site };
				bindings.order = order;
				for (const auto& colour : colours)
					bindings.colours.push_back(a_builder.RenderTarget(colour));
				for (const auto& depth : depths)
					bindings.depths.push_back(a_builder.DepthReadWrite(depth));
				if (reads)
					reads(a_builder);
				// The draws read t117 and t121-t123 through the frame record.
				FrameValues::Get().DeclareReads(a_builder);
				// And the batch set's sequences, through the ref every draw's push words carry.
				Draws::DeclareViewBatchReads(a_builder, bindings.sequences);
				// The sequences' rows are offsets from the payload's row parts.
				Draws::DeclarePayloadReads(a_builder);
				return bindings;
			}

			org::EmptyPassFrameData Prepare(const SiteBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				// The set's ref is latched when the frame takes it. Phase 4 records with the declaration's.
				RenderGraph::CheckDeclaredRefs(a_bindings.sequences, a_preparation, "a CPU draw site");
				return {};
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(static_cast<std::uint64_t>(site));
			}

			static void Record(const SiteBindings& a_bindings, org::PassRecordContext& a_recording)
			{
				// Find the run through the epoch's run, since the render thread may have opened the next site by now.
				const org::RunRef run = a_recording.Run();
				detail::SiteImages images;
				const auto resolve = [&](const std::vector<org::DeclaredResourceUse>& a_uses, std::vector<detail::SiteImages::Image>& a_out) {
					a_out.reserve(a_uses.size());
					for (const auto& use : a_uses)
						a_out.push_back({ a_recording.View(use.View()), a_recording.Resolve(use.Resource()).GetHandle() });
				};
				resolve(a_bindings.colours, images.colours);
				resolve(a_bindings.depths, images.depths);
				if (!a_bindings.order.empty()) {
					std::vector<detail::SiteImages::Image> ordered;
					ordered.reserve(a_bindings.order.size());
					for (const std::uint32_t index : a_bindings.order)
						ordered.push_back(images.colours[index]);
					images.colours = std::move(ordered);
				}
				(void)detail::RecordExecutingSite(a_bindings.site, a_recording.Commands(), &run, &images);
			}

		private:
			Site site;
			Textures colours, depths;
			Reads reads;
			std::vector<std::uint32_t> order;
		};

		/** @brief A site's pass, or null when it would declare no target, since a pass that writes nothing would be culled. */
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
		 * @brief What a main site's draws read and write by buffer view ref, declared in every graph. External epochs have full
		 * barriers, so the declarations are there for ordering.
		 */
		void DeclareFrameReads(org::PassBuilder& a_builder, const Draws::Resources& a_main)
		{
			a_builder.ShaderResource(a_main.frameConstants, std::span<const org::SrvView>{});
		}

		SitePass::Textures MainColours(const Draws::Resources& a_resources)
		{
			return { a_resources.native.begin(), a_resources.native.begin() + a_resources.targetCount };
		}

		// main.depth: main depth, the families' depth and the hosted views.
		std::shared_ptr<org::RenderPass> DepthSite(const Api::PassContext& a_context)
		{
			const auto& main = *a_context.Main();
			std::vector<std::shared_ptr<Draws::ViewResources>> hosted(a_context.hosted.begin(), a_context.hosted.end());
			return MakeSitePass(Site::Depth, {}, { main.nativeDepth },
				[resources = a_context.Main(), hosted = std::move(hosted)](org::PassBuilder& a_builder) {
					DeclareFrameReads(a_builder, *resources);
					// The hosted views read their blocks through their push words. Their latched copies are this epoch's, so the graph knows them.
					const std::span<const org::SrvView> noViews{};
					for (const auto& view : hosted) {
						if (view->frameConstants)
							a_builder.ShaderResource(view->frameConstants, noViews);
						if (view->blocks)
							a_builder.ShaderResource(view->blocks, noViews);
					}
				});
		}

		// main.colour: main colour, the decals, the families' colour and the second pass.
		std::shared_ptr<org::RenderPass> ColourSite(const Api::PassContext& a_context)
		{
			const auto& main = *a_context.Main();
			return MakeSitePass(Site::Colour, MainColours(main), { main.nativeDepth },
				[resources = a_context.Main()](org::PassBuilder& a_builder) { DeclareFrameReads(a_builder, *resources); });
		}

		// forward: the forward range and refraction normals. The GPU-driven graph draws its forward family here too.
		std::shared_ptr<org::RenderPass> ForwardSite(const Api::PassContext& a_context)
		{
			const auto& conditions = a_context.Conditions();
			const auto& main = *a_context.Main();
			if (!conditions.cpuPipeline && !main.pool)
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
		 * @brief What the shadow site's draws read, declared in every graph: the shadow constants, view blocks and index pool.
		 * The declarations give ordering, and Phase 4's in-pass recording needs them.
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

		// shadow: every shadow view and occlusion map, into the imported maps.
		std::shared_ptr<org::RenderPass> ShadowSite(const Api::PassContext& a_context)
		{
			// Use the build state's shadow maps, since the render thread imports the live ones.
			const auto& depth = a_context.Build().shadowSlots->depth;
			return MakeSitePass(Site::Shadow, {}, { depth.begin(), depth.end() },
				[resources = a_context.Shadow()](org::PassBuilder& a_builder) { DeclareShadowReads(a_builder, *resources); });
		}

		// fp.colour: first person's colour and forward part, into the main targets against kPOST_ZPREPASS_COPY.
		std::shared_ptr<org::RenderPass> FirstPersonSite(const Api::PassContext& a_context)
		{
			const auto& conditions = a_context.Conditions();
			// Use the build state's target and main resources, since the render thread imports the live ones.
			const auto* view = a_context.Build().ViewOf(a_context.View().get());
			if (!view || !view->main || (!conditions.cpuPipeline && !view->main->pool))
				return nullptr;
			SitePass::Reads reads;
			if (!conditions.cpuPipeline)
				reads = [resources = a_context.View(), main = view->main](org::PassBuilder& a_builder) {
					a_builder.ShaderResource(resources->blocks, std::span<const org::SrvView>{});
					DeclareMainReads(a_builder, *main);
				};
			else
				reads = [main = view->main](org::PassBuilder& a_builder) {
					a_builder.ShaderResource(main->frameConstants, std::span<const org::SrvView>{});
				};
			return MakeSitePass(Site::FirstPerson, MainColours(*view->main), { view->target }, std::move(reads));
		}

		/**
		 * @brief water.mask and water.surface: deferred range a_range's members, which the colour commit culled, in the target set the
		 * trigger chose. Every set's targets are declared once each, then the main depth. The draws read what the GPU path's range pass
		 * reads: the frame record's buffers and textures. Only where the sites do not record inline (guest): there the CS stages are
		 * kept recordings, so per-frame draws need an epoch of their own. Inline (native) the water family's anchor pass draws them
		 * inside the stage (Draws::TakeCpuRange), and the water epochs stay empty in the CPU graph.
		 */
		std::shared_ptr<org::RenderPass> WaterSite(const Api::PassContext& a_context, std::uint32_t a_range)
		{
			const auto& entry = Extensions::DeferredRanges()[a_range];
			if (!entry.taken || !entry.contribution.target || !a_context.main || WaterInStageAnchor())
				return nullptr;
			const auto& main = *a_context.Main();
			SitePass::Textures unique;
			std::vector<std::uint32_t> order;
			std::uint32_t perSet = 0;
			for (std::uint32_t v = 0; v < entry.contribution.variants; ++v) {
				std::uint32_t count = 0;
				for (std::uint32_t i = 0; i < Api::kDeferredRangeTargets; ++i, ++count) {
					auto target = entry.contribution.target(v, i);
					if (!target)
						break;
					auto found = std::ranges::find(unique, target);
					if (found == unique.end())
						found = unique.insert(unique.end(), std::move(target));
					order.push_back(static_cast<std::uint32_t>(found - unique.begin()));
				}
				if (v == 0)
					perSet = count;
				if (!count || count != perSet) {
					logger::error("[DCLF] CPU pipeline: deferred range {}'s target sets are empty or differ in their counts; its water site draws nothing", entry.name);
					return nullptr;
				}
			}
			return std::make_shared<SitePass>(a_range == 0 ? Site::WaterMask : Site::WaterSurface, std::move(unique), SitePass::Textures{ main.nativeDepth },
				[resources = a_context.Main()](org::PassBuilder& a_builder) {
					const std::span<const org::SrvView> noViews{};
					a_builder.ShaderResource(resources->frameConstants, noViews);
					for (const auto& frameBuffer : resources->frameBuffers)
						a_builder.ShaderResource(frameBuffer.copy, noViews);
					(void)Extensions::DeclareGraphTextures(a_builder, Extensions::ColourGraphTextureIds());
				},
				std::move(order));
		}

		std::shared_ptr<org::RenderPass> WaterMaskSite(const Api::PassContext& a_context) { return WaterSite(a_context, 0); }
		std::shared_ptr<org::RenderPass> WaterSurfaceSite(const Api::PassContext& a_context) { return WaterSite(a_context, 1); }
		// Absent while nobody registered the range, so the water epochs stay dormant then, as with the GPU path's range passes.
		// Native mode draws CPU water in the water family's anchor pass, so there the passes do not exist at all: an epoch listing a CPU
		// pipeline pass counts as a site epoch even when it makes none.
		bool WaterMaskPresent() { return Extensions::DeferredRanges()[0].taken && !WaterInStageAnchor(); }
		bool WaterSurfacePresent() { return Extensions::DeferredRanges()[1].taken && !WaterInStageAnchor(); }

		constexpr Api::PassDef kSitePasses[] = {
			{ .key = "core.z.direct", .passName = "cs.dclf.z.direct", .kind = Api::PassKind::Render, .make = &DepthSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "core.direct", .passName = "cs.dclf.direct", .kind = Api::PassKind::Render, .make = &ColourSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "core.forward-direct", .passName = "cs.dclf.forward.direct", .kind = Api::PassKind::Render, .make = &ForwardSite, .cpuPipeline = true, .recordAtExecution = true, .gpuLate = true },
			{ .key = "core.shadow-direct", .passName = "cs.dclf.shadow.direct", .kind = Api::PassKind::Render, .make = &ShadowSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "view.direct", .passName = "{view}.direct", .kind = Api::PassKind::Render, .make = &FirstPersonSite, .cpuPipeline = true, .recordAtExecution = true, .gpuLate = true },
			{ .key = "core.water-mask-direct", .passName = "cs.dclf.water-mask.direct", .kind = Api::PassKind::Render, .make = &WaterMaskSite, .cpuPipeline = true, .recordAtExecution = true, .present = &WaterMaskPresent },
			{ .key = "core.water-surface-direct", .passName = "cs.dclf.water-surface.direct", .kind = Api::PassKind::Render, .make = &WaterSurfaceSite, .cpuPipeline = true, .recordAtExecution = true, .present = &WaterSurfacePresent },
		};
	}

	std::span<const Api::PassDef> SitePassDefs()
	{
		return kSitePasses;
	}
}
