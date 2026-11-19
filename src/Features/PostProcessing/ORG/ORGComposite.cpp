#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/LinearLighting.h"
#	include "Features/PostProcessing/Composite.h"
#	include "Features/PostProcessing/HistogramAutoExposure.h"
#	include "Features/PostProcessing/LocalExposure.h"
#	include "Globals.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"

namespace ORGPostChain
{
	namespace
	{

		// COMPOSITE_RESOURCES order.
		struct CompositeBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle texColor, texBloom, texFlare, texGlare, texAdaptation, texLocalExposureLog, texLuminanceGrid,
				texBlurredLuminance, linearSampler, bloomSampler;
			ShaderBindings::Latch autoExposure, localExposure, composite;
		};

		/**
		* @brief Composite::Draw on the graph. A contributor whose published outputs are missing is left out. With none the image passes
		* through, unless linear lighting stores the scene pre-exposed: then the draw still divides the pre-exposure out.
		*/
		class CompositeEffect final : public Effect
		{
		public:
			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				drawn = false;
				const auto bloom = a_builder.Find(Published::kBloomOutput);
				const auto flare = a_builder.Find(Published::kLensFlareOutput);
				const auto glare = a_builder.Find(Published::kGlare);
				const auto adaptation = a_builder.Find(Published::kAdaptation);
				const auto autoExposure = a_builder.FindLatch<HistogramAutoExposure::AutoExposureCB>(Published::kAutoExposureConstants);
				const auto logLuminance = a_builder.Find(Published::kLogLuminance);
				const auto grid = a_builder.Find(Published::kLuminanceGrid);
				const auto blurred = a_builder.Find(Published::kBlurredLuminance);
				const auto localExposure = a_builder.FindLatch<LocalExposure::LocalExposureCB>(Published::kLocalExposureConstants);
				if (a_builder.Failed())
					return;

				hasBloom = static_cast<bool>(bloom);
				hasFlare = static_cast<bool>(flare);
				const bool hasGlare = static_cast<bool>(glare);
				const bool hasExposure = adaptation && autoExposure;
				const bool hasLocalExposure = logLuminance && grid && blurred && localExposure;
				// Nothing to add or resolve, so nothing drawn, as Composite::Draw. The chain counts on this draw to resolve exposure.
				const bool preExposed = globals::features::linearLighting.IsLinearLightingActive();
				if (!hasBloom && !hasFlare && !hasGlare && !hasExposure && !hasLocalExposure && !preExposed)
					return;

				if (!linearSampler || !borderSampler) {
					// Linear clamp for Local Exposure and Lens Flare, linear border for COD Bloom.
					linearSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					borderSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{ .addressU = rhi::AddressMode::Border,
						.addressV = rhi::AddressMode::Border,
						.addressW = rhi::AddressMode::Border });
					if (!linearSampler || !borderSampler) {
						a_builder.Fail("its samplers could not be made");
						return;
					}
				}

				// One permutation per set of contributors.
				std::vector<std::pair<std::string, std::string>> defines;
				for (const auto& [on, name] : { std::pair{ hasBloom, "HAS_BLOOM" }, std::pair{ hasFlare, "HAS_LENS_FLARE" }, std::pair{ hasGlare, "HAS_GLARE" },
						 std::pair{ hasExposure, "HAS_EXPOSURE" }, std::pair{ hasLocalExposure, "HAS_LOCAL_EXPOSURE" } }) {
					if (on)
						defines.emplace_back(name, "");
				}
				// As Composite::SetupResources: the resolved exposure can exceed FP16 before the tone mapper.
				const auto format = preExposed ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
				auto program = a_builder.Raster("PostProcessing/Composite/composite.ps.hlsl", ShaderBindings::Words<CompositeBlock>(), { format },
					std::move(defines), L"PSComposite");
				auto image = a_builder.CreateImage("PostProcessing::Composite::Output", format, D3D11_BIND_RENDER_TARGET);
				constants = a_builder.Latch<Composite::CompositeCB>();

				// Left-out contributors' entries are never read, so they get placeholders.
				const auto srvOrUnused = [](bool a_on, const Image& a_image) { return a_on ? Bind::Srv(a_image) : Bind::Unused(); };
				const auto unusedLatch = Bind::Latch(constants);
				a_builder.Draw({ .name = "draw",
					.program = std::move(program),
					.bindings = { Bind::Shared(), Bind::Srv(a_builder.Current()), srvOrUnused(hasBloom, bloom), srvOrUnused(hasFlare, flare),
						srvOrUnused(hasGlare, glare), srvOrUnused(hasExposure, adaptation), srvOrUnused(hasLocalExposure, logLuminance),
						srvOrUnused(hasLocalExposure, grid), srvOrUnused(hasLocalExposure, blurred), Bind::Handle(linearSampler.Index()),
						Bind::Handle(borderSampler.Index()), hasExposure ? Bind::Latch(*autoExposure) : unusedLatch,
						hasLocalExposure ? Bind::Latch(*localExposure) : unusedLatch, Bind::Latch(constants) },
					.target = image });
				a_builder.SetCurrent(image);
				const auto& shape = a_builder.GetShape();
				size = float2(static_cast<float>(shape.width), static_cast<float>(shape.height));
				flareSize = float2(static_cast<float>((std::max)(shape.width / 2, 1u)), static_cast<float>((std::max)(shape.height / 2, 1u)));
				drawn = true;
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				if (!drawn)
					return;
				auto data = static_cast<Composite&>(a_feature).Constants(size, hasBloom, hasFlare);
				// The size of the image Lens Flare published, not of its D3D11 texture.
				if (hasFlare) {
					data.FlareSize[0] = flareSize.x;
					data.FlareSize[1] = flareSize.y;
				}
				a_commit.Write(constants, data);
			}

		private:
			org::OwnedDescriptorBinding linearSampler, borderSampler;
			LatchRegion<Composite::CompositeCB> constants;
			float2 size;
			float2 flareSize;
			bool hasBloom = false;
			bool hasFlare = false;
			bool drawn = false;
		};
	}

	std::unique_ptr<Effect> MakeComposite() { return std::make_unique<CompositeEffect>(); }
}

#endif
