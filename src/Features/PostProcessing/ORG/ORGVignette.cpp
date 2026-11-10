#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/Vignette.h"
#	include "RenderGraph/ShaderBindings.h"

namespace ORGPostChain
{
	namespace
	{
		// vignette.ps.hlsl's push block: VIGNETTE_RESOURCES, in order.
		struct VignetteBlock
		{
			ShaderBindings::Handle texColor;
			ShaderBindings::Latch vignette;
		};

		/** @brief Vignette::Draw on the graph: one fullscreen draw over the dynamic-resolution area, into kMAIN_COPY's format. */
		class VignetteEffect final : public Effect
		{
		public:
			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				// As Vignette::SetupResources: the output takes kMAIN_COPY's format.
				const auto format = a_builder.GetShape().mainCopyFormat;
				auto program = a_builder.Raster("PostProcessing/Vignette/vignette.ps.hlsl", ShaderBindings::Words<VignetteBlock>(), { format });
				auto image = a_builder.CreateImage("PostProcessing::Vignette::Output", format, D3D11_BIND_RENDER_TARGET);
				constants = a_builder.Latch<Vignette::VignetteCB>();
				a_builder.Draw({ .name = "draw",
					.program = std::move(program),
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Latch(constants) },
					.target = image,
					.viewport = a_builder.DynamicViewport() });
				a_builder.SetCurrent(image);
				size = float2(a_builder.GetShape().dynamicWidth, a_builder.GetShape().dynamicHeight);
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				a_commit.Write(constants, static_cast<Vignette&>(a_feature).Constants(size));
			}

		private:
			LatchRegion<Vignette::VignetteCB> constants;
			float2 size;
		};
	}

	std::unique_ptr<Effect> MakeVignette() { return std::make_unique<VignetteEffect>(); }
}

#endif
