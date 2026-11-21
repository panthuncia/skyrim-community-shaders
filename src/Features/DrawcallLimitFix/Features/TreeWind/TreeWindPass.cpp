// Tree wind's pass: next frame's wind (TreeWindCS) on the compute queue in the depth epoch, beside its raster work.

// Shapes.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"

#include "RenderGraph/ComputeProgram.h"
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct TreeWindBindings
	{
		org::DeclaredViewToken trees, clocks, frame;
		std::array<org::DeclaredViewToken, 2> wind;
	};

	struct TreeWindFramePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		Features::TreeWind::TreeWindConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Advances each tree's clock and writes its wind into this frame's buffer, which next frame's draws read.
	 * Nothing this frame reads it, so it overlaps the raster work and readers need not declare it.
	 */
	class TreeWindPass final : public org::TypedRenderGraphPass<TreeWindPass, TreeWindFramePrepared, TreeWindBindings>
	{
	public:
		explicit TreeWindPass(std::shared_ptr<const Features::TreeWind::TreeWindScene> a_scene) :
			scene(std::move(a_scene)) {}

		TreeWindBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(AsyncComputeQueue());
			TreeWindBindings bindings{};
			bindings.trees = a_builder.ShaderResource(*scene->trees).View();
			bindings.frame = a_builder.ShaderResource(scene->treeFrameBuffer).View();
			bindings.clocks = a_builder.UnorderedAccess(*scene->treeClocks).View();
			for (std::uint32_t h = 0; h < 2; ++h)
				bindings.wind[h] = a_builder.UnorderedAccess(*scene->treeWindRows[h]).View();
			FrameValues::Get().DeclareReads(a_builder, FrameValues::kReadPools);
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>&) const {}

		TreeWindFramePrepared Prepare(const TreeWindBindings&, const org::PassPrepareContext& a_preparation) const
		{
			TreeWindFramePrepared prepared{};
			if (!scene->treeWind || !scene->treeFrameBuffer)
				return prepared;
			prepared.program = scene->treeWind;
			// Full capacity. The shader stops at the count.
			const std::uint64_t trees = std::min({ NamedUnitsOf(a_preparation, scene->trees), NamedUnitsOf(a_preparation, scene->treeClocks),
				NamedUnitsOf(a_preparation, scene->treeWindRows[0]), NamedUnitsOf(a_preparation, scene->treeWindRows[1]) });
			constexpr std::uint32_t group = Features::TreeWind::kTreeWindGroup;
			prepared.groups = static_cast<std::uint32_t>((trees + 1 + group - 1) / group);
			return prepared;
		}

		static void Record(const TreeWindBindings& a_bindings, const TreeWindFramePrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto constants = a_frame.constants;
			constants.treesIndex = a_recording.Index(a_bindings.trees);
			constants.clocksIndex = a_recording.Index(a_bindings.clocks);
			constants.frameIndex = a_recording.Index(a_bindings.frame);
			for (std::uint32_t h = 0; h < 2; ++h)
				constants.windIndices[h] = a_recording.Index(a_bindings.wind[h]);
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline(a_recording));
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, Features::TreeWind::kTreeWindConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<const Features::TreeWind::TreeWindScene> scene;
	};
}

namespace DCLF::Features::TreeWind
{
	namespace
	{
		std::shared_ptr<const void> Setup(const Api::PassSetup& a_setup)
		{
			auto scene = SharedSceneOf(a_setup.scene);
			if (!scene || !scene->treeWind || !scene->trees)
				return nullptr;
			return scene;
		}

		std::shared_ptr<org::RenderPass> MakePass(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::TreeWindPass>(a_context.State<TreeWindScene>());
		}
	}

	void RegisterPass(Api::InstallContext& a_context)
	{
		a_context.RegisterPass({ .key = "tree-wind.next-frame", .passName = "cs.dclf.z.tree-wind", .queue = Api::PassQueue::Compute, .make = &MakePass,
			.setup = &Setup, .cpuPipeline = true });
	}
}
