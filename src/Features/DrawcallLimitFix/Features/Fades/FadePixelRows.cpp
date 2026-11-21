// Per-object pixel-stage values (DCLFPixelObjectRow), such as material fades, EmitColor and the alpha test reference.
// Lighting.hlsl used to compute these per pixel although they are constant per draw.
// PixelRowsCS writes one row per object slot once a frame, first in the depth segment. Pixel shaders read them at t116.

#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"

#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/SetupPrograms.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "RenderGraph/BufferRefs.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace DCLF::Draws
{
	namespace
	{
		constexpr const char* kPixelRowsShader = "DrawcallLimitFix/Features/Fades/PixelRowsCS.hlsl";
		struct PixelRowsConstants
		{
			std::uint32_t latchIndex = 0, latchOffset = 0;
			std::uint32_t recordTexturesLo = 0, recordTexturesHi = 0;
			std::uint32_t lodFadeFrameLo = 0, lodFadeFrameHi = 0;
			std::uint32_t padding[2]{};
		};
		static_assert(sizeof(PixelRowsConstants) == 32);
		constexpr std::uint32_t kPixelRowsConstantWords = sizeof(PixelRowsConstants) / 4;
		constexpr std::uint32_t kPixelRowsGroups = 64;  // groups of 64 threads that stride over the rows
		constexpr std::uint32_t kPixelObjectRowBytes = 48;  // sizeof(DCLFPixelObjectRow)
	}

	struct PixelRowsBindings
	{
		RenderGraph::DeclaredRefs rows;
	};

	struct PixelRowsPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		RenderGraph::BufferRef frameConstants;
		// A CPU-pipeline epoch has its own barriers at entry and exit, so these are skipped there.
		bool outerBarriers = true;
	};

	/**
	 * @brief Writes every pixel row before any draw of the frame reads it.
	 * Full memory barriers on both sides keep guest-mode epochs ordered, because the pool's backing is reused across frames.
	 * Declaring the sequences as written keeps the pass before the depth build.
	 */
	class PixelRowsPass final : public org::TypedRenderGraphPass<PixelRowsPass, PixelRowsPrepared, PixelRowsBindings>
	{
	public:
		PixelRowsPass(std::shared_ptr<Resources> a_resources, std::shared_ptr<const ComputeProgram> a_program, RenderGraphRuntime::Segment a_segment,
			Api::FrameValuePoolId a_pool, const GraphConditions& a_conditions) :
			resources(std::move(a_resources)), program(std::move(a_program)), segment(a_segment), pool(a_pool),
			outerBarriers(!a_conditions.cpuPipeline) {}

		PixelRowsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noReads{};
			const std::span<const org::UavView> noViews{};
			(void)a_builder.ShaderResource(resources->frameConstants, noReads);
			(void)a_builder.UnorderedAccess(*resources->sequences, noViews);
			PixelRowsBindings bindings;
			FrameValues::Get().DeclarePoolWrite(a_builder, pool, FrameValues::kReadPlacements | FrameValues::kReadShading | FrameValues::kReadPools, bindings.rows);
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(resources->frameConstantsAddress); }

		PixelRowsPrepared Prepare(const PixelRowsBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			const auto frame = CurrentFrame(a_preparation, segment);
			if (!program || !frame || !frame->latch || !BuildsDraws(a_preparation, *resources, segment))
				return {};
			RenderGraph::CheckDeclaredRefs(a_bindings.rows, a_preparation, "cs.dclf.z.pixel-rows");
			return { program, frame->latch, resources->frameConstantsRef, outerBarriers };
		}

		static void Record(const PixelRowsBindings&, const PixelRowsPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			RecordRows(a_recording, a_frame);
		}

		static void RecordRows(org::PassRecordContext& a_recording, const PixelRowsPrepared& a_frame)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.frameConstants)
				return;
			auto& commands = a_recording.Commands();
			const org::RunRef& run = a_recording.Run();
			const RenderGraph::BufferRef textures = a_frame.frameConstants + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes + offsetof(DrawBindings, textures);
			const RenderGraph::BufferRef lodFadeFrame = a_frame.frameConstants + LodFadeFrameAddress(0);
			PixelRowsConstants constants{};
			SetLatchWords(constants, *a_frame.latch, run);
			constants.recordTexturesLo = textures.viewSlot;
			constants.recordTexturesHi = textures.byteOffset;
			constants.lodFadeFrameLo = lodFadeFrame.viewSlot;
			constants.lodFadeFrameHi = lodFadeFrame.byteOffset;
			const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
			const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
			if (!a_frame.outerBarriers) {
				RecordDispatch(a_recording, *a_frame.program, constants, kPixelRowsGroups);
				return;
			}
			commands.Barriers(batch);
			RecordDispatch(a_recording, *a_frame.program, constants, kPixelRowsGroups);
			commands.Barriers(batch);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const ComputeProgram> program;
		RenderGraphRuntime::Segment segment;
		Api::FrameValuePoolId pool;
		bool outerBarriers = true;  // fixed at graph build
	};
}

