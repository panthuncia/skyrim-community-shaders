// Creates the HZB with the main view's resources and rebuilds it from the main depth in the depth epoch, with OMS's HZB pass.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/SetupPrograms.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Features/GpuOcclusion/GpuOcclusion.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <ORGModuleServices/Passes/HzbPass.h>
#include <Render/PassBuilders.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/PixelBuffer.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace DCLF::Features::GpuOcclusion
{
	// The SPD finished-group counter is zeroed by the first depth commit, then reset by each dispatch's last group.
	struct HzbState
	{
		std::shared_ptr<const ComputeProgram> program;
	};

	namespace
	{
		// The HZB holds raw NDC depth. Skyrim's z_ndc = A - B/w grows with distance and the test only compares order, so
		// linearising would add a step that can go wrong for no gain. Each level keeps the maximum, the farthest surface under it,
		// and padding loads as the far plane. That keeps the test conservative, since a footprint with some sky never culls.
		constexpr float kHzbFarPlane = 1.0f;

		const SetupPrograms::Compute kHzbProgram{ "DCLF HZB",
			[] { return ComputeProgram::Desc{ .source = org::services::kHzbShader, .constantWords = org::services::kHzbConstantWords }; } };

		// Mip 0 is half the depth's next power of two, so log2 of a screen extent picks the mip.
		// Padding holds the far plane, which can only prevent culling, never cause it.
		std::shared_ptr<const void> Setup(const Api::DepthPyramidSetup& a_setup, Api::DepthPyramid& a_out)
		{
			const auto layout = org::services::PlanHzbLayout(a_setup.width, a_setup.height);
			a_out.width = layout.width;
			a_out.height = layout.height;
			a_out.mips = layout.mips;
			a_out.texture = org::PixelBuffer::CreateSharedUnmaterialized(org::services::HzbTextureDescription(layout));
			a_out.texture->SetName("cs.dclf.hzb");
			auto state = std::make_shared<HzbState>();
			a_out.scratch = Draws::CreateWords(1, true, "cs.dclf.hzb-counter");

			state->program = kHzbProgram.Get();
			return state;
		}

		// Runs after DCLF's depth draws, so it holds native and DCLF occluders. On AE it runs before the first-person depth and
		// room stencil draws, which only means fewer occluders. SPD avoids a full barrier per level.
		std::shared_ptr<org::RenderPass> MakePass(const Api::PassContext& a_context)
		{
			if (!a_context.PhaseTwo())
				return nullptr;
			const std::shared_ptr<Draws::Resources> resources = a_context.Main();
			const auto state = Extensions::DepthPyramidState<HzbState>(a_context);
			const auto segment = a_context.segment;
			org::services::HzbDesc desc{};
			desc.program.kernel = state ? state->program : nullptr;
			desc.program.reduction = org::services::HzbReduction::Max;
			desc.declareDepth = [resources](org::RenderPassBuilder& a_builder) { return a_builder.ShaderResource(resources->nativeDepth).View(); };
			desc.hzb = resources->hzb;
			desc.counter = resources->hzbScratch;
			desc.layout = { resources->hzbWidth, resources->hzbHeight, resources->hzbMips };
			desc.farValue = kHzbFarPlane;
			// The frame's render size. The depth image can be larger than the viewport.
			desc.source = [resources, segment](const org::PassPrepareContext& a_preparation) -> org::services::HzbSource {
				const auto frame = Draws::CurrentFrame(a_preparation, segment);
				if (!frame)
					return {};
				return { frame->width ? frame->width : resources->width, frame->height ? frame->height : resources->height };
			};
			desc.revision = [segment](std::vector<std::uint64_t>& a_out) { a_out.push_back(static_cast<std::uint64_t>(segment)); };
			return org::services::MakeHzbPass(std::move(desc));
		}
	}

	void RegisterPyramid(Api::InstallContext& a_context)
	{
		a_context.RegisterDepthPyramid({ .resource = "cs.dclf.hzb", .scratchResource = "cs.dclf.hzb-counter", .setup = &Setup });
		a_context.RegisterPass({ .key = "gpu-occlusion.hzb", .passName = "cs.dclf.hzb", .make = &MakePass });
	}
}
