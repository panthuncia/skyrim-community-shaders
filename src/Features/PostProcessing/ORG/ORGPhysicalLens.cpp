#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing.h"
#	include "Features/PostProcessing/PhysicalLens.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <OpenRenderGraph/Runtime.h>
#	include <Render/LatchBlock.h>
#	include <Render/Runtime/IReadbackService.h>
#	include <Render/Runtime/ReadbackCapturePlan.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/GloballyIndexedResource.h>

#	include <array>
#	include <atomic>
#	include <cmath>
#	include <cstring>
#	include <mutex>
#	include <optional>
#	include <span>

/*
* Physical Lens on the ORG post chain. Phase 1 of docs/development/plans/physical-lens-port.md: the camera focus, the lens
* model and the image mapping (distortion, vignetting, colour). The ghosts and the diffraction starburst hold the chain.
* The effect writes no chain image: Composite reads the published mapping and the diffraction region.
*/

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kFocusShader = "PostProcessing/PhysicalLensImaging/focus.cs.hlsl";
		constexpr const char* kTraceShader = "PostProcessing/PhysicalLensImaging/trace.cs.hlsl";
		constexpr DXGI_FORMAT kFocusFormat = DXGI_FORMAT_R32_FLOAT;
		constexpr DXGI_FORMAT kMappingFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
		// As ORGDoF: a commit cannot tell whether its epoch runs the new build yet, so the focus restarts for a few commits.
		constexpr std::uint32_t kFocusResetCommits = 4;

		constexpr std::uint32_t Align256(std::uint32_t a_bytes) { return (a_bytes + 255u) & ~255u; }

		// The large constant blocks, in the effect's own latch block, each copied into a constant buffer before its readers.
		constexpr std::uint32_t kImagingOffset = 0;
		constexpr std::uint32_t kImagingBytes = sizeof(PhysicalLensImaging::GPUParameters);
		constexpr std::uint32_t kTableOffset = Align256(kImagingOffset + kImagingBytes);
		constexpr std::uint32_t kTableBytes = sizeof(PhysicalLensImaging::CameraTableParameters);
		constexpr std::uint32_t kUploadBytes = Align256(kTableOffset + kTableBytes);
		constexpr const char* kImagingName = "PostProcessing::PhysicalLens::ReverseOptics";
		constexpr const char* kTableName = "PostProcessing::PhysicalLens::MappingParameters";

		// FOCUS_RESOURCES order.
		struct FocusBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch focusParameters;
			ShaderBindings::Handle depth, previous, linearSampler, focus;
		};

		// TRACE_RESOURCES order.
		struct TraceBlock
		{
			ShaderBindings::Handle imaging, cameraTable, inputRays, outputRays, cameraMaps;
		};

		// One entry of an effect pass's push block. The anchor helpers have no array UAV or CBV, which the lens passes need.
		struct LensUse
		{
			enum class Kind : std::uint8_t
			{
				Srv,
				Uav,
				Cbv,
				Handle,
			};

			Kind kind = Kind::Handle;
			std::shared_ptr<org::Resource> resource;
			std::uint32_t value = 0;             // Handle: the descriptor index
			std::uint32_t variant = UINT32_MAX;  // Srv and Uav: the view variant, UINT32_MAX for the default view

			static LensUse Srv(std::shared_ptr<org::Resource> a_resource, std::uint32_t a_variant = UINT32_MAX) { return { Kind::Srv, std::move(a_resource), 0, a_variant }; }
			static LensUse UavArray(std::shared_ptr<org::Resource> a_resource)
			{
				return { Kind::Uav, std::move(a_resource), 0, static_cast<std::uint32_t>(org::UAVViewType::Texture2DArrayFull) };
			}
			static LensUse Cbv(std::shared_ptr<org::Resource> a_resource) { return { Kind::Cbv, std::move(a_resource) }; }
			static LensUse Unused() { return {}; }
		};

		/** @brief A gated dispatch: it runs only on runs whose commit opened its entry, with the counts given there. No shared constants. */
		struct LensDispatch
		{
			std::string name;
			std::shared_ptr<const ComputeProgram> program;
			std::vector<LensUse> uses;
			std::shared_ptr<const RenderGraph::RunGate> gate;
			std::uint32_t entry = 0;
		};

		struct LensDispatchBindings
		{
			std::vector<org::DeclaredViewToken> views;
		};

		struct LensDispatchFrame
		{
			std::shared_ptr<const LensDispatch> spec;
		};

		class LensDispatchPass final : public org::TypedRenderGraphPass<LensDispatchPass, LensDispatchFrame, LensDispatchBindings>
		{
		public:
			explicit LensDispatchPass(std::shared_ptr<const LensDispatch> a_spec) :
				spec(std::move(a_spec)) {}

			LensDispatchBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				LensDispatchBindings bindings;
				bindings.views.resize(spec->uses.size());
				for (std::size_t i = 0; i < spec->uses.size(); ++i) {
					const auto& use = spec->uses[i];
					switch (use.kind) {
					case LensUse::Kind::Srv:
						bindings.views[i] = use.variant != UINT32_MAX ? a_builder.ShaderResource(use.resource, org::SrvView{ .variant = use.variant }).View() :
						                                                a_builder.ShaderResource(use.resource).View();
						break;
					case LensUse::Kind::Uav:
						bindings.views[i] = use.variant != UINT32_MAX ? a_builder.UnorderedAccess(use.resource, org::UavView{ .variant = use.variant }).View() :
						                                                a_builder.UnorderedAccess(use.resource).View();
						break;
					case LensUse::Kind::Cbv:
						bindings.views[i] = a_builder.ConstantBuffer(use.resource).View();
						break;
					case LensUse::Kind::Handle:
						break;
					}
				}
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

			LensDispatchFrame Prepare(const LensDispatchBindings&, const org::PassPrepareContext&) const { return { spec }; }

			static bool SkipsAtExecution(const LensDispatchBindings&, const LensDispatchFrame& a_frame, const org::PassRecordContext& a_recording)
			{
				return !a_frame.spec->gate->IsOpen(a_recording.Run(), a_frame.spec->entry);
			}

			static void Record(const LensDispatchBindings& a_bindings, const LensDispatchFrame& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& spec = *a_frame.spec;
				std::array<std::uint32_t, ShaderBindings::kMaxPushWords> words{};
				std::uint32_t count = 0;
				for (std::size_t i = 0; i < spec.uses.size() && count < words.size(); ++i)
					words[count++] = spec.uses[i].kind == LensUse::Kind::Handle ? spec.uses[i].value : a_recording.Index(a_bindings.views[i]);
				spec.gate->Dispatch(a_recording, *spec.program, std::span<const std::uint32_t>(words.data(), count), spec.entry);
			}

		private:
			std::shared_ptr<const LensDispatch> spec;
		};

		/** @brief Copies parts of the run's region of the effect's latch block into constant buffers, on runs its gate entry is open. */
		struct LensUpload
		{
			struct Part
			{
				std::shared_ptr<org::Resource> target;
				std::uint32_t offset = 0, bytes = 0;
			};

			std::shared_ptr<const org::LatchBlock> latch;
			std::vector<Part> parts;
			std::shared_ptr<const RenderGraph::RunGate> gate;
			std::uint32_t entry = 0;
		};

		struct LensUploadBindings
		{
			std::vector<org::ResourceBindingToken> targets;
		};

		struct LensUploadFrame
		{
			std::shared_ptr<const LensUpload> spec;
		};

		class LensUploadPass final : public org::TypedRenderGraphPass<LensUploadPass, LensUploadFrame, LensUploadBindings>
		{
		public:
			explicit LensUploadPass(std::shared_ptr<const LensUpload> a_spec) :
				spec(std::move(a_spec)) {}

			LensUploadBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				LensUploadBindings bindings;
				for (const auto& part : spec->parts)
					bindings.targets.push_back(a_builder.CopyDestination(part.target));
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

			LensUploadFrame Prepare(const LensUploadBindings&, const org::PassPrepareContext&) const { return { spec }; }

			// A closed run copies nothing, so its commit need not write the region.
			static bool SkipsAtExecution(const LensUploadBindings&, const LensUploadFrame& a_frame, const org::PassRecordContext& a_recording)
			{
				return !a_frame.spec->gate->IsOpen(a_recording.Run(), a_frame.spec->entry);
			}

			static void Record(const LensUploadBindings& a_bindings, const LensUploadFrame& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& spec = *a_frame.spec;
				const auto region = spec.latch->At(a_recording.Run());
				const auto source = region.buffer->GetAPIResource().GetHandle();
				for (std::size_t i = 0; i < spec.parts.size(); ++i)
					a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.targets[i]).GetHandle(), 0, source,
						region.offset + spec.parts[i].offset, spec.parts[i].bytes);
			}

		private:
			std::shared_ptr<const LensUpload> spec;
		};

		/** @brief The focus readback, shared by the render thread, the readback pass and the readback service's callback. */
		struct FocusReadback
		{
			std::shared_ptr<org::runtime::IReadbackService> service;
			std::atomic<bool> want = false;
			// A reset bumps it, so a readback issued before the reset is dropped.
			std::atomic<std::uint64_t> generation = 0;
			std::atomic<std::uint64_t> issued = 0;

			std::mutex mutex;
			std::optional<float> km;          // guarded, the newest result not taken yet
			std::uint64_t newest = 0;         // guarded, the sequence of the newest result taken or stored
			std::uint64_t newestGeneration = 0;  // guarded
		};

		struct FocusReadbackFrame
		{
			org::PreparedResourceReference source;
			org::runtime::ReadbackCapturePlan plan;
		};

		/** @brief On frames the camera is active, as the D3D11 staging copies. No revision, so it is prepared every frame and never replayed. */
		class FocusReadbackPass final : public org::TypedRenderGraphPass<FocusReadbackPass, FocusReadbackFrame>
		{
		public:
			FocusReadbackPass(std::shared_ptr<FocusReadback> a_shared, std::shared_ptr<org::Resource> a_source) :
				shared(std::move(a_shared)), source(std::move(a_source)) {}

			void Declare(org::PassBuilder& a_builder)
			{
				a_builder.CopySource(source);
				a_builder.PreferQueue(org::QueueKind::Graphics);
			}

			FocusReadbackFrame Prepare(const org::PassPrepareContext& a_preparation)
			{
				if (!shared->service || !shared->want.load(std::memory_order_acquire))
					return {};
				FocusReadbackFrame data{};
				data.source = a_preparation.CaptureResource(source->GetGlobalResourceID());
				const std::uint64_t generation = shared->generation.load(std::memory_order_acquire);
				const std::uint64_t sequence = shared->issued.fetch_add(1, std::memory_order_acq_rel) + 1;
				// The readback service's thread. The result waits for the next Commit.
				org::ReadbackCaptureCallback onDone = [weak = std::weak_ptr<FocusReadback>(shared), generation, sequence](org::ReadbackCaptureResult&& a_result) {
					const auto owner = weak.lock();
					if (!owner || a_result.data.size() < sizeof(float))
						return;
					float value = 0.0f;
					std::memcpy(&value, a_result.data.data(), sizeof(value));
					std::scoped_lock lock(owner->mutex);
					if (generation < owner->newestGeneration || (generation == owner->newestGeneration && sequence <= owner->newest))
						return;
					owner->km = value;
					owner->newest = sequence;
					owner->newestGeneration = generation;
				};
				data.plan = org::runtime::PlanReadbackCapture(a_preparation.device, shared->service, *source, a_preparation.ResolveCapturedResource(data.source),
					{}, std::move(onDone), org::QueueKind::Graphics, "cs.pp.physical-lens.focus");
				if (!data.plan)
					return {};
				if (data.plan.destinationLease)
					a_preparation.Retain(data.plan.destinationLease);
				a_preparation.Reserve(data.plan.reservation);
				data.plan.destinationLease.reset();
				data.plan.reservation.reset();
				return data;
			}

			static void Record(const FocusReadbackFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.plan)
					return;
				org::runtime::RecordReadbackCapture(a_frame.plan, a_recording.Commands(), a_recording.Resolve(a_frame.source).GetHandle());
			}

		private:
			std::shared_ptr<FocusReadback> shared;
			std::shared_ptr<org::Resource> source;
		};

		/** @brief A device-local buffer with a CBV over a_bytes rounded to 256, as SharedConstants makes its blocks. */
		std::shared_ptr<org::Buffer> MakeConstantBuffer(std::uint32_t a_bytes, const char* a_name)
		{
			const std::uint32_t size = Align256(a_bytes);
			auto buffer = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, size);
			org::BufferBase::DescriptorRequirements views{};
			views.createCBV = true;
			views.cbvDesc = { 0, size };
			buffer->SetDescriptorRequirements(views);
			buffer->SetName(a_name);
			buffer->Materialize();
			return buffer;
		}

		D3D11_TEXTURE2D_DESC TextureDesc(UINT a_width, UINT a_height, UINT a_slices, DXGI_FORMAT a_format)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = 1;
			desc.ArraySize = a_slices;
			desc.Format = a_format;
			desc.SampleDesc = { 1, 0 };
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			return desc;
		}

		/**
		* @brief PhysicalLens::Draw's focus and image mapping on the graph. The mapping is a persistent texture remade, through a gated
		* upload and dispatch, only when its inputs change; a request repeats until a frame with it reached the GPU with this build.
		* The camera focus is DoF's published focus or this effect's own, read back to the CPU without a wait.
		*/
		class PhysicalLensEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				auto& lens = static_cast<PhysicalLens&>(a_feature);
				lens.SanitizeSettings();
				if (lens.settings.Intensity > 0.f)
					return "its ghosts have no ORG implementation yet (Ghost Intensity above zero)";
				if (lens.settings.Diffraction)
					return "its diffraction starburst has no ORG implementation yet";
				depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
				if (!depth)
					return "the main depth cannot be imported";
				// As Draw, the aspect of the chain's input, which has kMAIN's size.
				auto* main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture;
				if (!main)
					return "kMAIN is missing";
				D3D11_TEXTURE2D_DESC mainDesc{};
				main->GetDesc(&mainDesc);
				aspect = static_cast<float>(mainDesc.Width) / static_cast<float>((std::max)(mainDesc.Height, 1u));
				a_key.push_back(reinterpret_cast<std::uintptr_t>(depth.Key()));
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				if (!Ensure()) {
					a_builder.Fail("its resources could not be made");
					return;
				}
				a_builder.Register(*mapping);
				a_builder.Register(*focus);
				a_builder.Register(kImagingName, imagingBuffer);
				a_builder.Register(kTableName, tableBuffer);
				if (!readback->service)
					readback->service = RenderGraphRuntime::Get().Host()->GetRuntime().Readbacks();
				gate = a_builder.RunGate();

				// The camera focus: DoF's when it runs in this chain, else this effect's own pass.
				const auto dofFocus = a_builder.Find(Published::kDoFFocus);
				ownFocus = !dofFocus;
				focusConstants = a_builder.Latch<PhysicalLens::FocusInputs>();
				if (ownFocus) {
					focusGate = a_builder.Gate();
					a_builder.Dispatch({ .name = "focus",
						.program = a_builder.Compute(kFocusShader, ShaderBindings::Words<FocusBlock>(), {}, L"UpdateFocus"),
						.bindings = { Bind::Shared(), Bind::Latch(focusConstants), Bind::Srv(depth), Bind::Unused(), Bind::Handle(sampler.Index()), Bind::Uav(focus->Graph()) },
						.gate = focusGate });
				}
				a_builder.Add("focus-readback", FrameAnchors::PassKind::Copy,
					[shared = readback, source = ownFocus ? focus->Graph() : dofFocus.graph.Plain()]() -> std::shared_ptr<org::RenderPass> {
						return std::make_shared<FocusReadbackPass>(shared, source);
					});
				focusReset = kFocusResetCommits;

				// The image mapping, as UpdateCameraMapping.
				mappingGate = a_builder.Gate();
				auto trace = a_builder.Compute(kTraceShader, ShaderBindings::Words<TraceBlock>(), {}, L"BuildCameraMaps");
				if (a_builder.Failed())
					return;
				auto upload = std::make_shared<LensUpload>(LensUpload{ .latch = constants,
					.parts = { { imagingBuffer, kImagingOffset, kImagingBytes }, { tableBuffer, kTableOffset, kTableBytes } },
					.gate = gate,
					.entry = mappingGate.index });
				a_builder.Add("mapping-upload", FrameAnchors::PassKind::Copy, [upload]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<LensUploadPass>(upload); });
				auto dispatch = std::make_shared<LensDispatch>(LensDispatch{ .name = "mapping",
					.program = std::move(trace),
					.uses = { LensUse::Cbv(imagingBuffer), LensUse::Cbv(tableBuffer), LensUse::Unused(), LensUse::Unused(), LensUse::UavArray(mapping->Graph()) },
					.gate = gate,
					.entry = mappingGate.index });
				a_builder.Add("mapping", FrameAnchors::PassKind::Compute, [dispatch]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<LensDispatchPass>(dispatch); });

				diffraction = a_builder.Latch<PhysicalLensImaging::DiffractionParameters>();
				a_builder.PublishLatch(Published::kPhysicalLensDiffraction, diffraction);
				a_builder.Publish(Published::kPhysicalLensMapping, Image{ mapping->Graph(), kMappingFormat });
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				auto& lens = static_cast<PhysicalLens&>(a_feature);
				const auto* camera = lens.owner ? lens.owner->GetActivePhysicalCameraState() : nullptr;
				const unsigned frame = globals::state->frameCount;
				float focusM = 0.f;
				if (camera) {
					// As ResolveCameraFocus: a frame gap or a history reset starts the focus over at the manual distance.
					const bool reset = lastFocusFrame == ~0u || frame != lastFocusFrame + 1 || globals::state->ShouldResetHistory();
					if (reset) {
						readback->generation.fetch_add(1, std::memory_order_acq_rel);
						resolvedFocusM = camera->ManualDistanceM;
					}
					TakeFocus();
					lastFocusFrame = frame;
					if (ownFocus) {
						a_commit.Write(focusConstants, lens.FocusInputsFor(*camera, reset || focusReset > 0));
						a_commit.Open(focusGate, 1);
					}
					focusM = resolvedFocusM;
					lens.SetResolvedFocus(focusM);
				} else {
					lastFocusFrame = ~0u;
				}
				readback->want.store(camera != nullptr, std::memory_order_release);
				if (focusReset > 0)
					--focusReset;

				const bool configured = lens.PrepareOptics(aspect, camera, focusM);
				pending.reset();
				const bool mapped = configured && lens.settings.ImageEffects;
				if (mapped) {
					const auto [imaging, table] = lens.CameraMapping(aspect);
					if (!made || std::memcmp(&imaging, &made->imaging, sizeof(imaging)) || std::memcmp(&table, &made->table, sizeof(table))) {
						const auto& run = a_commit.Run();
						constants->WriteValue(run, kImagingOffset, imaging);
						constants->WriteValue(run, kTableOffset, table);
						a_commit.Open(mappingGate, table.width, table.height, 3);
						pending = Mapping{ imaging, table };
					}
				}
				lens.orgComposite = { .cameraMap = mapped, .flare = false, .diffraction = {} };
				a_commit.Write(diffraction, lens.orgComposite.diffraction);
			}

			void Executed(PostProcessFeature&) override
			{
				if (!pending)
					return;
				made = std::move(pending);
				pending.reset();
			}

		private:
			struct Mapping
			{
				PhysicalLensImaging::GPUParameters imaging;
				PhysicalLensImaging::CameraTableParameters table;
			};

			/** @brief The persistent resources, made once for the session. */
			bool Ensure()
			{
				if (mapping && *mapping && focus && *focus && constants && imagingBuffer && tableBuffer && sampler)
					return true;
				try {
					const PhysicalLensImaging::CameraTableParameters table{};
					const GraphResources::Options persistent{ .lifetime = GraphResources::Lifetime::Persistent };
					mapping = std::make_unique<GraphResources::Texture2D>(TextureDesc(table.width, table.height, 3, kMappingFormat), std::string(Published::kPhysicalLensMapping), persistent);
					focus = std::make_unique<GraphResources::Texture2D>(TextureDesc(1, 1, 1, kFocusFormat), "PostProcessing::PhysicalLens::Focus",
						GraphResources::Options{ .lifetime = GraphResources::Lifetime::History });
					constants = org::LatchBlock::Create("cs.pp.physical-lens.constants", kUploadBytes);
					imagingBuffer = MakeConstantBuffer(kImagingBytes, kImagingName);
					tableBuffer = MakeConstantBuffer(kTableBytes, kTableName);
					// PhysicalLens::sampler: linear, border, every mip.
					sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{ .addressU = rhi::AddressMode::Border,
						.addressV = rhi::AddressMode::Border,
						.addressW = rhi::AddressMode::Border });
				} catch (const std::exception& e) {
					logger::error("[ORG] Physical Lens: its resources could not be made: {}", e.what());
					mapping.reset();
					return false;
				}
				made.reset();
				return mapping && *mapping && focus && *focus && constants && imagingBuffer && tableBuffer && sampler;
			}

			/** @brief The newest focus readback of this generation, in metres, as ResolveCameraFocus maps its staging copy. */
			void TakeFocus()
			{
				std::scoped_lock lock(readback->mutex);
				if (!readback->km)
					return;
				const float metres = *readback->km * 1000.f;
				const bool current = readback->newestGeneration == readback->generation.load(std::memory_order_acquire);
				if (current && std::isfinite(metres) && metres > 0.f)
					resolvedFocusM = metres;
				readback->km.reset();
			}

			FrameAnchors::Texture depth;
			float aspect = 16.f / 9.f;

			std::unique_ptr<GraphResources::Texture2D> mapping;  // the image mapping, RGBA32F, 3 bands
			std::unique_ptr<GraphResources::Texture2D> focus;    // this effect's focus in km when DoF is not in the chain
			std::shared_ptr<org::LatchBlock> constants;          // the large blocks, by run
			std::shared_ptr<org::Buffer> imagingBuffer, tableBuffer;
			org::OwnedDescriptorBinding sampler;
			std::shared_ptr<const RenderGraph::RunGate> gate;

			std::shared_ptr<FocusReadback> readback = std::make_shared<FocusReadback>();
			bool ownFocus = true;
			unsigned lastFocusFrame = ~0u;
			float resolvedFocusM = 10.f;
			std::uint32_t focusReset = 0;

			// What the mapping was made from, and what this frame asked for until Executed.
			std::optional<Mapping> made, pending;

			LatchRegion<PhysicalLens::FocusInputs> focusConstants;
			LatchRegion<PhysicalLensImaging::DiffractionParameters> diffraction;
			GateEntry focusGate, mappingGate;
		};
	}

	std::unique_ptr<Effect> MakePhysicalLens() { return std::make_unique<PhysicalLensEffect>(); }
}

#endif
