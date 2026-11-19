#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/LinearLighting.h"
#	include "Features/PostProcessing/HistogramAutoExposure.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>

#	include <algorithm>
#	include <atomic>
#	include <cmath>
#	include <cstring>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/HistogramAutoExposure/histogram.cs.hlsl";
		constexpr std::uint32_t kBins = 256;

		// HISTOGRAM_RESOURCES order, every entry.
		struct HistogramBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle texColor, rwHistogram, rwAdaptation;
			ShaderBindings::Latch autoExposure;
		};

		// The two parts of the readback.
		enum Part : std::uint32_t
		{
			kHistogramPart,
			kAdaptationPart,
		};

		D3D11_BUFFER_DESC StructuredDesc(UINT a_count, UINT a_stride)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = a_count * a_stride;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = a_stride;
			return desc;
		}

		/**
		* @brief HistogramAutoExposure::Draw on the graph. The histogram and adaptation are history buffers owned here, cleared once by a
		* one-shot CS_Reset until a frame with it reached the GPU. CS_Reset reads no constants, so it may run on a frame the chain does not.
		*/
		class HistogramAutoExposureEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>&) override
			{
				// The adaptation starts over after a shader reload, as ClearShaderCache's resetAdaptation.
				const auto generation = a_feature.shaderGeneration.load(std::memory_order_relaxed);
				if (generation != shaderGeneration) {
					shaderGeneration = generation;
					reset = true;
				}
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				if (!histogram || !adaptation) {
					const GraphResources::Options history{ .lifetime = GraphResources::Lifetime::History };
					histogram = std::make_unique<GraphResources::StructuredBuffer>(StructuredDesc(kBins, sizeof(std::uint32_t)), kBins,
						"PostProcessing::HistogramAutoExposure::Histogram", history);
					adaptation = std::make_unique<GraphResources::StructuredBuffer>(StructuredDesc(1, sizeof(float)), 1,
						"PostProcessing::HistogramAutoExposure::Adaptation", history);
					clearPending = true;
					reset = true;
				}
				if (!*histogram || !*adaptation) {
					a_builder.Fail("its history buffers could not be made");
					histogram.reset();
					adaptation.reset();
					return;
				}
				if (!readback)
					readback = RunReadback::Create("PostProcessing::HistogramAutoExposure::Readback",
						{ kBins * static_cast<std::uint32_t>(sizeof(std::uint32_t)), static_cast<std::uint32_t>(sizeof(float)) });
				if (!readback) {
					a_builder.Fail("its readback buffer could not be made");
					return;
				}
				readback->Build(a_builder);
				a_builder.Register(*histogram);
				a_builder.Register(*adaptation);

				const auto words = ShaderBindings::Words<HistogramBlock>();
				constants = a_builder.Latch<HistogramAutoExposure::AutoExposureCB>();
				const auto input = a_builder.Current();
				const auto shape = a_builder.GetShape();
				const auto groups = HistogramAutoExposure::HistogramGroups(shape.width, shape.height);

				if (clearPending)
					a_builder.Dispatch({ .name = "reset",
						.program = a_builder.Compute(kShader, words, {}, L"CS_Reset"),
						.bindings = { Bind::Shared(), Bind::Unused(), Bind::Uav(histogram->Graph()), Bind::Uav(adaptation->Graph()), Bind::Latch(constants) },
						.once = true });
				a_builder.Dispatch({ .name = "histogram",
					.program = a_builder.Compute(kShader, words, {}, L"CS_Histogram"),
					.bindings = { Bind::Shared(), Bind::Srv(input), Bind::Uav(histogram->Graph()), Bind::Uav(adaptation->Graph()), Bind::Latch(constants) },
					.groups = { groups[0], groups[1], 1 } });
				// Before CS_Average clears the histogram, as the D3D11 staging copy.
				readback->AddBuffer(a_builder, "histogram-readback", kHistogramPart, histogram->Graph());
				a_builder.Dispatch({ .name = "average",
					.program = a_builder.Compute(kShader, words, {}, L"CS_Average"),
					.bindings = { Bind::Shared(), Bind::Unused(), Bind::Uav(histogram->Graph()), Bind::Uav(adaptation->Graph()), Bind::Latch(constants) } });
				readback->AddBuffer(a_builder, "adaptation-readback", kAdaptationPart, adaptation->Graph());
				a_builder.Publish(Published::kAdaptation, Image{ adaptation->Graph(), DXGI_FORMAT_UNKNOWN });
				a_builder.PublishLatch(Published::kAutoExposureConstants, constants);
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				auto& exposure = static_cast<HistogramAutoExposure&>(a_feature);
				a_commit.Write(constants, exposure.Constants(reset || clearPending));
				reset = false;
				if (readback) {
					readback->Commit(a_commit);
					TakeReadbacks(exposure);
				}
			}

			// The one-shot clear ran before or with the first frame of this build.
			void Executed(PostProcessFeature&) override
			{
				clearPending = false;
				if (readback)
					readback->Executed();
			}

		private:
			/**
			* @brief Moves arrived readbacks into the UI values and the pre-exposure metering. Every run copies both parts.
			* As the D3D11 staging copies, only what the settings UI or the metering under linear lighting wants is taken.
			*/
			void TakeReadbacks(HistogramAutoExposure& a_exposure)
			{
				const auto wanted = a_exposure.WantedReadbacks();
				const bool metering = globals::features::linearLighting.IsLinearLightingActive();
				const auto bins = readback->Take(kHistogramPart);
				if (wanted.histogram && bins.size() >= sizeof(std::uint32_t) * kBins)
					std::memcpy(a_exposure.histogramData.data(), bins.data(), sizeof(std::uint32_t) * kBins);
				const auto adapted = readback->Take(kAdaptationPart);
				// CS_ORG_PP_LOG: the adapted luminance and the exposure Composite makes of it, at most every 120 frames.
				const auto frame = static_cast<std::uint64_t>(globals::state->frameCount);
				if (LogOn() && adapted.size() >= sizeof(float) && (loggedFrame == 0 || frame >= loggedFrame + 120)) {
					loggedFrame = frame;
					float value = 0.0f;
					std::memcpy(&value, adapted.data(), sizeof(value));
					const auto constants = a_exposure.Constants(false);
					const float gain = constants.ExposureTarget * constants.ExposureCompensation /
					                   (std::min)((std::max)(value, constants.AdaptationRange.x), constants.AdaptationRange.y);
					logger::info("[ORG] Post Processing log: frame {}: adapted luminance {} (range {} to {}), so the global exposure is {}", frame, value,
						constants.AdaptationRange.x, constants.AdaptationRange.y, gain);
				}
				if ((wanted.adaptation || metering) && adapted.size() >= sizeof(float)) {
					float value = 0.0f;
					std::memcpy(&value, adapted.data(), sizeof(value));
					// As GetPreExposure takes the D3D11 staging copy: the graph's adaptation meters the next frames' pre-exposure.
					if (std::isfinite(value) && value > 0.0f)
						a_exposure.preExposureLuminance = value;
					a_exposure.adaptationValue = value;
					a_exposure.adaptationReadbackFrame = static_cast<int64_t>(globals::state->frameCount);
				}
			}

			std::unique_ptr<GraphResources::StructuredBuffer> histogram;
			std::unique_ptr<GraphResources::StructuredBuffer> adaptation;
			std::shared_ptr<RunReadback> readback;  // kept across builds
			LatchRegion<HistogramAutoExposure::AutoExposureCB> constants;
			std::uint64_t shaderGeneration = 0;
			std::uint64_t loggedFrame = 0;  // CS_ORG_PP_LOG's last line
			bool clearPending = false;      // new history buffers not cleared yet
			bool reset = true;
		};
	}

	std::unique_ptr<Effect> MakeHistogramAutoExposure() { return std::make_unique<HistogramAutoExposureEffect>(); }
}

#endif
