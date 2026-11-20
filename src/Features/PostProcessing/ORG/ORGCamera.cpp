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
		// CAMERA_RESOURCES order, then the CameraFrame latch. Both passes share it.
		struct CameraBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle inputTexture, colorSampler;
			ShaderBindings::Latch camera;
		};

		/** @brief One build's latch region. */
		class CameraRun final : public Run
		{
		public:
			explicit CameraRun(LatchRegion<Camera::CameraCB> a_constants) :
				constants(a_constants) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				a_commit.Write(constants, static_cast<Camera&>(a_feature).Constants(a_commit.DynamicSize()));
			}

		private:
			LatchRegion<Camera::CameraCB> constants;
		};

		/**
		* @brief Camera::Draw on the graph: the lens pass into a transient lens image of the chain, then the finish pass into the
		* output. The chain declares it on the effect's enabled. Film grain reads SharedData's frame count, so the chain uploads the
		* shared constants.
		*/
		class CameraEffect final : public Effect
		{
		public:
			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t>) override { LoadPrograms(a_programs); }

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				if (!sampler) {
					// Linear, clamped, every mip.
					sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					if (!sampler) {
						a_builder.Fail("its sampler could not be made");
						return nullptr;
					}
				}
				// The output takes kMAIN_COPY's format.
				const auto format = a_builder.GetShape().mainCopyFormat;
				auto [lensProgram, finishProgram] = LoadPrograms(a_builder);
				auto lens = a_builder.CreateImage("PostProcessing::Camera::Lens", format, D3D11_BIND_RENDER_TARGET);
				auto image = a_builder.CreateImage("PostProcessing::Camera::Output", format, D3D11_BIND_RENDER_TARGET);
				const auto constants = a_builder.Latch<Camera::CameraCB>();
				const auto colorSampler = Bind::Handle(sampler.Index());
				a_builder.Draw({ .name = "lens",
					.program = std::move(lensProgram),
					.bindings = { Bind::Shared(), Bind::Srv(a_builder.Current()), colorSampler, Bind::Latch(constants) },
					.target = lens,
					.viewport = a_builder.DynamicViewport() });
				a_builder.Draw({ .name = "finish",
					.program = std::move(finishProgram),
					.bindings = { Bind::Shared(), Bind::Srv(lens), colorSampler, Bind::Latch(constants) },
					.target = image,
					.viewport = a_builder.DynamicViewport() });
				a_builder.SetCurrent(image);
				if (a_builder.Failed())
					return nullptr;
				return std::make_shared<CameraRun>(constants);
			}

		private:
			struct CameraPrograms
			{
				std::shared_ptr<const RasterProgram> lens, finish;
			};

			static CameraPrograms LoadPrograms(ProgramSource& a_programs)
			{
				const auto format = a_programs.GetShape().mainCopyFormat;
				const auto words = ShaderBindings::Words<CameraBlock>();
				return { a_programs.Raster("PostProcessing/Camera/camera.ps.hlsl", words, { format }),
					a_programs.Raster("PostProcessing/Camera/camera_finish.ps.hlsl", words, { format }) };
			}

			org::OwnedDescriptorBinding sampler;
		};
	}

	std::unique_ptr<Effect> MakeCamera() { return std::make_unique<CameraEffect>(); }
}

#endif
