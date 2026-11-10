#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/Camera.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"

namespace ORGPostChain
{
	namespace
	{
		// camera.ps.hlsl's push block: CAMERA_RESOURCES, in order.
		struct CameraBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle inputTexture, colorSampler;
			ShaderBindings::Latch camera;
		};

		/**
		 * @brief Camera::Draw on the graph: one fullscreen draw over the dynamic-resolution area, into kMAIN_COPY's format. The
		 * film grain reads SharedData's frame count, so the chain uploads CS's shared constants (Bind::Shared).
		 */
		class CameraEffect final : public Effect
		{
		public:
			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				if (!sampler) {
					// Camera::colorSampler's description: linear, clamped, every mip.
					sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					if (!sampler) {
						a_builder.Fail("its sampler could not be made");
						return;
					}
				}
				// As Camera::SetupResources: the output takes kMAIN_COPY's format.
				const auto& shape = a_builder.GetShape();
				const auto format = shape.mainCopyFormat;
				auto program = a_builder.Raster("PostProcessing/Camera/camera.ps.hlsl", ShaderBindings::Words<CameraBlock>(), { format });
				auto image = a_builder.CreateImage("PostProcessing::Camera::Output", format, D3D11_BIND_RENDER_TARGET);
				constants = a_builder.Latch<Camera::CameraCB>();
				a_builder.Draw({ .name = "draw",
					.program = std::move(program),
					.bindings = { Bind::Shared(), Bind::Srv(a_builder.Current()), Bind::Handle(sampler.Index()), Bind::Latch(constants) },
					.target = image,
					.viewport = a_builder.DynamicViewport() });
				a_builder.SetCurrent(image);
				size = float2(shape.dynamicWidth, shape.dynamicHeight);
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				a_commit.Write(constants, static_cast<Camera&>(a_feature).Constants(size));
			}

		private:
			org::OwnedDescriptorBinding sampler;
			LatchRegion<Camera::CameraCB> constants;
			float2 size;
		};
	}

	std::unique_ptr<Effect> MakeCamera() { return std::make_unique<CameraEffect>(); }
}

#endif
