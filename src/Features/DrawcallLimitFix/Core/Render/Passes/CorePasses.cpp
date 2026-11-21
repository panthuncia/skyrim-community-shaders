// Core's passes, which the schedule names by key: the main view's culling and copies (core.*), the diagnostics' anchors (diag.*)
// and the views' passes (view.*). Each make is both its pass's condition and its factory. Features register their own passes.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"

#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/IndexPoolPass.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF::Draws
{
	namespace
	{
		/** @brief diag.*: the observers' passes at Point, appended in the epoch's place. */
		template <Diagnostics::MainGraphPasses::Point Point>
		void EmitDiagnostics(const Api::PassContext& a_context, Api::PassOut& a_out)
		{
			Diagnostics::Notify(Diagnostics::MainGraphPasses{ Point, a_context.Main(), a_context.epoch, a_out.Descs() });
		}

		// Each epoch's first pass copies the commit's latched values. It publishes its declared targets so a commit latches only
		// what the pass will copy. A host epoch also copies its hosted views' lists, since those have no copies of their own.
		std::shared_ptr<org::RenderPass> LatchedCopies(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			std::vector<std::shared_ptr<ViewResources>> hosted(a_context.hosted.begin(), a_context.hosted.end());
			std::vector<ViewKind> views;
			for (const auto& view : hosted)
				views.push_back(view->traits->kind);
			return MakeLatchedCopiesPass(a_context.Conditions(),
				[resources = resources, hosted = std::move(hosted)] {
					auto targets = MainLatchedTargets(*resources);
					for (const auto& view : hosted) {
						const auto viewTargets = ViewLatchedTargets(*view, *resources->scene);
						targets.insert(targets.end(), viewTargets.begin(), viewTargets.end());
					}
					return targets;
				},
				[segment = a_context.segment, views = std::move(views)](const org::PassPrepareContext& a_preparation) {
					const auto frame = CurrentFrame(a_preparation, segment);
					if (!frame)
						return LatchedSource{};
					LatchedSource source{ frame, std::span(&frame->latched, 1) };
					if (const auto* shapes = views.empty() ? nullptr : RevisionShapesOf(a_preparation))
						for (const auto view : views) {
							auto& out = source.groups.emplace_back();
							if (const auto& viewFrame = shapes->views[ViewIndex(view)])
								out = { viewFrame, viewFrame->latched };
						}
					return source;
				},
				&resources->latchedTargets);
		}

		std::shared_ptr<org::RenderPass> BuildDraws(const Api::PassContext& a_context)
		{
			return MakeBuildDrawsPass(a_context.Main(), a_context.Conditions(), a_context.segment);
		}

		std::shared_ptr<org::RenderPass> MemberQueries(const Api::PassContext& a_context)
		{
			return MakeBuildDrawsPass(a_context.Main(), a_context.Conditions(), a_context.segment, 1, true);
		}

		// The pool copies the depth commit gave out, before the draws read them. Records nothing in the revision's depth
		// recording without the index pool.
		std::shared_ptr<org::RenderPass> IndexPoolCopies(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(resources->pool, [main = resources](const org::PassPrepareContext& a_preparation) {
				if (const auto* shapes = RevisionShapesOf(a_preparation); shapes && shapes->depthWithoutIndexPool)
					return IndexPoolPass::LatchOf{};
				const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
				return IndexPoolPass::LatchOf{ frame ? frame->latch : nullptr, frame ? frame->latchLayout.PoolOffset() : 0u };
			}));
		}

			// Phase 1 culls against last frame's HZB, which misses what just came out from behind an occluder. After the HZB is
			// rebuilt, phase 2 retests phase 1's rejects and draws them. Only while the HZB exists.
		std::shared_ptr<org::RenderPass> BuildDrawsPhaseTwo(const Api::PassContext& a_context)
		{
			return a_context.PhaseTwo() ? MakeBuildDrawsPass(a_context.Main(), a_context.Conditions(), a_context.segment, 2) : nullptr;
		}

		// Visibility changes once both phases' verdicts are final.
		bool VisibilityEventsReady(const Resources& a_resources)
		{
			const auto& sceneBuffers = *a_resources.scene;
			return sceneBuffers.visibilityEventsProgram && sceneBuffers.visibilityReadback && Extensions::CullBindingBuffer(Api::CullBindingSlot::RootRows, sceneBuffers) &&
			       a_resources.frustum;
		}

		std::shared_ptr<org::RenderPass> VisibilityEvents(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return VisibilityEventsReady(*resources) ? MakeVisibilityEventsPass(resources) : nullptr;
		}

		std::shared_ptr<org::RenderPass> VisibilityEventsCopy(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return VisibilityEventsReady(*resources) ? MakeEventReadbackPass(resources->scene, resources->scene->visibilityEvents, resources->scene->visibilityReadback->list) : nullptr;
		}

		// The scan runs every execution so the counts it clears stay zero even without a build. The scatter runs only after a build.
		std::shared_ptr<org::RenderPass> SortScan(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			// Not while the colour shape draws main plain buckets. BuildDraws raises no count then, and the last scan left them zero.
			const auto segment = a_context.segment;
			return resources->sort ? resources->sort->ScanPass([segment](const org::PassPrepareContext& a_preparation) {
				const auto shape = CurrentFrame(a_preparation, segment);
				return !(shape && shape->classPlan && !shape->classPlan->main.empty());
			}) :
			                         nullptr;
		}

		std::shared_ptr<org::RenderPass> SortScatter(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->sort ? MakeSortSequencesPass(resources, a_context.segment) : nullptr;
		}

		// The forward sort after the colour build. The forward epoch draws it.
		std::shared_ptr<org::RenderPass> ForwardSort(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			// The CPU sorts the forward draws, so only the decal buckets are left for this pass.
			return MakeForwardSortPass(resources->sequences, { nullptr, resources->classCounts }, [main = resources](const org::PassPrepareContext& a_preparation) {
				const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
				if (!frame || !BuildsDraws(a_preparation, *main, RenderGraphRuntime::Segment::MainOpaque))
					return ForwardSortSource{};
				ForwardSortSource source{ main->forwardSort };
				constexpr auto kWord = static_cast<std::uint32_t>(sizeof(std::uint32_t));
				// The decal buckets' pairs, keyed by engine ordinal and ranked into firstInstance for the plain draw.
				if (frame->classPlan)
					for (std::size_t b = 0; b < frame->classPlan->decals.size(); ++b) {
						const auto& bucket = frame->classPlan->decals[b];
						source.ranges.push_back({ bucket.first, bucket.capacity, frame->classPlan->DecalCountWord(b) * kWord, ForwardSortConstants::kRankInstance });
					}
				return source;
			});
		}

		using DiagPoint = Diagnostics::MainGraphPasses::Point;

		constexpr Api::PassDef kCorePasses[] = {
			// The main view's culling and copies.
			{ .key = "core.z.latched-copies", .passName = "cs.dclf.z.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies, .cpuPipeline = true },
			{ .key = "core.latched-copies", .passName = "cs.dclf.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies, .cpuPipeline = true },
			{ .key = "core.z.portal-queries", .passName = "cs.dclf.z.member-portal-queries", .make = &MemberQueries },
			{ .key = "core.z.build-draws", .passName = "cs.dclf.z.build-draws", .make = &BuildDraws },
			{ .key = "core.build-draws", .passName = "cs.dclf.build-draws", .make = &BuildDraws },
			{ .key = "core.z.index-pool", .passName = "cs.dclf.z.index-pool", .make = &IndexPoolCopies, .cpuPipeline = true },
			{ .key = "core.build-draws-phase2", .passName = "cs.dclf.build-draws-phase2", .make = &BuildDrawsPhaseTwo },
			{ .key = "core.z.visibility-events", .passName = "cs.dclf.z.visibility-events", .make = &VisibilityEvents },
			{ .key = "core.z.visibility-readback", .passName = "cs.dclf.z.visibility-readback", .kind = PassKind::Copy, .make = &VisibilityEventsCopy },
			{ .key = "core.sort-scan", .passName = "cs.dclf.sort-scan", .make = &SortScan },
			{ .key = "core.sort-scatter", .passName = "cs.dclf.sort-scatter", .make = &SortScatter },
			{ .key = "core.forward-sort", .passName = "cs.dclf.forward-sort", .make = &ForwardSort },
			// The diagnostics' anchors.
			{ .key = "diag.before-colour", .emit = &EmitDiagnostics<DiagPoint::kBeforeColour> },
			{ .key = "diag.after-colour", .emit = &EmitDiagnostics<DiagPoint::kAfterColour> },
			{ .key = "diag.after-depth", .emit = &EmitDiagnostics<DiagPoint::kAfterDepth> },
		};
	}

	void RegisterCorePasses()
	{
		for (const auto& pass : kCorePasses)
			RegisterPassDef(pass, "Core");
		for (const auto& pass : ViewPassDefs())
			RegisterPassDef(pass, "Core");
		for (const auto& pass : Direct::SitePassDefs())
			RegisterPassDef(pass, "Core");
	}
}
