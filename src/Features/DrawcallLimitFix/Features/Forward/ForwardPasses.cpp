// The forward epoch's passes: blended draws and refraction normals. Both record against the colour epoch's shape.
// The back-to-front sort is Core's (core.forward-sort).

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/GraphHost.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace DCLF::Draws
{

	// Draws the sorted forward draws at the engine's blended draw (FUN_1414b39d0), after the deferred composite.
	// Forward pipelines write only the main target.
	class ForwardDrawPass final : public org::TypedRenderGraphPass<ForwardDrawPass, PreparedDraws, PassBindings>
	{
	public:
		ForwardDrawPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_shapeSegment) :
			resources(std::move(a_resources)), shapeSegment(a_shapeSegment) {}

		PassBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			PassBindings bindings{};
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				bindings.targets[i] = a_builder.RenderTarget(resources->native[i]).View();
			bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			bindings.classCounts = a_builder.IndirectArguments(*resources->classCounts).Resource();
			if (resources->pool) {
				a_builder.ShaderResource(*resources->sequences, noViews);
				bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
			}
			if (resources->scene->vertexStream)
				a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
			DeclareFrameBuffers(a_builder, *resources);
			a_builder.ShaderResource(resources->frameConstants, noViews);
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>&) const {}

		PreparedDraws Prepare(const PassBindings&, const org::PassPrepareContext& a_preparation) const
		{
			PreparedDraws prepared{};
			auto frame = CurrentFrame(a_preparation, shapeSegment);
			if (!frame || !frame->forwardCapacity || !frame->indirect.valid || !resources->forwardSort || !resources->preprocessForward)
				return prepared;
			prepared.preprocess = resources->preprocessForward.get();
			prepared.framePushWords = MainFramePushWords(resources->frameConstantsAddress, resources->frameConstantsRef);
			prepared.frame = std::move(frame);
			prepared.targetCount = resources->targetCount;
			return prepared;
		}

		static void Record(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame || !a_prepared.preprocess)
				return;
			const auto& frame = *a_prepared.frame;
			// Load every attachment, since the draws blend onto the composited frame.
			auto colors = LoadedColours(a_recording, a_bindings.targets, a_prepared.targetCount);
			auto depth = LoadedDepth(a_recording, a_bindings.depth);
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;
			begin.depth = &depth;
			begin.colors = { colors.data(), a_prepared.targetCount };
			begin.debugName = "DCLF forward";
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto classCounts = a_recording.Resolve(a_bindings.classCounts).GetHandle();
			// CS_DCLF_FORWARD_PLAIN. Plain draws use the Z-prepass layout, and each draw finds its sequence by its instance.
			if (frame.classPlan && !frame.classPlan->forward.empty() && a_bindings.pool.globalResourceID != 0) {
				const auto& plan = *frame.classPlan;
				auto& commands = a_recording.Commands();
				static std::atomic<std::size_t> loggedBuckets{ 0 };
				if (loggedBuckets.exchange(plan.forward.size()) != plan.forward.size())
					logger::info("[DCLF] forward epoch recorded with {} plain draws (probe)", plan.forward.size());
				commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
				commands.BeginPass(begin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame.indirect.zLayout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
				const RenderGraph::BufferRef sequencesRef = DCLF::Draws::RecordingBufferRef(a_recording, a_bindings.sequences);
				for (std::size_t b = 0; b < plan.forward.size(); ++b) {
					const auto& bucket = plan.forward[b];
					RecordBucketDraw(commands, frame.indirect.zDrawSignature, sequences, sequencesRef, std::uint64_t(bucket.first) + bucket.capacity,
						frame.indirect.plainColour[bucket.set], classCounts, plan.ForwardCountWord(b), bucket.capacity);
				}
				commands.EndPass();
				return;
			}
			// One call per state class, in creation order. The sorted half follows the staging half.
			static const auto kForwardClassRanges = [] {
				std::array<std::string, 128> names;
				for (std::size_t c = 0; c < 64; ++c) {
					names[c] = fmt::format("cs.dclf.forward.class{}", c);
					names[64 + c] = fmt::format("cs.dclf.forward.pre{}", c);
				}
				return names;
			}();
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged.fetch_add(1) % 64 == 0) {
				std::string text;
				for (std::uint32_t c = 0; c < frame.classes.size(); ++c)
					if (const auto& range = frame.classes[c].ranges[kClassRangeForward]; range.capacity)
						text += fmt::format(" class {} capacity {} pipelines {};", c, range.capacity, c < frame.indirect.classes.size() ? frame.indirect.classes[c].pipelines : 0u);
				logger::info("[DCLF] forward calls (probe):{}", text);
			}
			std::vector<IndirectCall> calls;
			for (std::uint32_t c = 0; c < frame.classes.size() && c < frame.indirect.classes.size(); ++c)
				if (const auto& range = frame.classes[c].ranges[kClassRangeForward]; range.capacity)
					calls.push_back({ frame.indirect.classes[c].signatures[kColorVariant], (std::uint64_t(range.first) + range.capacity) * sizeof(DrawSequence), classCounts,
						std::uint64_t(ClassCountWord(c, kClassRangeForward)) * sizeof(std::uint32_t), range.capacity,
						c < 64 ? kForwardClassRanges[c].c_str() : nullptr, c < 64 ? kForwardClassRanges[64 + c].c_str() : nullptr });
			RecordPreprocessedDraws(a_recording, *a_prepared.preprocess, begin, frame.resourceHeap, frame.samplerHeap, frame.indirect.layout, a_prepared.framePushWords, sequences,
				calls, "cs.dclf.forward.preprocess");
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment shapeSegment;  // The colour epoch's.
	};

	// Draws refraction into kREFRACTION_NORMALS, after clearing it as the engine does before its group 5.
	// The blended-draw hook then marks the normals begun, so the engine's later draws add to DCLF's instead of clearing.
	struct RefractionBindings
	{
		org::DeclaredViewToken target, depth;
		org::ResourceBindingToken sequences, count, pool;
	};

	struct RefractionPrepared
	{
		std::shared_ptr<const PassFrame> frame;
		std::uint64_t frameConstants = 0;
		RenderGraph::BufferRef frameConstantsRef;
		// D3D12 only. The push words name these views.
		std::shared_ptr<const FrameBlockViews> frameBlocks;
	};

	class RefractionDrawPass final : public org::TypedRenderGraphPass<RefractionDrawPass, RefractionPrepared, RefractionBindings>
	{
	public:
		RefractionDrawPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_shapeSegment) :
			resources(std::move(a_resources)), shapeSegment(a_shapeSegment) {}

		RefractionBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			RefractionBindings bindings{};
			bindings.target = a_builder.RenderTarget(resources->refractionNormals).View();
			bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
			// The sequences are indirect arguments and are also read by address.
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			a_builder.ShaderResource(*resources->sequences, noViews);
			bindings.count = a_builder.IndirectArguments(resources->count);
			bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
			if (resources->scene->vertexStream)
				a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
			DeclareFrameBuffers(a_builder, *resources, false);
			a_builder.ShaderResource(resources->frameConstants, noViews);
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>&) const {}

		RefractionPrepared Prepare(const RefractionBindings&, const org::PassPrepareContext& a_preparation) const
		{
			RefractionPrepared prepared{};
			auto frame = CurrentFrame(a_preparation, shapeSegment);
			if (!frame || !frame->refractionCapacity || !frame->indirect.valid || !frame->indirect.refraction.valid() || !frame->indirect.refractionDrawSignature.valid() ||
				frame->indirect.refractionFormat != resources->refractionFormat || !resources->pool)
				return prepared;
			prepared.frame = std::move(frame);
			prepared.frameConstants = resources->frameConstantsAddress;
			prepared.frameConstantsRef = resources->frameConstantsRef;
			prepared.frameBlocks = resources->frameBlocks;
			return prepared;
		}

		static void Record(const RefractionBindings& a_bindings, const RefractionPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame || !a_prepared.frameConstants)
				return;
			// D3D12's layout holds only root constants, so the stages read their blocks through the push words.
			const bool d3d12 = ::RenderGraph::Backend::IsD3D12();
			if (d3d12 && (!a_prepared.frameConstantsRef || !a_prepared.frameBlocks)) {
				static std::atomic<bool> warned{ false };
				if (!warned.exchange(true))
					logger::error("[DCLF] D3D12 refraction: no buffer view ref or block views of the frame constants; the refraction draws nothing");
				return;
			}
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			rhi::ColorAttachment colour{};
			colour.rtv = a_recording.View(a_bindings.target);
			colour.loadOp = rhi::LoadOp::Clear;
			colour.storeOp = rhi::StoreOp::Store;
			colour.clear.type = rhi::ClearValueType::Color;
			colour.clear.rgba[0] = colour.clear.rgba[1] = 0.5f;  // FUN_1414b4860's clear. 0.5 means no offset.
			colour.clear.rgba[2] = colour.clear.rgba[3] = 0.0f;
			colour.resource = a_recording.Resolve(a_bindings.target.Resource()).GetHandle();
			auto depth = LoadedDepth(a_recording, a_bindings.depth);
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;
			begin.depth = &depth;
			begin.colors = { &colour, 1 };
			begin.debugName = "DCLF refraction normals";
			auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
				a_words[0] = static_cast<std::uint32_t>(a_value);
				a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
			};
			// Uses the shadow views' layout. The zeroed shared light block stands in as the zero block.
			const std::uint64_t constants = a_prepared.frameConstants;
			std::uint32_t pass[kShadowPushWords]{};
			MainShadowLayoutPush(constants, a_prepared.frameConstantsRef, pass);
			if (d3d12)
				MainShadowLayoutViews(a_prepared.frameConstantsRef, *a_prepared.frameBlocks, pass);
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
			const std::uint64_t first = RefractionSlot(frame) * sizeof(DrawSequence);
			// The refraction stage reads LodFadeFrame from the material rows' words. Vulkan gets a device address, D3D12 a buffer ref.
			std::uint32_t words[kDrawPushWords]{};
			const RenderGraph::BufferRef sequence = DCLF::Draws::RecordingBufferRef(a_recording, a_bindings.sequences) + first;
			words[kShadowDrawPushSequences] = sequence.viewSlot;
			words[kShadowDrawPushSequences + 1] = sequence.byteOffset;
			if (d3d12) {
				const RenderGraph::BufferRef lodFadeFrame = a_prepared.frameConstantsRef + LodFadeFrameAddress(0);
				words[kShadowDrawPushMaterialRows] = lodFadeFrame.viewSlot;
				words[kShadowDrawPushMaterialRows + 1] = lodFadeFrame.byteOffset;
			} else {
				split(words + kShadowDrawPushMaterialRows, LodFadeFrameAddress(constants));
			}
			commands.BeginPass(begin);
			commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			commands.BindLayout(frame.indirect.refractionLayout);
			commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
			commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kShadowPushWords, pass);
			commands.BindPipeline(frame.indirect.refraction);
			commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
			// D3D12's signature also reads the instance word just before the arguments.
			commands.ExecuteIndirect(frame.indirect.refractionDrawSignature, sequences, first + PlainDrawArgumentsOffset(), count,
				std::uint64_t(kCountRefractionWord) * sizeof(std::uint32_t), frame.refractionCapacity);
			commands.EndPass();
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment shapeSegment;
	};
}

namespace DCLF::Features::Forward
{
	namespace
	{
		std::shared_ptr<org::RenderPass> ForwardDraw(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->forwardSort && !Draws::CurrentGraphConditions().forwardDirect ? std::make_shared<Draws::ForwardDrawPass>(resources, a_context.ShapeSegment()) : nullptr;
		}

		std::shared_ptr<org::RenderPass> RefractionDraw(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return !Draws::CurrentGraphConditions().forwardDirect && resources->refractionNormals && resources->nativeDepth && resources->pool ?
			           std::make_shared<Draws::RefractionDrawPass>(resources, a_context.ShapeSegment()) :
			           nullptr;
		}
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		a_context.RegisterPass({ .key = "forward.draw", .passName = "cs.dclf.forward", .kind = Api::PassKind::Render, .make = &ForwardDraw });
		a_context.RegisterPass({ .key = "forward.refraction", .passName = "cs.dclf.refraction", .kind = Api::PassKind::Render, .make = &RefractionDraw });
	}
}
