// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/PostProcessing/ORGPostEffect.h"

#include "Features/PostProcessing/Composite.h"
#include "Features/PostProcessing/HistogramAutoExposure.h"
#include "Features/PostProcessing/LocalExposure.h"
#include "Features/PostProcessing/PhysicalLens.h"
#include "Globals.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"

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

		// The HAS_PHYSICAL_LENS list: the above, then the lens's entries.
		struct CompositeLensBlock
		{
			CompositeBlock base;
			ShaderBindings::Handle texPhysicalFlare, texPhysicalLens, texWaveKernel, texWaveRegions, texWaveSources, texWaveTiles, texWaveTail;
			ShaderBindings::Latch diffraction;
		};

		/**
		* @brief Composite::Draw on the graph. A contributor whose published outputs are missing is left out. With none the image passes
		* through, unless linear lighting stores the scene pre-exposed: then the draw still divides the pre-exposure out.
		* Physical Lens's permutation is built whenever it published, and its parts are switched per frame by the constants.
		*/
		/** @brief One build's draw: its constants region and the contributors it binds. Not drawn: the image passes through. */
		class CompositeRun final : public Run
		{
		public:
			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				if (!drawn)
					return;
				auto& composite = static_cast<Composite&>(a_feature);
				bool physicalFlare = false, physicalLens = false;
				if (hasPhysical) {
					// The lens committed this frame before Composite, in chain order.
					const auto* lens = composite.owner->GetPipelineFeature<PhysicalLens>(PostProcessing::FeaturePipelineIndex::PhysicalLens);
					physicalFlare = lens->orgComposite.flare;
					physicalLens = lens->orgComposite.cameraMap;
				}
				auto data = composite.Constants(size, hasBloom, hasFlare, physicalFlare, physicalLens);
				// The size of the image Lens Flare published, not of its D3D11 texture.
				if (hasFlare) {
					data.FlareSize[0] = flareSize.x;
					data.FlareSize[1] = flareSize.y;
				}
				a_commit.Write(constants, data);
			}

			LatchRegion<Composite::CompositeCB> constants;
			float2 size;
			float2 flareSize;
			bool hasBloom = false;
			bool hasFlare = false;
			bool hasPhysical = false;
			bool drawn = false;
		};

		/** @brief Which contributors this chain has: what earlier effects publish, and linear lighting. Plan and build read the same. */
		struct Contributors
		{
			bool bloom = false, flare = false, glare = false, exposure = false, localExposure = false, physical = false, preExposed = false;

			/** @brief Nothing to add or resolve, so nothing is drawn, as Composite::Draw. The chain counts on this draw to resolve exposure. */
			bool Drawn() const { return bloom || flare || glare || exposure || localExposure || physical || preExposed; }

			static Contributors Of(const ProgramSource& a_chain)
			{
				Contributors c;
				c.bloom = a_chain.Published(Published::kBloomOutput);
				c.flare = a_chain.Published(Published::kLensFlareOutput);
				c.glare = a_chain.Published(Published::kGlare);
				c.exposure = a_chain.Published(Published::kAdaptation) && a_chain.Published(Published::kAutoExposureConstants);
				c.localExposure = a_chain.Published(Published::kLogLuminance) && a_chain.Published(Published::kLuminanceGrid) &&
				                  a_chain.Published(Published::kBlurredLuminance) && a_chain.Published(Published::kLocalExposureConstants);
				// Physical Lens publishes these only while one of its parts is on (its key), so its permutation follows them.
				c.physical = a_chain.Published(Published::kPhysicalLensMapping) && a_chain.Published(Published::kPhysicalLensDiffraction);
				c.preExposed = a_chain.Linear();
				return c;
			}
		};

		/** @brief As Composite::SetupResources: the resolved exposure can exceed FP16 before the tone mapper. */
		DXGI_FORMAT CompositeFormat(const Contributors& a_contributors)
		{
			return a_contributors.preExposed ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
		}

		/** @brief One permutation per set of contributors. Null when nothing is drawn. */
		std::shared_ptr<const RasterProgram> CompositeProgram(ProgramSource& a_programs, const Contributors& a_contributors)
		{
			if (!a_contributors.Drawn())
				return nullptr;
			std::vector<std::pair<std::string, std::string>> defines;
			for (const auto& [on, name] : { std::pair{ a_contributors.bloom, "HAS_BLOOM" }, std::pair{ a_contributors.flare, "HAS_LENS_FLARE" },
					 std::pair{ a_contributors.glare, "HAS_GLARE" }, std::pair{ a_contributors.exposure, "HAS_EXPOSURE" },
					 std::pair{ a_contributors.localExposure, "HAS_LOCAL_EXPOSURE" }, std::pair{ a_contributors.physical, "HAS_PHYSICAL_LENS" } }) {
				if (on)
					defines.emplace_back(name, "");
			}
			const auto words = a_contributors.physical ? ShaderBindings::Words<CompositeLensBlock>() : ShaderBindings::Words<CompositeBlock>();
			return a_programs.Raster("PostProcessing/Composite/composite.ps.hlsl", words, { CompositeFormat(a_contributors) }, std::move(defines), L"PSComposite");
		}

		class CompositeEffect final : public Effect
		{
		public:
			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				CompositeProgram(a_programs, Contributors::Of(a_programs));
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				auto run = std::make_shared<CompositeRun>();
				const auto bloom = a_builder.Find(Published::kBloomOutput);
				const auto flare = a_builder.Find(Published::kLensFlareOutput);
				const auto glare = a_builder.Find(Published::kGlare);
				const auto adaptation = a_builder.Find(Published::kAdaptation);
				const auto autoExposure = a_builder.FindLatch<HistogramAutoExposure::AutoExposureCB>(Published::kAutoExposureConstants);
				const auto logLuminance = a_builder.Find(Published::kLogLuminance);
				const auto grid = a_builder.Find(Published::kLuminanceGrid);
				const auto blurred = a_builder.Find(Published::kBlurredLuminance);
				const auto localExposure = a_builder.FindLatch<LocalExposure::LocalExposureCB>(Published::kLocalExposureConstants);
				const auto lensMapping = a_builder.Find(Published::kPhysicalLensMapping);
				const auto lensDiffraction = a_builder.FindLatch<PhysicalLensImaging::DiffractionParameters>(Published::kPhysicalLensDiffraction);
				// Published only while the lens builds its ghosts or starburst.
				const auto lensGhosts = a_builder.Find(Published::kPhysicalLensGhosts);
				const auto waveKernel = a_builder.Find(Published::kPhysicalLensWaveKernel);
				const auto waveRegions = a_builder.Find(Published::kPhysicalLensWaveRegions);
				const auto waveSources = a_builder.Find(Published::kPhysicalLensWaveSources);
				const auto waveTiles = a_builder.Find(Published::kPhysicalLensWaveTiles);
				const auto waveTail = a_builder.Find(Published::kPhysicalLensWaveTail);
				if (a_builder.Failed())
					return nullptr;

				// The same contributors the plan saw (Programs), from what this build's earlier effects published.
				const auto contributors = Contributors::Of(a_builder);
				const bool hasBloom = run->hasBloom = contributors.bloom;
				const bool hasFlare = run->hasFlare = contributors.flare;
				const bool hasGlare = contributors.glare;
				const bool hasExposure = contributors.exposure;
				const bool hasLocalExposure = contributors.localExposure;
				// Which of the lens's parts draw is decided each frame, from the lens's commit.
				const bool hasPhysical = run->hasPhysical = contributors.physical;
				if (!contributors.Drawn())
					return run;

				if (!linearSampler || !borderSampler) {
					// Linear clamp for Local Exposure and Lens Flare, linear border for COD Bloom.
					linearSampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearClamp);
					borderSampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearBorder);
					if (!linearSampler || !borderSampler) {
						a_builder.Fail("its samplers could not be made");
						return nullptr;
					}
				}

				const auto format = CompositeFormat(contributors);
				auto program = CompositeProgram(a_builder, contributors);
				if (!program)
					return nullptr;
				auto image = a_builder.CreateImage("PostProcessing::Composite::Output", format, D3D11_BIND_RENDER_TARGET);
				const auto constants = run->constants = a_builder.Latch<Composite::CompositeCB>();

				// Left-out contributors' entries are never read, so they get placeholders.
				const auto srvOrUnused = [](bool a_on, const Image& a_image) { return a_on ? Bind::Srv(a_image) : Bind::Unused(); };
				const auto unusedLatch = Bind::Latch(constants);
				std::vector<Bind> bindings{ Bind::Shared(), Bind::Srv(a_builder.Current()), srvOrUnused(hasBloom, bloom), srvOrUnused(hasFlare, flare),
					srvOrUnused(hasGlare, glare), srvOrUnused(hasExposure, adaptation), srvOrUnused(hasLocalExposure, logLuminance),
					srvOrUnused(hasLocalExposure, grid), srvOrUnused(hasLocalExposure, blurred), Bind::Handle(linearSampler.Index()),
					Bind::Handle(borderSampler.Index()), hasExposure ? Bind::Latch(*autoExposure) : unusedLatch,
					hasLocalExposure ? Bind::Latch(*localExposure) : unusedLatch, Bind::Latch(constants) };
				if (hasPhysical) {
					// Without the lens's ghosts or starburst their branches stay off (HasPhysicalFlare, DiffractionEnabled), so they get placeholders.
					const auto arrayOrUnused = [](const Image& a_image) { return a_image ? Bind::SrvArray(a_image) : Bind::Unused(); };
					const auto bufferOrUnused = [](const Image& a_image) { return a_image ? Bind::Srv(a_image) : Bind::Unused(); };
					bindings.insert(bindings.end(), { arrayOrUnused(lensGhosts), Bind::SrvArray(lensMapping), bufferOrUnused(waveKernel), bufferOrUnused(waveRegions),
														bufferOrUnused(waveSources), bufferOrUnused(waveTiles), arrayOrUnused(waveTail), Bind::Latch(*lensDiffraction) });
				}
				a_builder.Draw({ .name = "draw", .program = std::move(program), .bindings = std::move(bindings), .target = image });
				a_builder.SetCurrent(image);
				const auto& shape = a_builder.GetShape();
				run->size = float2(static_cast<float>(shape.width), static_cast<float>(shape.height));
				run->flareSize = float2(static_cast<float>((std::max)(shape.width / 2, 1u)), static_cast<float>((std::max)(shape.height / 2, 1u)));
				run->drawn = true;
				return run;
			}

		private:
			org::OwnedDescriptorBinding linearSampler, borderSampler;
		};
	}

	std::unique_ptr<Effect> MakeComposite() { return std::make_unique<CompositeEffect>(); }
}

