#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/CODBloom.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <Render/OwnedDescriptorBinding.h>
#	include <rhi_helpers.h>

#	include <format>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/CODBloom/bloom.ps.hlsl";
		constexpr std::uint32_t kMips = static_cast<std::uint32_t>(CODBloom::s_BloomMips);

		// BLOOM_RESOURCES order.
		struct BloomBlock
		{
			ShaderBindings::Handle texColor, texBloomIn, sampColor;
			ShaderBindings::Latch bloom;
		};

		/**
		* @brief CODBloom::Draw on the graph: threshold into mip 0, downsamples, then upsamples back to mip 1 by blending. Mip 1 is published.
		* The D3D11 upsample uses a blend factor, but a reused recording would freeze it. So the shader returns CurrentMipMult in alpha,
		* the blend scales by source alpha, and alpha is not written.
		*/
		class CODBloomEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				// The mip blend settings set the pass count, so another count is another build.
				usedMips = static_cast<CODBloom&>(a_feature).UsedMips();
				a_key.push_back(static_cast<std::uint64_t>(usedMips));
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				if (!sampler) {
					// Linear, transparent black border, every mip.
					sampler = ShaderBindings::CreateSampler(
						{ .addressU = rhi::AddressMode::Border, .addressV = rhi::AddressMode::Border, .addressW = rhi::AddressMode::Border });
					if (!sampler) {
						a_builder.Fail("its sampler could not be made");
						return;
					}
				}

				// kMAIN_COPY's size and format, the whole mip chain.
				const auto format = a_builder.GetShape().mainCopyFormat;
				const auto words = ShaderBindings::Words<BloomBlock>();
				auto threshold = a_builder.Raster(kShader, words, { format }, {}, L"PS_Threshold");
				auto downsampleFirst = a_builder.Raster(kShader, words, { format }, { { "FIRST_MIP", "" } }, L"PS_Downsample");
				auto downsample = a_builder.Raster(kShader, words, { format }, {}, L"PS_Downsample");
				const RasterProgram::Target accumulate{ .format = rhi::helpers::ToRHI(format),
					.blend = { .enable = true,
						.srcColor = rhi::BlendFactor::One,
						.dstColor = rhi::BlendFactor::SrcAlpha,
						.colorOp = rhi::BlendOp::Add,
						.srcAlpha = rhi::BlendFactor::One,
						.dstAlpha = rhi::BlendFactor::Zero,
						.alphaOp = rhi::BlendOp::Add,
						.writeMask = static_cast<rhi::ColorWriteEnable>(rhi::ColorWriteEnable::R | rhi::ColorWriteEnable::G | rhi::ColorWriteEnable::B) } };
				auto upsample = a_builder.RasterTargets(kShader, words, { accumulate }, {}, L"PS_Upsample");
				auto image = a_builder.CreateMipChain("PostProcessing::CODBloom::Bloom", format, D3D11_BIND_RENDER_TARGET, kMips);
				if (a_builder.Failed())
					return;

				const auto linear = Bind::Handle(sampler.Index());
				constants[0] = a_builder.Latch<CODBloom::BloomCB>();
				a_builder.Draw({ .name = "threshold",
					.program = threshold,
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Unused(), linear, Bind::Latch(constants[0]) },
					.target = image.Mip(0) });

				// The downsamples read no constants and take the threshold's region.
				for (int i = 0; i < usedMips - 1; ++i) {
					const auto mip = static_cast<std::uint32_t>(i);
					a_builder.Draw({ .name = std::format("downsample-{}", mip + 1),
						.program = i == 0 ? downsampleFirst : downsample,
						.bindings = { Bind::Unused(), Bind::Srv(image.Mip(mip)), linear, Bind::Latch(constants[0]) },
						.target = image.Mip(mip + 1) });
				}

				for (int i = usedMips - 2; i >= 1; --i) {
					const auto mip = static_cast<std::uint32_t>(i);
					constants[mip] = a_builder.Latch<CODBloom::BloomCB>();
					a_builder.Draw({ .name = std::format("upsample-{}", mip),
						.program = upsample,
						.bindings = { Bind::Unused(), Bind::Srv(image.Mip(mip + 1)), linear, Bind::Latch(constants[mip]) },
						.target = image.Mip(mip) });
				}

				a_builder.Publish(Published::kBloomOutput, image.Mip(1));
				builtMips = usedMips;
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				const auto& bloom = static_cast<CODBloom&>(a_feature);
				a_commit.Write(constants[0], bloom.Constants(0));
				for (int i = builtMips - 2; i >= 1; --i)
					a_commit.Write(constants[static_cast<std::size_t>(i)], bloom.Constants(i));
			}

		private:
			org::OwnedDescriptorBinding sampler;
			// [0] is the threshold's, [i] the upsample into mip i.
			std::array<LatchRegion<CODBloom::BloomCB>, kMips> constants{};
			int usedMips = static_cast<int>(kMips);   // from Update, for the next build
			int builtMips = static_cast<int>(kMips);  // the registered build's, for Commit
		};
	}

	std::unique_ptr<Effect> MakeCODBloom() { return std::make_unique<CODBloomEffect>(); }
}

#endif
