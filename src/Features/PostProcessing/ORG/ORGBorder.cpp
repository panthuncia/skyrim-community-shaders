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
		// border.cs.hlsl's push block: BORDER_RESOURCES, in order.
		struct BorderBlock
		{
			ShaderBindings::Handle inputTexture, depthTexture, outputTexture, motionTexture;
			ShaderBindings::Latch border;
		};

		/**
		 * @brief Border::Draw on the graph: one dispatch over the dynamic-resolution area that writes the bordered image and zeroes
		 * the engine's motion vectors under the border. Border::ClearMotionVectorsForFrameGen stays on D3D11 (Upscaling runs it).
		 */
		class BorderEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature&, std::vector<std::uint64_t>& a_key) override
			{
				// The targets Border::Draw binds: kMAIN's depth and the motion vectors.
				depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
				motion = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMOTION_VECTOR));
				if (!depth || !motion)
					return "the main depth or the motion vectors cannot be imported";
				a_key.push_back(reinterpret_cast<std::uintptr_t>(depth.get()));
				a_key.push_back(reinterpret_cast<std::uintptr_t>(motion.get()));
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				const auto& shape = a_builder.GetShape();
				// As Border::SetupResources: the output takes kMAIN_COPY's format.
				auto image = a_builder.CreateImage("PostProcessing::Border::Output", shape.mainCopyFormat, D3D11_BIND_UNORDERED_ACCESS);
				constants = a_builder.Latch<Border::BorderCB>();
				size = float2(shape.dynamicWidth, shape.dynamicHeight);
				a_builder.Dispatch({ .name = "draw",
					.program = a_builder.Compute("PostProcessing/Border/border.cs.hlsl", ShaderBindings::Words<BorderBlock>()),
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Srv(depth), Bind::Uav(image), Bind::Uav(motion), Bind::Latch(constants) },
					.groups = { (static_cast<std::uint32_t>(size.x) + 7) >> 3, (static_cast<std::uint32_t>(size.y) + 7) >> 3, 1 } });
				a_builder.SetCurrent(image);
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				a_commit.Write(constants, static_cast<Border&>(a_feature).Constants(size));
			}

		private:
			std::shared_ptr<org::Resource> depth, motion;
			LatchRegion<Border::BorderCB> constants;
			float2 size;
		};
	}

	std::unique_ptr<Effect> MakeBorder() { return std::make_unique<BorderEffect>(); }
}

#endif
