// The main opaque pass: DCLF's draws in the Z-prepass (both phases) and in the main colour pass, plus the draw families' and
// main ranges' draws.

// Main/Resources.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/MainView/MainView.h"
#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/DrawFamilies/FamilySlots.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include "Features/RemoteControl/GpuCounters.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/GraphHost.h>
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

	namespace
	{
		/** @brief Records a draw family's draw for a family prepared with its pipelines. */
		void RecordFamily(std::size_t a_family, const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording,
			rhi::CommandList& a_commands, Api::DrawFamilyPass a_pass)
		{
			const auto& drawn = a_prepared.families[a_family];
			Extensions::DrawFamilies()[a_family].contribution.main.record(
				{ a_commands, a_pass, drawn.drawn, drawn.row, a_recording.Resolve(a_bindings.familyArguments[a_family]).GetHandle(), {}, {} });
		}

		/** @brief What MainOpaquePass::Record's stages share. */
		struct MainRecording
		{
			const PassFrame& frame;
			rhi::CommandList& commands;
			rhi::ResourceHandle sequences;
			rhi::ResourceHandle count;
			rhi::ResourceHandle classCounts;
			const rhi::PassBeginInfo& begin;  // depth attached, no colours
			const rhi::DepthAttachment& depth;
			PassStats* stats;
			std::uint32_t statsSlot;  // the run's statistics range (PassStats::RangeOf)
			org::RunRef run;          // the run being recorded: its preprocess state list
		};

		/**
		 * @brief The Z-prepass draws, one plain indexed indirect draw per bucket.
		 * A plain draw cannot bind its own vertex buffer or push data, so the vertex stage fetches through its sequence (Lighting.hlsl).
		 */
		void RecordDepthDraws(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording, const MainRecording& a_pass)
		{
			const auto& frame = a_pass.frame;
			auto& commands = a_pass.commands;
			const auto sequences = a_pass.sequences;
			const auto& begin = a_pass.begin;
			auto* stats = a_pass.stats;
			const std::uint32_t statsSlot = a_pass.statsSlot;
			const auto depthKind = a_prepared.phaseTwo ? PassStats::kDepthPhaseTwo : PassStats::kDepth;
			if (stats)
				stats->Start(commands, statsSlot, depthKind);
			commands.BeginPass(begin);
			commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			commands.BindLayout(frame.indirect.zLayout);
			commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
			commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
			if (stats)
				stats->Begin(commands, statsSlot, depthKind);
			const auto counts = a_recording.Resolve(a_bindings.bucketCounts).GetHandle();
			const RenderGraph::BufferRef sequencesRef = DCLF::Draws::RecordingBufferRef(a_recording, a_bindings.sequences);
			const std::uint64_t phaseBase = a_prepared.phaseTwo ? std::uint64_t(frame.sequenceDraws) : 0;
			auto drawCall = [&](const PassFrame::ZCall& a_call, bool a_offsetDepth) {
				RecordBucketDraw(commands, frame.indirect.zDrawSignature, sequences, sequencesRef, phaseBase + a_call.first, a_call.pipeline, counts, a_call.bucket,
					a_call.capacity, a_offsetDepth);
			};
			// Near terrain twins draw with Terrain Blending's depth offset, as its depth hook does (OFFSET_DEPTH).
			bool terrainNear = false;
			for (const auto& call : frame.zCalls) {
				drawCall(call, call.terrainNear);
				terrainNear = terrainNear || call.terrainNear;
			}
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (a_prepared.families[f].drawn)
					RecordFamily(f, a_bindings, a_prepared, a_recording, commands, a_prepared.phaseTwo ? Api::DrawFamilyPass::DepthPhaseTwo : Api::DrawFamilyPass::Depth);
			if (stats)
				stats->End(commands, statsSlot, depthKind);
			commands.EndPass();
			// Draw the twins again without the offset into Terrain Blending's terrain depth. BlendPrepassDepths reads it after this epoch.
			if (terrainNear && a_prepared.terrainDepth) {
				rhi::DepthAttachment terrainDepth = a_pass.depth;
				terrainDepth.dsv = a_recording.View(a_bindings.terrainDepth);
				rhi::PassBeginInfo terrainBegin = begin;
				terrainBegin.depth = &terrainDepth;
				terrainBegin.debugName = "DCLF terrain depth";
				commands.BeginPass(terrainBegin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame.indirect.zLayout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
				for (const auto& call : frame.zCalls)
					if (call.terrainNear)
						drawCall(call, false);
				commands.EndPass();
			}
			if (stats)
				stats->Resolve(commands, statsSlot, depthKind);
		}

		/** @brief Begins a draw pass identically on the command list and on a preprocess state list. */
		void BeginDrawPass(rhi::CommandList& a_list, const rhi::PassBeginInfo& a_begin, const PassFrame& a_frame, const PreparedDraws& a_prepared)
		{
			a_list.BeginPass(a_begin);
			a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			a_list.BindLayout(a_frame.indirect.layout);
			a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
		}

		/**
		 * @brief The colour segment's passes and calls, one call per state class with a sub-range.
		 * Each call uses its class's signature, so an execution set holds pipelines of one fixed-function state only.
		 */
		struct ColourPasses
		{
			const PassFrame& frame;
			rhi::PassBeginInfo mainBegin;
			rhi::PassBeginInfo decalBegin;
			rhi::PassBeginInfo decalDepthBegin;
			rhi::ResourceHandle count, classCounts;  // the decal groups' count words; every other sub-range's (ClassCountWord)
			// Plain bucket draws replace the classes' main calls.
			bool plainMain = false;
			// Plain bucket draws replace the terrain groups' calls.
			bool plainTerrain = false;
			// Plain bucket draws replace the decal groups' calls.
			bool plainDecals = false;
			bool anyDecals = false;  // any range of the second pass drawn
			bool anyDecalDepth = false;  // any depth-prepass range drawn

			/** @brief Class a_class's call of range a_range with a_variant's signature. A decal group's classes share one count word. */
			IndirectCall Call(std::uint32_t a_class, std::uint32_t a_range, std::uint32_t a_variant) const
			{
				const auto& range = frame.classes[a_class].ranges[a_range];
				const bool decal = a_range >= ClassRangeOfSlot(0) && a_range < ClassRangeOfSlot(kDecalGroups);
				const std::uint32_t word = decal ? DecalCountWord(a_range - ClassRangeOfSlot(0)) : ClassCountWord(a_class, a_range);
				return { frame.indirect.classes[a_class].signatures[a_variant], std::uint64_t(range.first) * sizeof(DrawSequence), decal ? count : classCounts,
					std::uint64_t(word) * sizeof(std::uint32_t), range.capacity };
			}
			template <class Each>
			void RangeCalls(std::uint32_t a_range, std::uint32_t a_variant, Each&& a_each) const
			{
				for (std::uint32_t c = 0; c < frame.classes.size() && c < frame.indirect.classes.size(); ++c)
					if (frame.classes[c].ranges[a_range].capacity)
						a_each(Call(c, a_range, a_variant));
			}
			bool RangeDrawn(std::uint32_t a_slot) const
			{
				bool drawn = false;
				RangeCalls(ClassRangeOfSlot(a_slot), kColorVariant, [&](const IndirectCall&) { drawn = true; });
				return drawn;
			}
			template <class Each>
			void MainDraws(Each&& a_each) const
			{
				if (!plainMain)
					RangeCalls(kClassRangeMain, kColorVariant, a_each);
			}
			// The second pass's ranges are in Extensions::MainRanges order. Terrain Blending's replay follows the engine's opaque groups.
			bool Plain(std::uint32_t a_slot) const { return a_slot >= kDecalGroups ? plainTerrain : plainDecals; }
			template <class Each, class PlainDraws>
			void SecondPass(Each&& a_each, PlainDraws&& a_plain) const
			{
				for (const auto& range : Extensions::MainRanges().drawn) {
					if (Plain(range.slot)) {
						a_plain(range.slot, range.colourRange.c_str());
						continue;
					}
					RangeCalls(ClassRangeOfSlot(range.slot), kColorVariant, [&](const IndirectCall& a_call) { a_each(a_call, range.colourRange.c_str()); });
				}
			}
			template <class Each, class PlainDraws>
			void DepthPrepass(Each&& a_each, PlainDraws&& a_plain) const
			{
				for (const auto& range : Extensions::MainRanges().drawn)
					if (range.depthPrepass) {
						if (Plain(range.slot))
							a_plain(range.slot);
						else
							RangeCalls(ClassRangeOfSlot(range.slot), kDepthVariant, a_each);
					}
			}
		};

		void PreprocessCall(rhi::CommandList& a_commands, rhi::CommandList& a_state, rhi::ResourceHandle a_sequences, const IndirectCall& a_call)
		{
			if (CallDrawable(a_call))
				a_commands.PreprocessIndirect(a_state, a_call.signature, a_sequences, a_call.argumentsOffset, a_call.counts, a_call.countOffset, a_call.capacity);
		}
		void ExecuteCall(rhi::CommandList& a_commands, rhi::ResourceHandle a_sequences, const IndirectCall& a_call)
		{
			if (CallDrawable(a_call))
				a_commands.ExecuteIndirect(a_call.signature, a_sequences, a_call.argumentsOffset, a_call.counts, a_call.countOffset, a_call.capacity);
		}

		/**
		 * @brief Preprocesses every colour call before the first pass. NVIDIA's driver otherwise idles the GPU per call in these
		 * eight-target passes. The state list must match the execution exactly, with the same pass setup and heaps.
		 */
		void RecordColourPreprocess(const PreparedDraws& a_prepared, const MainRecording& a_pass, const ColourPasses& a_passes)
		{
			if (!a_prepared.preprocess)
				return;
			const auto& frame = a_pass.frame;
			auto& commands = a_pass.commands;
			const auto sequences = a_pass.sequences;
			auto& state = a_prepared.preprocess->Begin(a_pass.run);
			state.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			if (a_passes.anyDecalDepth) {
				BeginDrawPass(state, a_passes.decalDepthBegin, frame, a_prepared);
				a_passes.DepthPrepass([&](const IndirectCall& a_call) { PreprocessCall(commands, state, sequences, a_call); }, [](std::uint32_t) {});
				state.EndPass();
			}
			BeginDrawPass(state, a_passes.mainBegin, frame, a_prepared);
			a_passes.MainDraws([&](const IndirectCall& a_call) { PreprocessCall(commands, state, sequences, a_call); });
			state.EndPass();
			if (a_passes.anyDecals) {
				BeginDrawPass(state, a_passes.decalBegin, frame, a_prepared);
				a_passes.SecondPass([&](const IndirectCall& a_call, const char*) { PreprocessCall(commands, state, sequences, a_call); }, [](std::uint32_t, const char*) {});
				state.EndPass();
			}
			state.End();
		}

		/** @brief Records the colour passes: decal depth, main draws and families, then the second pass. */
		void RecordColourExecution(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording, const MainRecording& a_pass,
			const ColourPasses& a_passes, void* const a_nvCommands)
		{
			const auto& frame = a_pass.frame;
			auto& commands = a_pass.commands;
			const auto sequences = a_pass.sequences;
			auto* stats = a_pass.stats;
			const std::uint32_t statsSlot = a_pass.statsSlot;
			auto subRange = [&](const char* a_name) { return a_nvCommands && GpuCounters::PushCommandBufferRange(a_nvCommands, a_name); };
			auto endSubRange = [&](bool a_pushed) { if (a_nvCommands) GpuCounters::PopCommandBufferRange(a_nvCommands, a_pushed); };
			auto plainDraws = [&](auto&& a_buckets) {
				commands.BindLayout(frame.indirect.zLayout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
				const RenderGraph::BufferRef sequencesRef = DCLF::Draws::RecordingBufferRef(a_recording, a_bindings.sequences);
				const auto counts = a_recording.Resolve(a_bindings.classCounts).GetHandle();
				a_buckets([&](std::uint32_t a_first, rhi::PipelineHandle a_pipeline, std::uint32_t a_countWord, std::uint32_t a_capacity) {
					RecordBucketDraw(commands, frame.indirect.zDrawSignature, sequences, sequencesRef, a_first, a_pipeline, counts, a_countWord, a_capacity);
				});
			};
			// Plain draws in a pass of device-generated calls need their layout and frame push data set again.
			auto restoreLayout = [&] {
				commands.BindLayout(frame.indirect.layout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
			};
			auto decalDraws = [&](std::uint32_t a_slot, bool a_depth) {
				plainDraws([&](const auto& a_draw) {
					const auto& plan = *frame.classPlan;
					for (std::size_t b = 0; b < plan.decals.size(); ++b) {
						const auto& bucket = plan.decals[b];
						if (bucket.group != a_slot + 1)
							continue;
						a_draw(bucket.first + bucket.capacity, a_depth ? frame.indirect.plainDepth[bucket.set] : frame.indirect.plainColour[bucket.set],
							plan.DecalCountWord(b), bucket.capacity);
					}
				});
				restoreLayout();
			};
			if (a_passes.anyDecalDepth) {
				const bool part = subRange("cs.dclf.colour.decal-depth");
				BeginDrawPass(commands, a_passes.decalDepthBegin, frame, a_prepared);
				a_passes.DepthPrepass([&](const IndirectCall& a_call) { ExecuteCall(commands, sequences, a_call); }, [&](std::uint32_t a_slot) { decalDraws(a_slot, true); });
				commands.EndPass();
				endSubRange(part);
			}
			if (stats)
				stats->Start(commands, statsSlot, PassStats::kColour);
			const bool mainPart = subRange("cs.dclf.colour.main");
			BeginDrawPass(commands, a_passes.mainBegin, frame, a_prepared);
			if (stats)
				stats->Begin(commands, statsSlot, PassStats::kColour);
			{
				const bool drawPart = subRange("cs.dclf.colour.main-draws");
				a_passes.MainDraws([&](const IndirectCall& a_call) { ExecuteCall(commands, sequences, a_call); });
				// The main buckets use the same pulled vertex stage as the Z-prepass, so the EQUAL depth test holds.
				if (a_passes.plainMain)
					plainDraws([&](const auto& a_draw) {
						for (std::size_t b = 0; b < frame.classPlan->main.size(); ++b) {
							const auto& bucket = frame.classPlan->main[b];
							a_draw(bucket.first, frame.indirect.plainColour[bucket.set], frame.classPlan->MainCountWord(b), bucket.capacity);
						}
					});
				endSubRange(drawPart);
			}
			// A draw family's colour tests EQUAL against its Z-prepass draw, under the Z-prepass layout.
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (a_prepared.families[f].drawn) {
					const bool familyPart = subRange(Extensions::DrawFamilies()[f].colourRange.c_str());
					commands.BindLayout(frame.indirect.zLayout);
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
					RecordFamily(f, a_bindings, a_prepared, a_recording, commands, Api::DrawFamilyPass::Colour);
					endSubRange(familyPart);
				}
			if (stats)
				stats->End(commands, statsSlot, PassStats::kColour);
			commands.EndPass();
			endSubRange(mainPart);
			if (stats)
				stats->Resolve(commands, statsSlot, PassStats::kColour);

			// The second pass draws decals after every opaque draw, in engine order, LESS_EQUAL with the engine's decal bias.
			// Terrain Blending's terrain and no-blend meshes go before the blended group, as that feature replays them.
			if (a_passes.anyDecals) {
				const bool decalPart = subRange("cs.dclf.colour.decals");
				if (stats)
					stats->Start(commands, statsSlot, PassStats::kDecals);
				BeginDrawPass(commands, a_passes.decalBegin, frame, a_prepared);
				if (stats)
					stats->Begin(commands, statsSlot, PassStats::kDecals);
				a_passes.SecondPass(
					[&](const IndirectCall& a_call, const char* a_name) {
						const bool groupPart = subRange(a_name);
						ExecuteCall(commands, sequences, a_call);
						endSubRange(groupPart);
					},
					// Terrain groups are unsorted, so their buckets draw in any order.
					[&](std::uint32_t a_slot, const char* a_name) {
						const bool groupPart = subRange(a_name);
						if (a_slot < kDecalGroups) {
							decalDraws(a_slot, false);
						} else {
							const std::uint32_t group = a_slot - kDecalGroups + 1;
							plainDraws([&](const auto& a_draw) {
								const auto& plan = *frame.classPlan;
								for (std::size_t b = 0; b < plan.terrain.size(); ++b)
									if (plan.terrain[b].group == group)
										a_draw(plan.terrain[b].first, frame.indirect.plainColour[plan.terrain[b].set], plan.TerrainCountWord(b), plan.terrain[b].capacity);
							});
							restoreLayout();
						}
						endSubRange(groupPart);
					});
				if (stats)
					stats->End(commands, statsSlot, PassStats::kDecals);
				commands.EndPass();
				if (stats)
					stats->Resolve(commands, statsSlot, PassStats::kDecals);
				endSubRange(decalPart);
			}
		}

		/**
		 * @brief The colour segment's draws. The main pass tests EQUAL against the Z-prepass and writes no depth.
		 * Opaque decal depth goes first, so a host fragment under an opaque decal fails EQUAL as with the engine's decal group.
		 */
		void RecordColourDraws(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording, const MainRecording& a_pass,
			std::array<rhi::ColorAttachment, kColorTargets>& a_colors)
		{
			const auto& frame = a_pass.frame;
			void* const nvCommands = GpuCounters::CommandBufferRangesActive() ? rhi::vulkan::get_cmd_list(a_pass.commands) : nullptr;
			ColourPasses passes{ .frame = frame, .mainBegin = a_pass.begin, .decalBegin = a_pass.begin, .decalDepthBegin = a_pass.begin, .count = a_pass.count,
				.classCounts = a_pass.classCounts };
			passes.mainBegin.colors = { a_colors.data(), a_prepared.targetCount };
			passes.mainBegin.debugName = "DCLF main opaque";
			passes.decalBegin.colors = { a_colors.data(), a_prepared.targetCount };
			passes.decalBegin.debugName = "DCLF decals";
			passes.decalDepthBegin.debugName = "DCLF decal depth";  // depth only
			passes.plainMain = frame.classPlan && !frame.classPlan->main.empty() && a_bindings.pool.globalResourceID != 0;
			passes.plainTerrain = frame.classPlan && !frame.classPlan->terrain.empty() && a_bindings.pool.globalResourceID != 0;
			passes.plainDecals = frame.classPlan && !frame.classPlan->decals.empty() && a_bindings.pool.globalResourceID != 0;
			for (const auto& range : Extensions::MainRanges().drawn)
				if (passes.RangeDrawn(range.slot)) {
					passes.anyDecals = true;
					passes.anyDecalDepth = passes.anyDecalDepth || range.depthPrepass;
				}

			{
				const bool preprocessPart = nvCommands && GpuCounters::PushCommandBufferRange(nvCommands, "cs.dclf.colour.preprocess");
				RecordColourPreprocess(a_prepared, a_pass, passes);
				if (nvCommands)
					GpuCounters::PopCommandBufferRange(nvCommands, preprocessPart);
			}
			RecordColourExecution(a_bindings, a_prepared, a_recording, a_pass, passes, nvCommands);
		}
	}

	class MainOpaquePass final : public org::TypedRenderGraphPass<MainOpaquePass, PreparedDraws, PassBindings>
	{
	public:
		// depth records the main depth shape. phaseTwo draws what the rebuilt HZB brought back.
		MainOpaquePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_depth, bool a_phaseTwo = false) :
			resources(std::move(a_resources)), segment(a_segment), depth(a_depth), phaseTwo(a_phaseTwo) {}

		PassBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};  // Device-address reads need ordering, not a descriptor.
			PassBindings bindings{};
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				bindings.targets[i] = a_builder.RenderTarget(resources->native[i]).View();
			bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
			if (!depth && resources->shadowMask)
				a_builder.ShaderResource(resources->shadowMask, noViews);
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			bindings.count = a_builder.IndirectArguments(resources->count);
			if (depth) {
				a_builder.ShaderResource(*resources->sequences, noViews);
				if (resources->scene->vertexStream)
					a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
				bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
				bindings.bucketCounts = a_builder.IndirectArguments(*resources->zBucketCounts[phaseTwo ? 1 : 0]).Resource();
				if (resources->terrainDepth)
					bindings.terrainDepth = a_builder.DepthReadWrite(resources->terrainDepth).View();
			} else {
				bindings.classCounts = a_builder.IndirectArguments(*resources->classCounts).Resource();
				if (resources->pool) {
					a_builder.ShaderResource(*resources->sequences, noViews);
					bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
				}
				if (resources->scene->vertexStream)
					a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
			}
			// The depth shape draws with the depth commit's record, which binds nothing the colour commit adds.
			DeclareFrameBuffers(a_builder, *resources, depth ? FrameRecordKind::Depth : FrameRecordKind::Colour);
			a_builder.ShaderResource(resources->frameConstants, noViews);  // the frame record's slots, by address
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (const auto& family = Extensions::DrawFamilies()[f]; family.taken) {
					Api::DrawFamilyDeclare declare{ .builder = a_builder, .scene = *resources->scene, .view = nullptr,
						.arguments = std::span(&bindings.familyArguments[f], 1), .depth = depth };
					family.contribution.main.declare(declare);
					bindings.familyRows[f] = declare.row;
				}
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(static_cast<std::uint64_t>(segment));
			a_out.push_back(phaseTwo ? 1 : 0);
			// Draw family pipelines live in the shape, which the revision does not see.
			static const std::shared_ptr<const void> kNoPipelines;
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (const auto& family = Extensions::DrawFamilies()[f]; family.taken)
					family.contribution.main.revision({ *resources->scene, kNoPipelines }, a_out);
		}

		PreparedDraws Prepare(const PassBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			PreparedDraws prepared{};
			const auto now = segment;
			auto frame = CurrentFrame(a_preparation, now);
			if (!frame || (!depth && frame->classes.empty()) || !frame->indirect.valid)
				return prepared;
			if (phaseTwo && !depth)
				return prepared;
			prepared.phaseTwo = phaseTwo;
			prepared.zPrepass = depth;
			prepared.stats = resources->passStats.get();
			if (!prepared.zPrepass)
				prepared.preprocess = resources->preprocessMain.get();
			prepared.framePushWords = MainFramePushWords(resources->frameConstantsAddress, resources->frameConstantsRef);
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (const auto family = static_cast<Api::DrawFamily>(f); FamilySetUp(*resources->scene, family))
					prepared.families[f] = { frame->families[f], a_bindings.familyRows[f] };
			prepared.frame = std::move(frame);
			prepared.targetCount = resources->targetCount;
			if (prepared.zPrepass && resources->terrainDepth)
				prepared.terrainDepth = true;
			return prepared;
		}

		static void Record(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			auto colors = LoadedColours(a_recording, a_bindings.targets, a_prepared.targetCount);
			const bool zPrepass = a_prepared.zPrepass;
			if (a_prepared.phaseTwo && !zPrepass)
				return;
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
			const auto classCounts = zPrepass ? rhi::ResourceHandle{} : a_recording.Resolve(a_bindings.classCounts).GetHandle();
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;

			// The Z-prepass draws inside the native depth pass, so everything after sees DCLF's objects in depth.
			auto depth = LoadedDepth(a_recording, a_bindings.depth);
			begin.depth = &depth;
			begin.debugName = "DCLF depth";
			const auto run = a_recording.Run();
			const MainRecording pass{ frame, commands, sequences, count, classCounts, begin, depth, a_prepared.stats,
				a_prepared.stats ? a_prepared.stats->RangeOf(run) : UINT32_MAX, run };
			if (zPrepass) {
				RecordDepthDraws(a_bindings, a_prepared, a_recording, pass);
				return;
			}
			RecordColourDraws(a_bindings, a_prepared, a_recording, pass, colors);
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		bool depth = false;
		bool phaseTwo = false;
	};
}

