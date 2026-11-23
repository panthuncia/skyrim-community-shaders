// The deferred ranges' draws: what the colour segment culled into a deferred range, drawn bucket by bucket in a later epoch that
// shares main.colour's shape. Each bucket is one plain indexed indirect draw with its own stencil reference.
// A range with several target sets has one pass per set. The epoch's commit stores which set this frame draws, for its run, and
// every other set's pass skips at execution. The passes' lists are kept per skip answer, so nothing records per frame.

// Main/Resources.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"

#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/GraphHost.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	namespace
	{
		constexpr std::uint32_t kTargets = Api::kDeferredRangeTargets;

		/** @brief The target set each range draws in one run of a deferred epoch. Stored by the epoch's commit. */
		struct DeferredChoice
		{
			std::array<std::uint8_t, kDeferredRanges> variant{};
		};

		/** @brief Each deferred epoch run's choice, by run. The passes recorded at execution read it on the host's thread. */
		const ::RenderGraph::RunSlots<DeferredChoice>& DeferredChoices()
		{
			static const ::RenderGraph::RunSlots<DeferredChoice> choices;
			return choices;
		}

		// A run whose choice was not found draws nothing of a range with several sets. Counted for the report, logged once.
		std::atomic<std::uint64_t> deferredRunsWithoutChoice{ 0 };

		struct DeferredBindings
		{
			std::array<org::DeclaredViewToken, kTargets> targets{};
			org::DeclaredViewToken depth;
			org::ResourceBindingToken sequences, classCounts, pool;
		};

		struct DeferredPrepared
		{
			std::shared_ptr<const PassFrame> frame;
			std::array<std::uint32_t, kFramePushWords> framePushWords{};
			std::uint32_t range = 0, variant = 0, targetCount = 0;
			bool chosen = false;  // the range has several target sets, so a run draws only the set its commit chose
			const char* name = nullptr;
		};

		/** @brief One deferred range's draws into the client's targets, testing and writing the main depth and stencil. */
		class DeferredRangePass final : public org::TypedRenderGraphPass<DeferredRangePass, DeferredPrepared, DeferredBindings>
		{
		public:
			DeferredRangePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, RenderGraphRuntime::Segment a_shape, std::uint32_t a_range,
				std::uint32_t a_variant, bool a_chosen, std::vector<FrameAnchors::Texture> a_targets) :
				resources(std::move(a_resources)),
				segment(a_segment), shape(a_shape), range(a_range), variant(a_variant), chosen(a_chosen), targets(std::move(a_targets))
			{}

			DeferredBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				const std::span<const org::SrvView> noViews{};  // Device-address reads need ordering, not a descriptor.
				DeferredBindings bindings{};
				for (std::size_t i = 0; i < targets.size(); ++i)
					bindings.targets[i] = a_builder.RenderTarget(targets[i]).View();
				bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
				bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
				a_builder.ShaderResource(*resources->sequences, noViews);
				bindings.classCounts = a_builder.IndirectArguments(*resources->classCounts).Resource();
				bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
				if (resources->scene->vertexStream)
					a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
				DeclareFrameBuffers(a_builder, *resources);
				a_builder.ShaderResource(resources->frameConstants, noViews);  // the frame record's slots, by address
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(static_cast<std::uint64_t>(segment));
				a_out.push_back(range);
				a_out.push_back(variant);
			}

			DeferredPrepared Prepare(const DeferredBindings&, const org::PassPrepareContext& a_preparation) const
			{
				DeferredPrepared prepared{};
				auto frame = CurrentFrame(a_preparation, shape);
				// Without buckets the pass records nothing, so a range with no members costs only its declarations.
				if (!frame || !frame->indirect.valid || !DeferredRangeDrawn(*frame, range))
					return prepared;
				prepared.framePushWords = MainFramePushWords(resources->frameConstantsAddress, resources->frameConstantsRef, resources->frameBlocks.get());
				prepared.range = range;
				prepared.variant = variant;
				prepared.chosen = chosen;
				prepared.targetCount = static_cast<std::uint32_t>(targets.size());
				prepared.name = Extensions::DeferredRanges()[range].colourRange.c_str();
				prepared.frame = std::move(frame);
				return prepared;
			}

			// The commands depend on the run only through the answer below, so a kept late batch keeps its list while it holds.
			static constexpr bool kRecordKeyedBySkip = true;

			/** @brief Asked only when the pass records at execution, which a range with several target sets does. */
			static bool SkipsAtExecution(const DeferredBindings&, const DeferredPrepared& a_prepared, const org::PassRecordContext& a_recording)
			{
				if (!a_prepared.frame)
					return true;
				if (!a_prepared.chosen)
					return false;
				const auto choice = DeferredChoices().Find(a_recording.Run());
				if (!choice) {
					if (deferredRunsWithoutChoice.fetch_add(1, std::memory_order_relaxed) == 0)
						logger::error("[DCLF] deferred range {}: a run has no target set chosen by its commit; it draws nothing <- DEFERRED TARGETS", a_prepared.name);
					return true;
				}
				return choice->variant[a_prepared.range] != a_prepared.variant;
			}

			static void Record(const DeferredBindings& a_bindings, const DeferredPrepared& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.frame)
					return;
				const auto& frame = *a_prepared.frame;
				auto& commands = a_recording.Commands();
				commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
				auto colours = LoadedColours(a_recording, a_bindings.targets, a_prepared.targetCount);
				auto depth = LoadedDepth(a_recording, a_bindings.depth);
				rhi::PassBeginInfo begin{};
				begin.width = frame.width;
				begin.height = frame.height;
				begin.minDepth = frame.minDepth;
				begin.maxDepth = frame.maxDepth;
				begin.colors = { colours.data(), a_prepared.targetCount };
				begin.depth = &depth;
				begin.debugName = a_prepared.name;
				commands.BeginPass(begin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				// The plain draws' layout and frame push data, as the colour pass's plain buckets have.
				commands.BindLayout(frame.indirect.zLayout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
				const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
				const RenderGraph::BufferRef sequencesRef = RecordingBufferRef(a_recording, a_bindings.sequences);
				const auto counts = a_recording.Resolve(a_bindings.classCounts).GetHandle();
				// The buckets come ordered by stencil reference, so the reference is set once per run of equal ones.
				std::uint32_t reference = ~0u;
				for (const auto& call : frame.deferred[a_prepared.range]) {
					if (call.stencilRef != reference) {
						reference = call.stencilRef;
						commands.SetStencilRef(reference);
					}
					RecordBucketDraw(commands, frame.indirect.zDrawSignature, sequences, sequencesRef, call.first, call.pipeline, counts, call.countWord, call.capacity);
				}
				commands.EndPass();
				// A list's reference starts at 0, and later passes on this list expect that.
				commands.SetStencilRef(0);
			}

		private:
			std::shared_ptr<Resources> resources;
			RenderGraphRuntime::Segment segment;
			RenderGraphRuntime::Segment shape;
			std::uint32_t range = 0;
			std::uint32_t variant = 0;
			bool chosen = false;
			std::vector<FrameAnchors::Texture> targets;
		};

		/** @brief Target set a_variant of a range, up to its first null target. */
		std::vector<FrameAnchors::Texture> DeferredTargetsOf(const Extensions::DeferredRangeEntry& a_entry, std::uint32_t a_variant)
		{
			std::vector<FrameAnchors::Texture> targets;
			for (std::uint32_t i = 0; i < kTargets; ++i) {
				auto target = a_entry.contribution.target(a_variant, i);
				if (!target)
					break;
				targets.push_back(std::move(target));
			}
			return targets;
		}

		/** @brief Whether every target set has set 0's count. The pipelines check the formats on the render thread. */
		bool DeferredSetCountsMatch(const std::vector<std::vector<FrameAnchors::Texture>>& a_sets)
		{
			return std::ranges::all_of(a_sets, [&](const auto& a_set) { return a_set.size() == a_sets.front().size(); });
		}

		/**
		 * @brief Range a_range's passes, one per target set. None while nobody registered it or the main resources lack what it draws
		 * with. Several sets need the epoch to record at execution, since a frame's set is chosen at its commit.
		 */
		void EmitDeferredRange(const Api::PassContext& a_context, Api::PassOut& a_out, std::uint32_t a_range)
		{
			const auto& entry = Extensions::DeferredRanges()[a_range];
			if (!entry.taken || !a_context.main)
				return;
			const auto& resources = a_context.Main();
			if (!resources || !resources->pool || !resources->classCounts || !resources->sequences)
				return;
			// It draws what the colour commit culled, so its epoch records against the colour shape.
			const auto shape = a_context.ShapeSegment();
			if (shape != RenderGraphRuntime::Segment::MainOpaque) {
				logger::error("[DCLF] deferred range {} is scheduled in epoch {}, which does not share main.colour's shape; it is not drawn", entry.name, a_context.epochId);
				return;
			}
			const std::uint32_t variants = entry.contribution.variants;
			const bool chosen = variants > 1;
			if (chosen && !RenderGraphRuntime::Get().RecordsAtExecution(a_context.epoch)) {
				logger::error("[DCLF] deferred range {} chooses its targets by frame, but epoch {} does not record at execution; it is not drawn", entry.name, a_context.epochId);
				return;
			}
			std::vector<std::vector<FrameAnchors::Texture>> sets;
			for (std::uint32_t v = 0; v < variants; ++v)
				sets.push_back(DeferredTargetsOf(entry, v));
			if (sets.front().empty() || !DeferredSetCountsMatch(sets)) {
				logger::error("[DCLF] deferred range {}'s target sets are empty or differ in their counts; it is not drawn", entry.name);
				return;
			}
			for (std::uint32_t v = 0; v < variants; ++v)
				a_out.Add(chosen ? fmt::format("cs.dclf.deferred-range{}.set{}", a_range, v) : fmt::format("cs.dclf.deferred-range{}", a_range), Api::PassKind::Render,
					std::make_shared<DeferredRangePass>(resources, a_context.segment, shape, a_range, v, chosen, std::move(sets[v])), Api::PassQueue::Default, chosen);
		}

		void EmitDeferredRange0(const Api::PassContext& a_context, Api::PassOut& a_out) { EmitDeferredRange(a_context, a_out, 0); }
		void EmitDeferredRange1(const Api::PassContext& a_context, Api::PassOut& a_out) { EmitDeferredRange(a_context, a_out, 1); }
		bool DeferredRange0Present() { return Extensions::DeferredRanges()[0].taken; }
		bool DeferredRange1Present() { return Extensions::DeferredRanges()[1].taken; }

		constexpr Api::PassDef kDeferredRangePasses[] = {
			{ .key = "core.deferred-range0", .passName = "cs.dclf.deferred-range0", .kind = Api::PassKind::Render, .emit = &EmitDeferredRange0, .present = &DeferredRange0Present },
			{ .key = "core.deferred-range1", .passName = "cs.dclf.deferred-range1", .kind = Api::PassKind::Render, .emit = &EmitDeferredRange1, .present = &DeferredRange1Present },
		};
		static_assert(std::size(kDeferredRangePasses) == kDeferredRanges, "a pass per deferred range slot");

		bool NoDeferredBuckets(const PassFrame& a_shape)
		{
			for (std::uint32_t r = 0; r < kDeferredRanges; ++r)
				if (DeferredRangeDrawn(a_shape, r))
					return false;
			return true;
		}
	}

	std::span<const Api::PassDef> DeferredRangePassDefs() { return kDeferredRangePasses; }

	bool SubmitDeferredEpoch(std::uint32_t a_epoch)
	{
		// The CPU-driven graph makes no deferred range pass, so its epochs have nothing to run.
		if (a_epoch == kNoEpoch || EpochTraitsOf(a_epoch).dormant || AttachedGraphConditions().cpuPipeline)
			return false;
		// This frame's target set per range, read now inside the trigger. The commit stores it for the execution's run.
		DeferredChoice choice;
		for (std::uint32_t r = 0; r < kDeferredRanges; ++r) {
			const auto& entry = Extensions::DeferredRanges()[r];
			if (entry.taken && entry.contribution.variant)
				choice.variant[r] = static_cast<std::uint8_t>(std::min(entry.contribution.variant(), entry.contribution.variants - 1));
		}
		const auto store = [](void* a_choice) {
			if (auto* host = RenderGraphRuntime::Get().Host())
				DeferredChoices().Store(host->CurrentRun(), std::make_shared<const DeferredChoice>(*static_cast<const DeferredChoice*>(a_choice)));
		};
		return IndirectDraws::Get().SubmitSharedEpoch(a_epoch, { .context = &choice, .empty = &NoDeferredBuckets, .commit = store }).submitted;
	}

	std::uint64_t TakeDeferredRunsWithoutChoice()
	{
		return deferredRunsWithoutChoice.exchange(0, std::memory_order_relaxed);
	}
}
