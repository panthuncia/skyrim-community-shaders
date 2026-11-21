// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/PostProcessing/ORGPostEffect.h"

#include "Features/PostProcessing/CODBloom.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"

#include <Render/OwnedDescriptorBinding.h>
#include <rhi_helpers.h>

#include <format>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/CODBloom/bloom.ps.hlsl";
		constexpr std::uint32_t kMips = static_cast<std::uint32_t>(CODBloom::s_BloomMips);

		// BLOOM_RESOURCES order.
		struct BloomBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle texColor, texBloomIn, sampColor;
			ShaderBindings::Latch bloom;
		};

		/** @brief One build's constants: [0] is the threshold's, [i] the upsample into mip i, for the mips it was built with. */
		class CODBloomRun final : public Run
		{
		public:
			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				const auto& bloom = static_cast<CODBloom&>(a_feature);
				a_commit.Write(constants[0], bloom.Constants(0));
				for (int i = builtMips - 2; i >= 1; --i)
					a_commit.Write(constants[static_cast<std::size_t>(i)], bloom.Constants(i));
			}

			std::array<LatchRegion<CODBloom::BloomCB>, kMips> constants{};
			int builtMips = static_cast<int>(kMips);
		};

		/**
		* @brief CODBloom::Draw on the graph: threshold into mip 0, downsamples, then upsamples back to mip 1 by blending. Mip 1 is published.
		* The D3D11 upsample uses a blend factor, but a reused recording would freeze it. So the shader returns CurrentMipMult in alpha,
		* the blend scales by source alpha, and alpha is not written.
		* The pass count follows the used mips, a variable derived from the blend factors, so a blend of the factors (a scene layer)
		* builds nothing unless the count changes.
		*/
		class CODBloomEffect final : public Effect
		{
		public:
			void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies& a_dependencies) override
			{
				usedMips = RenderGraph::Frame::DeriveFromSetting<std::array<float, CODBloom::s_BloomMips - 1>>("PostProcessing.CODBloom.usedMips",
					static_cast<CODBloom&>(a_feature).settings.MipBlendFactor,
					[](const std::array<float, CODBloom::s_BloomMips - 1>& a_factors) { return static_cast<std::uint32_t>(CODBloom::UsedMipsFor(a_factors)); });
				a_dependencies.push_back(RenderGraph::Frame::DependencyOf(usedMips));
			}

			void Key(const RenderGraph::Frame::Inputs& a_values, PostProcessFeature&, std::vector<std::uint64_t>& a_key) override
			{
				a_key.push_back(a_values.Get(usedMips));
			}

			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				LoadPrograms(a_programs);
				a_programs.Publishes(Published::kBloomOutput);
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t> a_key) override
			{
				if (!sampler) {
					// Linear, transparent black border, every mip.
					sampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearBorder);
					if (!sampler) {
						a_builder.Fail("its sampler could not be made");
						return nullptr;
					}
				}
				// Key's one word. Zero before the variable has a value (no graph) builds every mip.
				const auto keyMips = a_key.empty() ? 0u : static_cast<std::uint32_t>(a_key[0]);
				const int mips = keyMips >= 2 && keyMips <= kMips ? static_cast<int>(keyMips) : static_cast<int>(kMips);

				// kMAIN_COPY's size and format, the whole mip chain.
				const auto format = a_builder.GetShape().mainCopyFormat;
				auto [threshold, downsampleFirst, downsample, upsample] = LoadPrograms(a_builder);
				auto image = a_builder.CreateMipChain("PostProcessing::CODBloom::Bloom", format, D3D11_BIND_RENDER_TARGET, kMips);
				if (a_builder.Failed())
					return nullptr;

				auto run = std::make_shared<CODBloomRun>();
				auto& constants = run->constants;
				const auto linear = Bind::Handle(sampler.Index());
				constants[0] = a_builder.Latch<CODBloom::BloomCB>();
				a_builder.Draw({ .name = "threshold",
					.program = threshold,
					.bindings = { Bind::Shared(), Bind::Srv(a_builder.Current()), Bind::Unused(), linear, Bind::Latch(constants[0]) },
					.target = image.Mip(0) });

				// The downsamples read no constants and take the threshold's region.
				for (int i = 0; i < mips - 1; ++i) {
					const auto mip = static_cast<std::uint32_t>(i);
					a_builder.Draw({ .name = std::format("downsample-{}", mip + 1),
						.program = i == 0 ? downsampleFirst : downsample,
						.bindings = { Bind::Shared(), Bind::Unused(), Bind::Srv(image.Mip(mip)), linear, Bind::Latch(constants[0]) },
						.target = image.Mip(mip + 1) });
				}

				for (int i = mips - 2; i >= 1; --i) {
					const auto mip = static_cast<std::uint32_t>(i);
					constants[mip] = a_builder.Latch<CODBloom::BloomCB>();
					a_builder.Draw({ .name = std::format("upsample-{}", mip),
						.program = upsample,
						.bindings = { Bind::Shared(), Bind::Unused(), Bind::Srv(image.Mip(mip + 1)), linear, Bind::Latch(constants[mip]) },
						.target = image.Mip(mip) });
				}

				a_builder.Publish(Published::kBloomOutput, image.Mip(1));
				run->builtMips = mips;
				return run;
			}

		private:
			struct BloomPrograms
			{
				std::shared_ptr<const RasterProgram> threshold, downsampleFirst, downsample, upsample;
			};

			/** @brief Listed in Programs and built on the programs lane. Every mip count uses all four. */
			static BloomPrograms LoadPrograms(ProgramSource& a_programs)
			{
				const auto format = a_programs.GetShape().mainCopyFormat;
				const auto words = ShaderBindings::Words<BloomBlock>();
				const RasterProgram::Target accumulate{ .format = rhi::helpers::ToRHI(format),
					.blend = { .enable = true,
						.srcColor = rhi::BlendFactor::One,
						.dstColor = rhi::BlendFactor::SrcAlpha,
						.colorOp = rhi::BlendOp::Add,
						.srcAlpha = rhi::BlendFactor::One,
						.dstAlpha = rhi::BlendFactor::Zero,
						.alphaOp = rhi::BlendOp::Add,
						.writeMask = static_cast<rhi::ColorWriteEnable>(rhi::ColorWriteEnable::R | rhi::ColorWriteEnable::G | rhi::ColorWriteEnable::B) } };
				return { .threshold = a_programs.Raster(kShader, words, { format }, {}, L"PS_Threshold"),
					.downsampleFirst = a_programs.Raster(kShader, words, { format }, { { "FIRST_MIP", "" } }, L"PS_Downsample"),
					.downsample = a_programs.Raster(kShader, words, { format }, {}, L"PS_Downsample"),
					.upsample = a_programs.RasterTargets(kShader, words, { accumulate }, {}, L"PS_Upsample") };
			}

			org::OwnedDescriptorBinding sampler;
			RenderGraph::Frame::Vars::Var<std::uint32_t> usedMips;
		};
	}

	std::unique_ptr<Effect> MakeCODBloom() { return std::make_unique<CODBloomEffect>(); }
}

