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
#	include <OpenRenderGraph/Runtime.h>
#	include <Render/Runtime/IReadbackService.h>
#	include <Render/Runtime/ReadbackCapturePlan.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>

#	include <atomic>
#	include <cmath>
#	include <cstring>
#	include <mutex>
#	include <optional>

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

		/** @brief Shared by the render thread, the readback passes on the graph host thread, and the readback callbacks. */
		struct Readback
		{
			std::shared_ptr<org::runtime::IReadbackService> service;
			std::atomic<bool> wantHistogram = false;
			std::atomic<bool> wantAdaptation = false;

			std::mutex mutex;
			std::optional<std::array<std::uint32_t, kBins>> histogram;  // guarded, the latest result not taken yet
			std::optional<float> adaptation;                            // guarded
		};

		enum class Capture : std::uint8_t
		{
			Histogram,
			Adaptation,
		};

		struct ReadbackFrame
		{
			org::PreparedResourceReference source;
			org::runtime::ReadbackCapturePlan plan;
		};

		/**
		* @brief Only on frames something wants it, as the D3D11 staging copies: the settings UI, or the pre-exposure metering under linear
		* lighting. No revision, so it is prepared every frame and never replayed.
		*/
		class ReadbackPass final : public org::TypedRenderGraphPass<ReadbackPass, ReadbackFrame>
		{
		public:
			ReadbackPass(std::shared_ptr<Readback> a_shared, std::shared_ptr<org::Resource> a_source, Capture a_capture) :
				shared(std::move(a_shared)), source(std::move(a_source)), capture(a_capture) {}

			void Declare(org::PassBuilder& a_builder)
			{
				a_builder.CopySource(source);
				a_builder.PreferQueue(org::QueueKind::Graphics);
			}

			ReadbackFrame Prepare(const org::PassPrepareContext& a_preparation)
			{
				const bool histogram = capture == Capture::Histogram;
				if (!shared->service || !(histogram ? shared->wantHistogram : shared->wantAdaptation).load(std::memory_order_acquire))
					return {};
				ReadbackFrame data{};
				data.source = a_preparation.CaptureResource(source->GetGlobalResourceID());
				// The readback service's thread. The result waits for the next Commit.
				org::ReadbackCaptureCallback onDone = [weak = std::weak_ptr<Readback>(shared), histogram](org::ReadbackCaptureResult&& a_result) {
					const auto owner = weak.lock();
					if (!owner)
						return;
					std::scoped_lock lock(owner->mutex);
					if (histogram && a_result.data.size() >= kBins * sizeof(std::uint32_t)) {
						std::array<std::uint32_t, kBins> bins{};
						std::memcpy(bins.data(), a_result.data.data(), sizeof(bins));
						owner->histogram = bins;
					} else if (!histogram && a_result.data.size() >= sizeof(float)) {
						float value = 0.0f;
						std::memcpy(&value, a_result.data.data(), sizeof(value));
						owner->adaptation = value;
					}
				};
				data.plan = org::runtime::PlanReadbackCapture(a_preparation.device, shared->service, *source, a_preparation.ResolveCapturedResource(data.source),
					{}, std::move(onDone), org::QueueKind::Graphics, histogram ? "cs.pp.auto-exposure.histogram" : "cs.pp.auto-exposure.adaptation");
				if (!data.plan)
					return {};
				if (data.plan.destinationLease)
					a_preparation.Retain(data.plan.destinationLease);
				a_preparation.Reserve(data.plan.reservation);
				data.plan.destinationLease.reset();
				data.plan.reservation.reset();
				return data;
			}

			static void Record(const ReadbackFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.plan)
					return;
				org::runtime::RecordReadbackCapture(a_frame.plan, a_recording.Commands(), a_recording.Resolve(a_frame.source).GetHandle());
			}

		private:
			std::shared_ptr<Readback> shared;
			std::shared_ptr<org::Resource> source;
			Capture capture;
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
				if (!readback->service)
					readback->service = RenderGraphRuntime::Get().Host()->GetRuntime().Readbacks();
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
				a_builder.Add("histogram-readback", FrameAnchors::PassKind::Copy,
					[shared = readback, source = histogram->Graph()]() -> std::shared_ptr<org::RenderPass> {
						return std::make_shared<ReadbackPass>(shared, source, Capture::Histogram);
					});
				a_builder.Dispatch({ .name = "average",
					.program = a_builder.Compute(kShader, words, {}, L"CS_Average"),
					.bindings = { Bind::Shared(), Bind::Unused(), Bind::Uav(histogram->Graph()), Bind::Uav(adaptation->Graph()), Bind::Latch(constants) } });
				a_builder.Add("adaptation-readback", FrameAnchors::PassKind::Copy,
					[shared = readback, source = adaptation->Graph()]() -> std::shared_ptr<org::RenderPass> {
						return std::make_shared<ReadbackPass>(shared, source, Capture::Adaptation);
					});
				a_builder.Publish(Published::kAdaptation, Image{ adaptation->Graph(), DXGI_FORMAT_UNKNOWN });
				a_builder.PublishLatch(Published::kAutoExposureConstants, constants);
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				auto& exposure = static_cast<HistogramAutoExposure&>(a_feature);
				a_commit.Write(constants, exposure.Constants(reset || clearPending));
				reset = false;
				TakeReadbacks(exposure);
			}

			// The one-shot clear ran before or with the first frame of this build.
			void Executed(PostProcessFeature&) override { clearPending = false; }

		private:
			/** @brief Moves arrived readbacks into the UI values and the pre-exposure metering, and posts this frame's requests. */
			void TakeReadbacks(HistogramAutoExposure& a_exposure) const
			{
				{
					std::scoped_lock lock(readback->mutex);
					if (readback->histogram) {
						std::memcpy(a_exposure.histogramData.data(), readback->histogram->data(), sizeof(std::uint32_t) * kBins);
						readback->histogram.reset();
					}
					if (readback->adaptation) {
						// As GetPreExposure takes the D3D11 staging copy: the graph's adaptation meters the next frames' pre-exposure.
						if (const float value = *readback->adaptation; std::isfinite(value) && value > 0.0f)
							a_exposure.preExposureLuminance = value;
						a_exposure.adaptationValue = *readback->adaptation;
						a_exposure.adaptationReadbackFrame = static_cast<int64_t>(globals::state->frameCount);
						readback->adaptation.reset();
					}
				}
				const auto wanted = a_exposure.WantedReadbacks();
				readback->wantHistogram.store(wanted.histogram, std::memory_order_release);
				const bool metering = globals::features::linearLighting.IsLinearLightingActive();
				readback->wantAdaptation.store(wanted.adaptation || metering, std::memory_order_release);
			}

			std::unique_ptr<GraphResources::StructuredBuffer> histogram;
			std::unique_ptr<GraphResources::StructuredBuffer> adaptation;
			std::shared_ptr<Readback> readback = std::make_shared<Readback>();
			LatchRegion<HistogramAutoExposure::AutoExposureCB> constants;
			std::uint64_t shaderGeneration = 0;
			bool clearPending = false;  // new history buffers not cleared yet
			bool reset = true;
		};
	}

	std::unique_ptr<Effect> MakeHistogramAutoExposure() { return std::make_unique<HistogramAutoExposureEffect>(); }
}

#endif
