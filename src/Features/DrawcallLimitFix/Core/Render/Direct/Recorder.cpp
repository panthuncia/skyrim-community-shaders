// The CPU-driven pipeline's recorder: one plain indexed draw per resident sequence of a culled batch.

// Main/Resources.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Recorder.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "RenderGraph/Backend.h"

#include <rhi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace DCLF::Direct
{
	namespace
	{
		// Words a direct draw pushes per draw. BeginList zeroes the rest once.
		constexpr std::uint32_t kDirectDrawWords = kDirectDrawPushFlags + 1;
		static_assert(kZDrawPushSequences == 0 && kShadowDrawPushSequences == 0 && kDirectDrawWords <= kDrawPushWords);

		void Split(std::uint32_t* a_words, std::uint64_t a_value)
		{
			a_words[0] = static_cast<std::uint32_t>(a_value);
			a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
		}
		void Split(std::uint32_t* a_words, RenderGraph::BufferRef a_ref)
		{
			a_words[0] = a_ref.viewSlot;
			a_words[1] = a_ref.byteOffset;
		}

		/**
		 * @brief Binds the list's layout and the push data its draws share. a_main is the main layout of a pushed colour list,
		 * whose draws push their whole range.
		 */
		void BindListLayout(RecordContext& a_context, bool a_main)
		{
			const auto& bindings = a_context.bindings;
			auto& list = a_context.list;
			const bool shadowLayout = a_context.pass == RecordPass::Shadow || a_context.pass == RecordPass::Refraction;
			list.BindLayout(a_main                                     ? bindings.main->layout :
			                a_context.pass == RecordPass::Shadow       ? bindings.shadow->layout :
			                a_context.pass == RecordPass::Refraction   ? bindings.main->refractionLayout :
			                                                             bindings.main->zLayout);
			if (shadowLayout)
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kShadowPushWords, bindings.shadowPush.data());
			else
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, bindings.framePush.data());
			a_context.mainLayoutBound = a_main;
			if (a_main)
				return;
			// Zero the whole draw range once. Each draw then pushes only its first kDirectDrawWords.
			const std::uint32_t zeros[kDrawPushWords]{};
			list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, zeros);
			// The sequences' rows are relative to these bases.
			std::uint32_t bases[4]{};
			const auto& set = a_context.set;
			if (a_context.pass == RecordPass::Shadow) {
				Split(bases + 2, set.shadowRowsRef);
			} else {
				Split(bases, set.pipelineRowsRef);
				Split(bases + 2, set.materialRowsRef);
			}
			list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, kDirectDrawPushRowBases, 4, bases);
			// Animated rows from the frame's copy, for the texture transforms' TexcoordOffset.
			if (a_context.pass != RecordPass::Shadow && bindings.animatedRowsRef) {
				std::uint32_t animated[2]{};
				Split(animated, bindings.animatedRowsRef);
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, kDirectDrawPushAnimatedRows, 2, animated);
			}
		}

		/** @brief The key's own pipeline under the main layout for a pushed list, or null to use the pulled one. */
		rhi::PipelineHandle PushedPipeline(const RecordContext& a_context, std::uint32_t a_index)
		{
			if (!a_context.pushedColour)
				return {};
			const auto& main = *a_context.bindings.main;
			const auto& pipelines = a_context.pass == RecordPass::Depth ? main.depth : main.colour;
			return a_index < pipelines.size() ? pipelines[a_index] : rhi::PipelineHandle{};
		}

		/** @brief The pulled pipeline a bucket binds in a pass, and a shadow view's vertex layout. Null for none. */
		rhi::PipelineHandle PulledBucketPipeline(const ViewBatchSet& a_set, ViewType a_view, RecordPass a_pass, const RecordBindings& a_bindings, std::uint32_t a_bucket, std::uint64_t& a_vertexLayout)
		{
			a_vertexLayout = 0;
			const auto& bindings = a_bindings;
			if (a_pass == RecordPass::Refraction)
				return bindings.main->refraction;
			const auto& buckets = a_set.buckets[static_cast<std::size_t>(a_view)].pipeline;
			if (a_bucket >= buckets.size())
				return {};
			std::uint32_t index = buckets[a_bucket];
			switch (a_pass) {
			case RecordPass::Depth:
				{
					const auto& main = *bindings.main;
					return index < main.zGroups.size() && main.zGroups[index] < main.zPipelines.size() ? main.zPipelines[main.zGroups[index]] : rhi::PipelineHandle{};
				}
			case RecordPass::Colour:
				{
					const auto& main = *bindings.main;
					return index < main.plainColour.size() ? main.plainColour[index] : rhi::PipelineHandle{};
				}
			case RecordPass::DecalDepth:
				return index < bindings.main->plainDepth.size() ? bindings.main->plainDepth[index] : rhi::PipelineHandle{};
			case RecordPass::Shadow:
				{
					// The bucket is a key slot, so its pipeline comes from the view's map row.
					const auto& shadow = *bindings.shadow;
					if (index >= bindings.shadowMapRow.size())
						return {};
					index = bindings.shadowMapRow[index];
					if (index >= shadow.pipelines.size())
						return {};
					a_vertexLayout = index < shadow.vertexLayouts.size() ? shadow.vertexLayouts[index] : 0;
					return shadow.pipelines[index];
				}
			default:
				return {};
			}
		}

		/** @brief The pipeline a bucket binds, pushed if available, else pulled. */
		rhi::PipelineHandle BucketPipeline(const RecordContext& a_context, std::uint32_t a_bucket, std::uint64_t& a_vertexLayout)
		{
			if (a_context.pushedColour && (a_context.pass == RecordPass::Depth || a_context.pass == RecordPass::Colour)) {
				const auto& buckets = a_context.set.buckets[static_cast<std::size_t>(a_context.view)].pipeline;
				if (a_bucket < buckets.size())
					if (const auto pushed = PushedPipeline(a_context, buckets[a_bucket]); pushed.valid()) {
						a_vertexLayout = 0;
						return pushed;
					}
			}
			return PulledBucketPipeline(a_context.set, a_context.view, a_context.pass, a_context.bindings, a_bucket, a_vertexLayout);
		}

		/** @brief Binds a_bucket's pipeline unless already bound. False when the bucket has none, and its draws are skipped. */
		bool BindBucket(RecordContext& a_context, std::uint32_t a_bucket)
		{
			if (a_context.pass == RecordPass::Refraction || a_bucket == a_context.boundBucket)
				return a_context.boundPipeline.valid();  // refraction's, bound by BeginList
			a_context.boundBucket = a_bucket;
			const rhi::PipelineHandle previous = a_context.boundPipeline;
			a_context.boundPipeline = BucketPipeline(a_context, a_bucket, a_context.boundVertexLayout);
			if (a_context.boundPipeline.valid()) {
				if (a_context.pushedColour) {
					const auto& buckets = a_context.set.buckets[static_cast<std::size_t>(a_context.view)].pipeline;
					const bool main = a_bucket < buckets.size() && PushedPipeline(a_context, buckets[a_bucket]).valid();
					if (main != a_context.mainLayoutBound)
						BindListLayout(a_context, main);
				}
				a_context.list.BindPipeline(a_context.boundPipeline);
				++a_context.binds;
				if (previous.index == a_context.boundPipeline.index && previous.generation == a_context.boundPipeline.generation)
					++a_context.sameBinds;
			}
			return a_context.boundPipeline.valid();
		}

		/** @brief One sequence's direct draw, pushing the same words the GPU path's plain draws push, then its arguments. */
		bool DrawSequenceDirect(RecordContext& a_context, std::uint32_t a_sequence, std::uint32_t a_objectWord)
		{
			const auto& set = a_context.set;
			const auto& bindings = a_context.bindings;
			const DrawArgs* found = set.Args(a_sequence);
			if (!found)
				return false;
			const DrawArgs& args = *found;
			if (!args.indexCount || args.geometrySlot >= bindings.poolFirsts.size())
				return false;
			const std::uint32_t poolFirst = bindings.poolFirsts[args.geometrySlot];
			if (poolFirst == Draws::IndexPool::kNoRange)
				return false;
			auto& list = a_context.list;
			if (a_context.mainLayoutBound) {
				// The sequence's bytes 4-99 as a device-generated draw's Constant argument, with rows placed at the set's bases.
				const DrawSequence* source = set.Sequence(a_sequence);
				if (!source)
					return false;
				DrawSequence s = *source;
				s.pipelineRowAddress += set.pipelineRowsBase;
				const std::uint64_t row = s.materialRowAddress / kMaterialRowBytes;
				const std::uint32_t animated = bindings.animatedRows && row < set.animatedRowOf.size() ? set.animatedRowOf[row] : ~0u;
				s.materialRowAddress = animated != ~0u ? bindings.animatedRows + std::uint64_t(animated) * kMaterialRowBytes : s.materialRowAddress + set.materialRowsBase;
				s.objectIndex = a_objectWord;
				SetSequenceBlocks(s);
				std::uint32_t pushed[kDrawPushArgumentWords];
				std::memcpy(pushed, reinterpret_cast<const std::byte*>(&s) + 4, sizeof(pushed));
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushArgumentWords, pushed);
				++a_context.pushes;
				a_context.indices += args.indexCount;
				list.DrawIndexed(args.indexCount, 1, poolFirst + args.firstIndex, 0, 0);
				return true;
			}
			// A D3D12 run's signature left its last instance in the instance word, so reset it to 0 for direct draws.
			if (std::exchange(a_context.runWordsPushed, false) && a_context.indirect && a_context.indirect->signatureFor) {
				const std::uint32_t zero = 0;
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, kDrawPushInstance, 1, &zero);
			}
			std::uint32_t words[kDirectDrawWords]{};
			Split(words + kZDrawPushSequences, set.sequencesRef + std::uint64_t(a_sequence) * sizeof(DrawSequence));
			switch (a_context.pass) {
			case RecordPass::Shadow:
				Split(words + kShadowDrawPushVertexLayout, a_context.boundVertexLayout);
				break;
			case RecordPass::Refraction:
				if (bindings.lodFadeFrameRef)
					Split(words + kShadowDrawPushMaterialRows, bindings.lodFadeFrameRef);
				else
					Split(words + kShadowDrawPushMaterialRows, bindings.lodFadeFrame);
				break;
			default:
				words[kZDrawPushOffsetDepth] = bindings.offsetDepth ? 1u : 0u;
				break;
			}
			words[kDirectDrawPushObjectWord] = a_objectWord;
			words[kDirectDrawPushFlags] = kDirectFlagObjectWord | kDirectFlagRelativeRows;
			list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDirectDrawWords, words);
			++a_context.pushes;
			a_context.indices += args.indexCount;
			list.DrawIndexed(args.indexCount, 1, poolFirst + args.firstIndex, 0, 0);
			return true;
		}

		// Records.h's kObjectCrossfadeCopy.
		constexpr std::uint32_t kCrossfadeCopyBit = 1u << 30;
		constexpr std::uint32_t kRecorderDrawAll = 0xFFFFu;

		/**
		 * @brief Whether a lane's sequence is drawn under a_mask, and its object word. Sequences past the mask's 16 are drawn only
		 * when all are, never as the copy.
		 */
		bool SequenceDrawn(std::uint32_t a_mask, std::uint32_t a_sequence, std::uint32_t a_objectWord, std::uint32_t& a_word)
		{
			a_word = a_objectWord;
			if (a_sequence >= 16)
				return (a_mask & kRecorderDrawAll) == kRecorderDrawAll;
			if (!((a_mask >> a_sequence) & 1u))
				return false;
			if ((a_mask >> (16 + a_sequence)) & 1u)
				a_word |= kCrossfadeCopyBit;
			return true;
		}

		/** @brief Draws a kept lane's sequences that a_mask selects. */
		void DrawLane(RecordContext& a_context, const ViewBatch& a_batch, std::uint32_t a_lane, std::uint32_t a_objectWord, std::uint32_t a_mask)
		{
			// A cull-only lane has no sequences and draws nothing.
			const std::uint32_t count = a_batch.sequenceCount ? a_batch.sequenceCount[a_lane] : 1u;
			if (!(a_mask & kRecorderDrawAll) || !count)
				return;
			const std::uint32_t bucket = a_batch.pipelineBucket ? a_batch.pipelineBucket[a_lane] : 0u;
			if (!BindBucket(a_context, bucket))
				return;
			const std::uint32_t first = a_batch.sequence[a_lane];
			for (std::uint32_t s = 0; s < count; ++s) {
				std::uint32_t word = 0;
				if (SequenceDrawn(a_mask, s, a_objectWord, word))
					a_context.drawn += DrawSequenceDirect(a_context, first + s, word) ? 1u : 0u;
			}
		}

		std::uint32_t MaskOf(const CulledBatch& a_batch, std::uint32_t a_kept) { return a_batch.drawMasks ? a_batch.drawMasks[a_kept] : kRecorderDrawAll; }

		constexpr std::uint32_t kTerrainLanes = kLaneTerrainGroup1 | kLaneTerrainGroup2;
		std::uint32_t FlagsOf(const ViewBatch& a_batch, std::uint32_t a_lane) { return a_batch.flags ? a_batch.flags[a_lane] : 0u; }

		void CountDrawn(RecordContext& a_context, std::uint32_t a_drawn)
		{
			if (!a_context.counters)
				return;
			auto& counters = *a_context.counters;
			if (a_drawn)
				std::atomic_ref<std::uint32_t>(counters.drawn[static_cast<std::size_t>(a_context.view)]).fetch_add(a_drawn, std::memory_order_relaxed);
			const auto flush = [](std::uint32_t& a_counter, std::uint32_t a_value, std::uint32_t& a_counted) {
				if (a_value != a_counted)
					std::atomic_ref<std::uint32_t>(a_counter).fetch_add(a_value - std::exchange(a_counted, a_value), std::memory_order_relaxed);
			};
			flush(counters.binds, a_context.binds, a_context.countedBinds);
			flush(counters.sameBinds, a_context.sameBinds, a_context.countedSameBinds);
			flush(counters.pushes, a_context.pushes, a_context.countedPushes);
			flush(counters.indirects, a_context.indirects, a_context.countedIndirects);
			if (a_context.indices != a_context.countedIndices) {
				const std::uint64_t added = a_context.indices - std::exchange(a_context.countedIndices, a_context.indices);
				std::atomic_ref<std::uint64_t>(counters.indices[static_cast<std::size_t>(a_context.site)]).fetch_add(added, std::memory_order_relaxed);
				std::atomic_ref<std::uint64_t>(counters.viewIndices[static_cast<std::size_t>(a_context.view)]).fetch_add(added, std::memory_order_relaxed);
			}
		}

		/** @brief A forward draw's sort key, as in ForwardSortCS.hlsl. */
		struct ForwardDraw
		{
			float depth = 0.0f;
			std::uint32_t group = 0;
			std::uint32_t ordinal = 0;  // 0 for a plain draw
			std::uint32_t object = 0;
			std::uint32_t first = 0;
			std::uint32_t sequence = 0;
			std::uint32_t bucket = 0;
			std::uint32_t objectWord = 0;
		};

		// Matches ForwardSortCS.hlsl's Before.
		bool Before(const ForwardDraw& a, const ForwardDraw& b)
		{
			if (a.depth != b.depth)
				return a.depth > b.depth;
			if ((a.ordinal == 0) != (b.ordinal == 0))
				return a.ordinal == 0;
			if (a.group != b.group)
				return a.group < b.group;
			if (a.ordinal != b.ordinal)
				return a.ordinal < b.ordinal;
			if (a.object != b.object)
				return a.object < b.object;
			return a.first < b.first;
		}

		/** @brief A batch's indirect draws for one part, one per run. */
		void RecordRuns(RecordContext& a_context, const CulledBatch& a_batch, std::uint32_t a_part)
		{
			auto& list = a_context.list;
			const IndirectBuffer& buffer = *a_context.indirect;
			const rhi::CommandSignatureHandle signature = buffer.signatureFor ? buffer.signatureFor(a_context.layout) : buffer.signature;
			if (!signature.valid())
				return;
			const auto sameHandle = [](rhi::PipelineHandle a, rhi::PipelineHandle b) { return a.index == b.index && a.generation == b.generation; };
			std::uint32_t drawn = 0;
			for (std::uint32_t r = a_batch.runStart[a_part]; r < a_batch.runStart[a_part + 1]; ++r) {
				const IndirectRun& run = a_batch.runs[r];
				const bool bind = !sameHandle(run.pipeline, a_context.boundPipeline);
				if (bind) {
					list.BindPipeline(run.pipeline);
					++a_context.binds;
				}
				if (bind || !a_context.runWordsPushed) {
					std::uint32_t words[kDirectDrawWords]{};
					Split(words + kZDrawPushSequences, a_context.set.sequencesRef);
					if (a_context.pass == RecordPass::Shadow)
						Split(words + kShadowDrawPushVertexLayout, run.vertexLayout);
					else
						words[kZDrawPushOffsetDepth] = a_context.bindings.offsetDepth ? 1u : 0u;
					words[kDirectDrawPushFlags] = kDirectFlagDrawRecords | kDirectFlagRelativeRows;
					list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDirectDrawWords, words);
					++a_context.pushes;
					a_context.runWordsPushed = true;
				}
				// Forces a later direct draw to bind its bucket again.
				a_context.boundBucket = ~0u;
				a_context.boundPipeline = run.pipeline;
				a_context.boundVertexLayout = run.vertexLayout;
				list.ExecuteIndirect(signature, buffer.buffer, std::uint64_t(run.first) * sizeof(IndirectDraw) + buffer.argumentsOffset, {}, 0, run.count);
				++a_context.indirects;
				drawn += run.count;
			}
			a_context.drawn += drawn;
			CountDrawn(a_context, drawn);
		}

		// Per recording thread, reused and grown to the largest forward list, never shared.
		thread_local std::vector<ForwardDraw> forwardDraws;
	}

	RecordPass PassOf(ViewType a_view, bool a_depthPass)
	{
		switch (a_view) {
		case ViewType::Main:
		case ViewType::Reflection:
		case ViewType::FirstPerson:
			return a_depthPass ? RecordPass::Depth : RecordPass::Colour;
		case ViewType::Decal:
			return a_depthPass ? RecordPass::DecalDepth : RecordPass::Colour;
		case ViewType::Forward:
			return RecordPass::Colour;
		case ViewType::Refraction:
			return RecordPass::Refraction;
		default:
			return RecordPass::Shadow;
		}
	}

	bool BeginList(RecordContext& a_context)
	{
		a_context.pass = PassOf(a_context.view, a_context.depthPass);
		a_context.boundBucket = ~0u;
		a_context.boundPipeline = {};
		a_context.boundVertexLayout = 0;
		const auto& bindings = a_context.bindings;
		if (a_context.pass == RecordPass::Shadow ? !bindings.shadow || !bindings.shadow->valid : !bindings.main || !bindings.main->valid)
			return false;
		const rhi::PipelineLayoutHandle layout = a_context.pass == RecordPass::Shadow ? bindings.shadow->layout :
		                                         a_context.pass == RecordPass::Refraction ? bindings.main->refractionLayout :
		                                                                                    bindings.main->zLayout;
		if (!layout.valid() || (a_context.pass == RecordPass::Refraction && !bindings.main->refraction.valid()))
			return false;
		a_context.layout = layout;
		auto& list = a_context.list;
		list.SetDescriptorHeaps(bindings.resourceHeap, bindings.samplerHeap);
		list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
		// Pushed pipelines are built for the main pass's formats. The reflection faces draw into an R16G16B16A16 cube, so they keep
		// their view pipelines (VUID-vkCmdDrawIndexed-dynamicRenderingUnusedAttachments-08911). D3D12 builds no unpulled pipelines.
		a_context.pushedColour = !a_context.indirect && !::RenderGraph::Backend::IsD3D12() && bindings.main && bindings.main->layout.valid() &&
		                         ((a_context.pass == RecordPass::Colour && a_context.view != ViewType::Reflection && a_context.mainTargets &&
		                              !bindings.main->colour.empty()) ||
		                          (a_context.pass == RecordPass::Depth && !bindings.offsetDepth && !bindings.main->depth.empty()));
		BindListLayout(a_context, false);
		a_context.runWordsPushed = false;
		if (a_context.indirect) {
			std::uint32_t records[2]{};
			Split(records, a_context.indirect->ref);
			list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, kDirectDrawPushDrawRecords, 2, records);
		}
		list.SetIndexBuffer(rhi::IndexBufferView{ bindings.indexPool, 0, 0, rhi::Format::R16_UInt });
		if (a_context.pass == RecordPass::Refraction) {
			a_context.boundBucket = 0;
			a_context.boundPipeline = bindings.main->refraction;
			list.BindPipeline(a_context.boundPipeline);
		}
		return true;
	}

	std::uint64_t WriteIndirectDraws(CulledBatch& a_batch, const ViewBatchSet& a_set, ViewType a_view, bool a_depthPass, const RecordBindings& a_bindings, const IndirectBuffer& a_buffer, CullArena& a_arena)
	{
		a_batch.hasRuns = false;
		std::uint64_t indices = 0;
		if (!a_batch.batch || !a_batch.count)
			return indices;
		const ViewBatch& batch = *a_batch.batch;
		const RecordPass pass = PassOf(a_view, a_depthPass);
		// Main colour draws the terrain groups apart, in the second pass.
		const bool parts = a_view == ViewType::Main && pass == RecordPass::Colour;
		const auto partOf = [&](std::uint32_t a_lane) -> std::uint32_t {
			if (!parts)
				return 0u;
			const std::uint32_t flags = FlagsOf(batch, a_lane);
			return (flags & kLaneTerrainGroup1) ? 1u : (flags & kLaneTerrainGroup2) ? 2u : 0u;
		};
		// Each part's draws are consecutive in the buffer.
		std::array<std::uint32_t, 3> mostDraws{}, mostRuns{}, lastBucket{ ~0u, ~0u, ~0u };
		for (std::uint32_t k = 0; k < a_batch.count; ++k) {
			const std::uint32_t lane = a_batch.lanes[k];
			const std::uint32_t mask = MaskOf(a_batch, k);
			const std::uint32_t count = batch.sequenceCount ? batch.sequenceCount[lane] : 1u;
			if (!(mask & kRecorderDrawAll) || !count)
				continue;
			const std::uint32_t part = partOf(lane);
			mostDraws[part] += (mask & kRecorderDrawAll) == kRecorderDrawAll ? count : static_cast<std::uint32_t>(std::popcount(mask & kRecorderDrawAll));
			const std::uint32_t bucket = batch.pipelineBucket ? batch.pipelineBucket[lane] : 0u;
			if (bucket != lastBucket[part]) {
				lastBucket[part] = bucket;
				++mostRuns[part];
			}
		}
		const std::uint32_t reserve = mostDraws[0] + mostDraws[1] + mostDraws[2];
		if (!reserve)
			return indices;
		const std::uint32_t first = a_buffer.cursor->fetch_add(reserve, std::memory_order_relaxed);
		if (first > a_buffer.capacity || reserve > a_buffer.capacity - first)
			return indices;
		auto* runs = a_arena.Make<IndirectRun>(mostRuns[0] + mostRuns[1] + mostRuns[2]);
		std::array<std::uint32_t, 3> cursor{ first, first + mostDraws[0], first + mostDraws[0] + mostDraws[1] };
		const std::array<std::uint32_t, 3> runBase{ 0u, mostRuns[0], mostRuns[0] + mostRuns[1] };
		std::array<std::uint32_t, 3> runEnd = runBase;
		lastBucket = { ~0u, ~0u, ~0u };
		std::array<rhi::PipelineHandle, 3> pipeline{};
		std::array<std::uint64_t, 3> layout{};
		const auto sameHandle = [](rhi::PipelineHandle a, rhi::PipelineHandle b) { return a.index == b.index && a.generation == b.generation; };
		IndirectDraw* const mapped = a_buffer.mapped;
		const auto poolFirsts = a_bindings.poolFirsts;
		for (std::uint32_t k = 0; k < a_batch.count; ++k) {
			const std::uint32_t lane = a_batch.lanes[k];
			const std::uint32_t mask = MaskOf(a_batch, k);
			const std::uint32_t count = batch.sequenceCount ? batch.sequenceCount[lane] : 1u;
			if (!(mask & kRecorderDrawAll) || !count)
				continue;
			const std::uint32_t part = partOf(lane);
			const std::uint32_t bucket = batch.pipelineBucket ? batch.pipelineBucket[lane] : 0u;
			if (bucket != lastBucket[part]) {
				lastBucket[part] = bucket;
				pipeline[part] = PulledBucketPipeline(a_set, a_view, pass, a_bindings, bucket, layout[part]);
			}
			if (!pipeline[part].valid())
				continue;
			const std::uint32_t objectWord = a_batch.objectWords[k];
			const std::uint32_t sequence = batch.sequence[lane];
			for (std::uint32_t s = 0; s < count; ++s) {
				std::uint32_t word = 0;
				if (!SequenceDrawn(mask, s, objectWord, word))
					continue;
				const DrawArgs* args = a_set.Args(sequence + s);
				if (!args || !args->indexCount || args->geometrySlot >= poolFirsts.size())
					continue;
				const std::uint32_t poolFirst = poolFirsts[args->geometrySlot];
				if (poolFirst == Draws::IndexPool::kNoRange)
					continue;
				const std::uint32_t at = cursor[part]++;
				// Buckets sharing a pipeline, such as the Z groups, make one run.
				IndirectRun* open = runEnd[part] > runBase[part] ? &runs[runEnd[part] - 1] : nullptr;
				if (!open || !sameHandle(open->pipeline, pipeline[part]) || open->vertexLayout != layout[part])
					open = new (&runs[runEnd[part]++]) IndirectRun{ pipeline[part], layout[part], at, 0u };
				++open->count;
				// Write-combined memory, so write in order and never read.
				IndirectDraw& draw = mapped[at];
				draw.instance = at;
				draw.indexCount = args->indexCount;
				indices += args->indexCount;
				draw.instanceCount = 1;
				draw.firstIndex = poolFirst + args->firstIndex;
				draw.vertexOffset = 0;
				draw.firstInstance = at;
				draw.sequence = sequence + s;
				draw.objectWord = word;
			}
		}
		std::uint32_t written = runEnd[0];
		a_batch.runStart[0] = 0;
		a_batch.runStart[1] = written;
		for (std::uint32_t part = 1; part < 3; ++part) {
			for (std::uint32_t r = runBase[part]; r < runEnd[part]; ++r)
				runs[written++] = runs[r];
			a_batch.runStart[part + 1] = written;
		}
		a_batch.runs = runs;
		a_batch.hasRuns = true;
		return indices;
	}

	void RecordBatch(RecordContext& a_context, const CulledBatch& a_batch)
	{
		if (!a_batch.batch || !a_batch.count)
			return;
		if (a_context.indirect && a_batch.hasRuns) {
			RecordRuns(a_context, a_batch, 0);
			return;
		}
		const std::uint32_t before = a_context.drawn;
		const ViewBatch& batch = *a_batch.batch;
		// Main colour draws the terrain groups in the second pass.
		const bool skipTerrain = a_context.view == ViewType::Main && a_context.pass == RecordPass::Colour;
		for (std::uint32_t k = 0; k < a_batch.count; ++k) {
			const std::uint32_t lane = a_batch.lanes[k];
			if (!skipTerrain || !(FlagsOf(batch, lane) & kTerrainLanes))
				DrawLane(a_context, batch, lane, a_batch.objectWords[k], MaskOf(a_batch, k));
		}
		CountDrawn(a_context, a_context.drawn - before);
	}

	void RecordSortedBatches(RecordContext& a_context, std::span<const CulledBatch* const> a_batches, const std::array<float, 4>& a_plane)
	{
		constexpr std::uint32_t kSlabs = 8;
		struct Item
		{
			float distance;
			std::uint32_t slab;
			std::uint32_t bucket;
			const CulledBatch* batch;
			std::uint32_t kept;
		};
		thread_local std::vector<Item> items;
		items.clear();
		float nearest = std::numeric_limits<float>::max(), farthest = std::numeric_limits<float>::lowest();
		for (const CulledBatch* culled : a_batches) {
			if (!culled || !culled->batch || !culled->count)
				continue;
			const ViewBatch& batch = *culled->batch;
			for (std::uint32_t k = 0; k < culled->count; ++k) {
				const std::uint32_t lane = culled->lanes[k];
				// The sphere's nearest point to the plane, so casters nearest the light go first.
				const float distance = batch.cx ? a_plane[0] * batch.cx[lane] + a_plane[1] * batch.cy[lane] + a_plane[2] * batch.cz[lane] + a_plane[3] - batch.r[lane] : 0.0f;
				nearest = (std::min)(nearest, distance);
				farthest = (std::max)(farthest, distance);
				items.push_back({ distance, 0u, batch.pipelineBucket ? batch.pipelineBucket[lane] : 0u, culled, k });
			}
		}
		// Nearest slab first, each drawn by pipeline for fewer binds. With every caster at one distance, by distance alone.
		if (farthest > nearest) {
			const float scale = static_cast<float>(kSlabs) / (farthest - nearest);
			for (auto& item : items)
				item.slab = (std::min)(kSlabs - 1, static_cast<std::uint32_t>((item.distance - nearest) * scale));
			std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
				return a.slab != b.slab ? a.slab < b.slab : a.bucket != b.bucket ? a.bucket < b.bucket : a.distance < b.distance;
			});
		} else {
			std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.distance < b.distance; });
		}
		const std::uint32_t before = a_context.drawn;
		for (const Item& item : items) {
			const CulledBatch& culled = *item.batch;
			DrawLane(a_context, *culled.batch, culled.lanes[item.kept], culled.objectWords[item.kept], MaskOf(culled, item.kept));
		}
		CountDrawn(a_context, a_context.drawn - before);
	}

	void RecordTerrainGroup(RecordContext& a_context, std::uint32_t a_group, std::span<const CulledBatch* const> a_batches)
	{
		const std::uint32_t flag = a_group == 1 ? kLaneTerrainGroup1 : a_group == 2 ? kLaneTerrainGroup2 : 0u;
		if (!flag)
			return;
		const std::uint32_t before = a_context.drawn;
		for (const CulledBatch* culled : a_batches) {
			if (!culled || !culled->batch)
				continue;
			if (a_context.indirect && culled->hasRuns) {
				RecordRuns(a_context, *culled, a_group);
				continue;
			}
			const ViewBatch& batch = *culled->batch;
			for (std::uint32_t k = 0; k < culled->count; ++k) {
				const std::uint32_t lane = culled->lanes[k];
				if (FlagsOf(batch, lane) & flag)
					DrawLane(a_context, batch, lane, culled->objectWords[k], MaskOf(*culled, k));
			}
		}
		CountDrawn(a_context, a_context.drawn - before);
	}

	void RecordForward(RecordContext& a_context, std::span<const CulledBatch* const> a_batches)
	{
		const auto& bindings = a_context.bindings;
		const auto& set = a_context.set;
		const auto& m = bindings.viewProj;
		auto& draws = forwardDraws;
		draws.clear();
		for (const CulledBatch* culled : a_batches) {
			if (!culled || !culled->batch)
				continue;
			const ViewBatch& batch = *culled->batch;
			for (std::uint32_t k = 0; k < culled->count; ++k) {
				const std::uint32_t lane = culled->lanes[k];
				const std::uint32_t object = batch.object[lane];
				// A BSOrderedNode group's leaf sorts at its node's centre, otherwise at its bound's centre.
				ForwardDraw draw{};
				draw.object = object;
				draw.bucket = batch.pipelineBucket ? batch.pipelineBucket[lane] : 0u;
				const std::uint32_t laneWord = culled->objectWords[k];
				const std::uint32_t mask = MaskOf(*culled, k);
				if (object < bindings.placements.size()) {
					const BindlessPlacement& placement = bindings.placements[object];
					draw.ordinal = std::bit_cast<std::uint32_t>(placement.forwardSort[3]);
					const float* centre = draw.ordinal != 0 ? placement.forwardSort : placement.bound;
					const float clipW = m[12] * centre[0] + m[13] * centre[1] + m[14] * centre[2] + m[15];
					draw.depth = std::isnan(clipW) ? 0.0f : clipW;  // a NaN would break the order
					if (draw.ordinal != 0)
						draw.group = (std::bit_cast<std::uint32_t>(centre[0]) * 0x9E3779B1u) ^ (std::bit_cast<std::uint32_t>(centre[1]) * 0x85EBCA77u) ^
						             (std::bit_cast<std::uint32_t>(centre[2]) * 0xC2B2AE3Du);
				}
				// Each sequence is staged as its own draw, as on the GPU path.
				const std::uint32_t count = batch.sequenceCount ? batch.sequenceCount[lane] : 1u;
				for (std::uint32_t s = 0; s < count; ++s) {
					if (!SequenceDrawn(mask, s, laneWord, draw.objectWord))
						continue;
					draw.sequence = batch.sequence[lane] + s;
					const DrawArgs* args = set.Args(draw.sequence);
					draw.first = args ? args->firstIndex : 0u;
					const std::uint32_t slot = args ? args->geometrySlot : ~0u;
					if (slot < bindings.poolFirsts.size() && bindings.poolFirsts[slot] != Draws::IndexPool::kNoRange)
						draw.first += bindings.poolFirsts[slot];
					draws.push_back(draw);
				}
			}
		}
		std::sort(draws.begin(), draws.end(), Before);
		std::uint32_t drawn = 0;
		for (const ForwardDraw& draw : draws) {
			if (BindBucket(a_context, draw.bucket))
				drawn += DrawSequenceDirect(a_context, draw.sequence, draw.objectWord) ? 1u : 0u;
		}
		a_context.drawn += drawn;
		CountDrawn(a_context, drawn);
	}

	void RecordDecalGroup(RecordContext& a_context, std::uint32_t a_group, std::span<const CulledBatch* const> a_batches)
	{
		const auto& set = a_context.set;
		if (std::size_t(a_group) + 1 >= set.decalGroupFirst.size())
			return;
		const std::uint64_t from = set.decalGroupFirst[a_group], to = set.decalGroupFirst[a_group + 1];
		if (from >= to)
			return;
		const std::uint32_t before = a_context.drawn;
		for (const CulledBatch* culled : a_batches) {
			if (!culled || !culled->batch)
				continue;
			const ViewBatch& batch = *culled->batch;
			const std::uint64_t base = std::uint64_t(batch.index) * set.lanes;
			if (base >= to || base + batch.count <= from)
				continue;
			for (std::uint32_t k = 0; k < culled->count; ++k) {
				const std::uint32_t lane = culled->lanes[k];
				if (base + lane >= from && base + lane < to)
					DrawLane(a_context, batch, lane, culled->objectWords[k], MaskOf(*culled, k));
			}
		}
		CountDrawn(a_context, a_context.drawn - before);
	}

	void RecordFamilyDraws(RecordContext& a_context)
	{
		const auto& bindings = a_context.bindings;
		const bool colour = a_context.pass == RecordPass::Colour;
		if ((a_context.pass != RecordPass::Depth && !colour) || !bindings.main || !bindings.main->valid)
			return;
		const auto& families = Extensions::DrawFamilies();
		auto& list = a_context.list;
		std::uint32_t drawn = 0;
		for (std::size_t f = 0; f < bindings.families.size() && f < Extensions::kDrawFamilies; ++f) {
			const FamilyDraw& draw = bindings.families[f];
			const auto& family = families[f];
			if (!draw.pipelines || !draw.arguments.valid() || !family.taken || !family.contribution.main.record)
				continue;
			// A family's colour tests EQUAL against its depth draw.
			if (colour) {
				list.BindLayout(bindings.main->zLayout);
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, bindings.framePush.data());
			}
			// No HZB in the CPU pipeline, so the depth draw is phase 1's.
			family.contribution.main.record({ list, colour ? Api::DrawFamilyPass::Colour : Api::DrawFamilyPass::Depth, draw.pipelines, draw.row, draw.arguments, {}, {} });
			++drawn;
		}
		// Counts each family's draw so a site that drew only families still counts as drawing.
		a_context.drawn += drawn;
		CountDrawn(a_context, drawn);
		// The families bound their own pipelines and words, so the next lane must rebind.
		a_context.boundBucket = ~0u;
		a_context.boundPipeline = {};
		a_context.mainLayoutBound = false;
	}
}
