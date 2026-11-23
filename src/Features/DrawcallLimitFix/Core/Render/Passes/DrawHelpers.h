#pragma once

// Recording helpers for the draw passes: attachments, frame buffer declarations, preprocessed indirect draws, a bucket's
// plain draws and the frame slot addresses a draw pass pushes. Also the common passes a feature's epoch is made of: latched
// copies, the latched BuildDraws dispatch and the shadow index pool copies. Self-contained so feature passes can include it.

// volk must come before every Vulkan header, so Main/Resources.h goes first.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "RenderGraph/BufferRef.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "RenderGraph/Backend.h"

#include <Render/LatchBlock.h>
#include <RenderPasses/LatchedCopiesPass.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace DCLF::Draws
{
	class FrameBlockViews;
	struct GraphConditions;

	/**
	 * @brief A buffer's device address for push data. 0 once the render graph has stopped, since a worker may still be
	 * recording and nothing it records is submitted.
	 */
	std::uint64_t RecordingBufferAddress(rhi::ResourceHandle a_buffer);
	/**
	 * @brief A declared buffer's view ref at recording, for a pulled stage's push words. Null without a graph host device.
	 * It resolves the pass's declaration, so the pass must declare the buffer for the shader read too.
	 */
	RenderGraph::BufferRef RecordingBufferRef(const org::RecordingContext& a_recording, const org::ResourceBindingToken& a_binding);

	struct ShadowResources;

	// Records one BuildDrawsCS dispatch whose values and group count come from the latch at a_latchOffset.
	void RecordLatchedDispatch(BuildDrawsConstants a_constants, const org::LatchBlock& a_latch,
		rhi::CommandSignatureHandle a_signature, std::uint32_t a_latchOffset, org::PassRecordContext& a_recording);

	/**
	 * @brief What a latched copies pass copies: the shape (owner) and its lists. A host epoch also copies each hosted view's
	 * shape after its own, one group per hosted view in the host's order. copyFixed false leaves out the zeroed count lists.
	 */
	using LatchedSource = org::LatchedSource;
	/**
	 * @brief An epoch's first pass, which copies its latched values to where its passes read them. A copy to a target
	 * a_targets() did not declare is skipped. A host epoch copies its hosted shapes' lists in the same pass. a_conditions are
	 * those of the build that makes it.
	 */
	std::shared_ptr<org::RenderPass> MakeLatchedCopiesPass(const GraphConditions& a_conditions, std::function<std::vector<LatchedTarget>()> a_targets,
		std::function<LatchedSource(const org::PassPrepareContext&)> a_source, std::atomic<std::shared_ptr<const std::vector<const void*>>>* a_declared = nullptr);

	/** @brief The shadow epoch's index pool copies, dispatched from the revision's shadow shape's latch. */
	std::shared_ptr<org::RenderPass> MakeShadowIndexPoolPass(const std::shared_ptr<ShadowResources>& a_resources);

	/** @brief Which main commit wrote the frame record a pass draws with. It decides what the record binds from t16 up. */
	enum class FrameRecordKind : std::uint8_t
	{
		Depth,   // the depth commit: the Z-prepass phases
		Colour,  // the colour commit: colour, water, forward and first person
	};

	/**
	 * @brief Declares what a pass's draws read through a frame record of a_kind: the record's own buffers and the payload.
	 * A colour record adds its frame buffers (t16 and up), Light Limit Fix's lights (t35-t37) and the frame textures' ids.
	 * @return The published reads, for Extensions::CheckDeclaredReads in the pass's Prepare.
	 */
	Extensions::DeclaredReads DeclareFrameBuffers(org::PassBuilder& a_builder, const Resources& a_resources, FrameRecordKind a_kind = FrameRecordKind::Colour,
		bool a_lights = true);

	/** @brief Declares the late-bound buffers of the installed payload's parts that the pass reads. DeclareFrameBuffers does this too. */
	void DeclarePayloadReads(org::PassBuilder& a_builder);

	/** @brief One device-generated call of a draw pass, drawing one state class's sub-range of a range. */
	struct IndirectCall
	{
		rhi::CommandSignatureHandle signature{};
		std::uint64_t argumentsOffset = 0;
		rhi::ResourceHandle counts{};
		std::uint64_t countOffset = 0;
		std::uint32_t capacity = 0;
		const char* range = nullptr;  // the nvperf range around its execution when a capture selects it
		const char* preprocessRange = nullptr;  // and around its preprocess
	};
	/**
	 * @brief Whether a_call's class has a signature. Without device-generated commands plain buckets draw every range, so a
	 * call reaching a recording is a defect. It is logged once and its draws are lost.
	 */
	bool CallDrawable(const IndirectCall& a_call);
	/**
	 * @brief One explicitly preprocessed draw pass. Each call is preprocessed on the frame slot's state list, set up exactly
	 * as the execution will be, then the pass executes them in order.
	 */
	void RecordPreprocessedDraws(org::PassRecordContext& a_recording, PreprocessStates& a_preprocess, const rhi::PassBeginInfo& a_begin, rhi::DescriptorHeapHandle a_resourceHeap,
		rhi::DescriptorHeapHandle a_samplerHeap, rhi::PipelineLayoutHandle a_layout, const std::array<std::uint32_t, kFramePushWords>& a_push, rhi::ResourceHandle a_arguments,
		std::span<const IndirectCall> a_calls, const char* a_preprocessRange = nullptr);
	/** @brief RecordPreprocessedDraws of one call. */
	void RecordPreprocessedDraw(org::PassRecordContext& a_recording, PreprocessStates& a_preprocess, const rhi::PassBeginInfo& a_begin, rhi::DescriptorHeapHandle a_resourceHeap,
		rhi::DescriptorHeapHandle a_samplerHeap, rhi::PipelineLayoutHandle a_layout, const std::array<std::uint32_t, kFramePushWords>& a_push, rhi::CommandSignatureHandle a_signature,
		rhi::ResourceHandle a_arguments, std::uint64_t a_argumentsOffset, rhi::ResourceHandle a_counts, std::uint64_t a_countOffset, std::uint32_t a_capacity);

	/** @brief The frame push words of the main frame constants, for draw passes outside IndirectDraws. */
	std::array<std::uint32_t, kFramePushWords> MainFramePushWords(std::uint64_t a_frameConstants, RenderGraph::BufferRef a_frameConstantsRef, const FrameBlockViews* a_blocks);
	/** @brief The frame push block in the shadow views' layout for the main camera, with the zeroed shared light block as the zero block. */
	void MainShadowLayoutPush(std::uint64_t a_frameConstants, RenderGraph::BufferRef a_frameConstantsRef, std::uint32_t (&a_out)[kShadowPushWords]);
	/**
	 * @brief D3D12's form of MainShadowLayoutPush's words 4-13, which its stages read themselves by ref and by view.
	 * Words 0-3 are left as they are.
	 */
	void MainShadowLayoutViews(RenderGraph::BufferRef a_frameConstantsRef, const FrameBlockViews& a_blocks, std::uint32_t (&a_out)[kShadowPushWords]);
	/** @brief The address of the frame's LodFadeFrame in the main frame constants. */
	std::uint64_t LodFadeFrameAddress(std::uint64_t a_frameConstants);

	/**
	 * @brief Where a plain draw's arguments start in its sequence. On D3D12 below Shader Model 6.8 it is one word earlier, the
	 * instance word the Z-prepass's signature writes into kDrawPushInstance first.
	 */
	inline std::uint64_t PlainDrawArgumentsOffset() { return ::RenderGraph::Backend::D3D12InstanceWord() ? kSequenceDrawOffset - 4 : kSequenceDrawOffset; }

	inline void RecordBucketDraw(rhi::CommandList& a_commands, rhi::CommandSignatureHandle a_signature, rhi::ResourceHandle a_sequences, RenderGraph::BufferRef a_sequencesRef,
		std::uint64_t a_first, rhi::PipelineHandle a_pipeline, rhi::ResourceHandle a_counts, std::uint32_t a_bucket, std::uint32_t a_capacity, bool a_offsetDepth = false)
	{
		const std::uint64_t first = a_first * sizeof(DrawSequence);
		std::uint32_t words[kDrawPushWords]{};
		const RenderGraph::BufferRef sequence = a_sequencesRef + first;
		words[kZDrawPushSequences] = sequence.viewSlot;
		words[kZDrawPushSequences + 1] = sequence.byteOffset;
		words[kZDrawPushOffsetDepth] = a_offsetDepth ? 1u : 0u;
		a_commands.BindPipeline(a_pipeline);
		a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
		a_commands.ExecuteIndirect(a_signature, a_sequences, first + PlainDrawArgumentsOffset(), a_counts, std::uint64_t(a_bucket) * sizeof(std::uint32_t), a_capacity);
	}

	/** @brief A pass's colour attachments, loaded and stored. */
	template <std::size_t N>
	std::array<rhi::ColorAttachment, N> LoadedColours(org::PassRecordContext& a_recording, const std::array<org::DeclaredViewToken, N>& a_tokens, std::uint32_t a_count)
	{
		std::array<rhi::ColorAttachment, N> out{};
		for (std::uint32_t i = 0; i < a_count; ++i) {
			out[i].rtv = a_recording.View(a_tokens[i]);
			out[i].loadOp = rhi::LoadOp::Load;
			out[i].storeOp = rhi::StoreOp::Store;
			out[i].resource = a_recording.Resolve(a_tokens[i].Resource()).GetHandle();
		}
		return out;
	}
	/** @brief A pass's depth attachment, depth and stencil loaded and stored. */
	inline rhi::DepthAttachment LoadedDepth(org::PassRecordContext& a_recording, const org::DeclaredViewToken& a_view)
	{
		rhi::DepthAttachment depth{};
		depth.dsv = a_recording.View(a_view);
		depth.depthLoad = rhi::LoadOp::Load;
		depth.depthStore = rhi::StoreOp::Store;
		depth.stencilLoad = rhi::LoadOp::Load;
		depth.stencilStore = rhi::StoreOp::Store;
		return depth;
	}
}
