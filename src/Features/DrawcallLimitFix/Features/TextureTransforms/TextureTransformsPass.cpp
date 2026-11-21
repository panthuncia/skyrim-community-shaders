// The texture transforms' pass, first in the depth segment. It copies the installed payload's animated material rows into the frame's
// copy, then stores this frame's TexcoordOffset and frame variables into that copy. The installed rows are only read.

// Main/Resources.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"

#include "RenderGraph/BufferRefs.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <cstdint>
#include "Features/DrawcallLimitFix/Core/Gpu/SetupPrograms.h"

#include <memory>
#include <span>
#include <vector>

namespace DCLF::Draws
{
	namespace
	{
		// Two fixed dispatches: the copy (phase 0), then the stores (phase 1).
		constexpr const char* kShader = "DrawcallLimitFix/Features/TextureTransforms/TextureTransformsCS.hlsl";
		struct Constants
		{
			std::uint32_t latchIndex = 0, latchOffset = 0;
			std::uint32_t phase = 0;
			std::uint32_t padding = 0;
		};
		static_assert(sizeof(Constants) == 16);
		constexpr std::uint32_t kConstantWords = sizeof(Constants) / 4;
		constexpr std::uint32_t kGroups = 32;  // of TextureTransformsCS's 64 threads
	}

	struct TextureTransformsBindings
	{
		RenderGraph::DeclaredRefs copy;  // the frame's copy it writes, with the ref the main-rows latches carry (animatedRows)
	};

	struct TextureTransformsPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		// An external epoch's execution has its own full barriers at entry and exit, and none of its passes read the copy.
		bool outerBarriers = true;
	};

	/**
	 * @brief Copies the animated material rows into the frame's copy, then stores the frame's values, which DCLF's draws then read.
	 * The copy is read by address and declared by no pass, so this pass places full memory barriers before, between and after.
	 * It declares the sequences as written so the graph keeps it before the depth build.
	 */
	class TextureTransformsPass final : public org::TypedRenderGraphPass<TextureTransformsPass, TextureTransformsPrepared, TextureTransformsBindings>
	{
	public:
		TextureTransformsPass(std::shared_ptr<Resources> a_resources, std::shared_ptr<const ComputeProgram> a_program, RenderGraphRuntime::Segment a_segment,
			const GraphConditions& a_conditions) :
			resources(std::move(a_resources)), program(std::move(a_program)), segment(a_segment),
			outerBarriers(!a_conditions.cpuPipeline) {}

		TextureTransformsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::UavView> noViews{};
			(void)a_builder.UnorderedAccess(*resources->sequences, noViews);
			TextureTransformsBindings bindings;
			FrameValues::Get().DeclarePoolWrite(a_builder, ::DCLF::Features::TextureTransforms::CopyPool(), FrameValues::kReadPools, bindings.copy);
			DeclarePayloadReads(a_builder);
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>&) const {}

		TextureTransformsPrepared Prepare(const TextureTransformsBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			TextureTransformsPrepared prepared{};
			const auto frame = CurrentFrame(a_preparation, segment);
			if (!program || !frame || !frame->latch || !BuildsDraws(a_preparation, *resources, segment))
				return prepared;
			RenderGraph::CheckDeclaredRefs(a_bindings.copy, a_preparation, "cs.dclf.z.texture-transforms");
			prepared.program = program;
			prepared.latch = frame->latch;
			prepared.outerBarriers = outerBarriers;
			return prepared;
		}

		static void Record(const TextureTransformsBindings&, const TextureTransformsPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			RecordApply(a_recording, a_frame);
		}

		static void RecordApply(org::PassRecordContext& a_recording, const TextureTransformsPrepared& a_frame)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			Constants constants{};
			SetLatchWords(constants, *a_frame.latch, a_recording.Run());
			auto& commands = a_recording.Commands();
			const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
			const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
			if (a_frame.outerBarriers)
				commands.Barriers(batch);
			// Copy every animated row before storing any entry, because an entry's row may belong to another group.
			constants.phase = 0;
			RecordDispatch(a_recording, *a_frame.program, constants, kGroups);
			commands.Barriers(batch);
			constants.phase = 1;
			RecordDispatch(a_recording, *a_frame.program, constants, kGroups);
			if (a_frame.outerBarriers)
				commands.Barriers(batch);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const ComputeProgram> program;
		RenderGraphRuntime::Segment segment;
		bool outerBarriers = true;  // the graph's conditions at its build
	};
}

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		const SetupPrograms::Compute kProgram{ "DCLF texture transforms", [] {
			return ComputeProgram::Desc{ .source = Draws::kShader, .constantWords = Draws::kConstantWords };
		} };

		std::shared_ptr<const void> Setup(const Api::PassSetup&)
		{
			auto program = kProgram.Get();
			SetCopyAvailable(true);
			return program;
		}

		std::shared_ptr<org::RenderPass> MakePass(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::TextureTransformsPass>(a_context.Main(), a_context.State<ComputeProgram>(), a_context.segment, a_context.Conditions());
		}
	}

	void RegisterPass(Api::InstallContext& a_context)
	{
		// The depth latch's stage word names the pool's rows. 0 means no producer this frame, and the shader writes nothing.
		a_context.RegisterPass({ .key = "texture-transforms.apply", .passName = "cs.dclf.z.texture-transforms", .make = &MakePass, .setup = &Setup,
			.latchWord = &RowsIndex, .animatedRows = &AnimatedRowsAddress, .cpuPipeline = true });
	}
}