namespace DCLF::Features::Fades
{
	namespace
	{
		Api::FrameValuePoolId pixelRowsPool;
		std::uint32_t kickedRows = 0;  // render thread

		// Nothing is sampled or sent, because only the GPU writes these rows.
		Api::FrameValueInput PixelRowsKick(const Api::FrameValuePoolKick& a_kick)
		{
			DCLF_ZONE("CS.DCLF.FV.Kick.PixelRows");
			kickedRows = static_cast<std::uint32_t>(a_kick.objectSlots);
			return { nullptr, a_kick.objectSlots };
		}
		void PixelRowsSample(Api::FrameValuePoolProducer&) {}
		void PixelRowsSend(Api::FrameValuePoolProducer&) {}

		void SelectPixelRowsBinding(Api::FrameBindingSelect& a_select)
		{
			if (a_select.renderThread)
				a_select.index = FrameValues::Get().PoolIndex(pixelRowsPool);
		}

		Api::FrameRows PixelFrameRows()
		{
			const auto& values = FrameValues::Get();
			const std::uint64_t address = values.PoolAddress(pixelRowsPool);
			const RenderGraph::BufferRef ref = values.PoolRef(pixelRowsPool);
			return { address, ref, ref ? kickedRows : 0u };
		}

		const SetupPrograms::Compute kPixelRowsProgram{ "DCLF object rows",
			[] { return ComputeProgram::Desc{ .source = Draws::kPixelRowsShader, .constantWords = Draws::kPixelRowsConstantWords }; } };

		std::shared_ptr<const void> PixelRowsSetup(const Api::PassSetup&)
		{
			return kPixelRowsProgram.Get();
		}

		std::shared_ptr<org::RenderPass> MakePixelRows(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::PixelRowsPass>(a_context.Main(), a_context.State<ComputeProgram>(), a_context.segment, pixelRowsPool, a_context.Conditions());
		}
	}

	void RegisterPixelRows(Api::InstallContext& a_context)
	{
		pixelRowsPool = a_context.RegisterFrameValuePool({ .name = "pixel-rows", .stride = Draws::kPixelObjectRowBytes, .deviceAddress = true,
			.unorderedAccess = true,  // without it, a buffer view ref's UAV is a null view on D3D12
			.kick = &PixelRowsKick,
			.sample = &PixelRowsSample, .send = &PixelRowsSend });
		a_context.RegisterFrameBinding({ .name = "object rows SRV", .textureRegister = kPixelObjectsRegister, .select = &SelectPixelRowsBinding });
		a_context.RegisterPass({ .key = "fades.pixel-rows", .passName = "cs.dclf.z.pixel-rows", .make = &MakePixelRows, .setup = &PixelRowsSetup,
			.frameRows = &PixelFrameRows, .cpuPipeline = true });
	}
}
