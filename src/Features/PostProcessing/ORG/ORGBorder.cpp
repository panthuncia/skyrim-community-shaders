#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/Border.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <Resources/ExternalTextureResource.h>

namespace ORGPostChain
{
	namespace
	{
		// BORDER_RESOURCES order.
		struct BorderBlock
		{
			ShaderBindings::Handle inputTexture, depthTexture, outputTexture, motionTexture;
			ShaderBindings::Latch border;
		};

		/** @brief One build's latch region. */
		class BorderRun final : public Run
		{
		public:
			explicit BorderRun(LatchRegion<Border::BorderCB> a_constants) :
				constants(a_constants) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				// The shader skips pixels past this frame's dynamic size.
				a_commit.Write(constants, static_cast<Border&>(a_feature).Constants(a_commit.DynamicSize()));
			}

		private:
			LatchRegion<Border::BorderCB> constants;
		};

		/** @brief Border::Draw on the graph. Also zeroes the motion vectors under the border. ClearMotionVectorsForFrameGen stays on D3D11. */
		class BorderEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature&, std::vector<std::uint64_t>& a_observed) override
			{
				depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
				motion = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMOTION_VECTOR));
				if (!depth || !motion)
					return "the main depth or the motion vectors cannot be imported";
				a_observed.push_back(reinterpret_cast<std::uintptr_t>(depth.Key()));
				a_observed.push_back(reinterpret_cast<std::uintptr_t>(motion.Key()));
				return nullptr;
			}

			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t>) override { LoadProgram(a_programs); }

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				const auto& shape = a_builder.GetShape();
				// The output takes kMAIN_COPY's format.
				auto image = a_builder.CreateImage("PostProcessing::Border::Output", shape.mainCopyFormat, D3D11_BIND_UNORDERED_ACCESS);
				const auto constants = a_builder.Latch<Border::BorderCB>();
				// The dispatch covers kMAIN. The shader skips what is past the dynamic size in the constants.
				a_builder.Dispatch({ .name = "draw",
					.program = LoadProgram(a_builder),
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Srv(depth), Bind::Uav(image), Bind::Uav(motion), Bind::Latch(constants) },
					.groups = { (shape.width + 7) >> 3, (shape.height + 7) >> 3, 1 } });
				a_builder.SetCurrent(image);
				if (a_builder.Failed())
					return nullptr;
				return std::make_shared<BorderRun>(constants);
			}

		private:
			/** @brief Listed in Programs and built on the programs lane. */
			static std::shared_ptr<const ComputeProgram> LoadProgram(ProgramSource& a_programs)
			{
				return a_programs.Compute("PostProcessing/Border/border.cs.hlsl", ShaderBindings::Words<BorderBlock>());
			}

			// This frame's imports. The build binds them, and their identities are observed.
			FrameAnchors::Texture depth, motion;
		};
	}

	std::unique_ptr<Effect> MakeBorder() { return std::make_unique<BorderEffect>(); }
}

#endif
