// The deferred ranges' draws: what the colour segment culled into a deferred range, drawn bucket by bucket in a later epoch that
// shares main.colour's shape. Each bucket is one plain indexed indirect draw with its own stencil reference.

// Main/Resources.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <array>
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
			std::uint32_t range = 0, targetCount = 0;
			const char* name = nullptr;
		};

		/** @brief One deferred range's draws into the client's targets, testing and writing the main depth and stencil. */
		class DeferredRangePass final : public org::TypedRenderGraphPass<DeferredRangePass, DeferredPrepared, DeferredBindings>
		{
		public:
			DeferredRangePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, RenderGraphRuntime::Segment a_shape, std::uint32_t a_range,
				std::vector<FrameAnchors::Texture> a_targets) :
				resources(std::move(a_resources)),
				segment(a_segment), shape(a_shape), range(a_range), targets(std::move(a_targets))
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
			}

			DeferredPrepared Prepare(const DeferredBindings&, const org::PassPrepareContext& a_preparation) const
			{
				DeferredPrepared prepared{};
				auto frame = CurrentFrame(a_preparation, shape);
				// Without buckets the pass records nothing, so a range with no members costs only its declarations.
				if (!frame || !frame->indirect.valid || !DeferredRangeDrawn(*frame, range))
					return prepared;
				prepared.framePushWords = MainFramePushWords(resources->frameConstantsAddress, resources->frameConstantsRef);
				prepared.range = range;
				prepared.targetCount = static_cast<std::uint32_t>(targets.size());
				prepared.name = Extensions::DeferredRanges()[range].colourRange.c_str();
				prepared.frame = std::move(frame);
				return prepared;
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
			std::vector<FrameAnchors::Texture> targets;
		};

		/** @brief The pass of range a_range, or null while nobody registered it or the main resources lack what it draws with. */
		std::shared_ptr<org::RenderPass> MakeDeferredRange(const Api::PassContext& a_context, std::uint32_t a_range)
		{
			const auto& entry = Extensions::DeferredRanges()[a_range];
			if (!entry.taken || !a_context.main)
				return nullptr;
			const auto& resources = a_context.Main();
			if (!resources || !resources->pool || !resources->classCounts || !resources->sequences)
				return nullptr;
			// It draws what the colour commit culled, so its epoch records against the colour shape.
			const auto shape = a_context.ShapeSegment();
			if (shape != RenderGraphRuntime::Segment::MainOpaque) {
				logger::error("[DCLF] deferred range {} is scheduled in epoch {}, which does not share main.colour's shape; it is not drawn", entry.name, a_context.epochId);
				return nullptr;
			}
			std::vector<FrameAnchors::Texture> targets;
			for (std::uint32_t i = 0; i < kTargets; ++i) {
				auto target = entry.contribution.target(i);
				if (!target)
					break;
				targets.push_back(std::move(target));
			}
			return std::make_shared<DeferredRangePass>(resources, a_context.segment, shape, a_range, std::move(targets));
		}

		std::shared_ptr<org::RenderPass> DeferredRange0(const Api::PassContext& a_context) { return MakeDeferredRange(a_context, 0); }
		std::shared_ptr<org::RenderPass> DeferredRange1(const Api::PassContext& a_context) { return MakeDeferredRange(a_context, 1); }

		constexpr Api::PassDef kDeferredRangePasses[] = {
			{ .key = "core.deferred-range0", .passName = "cs.dclf.deferred-range0", .kind = Api::PassKind::Render, .make = &DeferredRange0 },
			{ .key = "core.deferred-range1", .passName = "cs.dclf.deferred-range1", .kind = Api::PassKind::Render, .make = &DeferredRange1 },
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
		if (a_epoch == kNoEpoch)
			return false;
		return IndirectDraws::Get().SubmitSharedEpoch(a_epoch, { .empty = &NoDeferredBuckets }).submitted;
	}
}
