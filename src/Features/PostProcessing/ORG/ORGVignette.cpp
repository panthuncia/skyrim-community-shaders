// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/PostProcessing/ORGPostEffect.h"

#include "Features/PostProcessing/Vignette.h"
#include "RenderGraph/ShaderBindings.h"

namespace ORGPostChain
{
	namespace
	{
		// VIGNETTE_RESOURCES order.
		struct VignetteBlock
		{
			ShaderBindings::Handle texColor;
			ShaderBindings::Latch vignette;
		};

		/** @brief One build's latch region. */
		class VignetteRun final : public Run
		{
		public:
			explicit VignetteRun(LatchRegion<Vignette::VignetteCB> a_constants) :
				constants(a_constants) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				// The dynamic size is this frame's, so it goes to the constants.
				a_commit.Write(constants, static_cast<Vignette&>(a_feature).Constants(a_commit.DynamicSize()));
			}

		private:
			LatchRegion<Vignette::VignetteCB> constants;
		};

		/** @brief Vignette::Draw on the graph. */
		class VignetteEffect final : public Effect
		{
		public:
			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t>) override { LoadProgram(a_programs); }

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				// The output takes kMAIN_COPY's format.
				const auto format = a_builder.GetShape().mainCopyFormat;
				auto program = LoadProgram(a_builder);
				auto image = a_builder.CreateImage("PostProcessing::Vignette::Output", format, D3D11_BIND_RENDER_TARGET);
				const auto constants = a_builder.Latch<Vignette::VignetteCB>();
				a_builder.Draw({ .name = "draw",
					.program = std::move(program),
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Latch(constants) },
					.target = image,
					.viewport = a_builder.DynamicViewport() });
				a_builder.SetCurrent(image);
				if (a_builder.Failed())
					return nullptr;
				return std::make_shared<VignetteRun>(constants);
			}

		private:
			/** @brief Listed in Programs and built on the programs lane. */
			static std::shared_ptr<const RasterProgram> LoadProgram(ProgramSource& a_programs)
			{
				return a_programs.Raster("PostProcessing/Vignette/vignette.ps.hlsl", ShaderBindings::Words<VignetteBlock>(), { a_programs.GetShape().mainCopyFormat });
			}
		};
	}

	std::unique_ptr<Effect> MakeVignette() { return std::make_unique<VignetteEffect>(); }
}