namespace DCLF::Features::MainView
{
	namespace
	{
		// Whether a_context's epoch records the main depth shape. Searched at graph build.
		bool DepthShape(const Api::PassContext& a_context)
		{
			const std::uint32_t row = Draws::EpochById(a_context.epochId);
			return row != Draws::kNoEpoch && Draws::EpochTraitsOf(row).index == static_cast<std::uint32_t>(Api::MainShape::Depth);
		}

		std::shared_ptr<org::RenderPass> MainDraws(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::MainOpaquePass>(a_context.Main(), a_context.segment, DepthShape(a_context));
		}

		std::shared_ptr<org::RenderPass> DepthDrawPhaseTwo(const Api::PassContext& a_context)
		{
			return a_context.PhaseTwo() ? std::make_shared<Draws::MainOpaquePass>(a_context.Main(), a_context.segment, DepthShape(a_context), true) : nullptr;
		}
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		a_context.RegisterPass({ .key = "main-view.z.depth", .passName = "cs.dclf.z.depth", .kind = Api::PassKind::Render, .make = &MainDraws });
		a_context.RegisterPass({ .key = "main-view.depth-phase2", .passName = "cs.dclf.depth-phase2", .kind = Api::PassKind::Render, .make = &DepthDrawPhaseTwo });
		a_context.RegisterPass({ .key = "main-view.colour", .passName = "cs.dclf.main-opaque", .kind = Api::PassKind::Render, .make = &MainDraws });
	}
}
