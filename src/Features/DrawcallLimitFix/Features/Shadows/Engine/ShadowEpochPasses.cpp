// The shadow epoch's passes: the views' latched copies, the index pool copies, one culling dispatch for every view, the views'
// draws and the cull counter readback. The occlusion maps' views ride along with the shadow views.

// Main/Resources.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowFrame.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include "Features/RemoteControl/GpuCounters.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/PreparedTablePublisher.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace DCLF::Draws
{

	struct ShadowCasterRowsBindings
	{
		org::DeclaredViewToken rows, firsts, used;
	};

	struct ShadowCasterRowsPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		rhi::CommandSignatureHandle signature{};
		std::uint32_t listOffset = 0, dispatchOffset = 0;  // in the frame slot's region (ShadowLatchLayout)
		std::uint32_t rowCapacity = 0;                     // the rows the revision's version of the caster rows holds
	};

	/** @brief Hands out the frame's caster rows, which the culling writes and the views' draws read. */
	class ShadowCasterRowsPass final : public org::TypedRenderGraphPass<ShadowCasterRowsPass, ShadowCasterRowsPrepared, ShadowCasterRowsBindings>
	{
	public:
		explicit ShadowCasterRowsPass(std::shared_ptr<ShadowResources> a_resources) :
			resources(std::move(a_resources)) {}

		ShadowCasterRowsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ShadowCasterRowsBindings bindings{};
			bindings.rows = a_builder.UnorderedAccess(*resources->casterRows).View();
			bindings.firsts = a_builder.UnorderedAccess(*resources->casterFirsts).View();
			bindings.used = a_builder.UnorderedAccess(resources->casterRowsUsed).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>&) const {}

		ShadowCasterRowsPrepared Prepare(const ShadowCasterRowsBindings&, const org::PassPrepareContext& a_preparation) const
		{
			ShadowCasterRowsPrepared prepared{};
			const auto frame = CurrentShadowFrame(a_preparation);
			if (!frame || frame->views.empty() || !resources->assignCasterRows || !frame->latch || !resources->rowsDispatchSignature)
				return prepared;
			prepared.program = resources->assignCasterRows;
			prepared.latch = frame->latch;
			prepared.signature = resources->rowsDispatchSignature->GetHandle();
			prepared.listOffset = frame->latchLayout.RowsInputsOffset();
			prepared.dispatchOffset = frame->latchLayout.RowsDispatchOffset();
			prepared.rowCapacity = static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, resources->casterRows));
			return prepared;
		}

		static void Record(const ShadowCasterRowsBindings& a_bindings, const ShadowCasterRowsPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.program || !a_prepared.latch)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_prepared.program->layout->GetHandle());
			commands.BindPipeline(a_prepared.program->Pipeline(a_recording));
			const auto at = a_prepared.latch->At(a_recording.Run());
			const std::uint64_t region = at.offset;
			BuildDrawsConstants constants{};
			constants.latchIndex = at.srvIndex;
			constants.latchOffset = static_cast<std::uint32_t>(region + a_prepared.listOffset);
			constants.sequencesIndex = a_recording.Index(a_bindings.firsts);
			constants.countIndex = a_recording.Index(a_bindings.used);
			constants.visibilityIndex = a_recording.Index(a_bindings.rows);
			constants.phaseTwoBase = a_prepared.rowCapacity;
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.ExecuteIndirect(a_prepared.signature, at.buffer->GetAPIResource().GetHandle(), region + a_prepared.dispatchOffset, {}, 0, 1);
		}

	private:
		std::shared_ptr<ShadowResources> resources;
	};

	struct ShadowBuildBindings
	{
		std::vector<org::DeclaredViewToken> drawArgs, count, bucketCounts;  // per view slot
		org::DeclaredViewToken casterRows, casterFirsts;
		org::DeclaredViewToken visibility, poolFirsts;
		org::DeclaredViewToken fadeRoots;
		org::DeclaredViewToken objectGates;  // the cull binding ObjectGates: a portal-strict light's casters' gates
	};

	struct ShadowBuildPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		rhi::CommandSignatureHandle signature{};
		// latchOffset is set at record time from the frame slot's region.
		BuildDrawsConstants constants{};
		std::uint32_t dispatchOffset = 0;  // the epoch's groups in the frame slot's region (ShadowLatchLayout::BuildDispatchOffset)
		std::uint32_t views = 0;           // the table's rows; 0: nothing to cull
		bool fadeRoots = false;            // the fade roots' rows are bound (their view's index)
	};

	/**
	 * @brief The shadow views' culling: BuildDrawsCS frustum-only, every view of the epoch in one dispatch.
	 * x runs over the shadow list's inputs, y over the views' table, and each thread writes its view's slot.
	 */
	class ShadowBuildDrawsPass final : public org::TypedRenderGraphPass<ShadowBuildDrawsPass, ShadowBuildPrepared, ShadowBuildBindings>
	{
	public:
		ShadowBuildDrawsPass(std::shared_ptr<ShadowResources> a_resources, std::shared_ptr<const ShadowSlotBuffers> a_slots) :
			resources(std::move(a_resources)), slots(std::move(a_slots)), viewTable("cs.dclf.shadow.build-views") {}

		ShadowBuildBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ShadowBuildBindings bindings{};
			Extensions::DeclareCullBindingPublications(a_builder, *resources->scene, { Api::CullBindingSlot::LightContexts, Api::CullBindingSlot::StrictGates });
			bindings.casterRows = a_builder.UnorderedAccess(*resources->casterRows).View();
			bindings.casterFirsts = a_builder.ShaderResource(*resources->casterFirsts).View();
			// The live vectors grow on the render thread, so use the build state's copy.
			for (std::size_t s = 0; s < slots->drawArgs.size(); ++s) {
				bindings.drawArgs.push_back(slots->OwnsDrawArgs(s) ? a_builder.UnorderedAccess(*slots->drawArgs[s]).View() : bindings.drawArgs[slots->drawArgsOwner[s]]);
				bindings.count.push_back(a_builder.UnorderedAccess(slots->count[s]).View());
				bindings.bucketCounts.push_back(a_builder.UnorderedAccess(*slots->bucketCounts[s]).View());
			}
			bindings.visibility = a_builder.UnorderedAccess(*resources->visibility).View();
			bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			// Casters under stood-in roots follow FadeStateCS's state, occluders their roots' OnVisible. The states are last frame's.
			if (const auto& roots = Extensions::CullBindingBuffer(Api::CullBindingSlot::RootRows, *resources->scene))
				bindings.fadeRoots = a_builder.ShaderResource(*roots).View();
			if (const auto& gates = Extensions::CullBindingBuffer(Api::CullBindingSlot::ObjectGates, *resources->scene))
				bindings.objectGates = a_builder.ShaderResource(*gates).View();
			FrameValues::Get().DeclareReads(a_builder, FrameValues::kReadPlacements | FrameValues::kReadPools);
			Draws::DeclarePayloadReads(a_builder);
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(FadeRows() ? 1u : 0u); }

		ShadowBuildPrepared Prepare(const ShadowBuildBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			ShadowBuildPrepared prepared{};
			const auto frame = CurrentShadowFrame(a_preparation);
			if (!frame || frame->views.empty() || !resources->buildDraws || !frame->latch || !resources->dispatchSignature)
				return prepared;
			std::vector<ShadowBuildView> rows;
			rows.reserve(frame->views.size());
			for (std::size_t i = 0; i < frame->views.size(); ++i) {
				const auto& view = frame->views[i];
				// The shader leaves a y past the rows idle.
				if (!ShadowBuildRow(*frame, i) || view.slot >= a_bindings.drawArgs.size())
					continue;
				ShadowBuildView row{};
				row.latchAt = view.slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
				row.drawArgsIndex = a_preparation.Resolve(a_bindings.drawArgs[view.slot]).index;
				row.casterRowsIndex = a_preparation.Resolve(a_bindings.casterRows).index;
				row.casterFirstsIndex = a_preparation.Resolve(a_bindings.casterFirsts).index;
				row.countIndex = a_preparation.Resolve(a_bindings.count[view.slot]).index;
				row.bucketCountsIndex = a_preparation.Resolve(a_bindings.bucketCounts[view.slot]).index;
				row.sequenceDraws = view.sequenceDraws;
				rows.push_back(row);
			}
			if (rows.empty())
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			prepared.dispatchOffset = frame->latchLayout.BuildDispatchOffset();
			prepared.views = static_cast<std::uint32_t>(rows.size());
			auto& constants = prepared.constants;
			// Republished only when its bytes change.
			constants.sequencesIndex = viewTable.Publish(a_preparation, std::span<const ShadowBuildView>(rows));
			constants.countIndex = prepared.views;
			constants.materialRowStride = static_cast<std::uint32_t>(kConstantAlignment);
			constants.phaseBits = 0;
			prepared.fadeRoots = FadeRows();
			return prepared;
		}

		static void Record(const ShadowBuildBindings& a_bindings, const ShadowBuildPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.program || !a_prepared.latch || !a_prepared.views)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_prepared.program->layout->GetHandle());
			commands.BindPipeline(a_prepared.program->Pipeline(a_recording));
			const auto at = a_prepared.latch->At(a_recording.Run());
			const std::uint64_t region = at.offset;
			auto constants = a_prepared.constants;
			constants.latchIndex = at.srvIndex;
			constants.latchOffset = static_cast<std::uint32_t>(region);
			constants.poolFirstsIndex = a_recording.Index(a_bindings.poolFirsts);
			// Every view writes the visibility words, though nothing reads them here.
			constants.visibilityIndex = a_recording.Index(a_bindings.visibility);
			constants.fadeRootsIndex = a_prepared.fadeRoots ? a_recording.Index(a_bindings.fadeRoots) : 0u;
			constants.objectGatesIndex = a_bindings.objectGates.layout ? a_recording.Index(a_bindings.objectGates) : 0u;
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.ExecuteIndirect(a_prepared.signature, at.buffer->GetAPIResource().GetHandle(), region + a_prepared.dispatchOffset, {}, 0, 1);
		}

	private:
		bool FadeRows() const { return RootRowsCurrent(*resources->scene); }

		std::shared_ptr<ShadowResources> resources;
		std::shared_ptr<const ShadowSlotBuffers> slots;  // the build state's (BuildState::shadowSlots)
		// One upload-heap buffer per distinct content, each kept by the packets naming it.
		org::PreparedTablePublisher viewTable;
	};

	struct ShadowPassBindings
	{
		std::array<std::vector<org::DeclaredViewToken>, kShadowDepthTargets> depthViews{};
		std::vector<org::ResourceBindingToken> drawArgs, bucketCounts;  // per view slot
		org::ResourceBindingToken casterRows;  // read by address (DCLF_PULLED)
		org::ResourceBindingToken pool;  // the index pool (IndexPool)
		org::ResourceBindingToken constants, viewBlocks;
	};

	struct ShadowPrepared
	{
		std::shared_ptr<const ShadowFrame> frame;
		struct View
		{
			std::uint32_t index = 0;  // into frame->views (its target's slice: the bindings' depth view)
		};
		std::vector<View> views;
		std::array<std::uint32_t, 2> blockViews{};
		bool blockViewsMade = false;
	};

	/**
	 * @brief Draws each view's culled sequences into its slice and viewport.
	 * The engine's RenderMask already cleared the targets before the shadow maps were drawn.
	 */
	class ShadowViewPass final : public org::TypedRenderGraphPass<ShadowViewPass, ShadowPrepared, ShadowPassBindings>
	{
	public:
		ShadowViewPass(std::shared_ptr<ShadowResources> a_resources, std::shared_ptr<const ShadowSlotBuffers> a_slots) :
			resources(std::move(a_resources)), slots(std::move(a_slots)) {}

		ShadowPassBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			ShadowPassBindings bindings{};
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (slots->depth[i]) {
					bindings.depthViews[i].reserve(slots->depthLayers[i]);
					for (std::uint32_t slice = 0; slice < slots->depthLayers[i]; ++slice)
						bindings.depthViews[i].push_back(a_builder.DepthReadWrite(slots->depth[i], org::DsvView{ UINT32_MAX, 0, slice }).View());
				}
			}
			bindings.casterRows = a_builder.ShaderResource(*resources->casterRows, noViews).Resource();
			for (std::size_t s = 0; s < slots->drawArgs.size(); ++s) {
				if (slots->OwnsDrawArgs(s))
					bindings.drawArgs.push_back(a_builder.IndirectArguments(*slots->drawArgs[s]).Resource());
				else
					bindings.drawArgs.push_back(bindings.drawArgs[slots->drawArgsOwner[s]]);
				bindings.bucketCounts.push_back(a_builder.IndirectArguments(*slots->bucketCounts[s]).Resource());
			}
			bindings.constants = a_builder.ShaderResource(resources->constants, noViews).Resource();
			bindings.viewBlocks = a_builder.ShaderResource(*resources->viewBlocks.buffer, noViews).Resource();
			if (resources->scene->vertexStream)
				a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
			bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
			FrameValues::Get().DeclareReads(a_builder);
			Draws::DeclarePayloadReads(a_builder);
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>&) const {}

		ShadowPrepared Prepare(const ShadowPassBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			ShadowPrepared prepared{};
			auto frame = CurrentShadowFrame(a_preparation);
			if (!frame || frame->views.empty() || !frame->indirect.valid)
				return prepared;
			for (std::uint32_t i = 0; i < frame->views.size(); ++i) {
				const auto& view = frame->views[i];
				// A view the variant's frames leave unused has a zero latch, with nothing to draw or load.
				if (i < 64 && !((frame->activeViews >> i) & 1))
					continue;
				if (!view.capacity || view.slot >= a_bindings.drawArgs.size() || view.target >= kShadowDepthTargets || !slots->depth[view.target])
					continue;
				// Use the declared slices. A target grown since has slices these bindings cannot name.
				if (view.slice >= slots->depthLayers[view.target] || view.slice >= a_bindings.depthViews[view.target].size())
					continue;
				prepared.views.push_back({ i });
			}
			if (!prepared.views.empty())
				prepared.frame = std::move(frame);
			prepared.blockViews = resources->blockViewIndices;
			prepared.blockViewsMade = resources->blockViews != nullptr;
			return prepared;
		}

		static void Record(const ShadowPassBindings& a_bindings, const ShadowPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			std::vector<rhi::DepthAttachment> depths(a_prepared.views.size());
			std::vector<rhi::PassBeginInfo> begins(a_prepared.views.size());
			for (std::size_t i = 0; i < a_prepared.views.size(); ++i) {
				const auto& prepared = a_prepared.views[i];
				const auto& view = frame.views[prepared.index];
				auto& begin = begins[i];
				begin.x = view.x;
				begin.y = view.y;
				begin.width = view.width;
				begin.height = view.height;
				begin.minDepth = view.minDepth;
				begin.maxDepth = view.maxDepth;
				// The engine's casters are already in the slice, so load, add and store.
				auto& depth = depths[i];
				depth.dsv = a_recording.View(a_bindings.depthViews[view.target][view.slice]);
				depth.depthLoad = rhi::LoadOp::Load;
				depth.depthStore = rhi::StoreOp::Store;
				depth.stencilLoad = rhi::LoadOp::Load;
				depth.stencilStore = rhi::StoreOp::Store;
				begin.depth = &depth;
				begin.debugName = IsOcclusionTarget(view.target) ? "DCLF occlusion map" : "DCLF shadow view";
			}
			// One plain indexed indirect draw per bucket. A plain draw cannot bind its own vertex buffer, so the vertex stage fetches
			// through its first instance's caster row (Utility.hlsl, DCLF_PULLED).
			const RenderGraph::BufferRef casterRowsRef = DCLF::Draws::RecordingBufferRef(a_recording, a_bindings.casterRows);
			auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
				a_words[0] = static_cast<std::uint32_t>(a_value);
				a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
			};
			const rhi::IndexBufferView pool{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt };  // the whole buffer
			// D3D12's layout holds root constants only, so push words name blocks by ref and by constant buffer view
			// (DCLF_D3D12_SHADOW_FRAME). Vulkan's layout reads the shape's addresses.
			const bool d3d12 = ::RenderGraph::Backend::IsD3D12();
			RenderGraph::BufferRef viewBlocksRef, constantsRef;
			if (d3d12) {
				viewBlocksRef = DCLF::Draws::RecordingBufferRef(a_recording, a_bindings.viewBlocks);
				constantsRef = DCLF::Draws::RecordingBufferRef(a_recording, a_bindings.constants);
				if (!viewBlocksRef || !constantsRef || !a_prepared.blockViewsMade) {
					static std::atomic<bool> warned{ false };
					if (!warned.exchange(true))
						logger::error("[DCLF] D3D12 shadow views: no buffer view ref of the view blocks or the constants, or no SharedData view; the views draw nothing");
					return;
				}
			}
			auto viewPush = [&](const ShadowFrameView& a_view) {
				auto push = a_view.push;
				if (d3d12) {
					auto ref = [&](std::uint32_t a_word, RenderGraph::BufferRef a_ref) {
						push[a_word] = a_ref.viewSlot;
						push[a_word + 1] = a_ref.byteOffset;
					};
					const RenderGraph::BufferRef viewBlock = viewBlocksRef + std::uint64_t(a_view.slot) * kShadowViewSlotBytes;
					ref(kShadowPushViewBlock, viewBlock);
					ref(kShadowPushPerFrame, viewBlock + kShadowPerFrameOffset);
					ref(kShadowPushZeros, constantsRef + kShadowArenaBlocksOffset);  // the zero block heads the arena (ShadowArenaBlocksOf)
					push[kShadowPushSharedData] = a_prepared.blockViews[0];
					push[kShadowPushSharedData + 1] = 0;
					push[kShadowPushFeatureData] = a_prepared.blockViews[1];
					push[kShadowPushFeatureData + 1] = 0;
				}
				return push;
			};
			// D3D12's signature also reads the instance word before the arguments. Vulkan reads only the arguments.
			const std::uint64_t argumentsOffset = d3d12 ? 0u : kShadowDrawArgumentsOffset;
			void* const nvCommands = GpuCounters::CommandBufferRangesActive() ? rhi::vulkan::get_cmd_list(commands) : nullptr;
			std::uint32_t recordedDraws = 0;
			static constexpr const char* kViewRanges[] = { "cs.dclf.shadow.view-0", "cs.dclf.shadow.view-1", "cs.dclf.shadow.view-2", "cs.dclf.shadow.view-3",
				"cs.dclf.shadow.view-4", "cs.dclf.shadow.view-5", "cs.dclf.shadow.view-6", "cs.dclf.shadow.view-7+" };
			for (std::size_t i = 0; i < a_prepared.views.size(); ++i) {
				const auto& view = frame.views[a_prepared.views[i].index];
				const char* range = IsOcclusionTarget(view.target) ? "cs.dclf.shadow.occlusion-map" : kViewRanges[std::min<std::size_t>(i, std::size(kViewRanges) - 1)];
				const bool viewRange = nvCommands && GpuCounters::PushCommandBufferRange(nvCommands, range);
				commands.BeginPass(begins[i]);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame.indirect.layout);
				commands.SetIndexBuffer(pool);
				const auto push = viewPush(view);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kShadowPushWords, push.data());
				const auto drawArgs = a_recording.Resolve(a_bindings.drawArgs[view.slot]).GetHandle();
				const auto counts = a_recording.Resolve(a_bindings.bucketCounts[view.slot]).GetHandle();
				for (std::uint32_t b = 0; b < view.buckets.size(); ++b) {
					const auto& bucket = view.buckets[b];
					if (!bucket.capacity || bucket.pipeline >= frame.indirect.pipelines.size())
						continue;
					std::uint32_t words[kDrawPushWords]{};
					words[kShadowDrawPushSequences] = casterRowsRef.viewSlot;
					words[kShadowDrawPushSequences + 1] = casterRowsRef.byteOffset;
					// The rows word is 0, as in the Z-prepass's pulled draws. Each draw's row is its sequence's.
					split(words + kShadowDrawPushMaterialRows, 0);
					split(words + kShadowDrawPushVertexLayout, frame.indirect.vertexLayouts[bucket.pipeline]);
					commands.BindPipeline(frame.indirect.pipelines[bucket.pipeline]);
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
					commands.ExecuteIndirect(frame.indirect.drawSignature, drawArgs, std::uint64_t(bucket.first) * kShadowDrawBytes + argumentsOffset, counts,
						std::uint64_t(b) * sizeof(std::uint32_t),
						bucket.capacity);
					++recordedDraws;
				}
				commands.EndPass();
				if (nvCommands)
					GpuCounters::PopCommandBufferRange(nvCommands, viewRange);
			}
			// A recording replays until its shape changes, so a few logs show the views' indirect draw cost.
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged.fetch_add(1, std::memory_order_relaxed) < 200)
				logger::info("[DCLF] shadow views recorded: {} views, {} indirect draws ({:.1f} a view), view slot {}", a_prepared.views.size(), recordedDraws,
					a_prepared.views.empty() ? 0.0 : double(recordedDraws) / double(a_prepared.views.size()),
					a_prepared.views.empty() ? 0u : frame.views[a_prepared.views[0].index].slot);
		}

	private:
		std::shared_ptr<ShadowResources> resources;
		std::shared_ptr<const ShadowSlotBuffers> slots;  // the build state's (BuildState::shadowSlots)
	};
}

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		/**
		 * @brief What the views' latched copies write.
		 * A CPU pipeline's graph declares no counters, since each would add a barrier to every execution for nothing.
		 */
		std::vector<Draws::LatchedTarget> LatchedTargets(const Draws::ShadowResources& a_shadow, const Draws::ShadowSlotBuffers& a_slots, bool a_cpuPipeline)
		{
			std::vector<Draws::LatchedTarget> out{ { nullptr, a_shadow.viewBlocks.buffer }, { a_shadow.constants, nullptr } };
			if (a_cpuPipeline)
				return out;
			out.push_back({ a_shadow.casterRowsUsed, nullptr });
			for (std::size_t s = 0; s < a_slots.count.size(); ++s) {
				out.push_back({ a_slots.count[s], nullptr });
				out.push_back({ nullptr, a_slots.bucketCounts[s] });
			}
			return out;
		}

		/** @brief The views' latched copies, the occlusion maps' too. */
		std::shared_ptr<org::RenderPass> LatchedCopies(const Api::PassContext& a_context)
		{
			// The targets are read when the pass is declared, which may be after the build.
			return Draws::MakeLatchedCopiesPass(a_context.Conditions(), [shadow = a_context.Shadow(), slots = a_context.Build().shadowSlots, cpu = a_context.Conditions().cpuPipeline] {
				return LatchedTargets(*shadow, *slots, cpu);
			}, [](const org::PassPrepareContext& a_preparation) {
				const auto frame = Draws::CurrentShadowFrame(a_preparation);
				return frame ? Draws::LatchedSource{ frame, frame->latched } : Draws::LatchedSource{};
			});
		}

		std::shared_ptr<org::RenderPass> IndexPoolCopies(const Api::PassContext& a_context)
		{
			return Draws::MakeShadowIndexPoolPass(a_context.Shadow());
		}

		std::shared_ptr<org::RenderPass> CasterRows(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::ShadowCasterRowsPass>(a_context.Shadow());
		}

		std::shared_ptr<org::RenderPass> BuildDraws(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::ShadowBuildDrawsPass>(a_context.Shadow(), a_context.Build().shadowSlots);
		}

		std::shared_ptr<org::RenderPass> ViewDraws(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::ShadowViewPass>(a_context.Shadow(), a_context.Build().shadowSlots);
		}

		// Only added while CS_DCLF_STATS runs, so otherwise the epoch copies nothing each frame.
		std::shared_ptr<org::RenderPass> CullCounters(const Api::PassContext& a_context)
		{
			return Diagnostics::Observe().stats ? Draws::MakeShadowCullCounterPass(a_context.Shadow()) : nullptr;
		}

		using PassKind = Api::PassKind;
		using PassQueue = Api::PassQueue;

		constexpr Api::PassDef kShadowPasses[] = {
			{ .key = "shadows.latched-copies", .passName = "cs.dclf.shadow.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies, .cpuPipeline = true },
			{ .key = "shadows.index-pool", .passName = "cs.dclf.shadow.index-pool", .make = &IndexPoolCopies, .cpuPipeline = true },
			{ .key = "shadows.caster-rows", .passName = "cs.dclf.shadow.caster-rows", .make = &CasterRows },
			{ .key = "shadows.build-draws", .passName = "cs.dclf.shadow.build-draws", .make = &BuildDraws },
			{ .key = "shadows.view", .passName = "cs.dclf.shadow.view", .kind = PassKind::Render, .make = &ViewDraws },
			{ .key = "shadows.cull-counters", .passName = "cs.dclf.shadow.cull-counters", .kind = PassKind::Copy, .make = &CullCounters },
		};
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		for (const auto& pass : kShadowPasses)
			a_context.RegisterPass(pass);
	}
}
