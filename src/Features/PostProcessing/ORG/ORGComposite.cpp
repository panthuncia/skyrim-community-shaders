#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/Composite.h"
#	include "Features/PostProcessing/HistogramAutoExposure.h"
#	include "Features/PostProcessing/LocalExposure.h"
#	include "RenderGraph/ShaderBindings.h"

namespace ORGPostChain
{
	namespace
	{
		constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // Composite::texOutput's

		// The names COD Bloom and Lens Flare publish under (their kBloomOutput, kLensFlareOutput): bloom's mip 1, and the ghosts
		// and halo at half size, max(w / 2, 1) x max(h / 2, 1).
		constexpr std::string_view kBloomName = "PostProcessing::CODBloom::Bloom";
		constexpr std::string_view kLensFlareName = "PostProcessing::LensFlare::GhostHalo";

		// composite.ps.hlsl's push block: COMPOSITE_RESOURCES, in order.
		struct CompositeBlock
		{
			ShaderBindings::Handle texColor, texBloom, texFlare, texGlare, texAdaptation, texLocalExposureLog, texLuminanceGrid,
				texBlurredLuminance, linearSampler, bloomSampler;
			ShaderBindings::Latch autoExposure, localExposure, composite;
		};

		/**
		 * @brief Composite::Draw on the graph: one fullscreen draw that adds the bloom, lens flare and glare to the image and applies
		 * the exposure. It reads the contributors' side outputs and constants by the names they publish (ORGPostChain::Published).
		 * A contributor whose outputs are missing is left out of the permutation, and with none the image passes through
		 * untouched, as on D3D11.
		 */
		class CompositeEffect final : public Effect
		{
		public:
			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				drawn = false;
				const auto bloom = a_builder.Find(kBloomName);
				const auto flare = a_builder.Find(kLensFlareName);
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
				// As Composite::Draw: nothing to add, nothing drawn.
				if (!hasBloom && !hasFlare && !hasGlare && !hasExposure && !hasLocalExposure)
					return;

				if (!linearSampler || !borderSampler) {
					// The samplers Composite::Draw binds: linear clamp (Local Exposure's, Lens Flare's), linear border (COD Bloom's).
					linearSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					borderSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{ .addressU = rhi::AddressMode::Border,
						.addressV = rhi::AddressMode::Border,
						.addressW = rhi::AddressMode::Border });
					if (!linearSampler || !borderSampler) {
						a_builder.Fail("its samplers could not be made");
						return;
					}
				}

				// As Composite::CompileRasterShaders: one permutation per set of contributors.
				std::vector<std::pair<std::string, std::string>> defines;
				for (const auto& [on, name] : { std::pair{ hasBloom, "HAS_BLOOM" }, std::pair{ hasFlare, "HAS_LENS_FLARE" }, std::pair{ hasGlare, "HAS_GLARE" },
						 std::pair{ hasExposure, "HAS_EXPOSURE" }, std::pair{ hasLocalExposure, "HAS_LOCAL_EXPOSURE" } }) {
					if (on)
						defines.emplace_back(name, "");
				}
				auto program = a_builder.Raster("PostProcessing/Composite/composite.ps.hlsl", ShaderBindings::Words<CompositeBlock>(), { kFormat },
					std::move(defines), L"PSComposite");
				auto image = a_builder.CreateImage("PostProcessing::Composite::Output", kFormat, D3D11_BIND_RENDER_TARGET);
				constants = a_builder.Latch<Composite::CompositeCB>();

				// A permutation never reads the entries of the contributors it leaves out: those get placeholders.
				const auto srvOrUnused = [](bool a_on, const Image& a_image) { return a_on ? Bind::Srv(a_image) : Bind::Unused(); };
				const auto unusedLatch = Bind::Latch(constants);
				a_builder.Draw({ .name = "draw",
					.program = std::move(program),
					.bindings = { Bind::Srv(a_builder.Current()), srvOrUnused(hasBloom, bloom), srvOrUnused(hasFlare, flare),
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
