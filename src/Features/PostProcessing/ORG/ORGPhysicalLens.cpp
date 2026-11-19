#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing.h"
#	include "Features/PostProcessing/HistogramAutoExposure.h"
#	include "Features/PostProcessing/PhysicalLens.h"
#	include "Globals.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/GenerateMips.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <OpenRenderGraph/Runtime.h>
#	include <Render/LatchBlock.h>
#	include <Render/PassBuilders.h>
#	include <Render/Runtime/IUploadService.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/Buffers/VersionedBuffer.h>
#	include <Resources/GloballyIndexedResource.h>
#	include <Resources/VersionedTexture.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <bit>
#	include <cmath>
#	include <cstring>
#	include <format>
#	include <mutex>
#	include <optional>
#	include <span>

/*
* Physical Lens on the ORG post chain (docs/development/plans/physical-lens-port.md): the camera focus, the lens model, the
* image mapping, the diffraction starburst and the ghosts. The effect writes no chain image. Composite reads its published
* mapping, ghosts and diffraction outputs.
*
* Every pass may be replayed from a kept recording on a run whose commit did not open it. Direct dispatches then read zero
* group counts from the run gate. Indirect dispatches and draws read their counts from GPU buffers, which a copy from the
* effect's latch block seeds on every run; a run without that work seeds zeros.
*/

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kFocusShader = "PostProcessing/PhysicalLensImaging/focus.cs.hlsl";
		constexpr const char* kTraceShader = "PostProcessing/PhysicalLensImaging/trace.cs.hlsl";
		constexpr const char* kDetailShader = "PostProcessing/PhysicalLensImaging/diffraction_detail.cs.hlsl";
		constexpr const char* kWaveShader = "PostProcessing/PhysicalLensImaging/diffraction.cs.hlsl";
		constexpr const char* kAmplitudeShader = "PostProcessing/PhysicalLensImaging/diffraction_amplitude.cs.hlsl";
		constexpr const char* kSourcesShader = "PostProcessing/PhysicalLensImaging/diffraction_sources.cs.hlsl";
		constexpr const char* kTailShader = "PostProcessing/PhysicalLensImaging/diffraction_tail.cs.hlsl";
		constexpr const char* kFlareShader = "PostProcessing/PhysicalLensFlare/flare.cs.hlsl";
		constexpr const char* kPlanningShader = "PostProcessing/PhysicalLensFlare/planning.cs.hlsl";
		constexpr const char* kBeamTraceShader = "PostProcessing/PhysicalLensFlare/beam.trace.cs.hlsl";
		constexpr const char* kGhostWaveShader = "PostProcessing/PhysicalLensFlare/ghost_wave.cs.hlsl";
		constexpr const char* kResolveShader = "PostProcessing/PhysicalLensFlare/area.resolve.cs.hlsl";
		constexpr const char* kBeamShader = "PostProcessing/PhysicalLensFlare/beam.hlsl";

		constexpr DXGI_FORMAT kFocusFormat = DXGI_FORMAT_R32_FLOAT;
		constexpr DXGI_FORMAT kMappingFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
		// As ORGDoF: a commit cannot tell whether its epoch runs the new build yet, so the focus restarts for a few commits.
		constexpr std::uint32_t kFocusResetCommits = 4;

		constexpr std::uint32_t Align256(std::uint32_t a_bytes) { return (a_bytes + 255u) & ~255u; }

		using Parameters = PhysicalLensOptics::Parameters;
		using std::uint32_t;

		// The effect's own latch block: the large blocks, each copied into a constant buffer before its readers, and the seeds
		// of the GPU-written indirect arguments.
		constexpr uint32_t kImagingOffset = 0;
		constexpr uint32_t kImagingBytes = sizeof(PhysicalLensImaging::GPUParameters);
		constexpr uint32_t kTableOffset = Align256(kImagingOffset + kImagingBytes);
		constexpr uint32_t kTableBytes = sizeof(PhysicalLensImaging::CameraTableParameters);
		constexpr uint32_t kLensOffset = Align256(kTableOffset + kTableBytes);
		constexpr uint32_t kLensBytes = sizeof(Parameters);
		constexpr uint32_t kWaveSeedOffset = Align256(kLensOffset + kLensBytes);
		constexpr uint32_t kWaveSeedBytes = 16;
		constexpr uint32_t kGhostSeedOffset = kWaveSeedOffset + kWaveSeedBytes;
		constexpr uint32_t kGhostSeedBytes = 128;
		constexpr uint32_t kPlanSeedOffset = kGhostSeedOffset + kGhostSeedBytes;
		constexpr uint32_t kPlanSeedBytes = 16;
		constexpr uint32_t kUploadBytes = Align256(kPlanSeedOffset + kPlanSeedBytes);
		constexpr const char* kImagingName = "PostProcessing::PhysicalLens::ReverseOptics";
		constexpr const char* kTableName = "PostProcessing::PhysicalLens::MappingParameters";
		constexpr const char* kLensName = "PostProcessing::PhysicalLens::Parameters";

		// PhysicalLens::Draw's seeds of the ghost arguments: TraceBeams/ShadeBeams at 0, the fine draw at 12 (read as a non-indexed
		// draw: count, instances, then two zero words), RequestPlans/ExpandCachedJobs at 32, ReduceGhosts at 48, the area draws at 64 + 20k.
		constexpr std::array<uint32_t, 32> kGhostSeed{ (PhysicalLensOptics::Vertices + 63) / 64, 0, 3, PhysicalLensOptics::BeamDrawVertices, 0, 0, 0, 0, 1, 0, 1,
			0, 1, 0, 3, 0, PhysicalLensOptics::BeamDrawVertices, 0, 0, 0, 0, PhysicalLensOptics::BeamDrawVertices, 0, 0, 0, 0,
			PhysicalLensOptics::BeamDrawVertices, 0, 0, 0, 0, 0 };
		constexpr std::array<uint32_t, 4> kPlanSeed{ 0, 1, 1, 0 };
		constexpr std::array<uint32_t, 4> kWaveSeed{ 0, 12, 0, 0 };

		constexpr uint32_t kPlanCount = 65 * PhysicalLensOptics::MaxPaths;
		constexpr uint32_t kAtlasMips = std::bit_width(PhysicalLensGhostWave::Size);

		// FOCUS_RESOURCES order.
		struct FocusBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch focusParameters;
			ShaderBindings::Handle depth, previous, linearSampler, focus;
		};

		/** @brief One entry of a lens shader's push block: the kind and the D3D11 register it reads. */
		struct Slot
		{
			enum class Kind : std::uint8_t
			{
				Srv,
				Uav,
				Cbv,
				Latch,
				Sampler,
			};
			Kind kind;
			std::uint8_t reg;
		};
		using Layout = std::vector<Slot>;
		using K = Slot::Kind;

		// The shaders' resource lists, in order. A pass binds registers, as the D3D11 path does, and every entry at a register
		// takes what is bound there, so names that share a D3D11 register share a binding.
		// trace.cs.hlsl's TRACE_RESOURCES.
		const Layout kTraceLayout{ { K::Cbv, 0 }, { K::Cbv, 1 }, { K::Srv, 0 }, { K::Uav, 0 }, { K::Uav, 1 } };
		// diffraction.cs.hlsl: the trace list, then its own lists.
		const Layout kWaveLayout{ { K::Cbv, 0 }, { K::Cbv, 1 }, { K::Srv, 0 }, { K::Uav, 0 }, { K::Uav, 1 }, { K::Latch, 2 }, { K::Uav, 2 }, { K::Srv, 3 },
			{ K::Uav, 0 }, { K::Srv, 1 }, { K::Uav, 3 }, { K::Uav, 4 }, { K::Uav, 5 }, { K::Uav, 3 }, { K::Srv, 2 } };
		// flare.cs.hlsl's push block: optics, exposure, flare, pupil and planning lists.
		const Layout kFlareLayout{ { K::Cbv, 0 }, { K::Srv, 2 }, { K::Srv, 5 }, { K::Latch, 1 }, { K::Srv, 0 }, { K::Srv, 1 }, { K::Uav, 0 }, { K::Uav, 1 },
			{ K::Uav, 4 }, { K::Uav, 1 }, { K::Uav, 4 }, { K::Uav, 2 }, { K::Uav, 3 }, { K::Srv, 3 }, { K::Srv, 4 }, { K::Uav, 4 }, { K::Srv, 0 }, { K::Uav, 5 },
			{ K::Srv, 6 }, { K::Uav, 0 }, { K::Uav, 4 }, { K::Srv, 0 } };
		// diffraction_sources.cs.hlsl: optics, exposure, then its lists.
		const Layout kSourcesLayout{ { K::Cbv, 0 }, { K::Srv, 2 }, { K::Srv, 5 }, { K::Latch, 1 }, { K::Srv, 3 }, { K::Latch, 2 }, { K::Srv, 4 }, { K::Srv, 1 },
			{ K::Srv, 3 }, { K::Srv, 1 }, { K::Srv, 0 }, { K::Uav, 0 }, { K::Srv, 2 }, { K::Uav, 1 }, { K::Uav, 2 }, { K::Uav, 3 } };
		// diffraction_tail.cs.hlsl.
		const Layout kTailLayout{ { K::Latch, 2 }, { K::Srv, 0 }, { K::Srv, 1 }, { K::Srv, 2 }, { K::Srv, 3 }, { K::Srv, 4 }, { K::Uav, 0 }, { K::Sampler, 0 } };
		// ghost_wave.cs.hlsl, then its stage push constant.
		const Layout kGhostWaveLayout{ { K::Srv, 0 }, { K::Srv, 1 }, { K::Uav, 0 }, { K::Uav, 1 }, { K::Uav, 2 }, { K::Latch, 1 } };
		// beam.hlsl: optics, its list, BeamIndices, then the channel push constant.
		const Layout kBeamLayout{ { K::Cbv, 0 }, { K::Srv, 2 }, { K::Srv, 0 }, { K::Srv, 1 }, { K::Srv, 3 }, { K::Srv, 4 }, { K::Srv, 5 }, { K::Sampler, 0 },
			{ K::Srv, 7 } };
		// area.resolve.cs.hlsl.
		const Layout kResolveLayout{ { K::Srv, 0 }, { K::Srv, 1 }, { K::Srv, 2 }, { K::Srv, 3 }, { K::Srv, 4 }, { K::Uav, 0 }, { K::Uav, 1 }, { K::Sampler, 0 } };

		/** @brief The push words: a handle per entry, two per latch, then a_push words on a 16-byte boundary (cbuffer packing). */
		uint32_t WordsOf(const Layout& a_layout, uint32_t a_push = 0)
		{
			uint32_t words = 0;
			for (const auto& slot : a_layout)
				words += slot.kind == K::Latch ? 2u : 1u;
			return a_push ? ((words + 3u) & ~3u) + a_push : words;
		}

		/** @brief What a pass binds at one register. */
		struct View
		{
			std::shared_ptr<org::Resource> resource;
			std::shared_ptr<org::VersionedBuffer> versioned;
			FrameAnchors::Texture texture;  // a chain image, or another effect's output
			bool array = false;             // the whole-array view, for a shader that declares Texture2DArray
			uint32_t mip = kAllMips;        // one mip, read with the mips below it as ORG views do

			explicit operator bool() const { return resource || versioned || static_cast<bool>(texture); }
		};

		View Of(std::shared_ptr<org::Resource> a_resource) { return { .resource = std::move(a_resource) }; }
		View ArrayOf(std::shared_ptr<org::Resource> a_resource, uint32_t a_mip = kAllMips) { return { .resource = std::move(a_resource), .array = true, .mip = a_mip }; }
		View MipOf(std::shared_ptr<org::Resource> a_resource, uint32_t a_mip) { return { .resource = std::move(a_resource), .mip = a_mip }; }
		View ImageOf(const Image& a_image, bool a_array = false) { return { .texture = a_image.graph, .array = a_array }; }

		/** @brief A pass's bindings by register. */
		struct Binds
		{
			std::array<View, 8> srv, uav;
			std::array<View, 4> cbv;
			std::array<std::optional<uint32_t>, 4> latch;  // offsets in the chain's latch block
			std::array<uint32_t, 2> sampler{};
		};

		/** @brief Declares each bound register of the layout once, so names sharing a register share the view. */
		struct DeclaredBinds
		{
			std::array<std::optional<org::DeclaredViewToken>, 8> srv, uav;
			std::array<std::optional<org::DeclaredViewToken>, 4> cbv;
		};

		org::DeclaredViewToken DeclareView(org::PassBuilder& a_builder, const View& a_view, bool a_unordered)
		{
			const uint32_t variant = !a_view.array ? UINT32_MAX :
			                         a_unordered   ? static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull) :
			                                         static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull);
			if (a_view.versioned)
				return a_builder.ShaderResource(*a_view.versioned, org::SrvView{}).View();
			if (a_view.texture && a_view.texture.Slot()) {
				org::ResourceResolverAndRange declared(*a_view.texture.Slot());
				return a_unordered ? a_builder.UnorderedAccess(declared, org::UavView{ .variant = variant }).View() :
				                     a_builder.ShaderResource(declared, org::SrvView{ .variant = variant }).View();
			}
			const auto& resource = a_view.texture ? a_view.texture.Plain() : a_view.resource;
			if (a_view.mip == kAllMips)
				return a_unordered ? a_builder.UnorderedAccess(resource, org::UavView{ .variant = variant }).View() :
				                     a_builder.ShaderResource(resource, org::SrvView{ .variant = variant }).View();
			const auto range = org::Subresources(resource, org::Mip(a_view.mip, 1));
			return a_unordered ? a_builder.UnorderedAccess(range, org::UavView{ .variant = variant, .mip = a_view.mip }).View() :
			                     a_builder.ShaderResource(range, org::SrvView{ .variant = variant, .mip = a_view.mip }).View();
		}

		DeclaredBinds DeclareBinds(org::PassBuilder& a_builder, const Layout& a_layout, const Binds& a_binds)
		{
			DeclaredBinds declared;
			for (const auto& slot : a_layout) {
				switch (slot.kind) {
				case K::Srv:
					if (a_binds.srv[slot.reg] && !declared.srv[slot.reg])
						declared.srv[slot.reg] = DeclareView(a_builder, a_binds.srv[slot.reg], false);
					break;
				case K::Uav:
					if (a_binds.uav[slot.reg] && !declared.uav[slot.reg])
						declared.uav[slot.reg] = DeclareView(a_builder, a_binds.uav[slot.reg], true);
					break;
				case K::Cbv:
					if (a_binds.cbv[slot.reg] && !declared.cbv[slot.reg])
						declared.cbv[slot.reg] = a_builder.ConstantBuffer(a_binds.cbv[slot.reg].resource).View();
					break;
				default:
					break;
				}
			}
			return declared;
		}

		/** @brief The push words of a pass for its recording's run. Unbound registers are 0: the shader never reads them there. */
		std::vector<uint32_t> FillWords(const Layout& a_layout, const Binds& a_binds, const DeclaredBinds& a_declared, const org::LatchBlock& a_latch,
			std::span<const uint32_t> a_push, org::PassRecordContext& a_recording)
		{
			std::vector<uint32_t> words;
			words.reserve(ShaderBindings::kMaxPushWords);
			const auto index = [&](const std::optional<org::DeclaredViewToken>& a_view) { return a_view ? a_recording.Index(*a_view) : 0u; };
			for (const auto& slot : a_layout) {
				switch (slot.kind) {
				case K::Srv:
					words.push_back(index(a_declared.srv[slot.reg]));
					break;
				case K::Uav:
					words.push_back(index(a_declared.uav[slot.reg]));
					break;
				case K::Cbv:
					words.push_back(index(a_declared.cbv[slot.reg]));
					break;
				case K::Latch:
					{
						// Build refuses a layout latch without a binding.
						const auto latch = ShaderBindings::LatchOf(a_latch, a_recording.Run(), *a_binds.latch[slot.reg]);
						words.push_back(latch.handle);
						words.push_back(latch.offset);
						break;
					}
				case K::Sampler:
					words.push_back(a_binds.sampler[slot.reg]);
					break;
				}
			}
			if (!a_push.empty()) {
				words.resize((words.size() + 3u) & ~std::size_t(3));
				words.insert(words.end(), a_push.begin(), a_push.end());
			}
			return words;
		}

		/**
		* @brief A dispatch through the chain's run gate: direct, with the counts the commit opened, or indirect, from a GPU-written
		* arguments buffer, on runs whose commit opened its entry.
		*/
		struct LensDispatch
		{
			std::string name;
			std::shared_ptr<const ComputeProgram> program;
			Layout layout;
			Binds binds;
			std::vector<uint32_t> push;
			std::shared_ptr<const org::LatchBlock> latch;  // the chain's
			std::shared_ptr<const RenderGraph::RunGate> gate;
			uint32_t entry = 0;
			std::shared_ptr<org::Resource> indirect;  // the arguments, else direct
			uint32_t indirectOffset = 0;
		};

		struct LensDispatchBindings
		{
			DeclaredBinds views;
			std::optional<org::ResourceBindingToken> arguments;
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
				LensDispatchBindings bindings{ DeclareBinds(a_builder, spec->layout, spec->binds) };
				if (spec->indirect)
					bindings.arguments = a_builder.IndirectArguments(spec->indirect);
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
				const auto words = FillWords(spec.layout, spec.binds, a_bindings.views, *spec.latch, spec.push, a_recording);
				if (a_bindings.arguments)
					spec.program->DispatchIndirect(a_recording, words, a_recording.Resolve(*a_bindings.arguments).GetHandle(), spec.indirectOffset);
				else
					spec.gate->Dispatch(a_recording, *spec.program, words, spec.entry);
			}

		private:
			std::shared_ptr<const LensDispatch> spec;
		};

		/** @brief Ghost draws into one target, each with its push words, counts from the GPU-written arguments. */
		struct LensDraw
		{
			struct Step
			{
				std::shared_ptr<const RasterProgram> program;
				std::array<uint32_t, 4> push{};
				uint32_t indirectOffset = 0;
			};

			std::string name;
			Layout layout;
			Binds binds;
			std::shared_ptr<const org::LatchBlock> latch;
			std::shared_ptr<const RenderGraph::RunGate> gate;
			uint32_t entry = 0;
			std::shared_ptr<org::Resource> target;
			std::optional<uint32_t> slice;  // one slice of an array target
			RasterProgram::Viewport viewport;
			std::shared_ptr<org::Resource> arguments;
			std::vector<Step> steps;  // the first clears the target
		};

		struct LensDrawBindings
		{
			DeclaredBinds views;
			org::DeclaredViewToken target;
			org::ResourceBindingToken arguments;
		};

		struct LensDrawFrame
		{
			std::shared_ptr<const LensDraw> spec;
		};

		class LensDrawPass final : public org::TypedRenderGraphPass<LensDrawPass, LensDrawFrame, LensDrawBindings>
		{
		public:
			explicit LensDrawPass(std::shared_ptr<const LensDraw> a_spec) :
				spec(std::move(a_spec)) {}

			LensDrawBindings Declare(org::PassBuilder& a_builder)
			{
				LensDrawBindings bindings;
				bindings.views = DeclareBinds(a_builder, spec->layout, spec->binds);
				bindings.target = spec->slice ? a_builder.RenderTarget(org::Subresources(spec->target, org::Mip(0, 1), org::Slice(*spec->slice, 1)),
													org::RtvView{ .slice = *spec->slice })
				                                    .View() :
				                                a_builder.RenderTarget(spec->target).View();
				bindings.arguments = a_builder.IndirectArguments(spec->arguments);
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

			LensDrawFrame Prepare(const LensDrawBindings&, const org::PassPrepareContext&) const { return { spec }; }

			static bool SkipsAtExecution(const LensDrawBindings&, const LensDrawFrame& a_frame, const org::PassRecordContext& a_recording)
			{
				return !a_frame.spec->gate->IsOpen(a_recording.Run(), a_frame.spec->entry);
			}

			static void Record(const LensDrawBindings& a_bindings, const LensDrawFrame& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& spec = *a_frame.spec;
				const auto arguments = a_recording.Resolve(a_bindings.arguments).GetHandle();
				for (std::size_t i = 0; i < spec.steps.size(); ++i) {
					const auto& step = spec.steps[i];
					const auto words = FillWords(spec.layout, spec.binds, a_bindings.views, *spec.latch, step.push, a_recording);
					const RasterProgram::Colour colour{ .view = &a_bindings.target, .load = i == 0 ? rhi::LoadOp::Clear : rhi::LoadOp::Load };
					step.program->Draw(a_recording, RasterProgram::DrawDesc{ .colours = { &colour, 1 },
														.viewport = spec.viewport,
														.constants = words,
														.debugName = spec.name.c_str(),
														.indirectBuffer = arguments,
														.indirectBufferOffset = step.indirectOffset });
				}
			}

		private:
			std::shared_ptr<const LensDraw> spec;
		};

		/** @brief Copies parts of the run's region of the effect's latch block into buffers. Without a gate it copies on every run. */
		struct LensUpload
		{
			struct Part
			{
				std::shared_ptr<org::Resource> target;
				uint32_t offset = 0, bytes = 0;
			};

			std::shared_ptr<const org::LatchBlock> latch;
			std::vector<Part> parts;
			std::shared_ptr<const RenderGraph::RunGate> gate;
			uint32_t entry = 0;
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

			// A kept recording copies on closed runs too. Its readers then do nothing, so the copy is harmless.
			static bool SkipsAtExecution(const LensUploadBindings&, const LensUploadFrame& a_frame, const org::PassRecordContext& a_recording)
			{
				return a_frame.spec->gate && !a_frame.spec->gate->IsOpen(a_recording.Run(), a_frame.spec->entry);
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

		/**
		* @brief A buffer whose contents change rarely, such as the ghost paths. Each change is a new version, filled by the
		* uploader's worker path while the old version stays current, and made current at the next frame boundary after its
		* copies complete. Nothing waits: readers use the current version until then. Render thread.
		*/
		class StagedBuffer
		{
		public:
			/** @brief Throws when the buffer cannot be made. */
			StagedBuffer(std::string a_name, uint32_t a_elements, uint32_t a_stride) :
				name(std::move(a_name)), elements(a_elements), state(std::make_shared<Shared>())
			{
				auto first = org::Buffer::CreateUnmaterializedStructuredBuffer(a_elements, a_stride, false);
				first->SetName(name);
				first->Materialize();
				versions = org::VersionedBuffer::Create(std::move(first));
			}

			const std::shared_ptr<org::VersionedBuffer>& Versions() const { return versions; }

			/** @brief The payload of the current version: what the caller passed with its bytes. Null until one is current. */
			std::shared_ptr<const void> Current() const { return state->current; }

			/** @brief A new version of a_bytes, started by the next Poll. A version still uploading finishes first; only the newest
			*  waiting bytes follow it. Any thread that owns the effect's commit. */
			void Publish(std::vector<std::byte> a_bytes, std::shared_ptr<const void> a_payload) { waiting = Waiting{ std::move(a_bytes), std::move(a_payload) }; }

			/** @brief Starts the next upload, or makes a finished one current at the next frame boundary. Render thread, each frame. */
			void Poll()
			{
				if (uploading) {
					const auto& ticket = uploading->ticket;
					if (ticket->state.load(std::memory_order_acquire) == org::TrackedUploadTicketState::Cancelled) {
						Fatal("its upload was cancelled");
						return;
					}
					if (!ticket->Complete())
						return;
					RenderGraph::Frame::Driver::AtNextBegin([weak = std::weak_ptr<Shared>(state), versions = versions, done = std::move(*uploading)]() {
						const auto shared = weak.lock();
						if (!shared)
							return;
						versions->Adopt(done.version);
						shared->current = done.payload;
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->NoteNewVersions();
					});
					uploading.reset();
				}
				if (!waiting)
					return;
				auto* host = RenderGraphRuntime::Get().Host();
				const auto uploads = host ? host->RetainUploads() : nullptr;
				if (!uploads) {
					Fatal("the render graph has no uploader");
					return;
				}
				Upload next{ .version = versions->MakeStructured(elements), .payload = std::move(waiting->payload) };
				next.bytes = std::make_shared<std::vector<std::byte>>(std::move(waiting->bytes));
				waiting.reset();
				const org::StreamingUploadSegment segment{ next.bytes->data(), next.bytes->size() };
				next.ticket = uploads->QueueTrackedStreamingUploadSegments({ &segment, 1 }, segment.size,
					org::WorkerOwnedDestination{ next.version->buffer, org::WorkerOwnedDestination::Ownership::PendingVersion });
				if (!next.ticket) {
					Fatal("the uploader refused it");
					return;
				}
				uploading = std::move(next);
			}

		private:
			struct Shared
			{
				std::shared_ptr<const void> current;
			};
			struct Upload
			{
				std::shared_ptr<const org::BufferVersion> version;
				std::shared_ptr<const void> payload;
				std::shared_ptr<std::vector<std::byte>> bytes;  // kept until the copy completes
				std::shared_ptr<org::TrackedUploadTicket> ticket;
			};
			struct Waiting
			{
				std::vector<std::byte> bytes;
				std::shared_ptr<const void> payload;
			};

			/** @brief Errors are fatal: a lens model that never shows would be silent. */
			void Fatal(std::string_view a_why) const
			{
				const auto message = std::format("Community Shaders: {} could not be uploaded: {}", name, a_why);
				logger::critical("[ORG] {}", message);
				stl::report_and_fail(message);
			}

			std::string name;
			uint32_t elements = 0;
			std::shared_ptr<org::VersionedBuffer> versions;
			std::shared_ptr<Shared> state;
			std::optional<Upload> uploading;
			std::optional<Waiting> waiting;
		};

		/** @brief A device-local buffer with a CBV over a_bytes rounded to 256, as SharedConstants makes its blocks. */
		std::shared_ptr<org::Buffer> MakeConstantBuffer(uint32_t a_bytes, const char* a_name)
		{
			const uint32_t size = Align256(a_bytes);
			auto buffer = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, size);
			org::BufferBase::DescriptorRequirements views{};
			views.createCBV = true;
			views.cbvDesc = { 0, size };
			buffer->SetDescriptorRequirements(views);
			buffer->SetName(a_name);
			buffer->Materialize();
			return buffer;
		}

		/** @brief A structured buffer with an SRV and a UAV. */
		std::shared_ptr<org::Buffer> MakeStructured(uint32_t a_count, uint32_t a_stride, const char* a_name)
		{
			auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(a_count, a_stride, true);
			buffer->SetName(a_name);
			buffer->Materialize();
			return buffer;
		}

		/** @brief A raw buffer with raw SRV and UAV views, also read as indirect arguments. */
		std::shared_ptr<org::Buffer> MakeRaw(uint32_t a_bytes, const char* a_name)
		{
			auto buffer = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, a_bytes, true);
			org::BufferBase::DescriptorRequirements views{};
			views.createSRV = views.createUAV = true;
			views.srvDesc.dimension = rhi::SrvDim::Buffer;
			views.srvDesc.buffer.kind = rhi::BufferViewKind::Raw;
			views.srvDesc.buffer.numElements = a_bytes / 4;
			views.uavDesc.dimension = rhi::UavDim::Buffer;
			views.uavDesc.buffer.kind = rhi::BufferViewKind::Raw;
			views.uavDesc.buffer.numElements = a_bytes / 4;
			buffer->SetDescriptorRequirements(views);
			buffer->SetName(a_name);
			buffer->Materialize();
			return buffer;
		}

		D3D11_TEXTURE2D_DESC TextureDesc(UINT a_width, UINT a_height, UINT a_slices, DXGI_FORMAT a_format, UINT a_mips = 1,
			UINT a_bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = a_mips;
			desc.ArraySize = a_slices;
			desc.Format = a_format;
			desc.SampleDesc = { 1, 0 };
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = a_bind;
			return desc;
		}

		/** @brief The model fields of the flare constants, which the ghost paths were built for. */
		void CopyModel(Parameters& a_to, const Parameters& a_model)
		{
			a_to.surfaceCount = a_model.surfaceCount;
			a_to.surfaces = a_model.surfaces;
			a_to.pathCount = a_model.pathCount;
			a_to.sensorZ = a_model.sensorZ;
			a_to.focalLength = a_model.focalLength;
			a_to.pupilRadius = a_model.pupilRadius;
			a_to.coating = a_model.coating;
		}

		/** @brief ghost_wave.cs.hlsl's GhostWaveConstants: the cache's aperture and surface. */
		struct GhostWaveLatch
		{
			std::array<float, 4> aperture{}, surface{};
		};

		using MipGroups = std::array<GraphPasses::DispatchGroups, kAtlasMips - 1>;

		/**
		* @brief PhysicalLens::Draw on the graph. The caches (mapping, diffraction, ghost wave, planning status) are persistent
		* resources remade through gated passes only when their inputs change; a request repeats until a frame with it reached
		* the GPU with this build. The camera focus is DoF's published focus or this effect's own, read back without a wait.
		* The ghost paths are a staged buffer: a new lens model shows once its upload is done.
		*/
		class PhysicalLensEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				auto& lens = static_cast<PhysicalLens&>(a_feature);
				lens.SanitizeSettings();
				depth = FrameAnchors::EngineDepth(static_cast<uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
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
				// The starburst's and the ghosts' passes are in the build only while they are on.
				a_key.push_back((lens.settings.Diffraction ? 1u : 0u) | (lens.settings.Intensity > 0.f ? 2u : 0u));
				if (paths)
					paths->Poll();
				if (indices)
					indices->Poll();
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature& a_feature) override
			{
				auto& lens = static_cast<PhysicalLens&>(a_feature);
				const auto& shape = a_builder.GetShape();
				built = false;
				waveOn = lens.settings.Diffraction;
				ghostsOn = lens.settings.Intensity > 0.f;
				width = shape.width;
				height = shape.height;
				if (!Ensure(width, height)) {
					a_builder.Fail("its resources could not be made");
					return;
				}
				gate = a_builder.RunGate();
				chainLatch = a_builder.LatchBlock();
				a_builder.Register(*mapping);
				a_builder.Register(*focus);
				a_builder.Register(kImagingName, imagingBuffer);
				a_builder.Register(kTableName, tableBuffer);
				a_builder.Register(kLensName, lensBuffer);
				if (!readback)
					readback = RunReadback::Create("PostProcessing::PhysicalLens::FocusReadback", { 256 });
				if (!readback) {
					a_builder.Fail("its focus readback buffer could not be made");
					return;
				}
				readback->Build(a_builder);

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
				// Every run copies it, so a kept recording can replay it. Commit takes it only while the camera is active.
				readback->AddTexture(a_builder, "focus-readback", 0, ownFocus ? focus->Graph() : dofFocus.graph.Plain(), 1, 1);
				focusReset = kFocusResetCommits;

				diffraction = a_builder.Latch<PhysicalLensImaging::DiffractionParameters>();
				a_builder.PublishLatch(Published::kPhysicalLensDiffraction, diffraction);
				a_builder.Publish(Published::kPhysicalLensMapping, Image{ mapping->Graph(), kMappingFormat });

				// The reverse optics and mapping table, for the mapping and the diffraction cache.
				imagingEntry = a_builder.Gate();
				AddUpload(a_builder, "imaging-upload", { { imagingBuffer, kImagingOffset, kImagingBytes }, { tableBuffer, kTableOffset, kTableBytes } }, imagingEntry);
				mappingGate = a_builder.Gate();
				{
					Binds binds;
					binds.cbv[0] = Of(imagingBuffer);
					binds.cbv[1] = Of(tableBuffer);
					binds.uav[1] = ArrayOf(mapping->Graph());
					AddDispatch(a_builder, "mapping", kTraceShader, L"BuildCameraMaps", {}, kTraceLayout, std::move(binds), mappingGate);
				}

				if (waveOn || ghostsOn)
					BuildFlare(a_builder);
				if (a_builder.Failed())
					return;
				built = true;
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				auto& lens = static_cast<PhysicalLens&>(a_feature);
				const auto& run = a_commit.Run();
				const auto* camera = lens.owner ? lens.owner->GetActivePhysicalCameraState() : nullptr;
				const unsigned frame = globals::state->frameCount;
				float focusM = 0.f;
				const auto stamp = readback->Commit(a_commit);
				if (camera) {
					// As ResolveCameraFocus: a frame gap or a history reset starts the focus over at the manual distance.
					const bool reset = lastFocusFrame == ~0u || frame != lastFocusFrame + 1 || globals::state->ShouldResetHistory();
					if (reset) {
						// A readback of a run before the reset is dropped.
						firstFocusStamp = stamp;
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
				if (focusReset > 0)
					--focusReset;

				const bool configured = lens.PrepareOptics(aspect, camera, focusM);
				pendingMapping.reset();
				pendingWave.reset();
				pendingGhostWave.reset();
				pendingPlan.reset();
				const bool mapped = configured && lens.settings.ImageEffects;
				if (mapped != lastMapped || configured != lastConfigured)
					logger::info("[ORG] Physical Lens: the image mapping is {}{}", mapped ? "on" : "off",
						configured ? "" : " (the lens cannot be configured for these settings)");
				lastMapped = mapped;
				lastConfigured = configured;

				bool imaging = false;
				if (mapped) {
					const auto [optics, table] = lens.CameraMapping(aspect);
					if (!madeMapping || std::memcmp(&optics, &madeMapping->imaging, sizeof(optics)) || std::memcmp(&table, &madeMapping->table, sizeof(table))) {
						constants->WriteValue(run, kImagingOffset, optics);
						constants->WriteValue(run, kTableOffset, table);
						imaging = true;
						a_commit.Open(mappingGate, table.width, table.height, 3);
						pendingMapping = Mapping{ optics, table };
					}
				}

				const bool wave = built && waveOn && configured;
				const bool drawGhosts = built && ghostsOn && configured && GhostsReady();
				const bool extract = wave || (built && ghostsOn && configured);
				PhysicalLensImaging::DiffractionParameters composite{};

				float sensorWidth = 0.f, sensorHeight = 0.f;
				if (extract) {
					auto data = lens.FlareParameters(width, height, ghostSize[0], ghostSize[1], hasExposure);
					if (ghostsOn && configured)
						StageGhostPaths(lens, data);
					if (drawGhosts) {
						// The ghosts use the model their paths were built for. A new model starts the beams over, as PrepareOptics does.
						const auto model = paths->Current();
						CopyModel(data, *std::static_pointer_cast<const Parameters>(model));
						if (model.get() != beamModel) {
							beamModel = model.get();
							lastGhostFrame = ~0u;
						}
					}
					sensorWidth = data.sensorWidth;
					sensorHeight = data.sensorHeight;
					data.beamState = lastGhostFrame != ~0u && frame == lastGhostFrame + 1 && !globals::state->ShouldResetHistory() ? 2.f : 1.f;
					constants->WriteValue(run, kLensOffset, data);
					a_commit.Open(lensEntry, 1);
					a_commit.Open(extractEntry, 64, 36);
					a_commit.Open(extractOneEntry, 1);
				}

				if (wave) {
					const auto inputs = lens.DiffractionFor(width, height);
					if (!madeWave || !PhysicalLens::DiffractionCacheEqual(inputs, *madeWave)) {
						if (!imaging) {
							constants->WriteValue(run, kImagingOffset, inputs.optics);
							constants->WriteValue(run, kTableOffset, inputs.table);
							imaging = true;
						}
						for (const auto& [entry, groups] : cacheEntries)
							a_commit.Open(entry, groups[0], groups[1], groups[2]);
						pendingWave = inputs;
					}
					composite = inputs.sampling;
					a_commit.Open(waveOneEntry, 1);
					a_commit.Open(waveTilesEntry, ((width + 15) / 16 + 7) / 8, ((height + 15) / 16 + 7) / 8);
					a_commit.Open(waveTailEntry, 1);
				}
				if (imaging)
					a_commit.Open(imagingEntry, 1);
				a_commit.Write(diffraction, composite);
				constants->WriteValue(run, kWaveSeedOffset, wave ? kWaveSeed : std::array<uint32_t, 4>{});

				MipGroups mips{};
				if (drawGhosts) {
					if (lens.settings.GhostDiffraction > 0.f) {
						const auto ghostWave = lens.GhostWaveFor();
						if (!madeGhostWave || ghostWave.aperture != madeGhostWave->aperture || ghostWave.surface != madeGhostWave->surface) {
							a_commit.Write(ghostWaveLatch, GhostWaveLatch{ ghostWave.aperture, ghostWave.surface });
							a_commit.Open(ghostMaterialEntry, PhysicalLensGhostWave::Size / 8, PhysicalLensGhostWave::Size / 8);
							a_commit.Open(ghostFieldEntry, PhysicalLensGhostWave::Size / 8, PhysicalLensGhostWave::Size / 8, 3);
							a_commit.Open(ghostTransformEntry, PhysicalLensGhostWave::Size, 3);
							const auto groups = GraphPasses::GenerateMipsGroups(PhysicalLensGhostWave::Size, PhysicalLensGhostWave::Size, kAtlasMips, 1);
							std::copy_n(groups.begin(), (std::min)(groups.size(), mips.size()), mips.begin());
							pendingGhostWave = ghostWave;
						}
					}
					// Cache only selected paths' plans. A new model or sensor starts the table over.
					const PlanKey key{ paths->Current().get(), sensorWidth, sensorHeight };
					if (!madePlan || madePlan->model != key.model || madePlan->sensorWidth != key.sensorWidth || madePlan->sensorHeight != key.sensorHeight) {
						a_commit.Open(planClearEntry, (kPlanCount + 63) / 64);
						pendingPlan = key;
					}
					a_commit.Open(jobsEntry, static_cast<uint32_t>(lens.settings.MaxSources));
					a_commit.Open(ghostEntry, 1);
					a_commit.Open(resolveEntry, (ghostSize[0] + 7) / 8, (ghostSize[1] + 7) / 8);
					lastGhostFrame = frame;
				} else if (!ghostsOn || !configured)
					lastGhostFrame = ~0u;
				if (ghostsOn && built)
					a_commit.Write(mipGroups, mips);
				constants->WriteValue(run, kGhostSeedOffset, drawGhosts ? kGhostSeed : std::array<uint32_t, 32>{});
				constants->WriteValue(run, kPlanSeedOffset, drawGhosts ? kPlanSeed : std::array<uint32_t, 4>{});

				lens.orgComposite = { .cameraMap = mapped, .flare = drawGhosts, .diffraction = composite };
			}

			void Executed(PostProcessFeature&) override
			{
				readback->Executed();
				if (pendingMapping)
					madeMapping = std::exchange(pendingMapping, std::nullopt);
				if (pendingWave)
					madeWave = std::exchange(pendingWave, std::nullopt);
				if (pendingGhostWave)
					madeGhostWave = std::exchange(pendingGhostWave, std::nullopt);
				if (pendingPlan)
					madePlan = std::exchange(pendingPlan, std::nullopt);
			}

		private:
			struct Mapping
			{
				PhysicalLensImaging::GPUParameters imaging;
				PhysicalLensImaging::CameraTableParameters table;
			};

			struct PlanKey
			{
				const void* model = nullptr;  // the current ghost paths' snapshot
				float sensorWidth = 0, sensorHeight = 0;
			};

			bool GhostsReady() const { return paths && indices && paths->Current() && indices->Current(); }

			/** @brief Stages the lens model's ghost paths when they changed, with the flare constants they were built for. */
			void StageGhostPaths(const PhysicalLens& a_lens, const Parameters& a_data)
			{
				if (!paths || a_lens.GhostPathsVersion() == stagedPaths)
					return;
				stagedPaths = a_lens.GhostPathsVersion();
				const auto& source = a_lens.GhostPaths();
				std::vector<std::byte> bytes(source.size() * sizeof(source[0]));
				std::memcpy(bytes.data(), source.data(), bytes.size());
				paths->Publish(std::move(bytes), std::make_shared<const Parameters>(a_data));
			}

			void AddUpload(Builder& a_builder, std::string_view a_name, std::vector<LensUpload::Part> a_parts, std::optional<GateEntry> a_entry)
			{
				auto spec = std::make_shared<const LensUpload>(LensUpload{ .latch = constants,
					.parts = std::move(a_parts),
					.gate = a_entry ? gate : nullptr,
					.entry = a_entry ? a_entry->index : 0 });
				a_builder.Add(a_name, FrameAnchors::PassKind::Copy, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<LensUploadPass>(spec); });
			}

			/** @brief A gated direct dispatch, or with a_indirect an indirect one on runs that opened a_entry. */
			void AddDispatch(Builder& a_builder, std::string_view a_name, const char* a_source, std::wstring a_entry,
				std::vector<std::pair<std::string, std::string>> a_defines, const Layout& a_layout, Binds a_binds, GateEntry a_gate,
				std::vector<uint32_t> a_push = {}, std::shared_ptr<org::Resource> a_indirect = nullptr, uint32_t a_indirectOffset = 0)
			{
				for (const auto& slot : a_layout)
					if (slot.kind == K::Latch && !a_binds.latch[slot.reg]) {
						a_builder.Fail(std::format("{}: no latch region at b{}", a_name, slot.reg));
						return;
					}
				auto program = a_builder.Compute(a_source, WordsOf(a_layout, static_cast<uint32_t>(a_push.size())), std::move(a_defines), std::move(a_entry));
				if (!program)
					return;
				auto spec = std::make_shared<const LensDispatch>(LensDispatch{ .name = std::string(a_name),
					.program = std::move(program),
					.layout = a_layout,
					.binds = std::move(a_binds),
					.push = std::move(a_push),
					.latch = chainLatch,
					.gate = gate,
					.entry = a_gate.index,
					.indirect = std::move(a_indirect),
					.indirectOffset = a_indirectOffset });
				a_builder.Add(a_name, FrameAnchors::PassKind::Compute, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<LensDispatchPass>(spec); });
			}

			/** @brief The flare constants, the extraction front end, the diffraction passes and the ghosts. */
			void BuildFlare(Builder& a_builder)
			{
				for (const auto& [name, resource] : flareBuffers)
					a_builder.Register(name, resource);
				for (const auto* texture : { waveTextures[0].get(), waveTextures[1].get(), waveTextures[2].get(), waveMax.get(), waveRadiance.get(), ghosts.get(),
						 blueViolet.get(), areaLayers[0].get(), areaLayers[1].get(), areaLayers[2].get(), atlas.get(), material.get() })
					a_builder.Register(*texture);

				// Auto Exposure's adaptation and constants. Without them the shaders read neither (AutoExposureEnabled is 0), and the
				// latch entry takes any valid region.
				const auto adaptation = a_builder.Find(Published::kAdaptation);
				const auto autoExposure = a_builder.FindLatch<HistogramAutoExposure::AutoExposureCB>(Published::kAutoExposureConstants);
				hasExposure = adaptation && autoExposure;
				const uint32_t exposureLatch = hasExposure ? autoExposure->offset : diffraction.offset;

				lensEntry = a_builder.Gate();
				AddUpload(a_builder, "parameters-upload", { { lensBuffer, kLensOffset, kLensBytes } }, lensEntry);
				// The GPU-written arguments' seeds, on every run, so a kept recording's indirect work has zero counts on runs without it.
				AddUpload(a_builder, "arguments-seed",
					{ { waveDispatch, kWaveSeedOffset, kWaveSeedBytes }, { ghostArguments, kGhostSeedOffset, kGhostSeedBytes },
						{ planArguments, kPlanSeedOffset, kPlanSeedBytes } },
					std::nullopt);

				Binds front;
				front.cbv[0] = Of(lensBuffer);
				front.srv[0] = ImageOf(a_builder.Current());
				if (hasExposure)
					front.srv[5] = ImageOf(adaptation);
				front.latch[1] = exposureLatch;
				front.uav[0] = Of(candidates);
				front.uav[4] = Of(regions);
				extractEntry = a_builder.Gate();
				extractOneEntry = a_builder.Gate();
				AddDispatch(a_builder, "extract", kFlareShader, L"Extract", {}, kFlareLayout, front, extractEntry);
				AddDispatch(a_builder, "prepare-regions", kFlareShader, L"PrepareRegions", {}, kFlareLayout, front, extractOneEntry);
				AddDispatch(a_builder, "extract-layers", kFlareShader, L"ExtractLayers", {}, kFlareLayout, front, extractEntry);
				front.uav[1] = Of(sources);
				AddDispatch(a_builder, "select-sources", kFlareShader, L"SelectSources", {}, kFlareLayout, front, extractOneEntry);

				if (waveOn)
					BuildWave(a_builder, adaptation, exposureLatch);
				if (ghostsOn)
					BuildGhosts(a_builder, adaptation, exposureLatch);

				a_builder.Publish(Published::kPhysicalLensWaveKernel, Image{ waveKernel });
				a_builder.Publish(Published::kPhysicalLensWaveRegions, Image{ regions });
				a_builder.Publish(Published::kPhysicalLensWaveSources, Image{ waveSources });
				a_builder.Publish(Published::kPhysicalLensWaveTiles, Image{ waveTiles });
				a_builder.Publish(Published::kPhysicalLensWaveTail, Image{ waveRadiance->Graph(), DXGI_FORMAT_R32_FLOAT });
				a_builder.Publish(Published::kPhysicalLensGhosts, Image{ ghosts->Graph(), DXGI_FORMAT_R32_FLOAT });
			}

			/** @brief UpdateDiffraction's cache and Draw's per-frame diffraction passes. */
			void BuildWave(Builder& a_builder, const Image& a_adaptation, uint32_t a_exposureLatch)
			{
				// The cache, as UpdateDiffraction: t1 the input, the output at the register its shader writes.
				cacheEntries.clear();
				const auto entryFor = [&](std::array<uint32_t, 3> a_groups) {
					for (const auto& [entry, groups] : cacheEntries)
						if (groups == a_groups)
							return entry;
					cacheEntries.emplace_back(a_builder.Gate(), a_groups);
					return cacheEntries.back().first;
				};
				const auto wave = [&](uint32_t a_index) { return waveTextures[a_index]->Graph(); };
				uint32_t step = 0;
				const auto cache = [&](const char* a_source, const wchar_t* a_entry, std::optional<uint32_t> a_input, uint8_t a_slot, View a_output,
									   std::array<uint32_t, 3> a_groups) {
					Binds binds;
					binds.cbv[0] = Of(imagingBuffer);
					binds.cbv[1] = Of(tableBuffer);
					binds.latch[2] = diffraction.offset;
					if (a_input)
						binds.srv[1] = ArrayOf(wave(*a_input));
					binds.uav[a_slot] = std::move(a_output);
					AddDispatch(a_builder, std::format("wave-cache-{}", step++), a_source, a_entry, {}, kWaveLayout, std::move(binds), entryFor(a_groups));
				};
				// The paper path reuses one real-pupil spectrum for all wavelengths. Higher-density pupils are cache-only.
				cache(kDetailShader, L"BuildDetailedPupil", std::nullopt, 2, ArrayOf(wave(0)), { 128, 128, 1 });
				cache(kDetailShader, L"TransformDetailed", 0, 3, ArrayOf(wave(2)), { 1024, 1, 1 });
				cache(kDetailShader, L"TransformDetailed", 2, 3, ArrayOf(wave(0)), { 1024, 1, 1 });
				cache(kDetailShader, L"NormalizeDetailed", 0, 3, ArrayOf(wave(2)), { 128, 128, 1 });
				cache(kDetailShader, L"PrefixDetailed", 2, 3, ArrayOf(wave(0)), { 1024, 1, 1 });
				cache(kDetailShader, L"PrefixDetailed", 0, 3, ArrayOf(wave(2)), { 1024, 1, 1 });
				cache(kAmplitudeShader, L"FilterAmplitudeSpectrum", 2, 4, ArrayOf(wave(0)), { 64, 64, 3 });
				cache(kWaveShader, L"PrefixTail", 0, 3, ArrayOf(wave(2)), { 512, 3, 1 });
				cache(kWaveShader, L"PrefixTail", 2, 3, ArrayOf(wave(1)), { 512, 3, 1 });
				cache(kWaveShader, L"BuildCoreKernel", 1, 5, Of(waveKernel), { 3, 1, 1 });
				cache(kWaveShader, L"BuildWaveMax", 1, 3, ArrayOf(waveMax->Graph(), 0), { 64, 64, 3 });
				for (uint32_t mip = 1; mip < 7; ++mip) {
					const uint32_t groups = (std::max)(1u, (64u >> mip) / 8);
					Binds binds;
					binds.cbv[0] = Of(imagingBuffer);
					binds.cbv[1] = Of(tableBuffer);
					binds.latch[2] = diffraction.offset;
					binds.srv[1] = ArrayOf(waveMax->Graph(), mip - 1);
					binds.uav[3] = ArrayOf(waveMax->Graph(), mip);
					AddDispatch(a_builder, std::format("wave-max-{}", mip), kWaveShader, L"ReduceWaveMax", {}, kWaveLayout, std::move(binds), entryFor({ groups, groups, 3 }));
				}

				// Per frame, as Draw.
				Binds waveBinds;
				waveBinds.cbv[0] = Of(lensBuffer);
				waveBinds.latch[1] = a_exposureLatch;
				waveBinds.latch[2] = diffraction.offset;
				if (hasExposure)
					waveBinds.srv[5] = ImageOf(a_adaptation);
				waveBinds.srv[0] = ArrayOf(mapping->Graph());
				waveBinds.srv[1] = Of(this->sources);
				waveBinds.srv[3] = Of(waveKernel);
				waveBinds.srv[4] = ArrayOf(waveMax->Graph());
				waveBinds.uav[0] = Of(waveSources);
				waveOneEntry = a_builder.Gate();
				AddDispatch(a_builder, "wave-sources", kSourcesShader, L"BuildWaveSources", {}, kSourcesLayout, waveBinds, waveOneEntry);

				Binds tiles;
				tiles.cbv[0] = Of(lensBuffer);
				tiles.latch[1] = a_exposureLatch;
				tiles.latch[2] = diffraction.offset;
				if (hasExposure)
					tiles.srv[5] = ImageOf(a_adaptation);
				tiles.srv[1] = ArrayOf(mapping->Graph());
				tiles.srv[2] = Of(waveSources);
				tiles.srv[3] = Of(regions);
				tiles.uav[1] = Of(waveTiles);
				tiles.uav[2] = Of(waveDispatch);
				tiles.uav[3] = Of(waveWorkTiles);
				waveTilesEntry = a_builder.Gate();
				AddDispatch(a_builder, "wave-tiles", kSourcesShader, L"BuildWaveTiles", {}, kSourcesLayout, tiles, waveTilesEntry);

				Binds tail;
				tail.latch[2] = diffraction.offset;
				tail.srv[0] = Of(waveSources);
				tail.srv[1] = Of(waveTiles);
				tail.srv[2] = ArrayOf(wave(1));
				tail.srv[3] = Of(waveWorkTiles);
				tail.srv[4] = Of(waveDispatch);
				tail.uav[0] = ArrayOf(waveRadiance->Graph());
				tail.sampler[0] = sampler.Index();
				waveTailEntry = a_builder.Gate();
				AddDispatch(a_builder, "wave-tail", kTailShader, L"BuildWaveTail", {}, kTailLayout, tail, waveTailEntry, {}, waveDispatch, 0);
			}

			/** @brief UpdateGhostWave's cache, then Draw's ghost jobs, planning, tracing, raster and resolve. */
			void BuildGhosts(Builder& a_builder, const Image& a_adaptation, uint32_t a_exposureLatch)
			{
				// The ghost-wave cache: material, then for each of 11 samples the field, four transforms and the accumulation.
				ghostWaveLatch = a_builder.Latch<GhostWaveLatch>();
				ghostMaterialEntry = a_builder.Gate();
				ghostFieldEntry = a_builder.Gate();
				ghostTransformEntry = a_builder.Gate();
				const std::array scratch{ waveTextures[0]->Graph(), waveTextures[2]->Graph() };
				{
					Binds binds;
					binds.latch[1] = ghostWaveLatch.offset;
					binds.uav[2] = Of(material->Graph());
					AddDispatch(a_builder, "ghost-material", kGhostWaveShader, L"BuildMaterial", {}, kGhostWaveLayout, std::move(binds), ghostMaterialEntry, { 0, 0, 0, 0 });
				}
				for (uint32_t sample = 0; sample < 11; ++sample) {
					Binds field;
					field.latch[1] = ghostWaveLatch.offset;
					field.srv[1] = Of(material->Graph());
					field.uav[0] = ArrayOf(scratch[0]);
					AddDispatch(a_builder, std::format("ghost-field-{}", sample), kGhostWaveShader, L"BuildField", {}, kGhostWaveLayout, std::move(field),
						ghostFieldEntry, { sample, 0, 0, 0 });
					for (uint32_t pass = 0; pass < 4; ++pass) {
						Binds transform;
						transform.latch[1] = ghostWaveLatch.offset;
						transform.srv[0] = ArrayOf(scratch[pass % 2]);
						transform.uav[0] = ArrayOf(scratch[1 - pass % 2]);
						AddDispatch(a_builder, std::format("ghost-transform-{}-{}", sample, pass), kGhostWaveShader, L"Transform", {}, kGhostWaveLayout,
							std::move(transform), ghostTransformEntry, { sample, pass >= 2 ? 1u : 0u, pass == 2 ? 1u : 0u, 0 });
					}
					Binds accumulate;
					accumulate.latch[1] = ghostWaveLatch.offset;
					accumulate.srv[0] = ArrayOf(scratch[0]);
					accumulate.uav[1] = MipOf(atlas->Graph(), 0);
					AddDispatch(a_builder, std::format("ghost-accumulate-{}", sample), kGhostWaveShader, L"Accumulate", {}, kGhostWaveLayout, std::move(accumulate),
						ghostMaterialEntry, { sample, 0, 0, 0 });
				}
				mipGroups = a_builder.Latch<MipGroups>();
				if (!generateMips)
					generateMips = GraphPasses::LoadGenerateMips(RenderGraphRuntime::Get().Host()->Device(), false);
				if (!generateMips) {
					a_builder.Fail("the ghost-wave atlas's mip program did not build");
					return;
				}
				a_builder.Add("ghost-mips", FrameAnchors::PassKind::Compute,
					[program = generateMips, texture = atlas->Graph(), latch = chainLatch, offset = mipGroups.offset]() {
						return GraphPasses::MakeGenerateMips(program, texture, { latch, offset });
					});

				// Draw's ghost passes. Every flare-list pass binds the lens block, the exposure and the ghost paths.
				const auto common = [&]() {
					Binds binds;
					binds.cbv[0] = Of(lensBuffer);
					binds.latch[1] = a_exposureLatch;
					if (hasExposure)
						binds.srv[5] = ImageOf(a_adaptation);
					binds.srv[1] = Of(sources);
					binds.srv[2] = View{ .versioned = paths->Versions() };
					return binds;
				};
				ghostEntry = a_builder.Gate();
				jobsEntry = a_builder.Gate();
				planClearEntry = a_builder.Gate();
				resolveEntry = a_builder.Gate();
				const std::vector<std::pair<std::string, std::string>> area{ { "PHYSICAL_LENS_AREA_LAYERS", "1" } };
				const std::vector<std::pair<std::string, std::string>> planning{ { "PHYSICAL_LENS_PLANNING", "1" } };
				{
					auto binds = common();
					binds.uav[2] = Of(jobs);
					binds.uav[3] = Of(ghostArguments);
					AddDispatch(a_builder, "build-jobs", kFlareShader, L"BuildJobs", area, kFlareLayout, std::move(binds), jobsEntry);
				}
				{
					Binds binds = common();
					binds.uav[0] = Of(planStatus);
					AddDispatch(a_builder, "plan-clear", kPlanningShader, L"ClearPlanStatus", {}, kFlareLayout, std::move(binds), planClearEntry);
				}
				{
					auto binds = common();
					binds.uav[0] = Of(planStatus);
					binds.uav[3] = Of(planArguments);
					binds.uav[4] = Of(planRequests);
					binds.srv[3] = Of(jobs);
					AddDispatch(a_builder, "request-plans", kPlanningShader, L"RequestPlans", {}, kFlareLayout, std::move(binds), ghostEntry, {}, ghostArguments, 32);
				}
				{
					auto binds = common();
					binds.srv[0] = Of(planRequests);
					binds.uav[5] = Of(planTable);
					AddDispatch(a_builder, "bake-plans", kPlanningShader, L"BakeRequestedPlans", {}, kFlareLayout, std::move(binds), ghostEntry, {}, planArguments, 0);
				}
				{
					auto binds = common();
					binds.uav[2] = Of(jobs);
					binds.uav[4] = Of(pupils);
					binds.srv[6] = Of(planTable);
					AddDispatch(a_builder, "expand-jobs", kPlanningShader, L"ExpandCachedJobs", {}, kFlareLayout, std::move(binds), ghostEntry, {}, ghostArguments, 32);
				}
				{
					auto binds = common();
					binds.srv[3] = Of(jobs);
					binds.srv[4] = Of(pupils);
					binds.uav[1] = Of(rays);
					AddDispatch(a_builder, "trace-beams", kBeamTraceShader, L"TraceBeams", planning, kFlareLayout, binds, ghostEntry, {}, ghostArguments, 0);
					AddDispatch(a_builder, "shade-beams", kBeamTraceShader, L"ShadeBeams", area, kFlareLayout, std::move(binds), ghostEntry, {}, ghostArguments, 0);
				}
				{
					auto binds = common();
					binds.srv[0] = Of(rays);
					binds.srv[3] = Of(jobs);
					binds.srv[4] = Of(pupils);
					binds.uav[4] = Of(bundles);
					AddDispatch(a_builder, "reduce-ghosts", kFlareShader, L"ReduceGhosts", planning, kFlareLayout, std::move(binds), ghostEntry, {}, ghostArguments, 48);
				}

				// The raster, as Draw: two bands into the ghost slices, blue and violet packed, then the three area layers.
				Binds beam;
				beam.cbv[0] = Of(lensBuffer);
				beam.srv[2] = View{ .versioned = paths->Versions() };
				beam.srv[0] = Of(rays);
				beam.srv[1] = Of(sources);
				beam.srv[3] = Of(jobs);
				beam.srv[4] = Of(bundles);
				beam.srv[5] = Of(atlas->Graph());
				beam.srv[7] = View{ .versioned = indices->Versions() };
				beam.sampler[0] = sampler.Index();
				const auto additive = [](DXGI_FORMAT a_format) {
					return RasterProgram::Target{ .format = rhi::helpers::ToRHI(a_format),
						.blend = { .enable = true,
							.srcColor = rhi::BlendFactor::One,
							.dstColor = rhi::BlendFactor::One,
							.colorOp = rhi::BlendOp::Add,
							.srcAlpha = rhi::BlendFactor::One,
							.dstAlpha = rhi::BlendFactor::One,
							.alphaOp = rhi::BlendOp::Add } };
				};
				const uint32_t words = WordsOf(kBeamLayout, 4);
				const auto program = [&](const wchar_t* a_entry, DXGI_FORMAT a_format) {
					return a_builder.RasterTargets(kBeamShader, words, { additive(a_format) }, area, a_entry, kBeamShader, L"VSMain");
				};
				const auto fine = program(L"PSMain", DXGI_FORMAT_R32_FLOAT);
				const auto violet = program(L"PSPackedViolet", DXGI_FORMAT_R32G32_FLOAT);
				const auto broad = program(L"PSArea", DXGI_FORMAT_R32G32B32A32_FLOAT);
				if (!fine || !violet || !broad)
					return;
				const RasterProgram::Viewport output{ .width = ghostSize[0], .height = ghostSize[1] };
				const auto draw = [&](std::string a_name, std::shared_ptr<org::Resource> a_target, std::optional<uint32_t> a_slice, RasterProgram::Viewport a_viewport,
									  std::vector<LensDraw::Step> a_steps) {
					auto spec = std::make_shared<const LensDraw>(LensDraw{ .name = a_name,
						.layout = kBeamLayout,
						.binds = beam,
						.latch = chainLatch,
						.gate = gate,
						.entry = ghostEntry.index,
						.target = std::move(a_target),
						.slice = a_slice,
						.viewport = a_viewport,
						.arguments = ghostArguments,
						.steps = std::move(a_steps) });
					a_builder.Add(a_name, FrameAnchors::PassKind::Render, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<LensDrawPass>(spec); });
				};
				draw("ghosts-red", ghosts->Graph(), 0, output, { { fine, { 0, 0, 0, 0 }, 12 } });
				draw("ghosts-green", ghosts->Graph(), 1, output, { { fine, { 1, 0, 0, 0 }, 12 } });
				draw("ghosts-blue", blueViolet->Graph(), std::nullopt, output, { { violet, { 2, 0, 0, 0 }, 12 } });
				for (uint32_t level = 0; level < 3; ++level) {
					const auto& desc = areaLayers[level]->Desc();
					std::vector<LensDraw::Step> steps;
					for (uint32_t band = 0; band < 3; ++band)
						steps.push_back({ broad, { band, level + 1, 0, 0 }, 64 + 20 * level });
					draw(std::format("ghosts-area-{}", level), areaLayers[level]->Graph(), std::nullopt, { .width = desc.Width, .height = desc.Height }, std::move(steps));
				}

				Binds resolve;
				resolve.srv[0] = Of(ghostArguments);
				resolve.srv[1] = Of(blueViolet->Graph());
				for (uint32_t level = 0; level < 3; ++level)
					resolve.srv[2 + level] = Of(areaLayers[level]->Graph());
				resolve.uav[0] = ArrayOf(ghosts->Graph());
				resolve.uav[1] = Of(waveTiles);
				resolve.sampler[0] = sampler.Index();
				AddDispatch(a_builder, "resolve", kResolveShader, L"ResolveArea", {}, kResolveLayout, std::move(resolve), resolveEntry);
			}

			/** @brief The persistent resources: made once for the session, the size-dependent ones again when the chain's size changes. */
			bool Ensure(uint32_t a_width, uint32_t a_height)
			{
				try {
					const GraphResources::Options persistent{ .lifetime = GraphResources::Lifetime::Persistent };
					if (!mapping) {
						const PhysicalLensImaging::CameraTableParameters table{};
						mapping = std::make_unique<GraphResources::Texture2D>(TextureDesc(table.width, table.height, 3, kMappingFormat), std::string(Published::kPhysicalLensMapping), persistent);
						focus = std::make_unique<GraphResources::Texture2D>(TextureDesc(1, 1, 1, kFocusFormat), "PostProcessing::PhysicalLens::Focus",
							GraphResources::Options{ .lifetime = GraphResources::Lifetime::History });
						constants = org::LatchBlock::Create("cs.pp.physical-lens.constants", kUploadBytes);
						imagingBuffer = MakeConstantBuffer(kImagingBytes, kImagingName);
						tableBuffer = MakeConstantBuffer(kTableBytes, kTableName);
						lensBuffer = MakeConstantBuffer(kLensBytes, kLensName);
						// PhysicalLens::sampler: linear, border, every mip.
						sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{ .addressU = rhi::AddressMode::Border,
							.addressV = rhi::AddressMode::Border,
							.addressW = rhi::AddressMode::Border });
						madeMapping.reset();
					}
					if (!candidates) {
						// As PhysicalLens::SetupResources.
						constexpr uint32_t sourceCount = PhysicalLens::SourceCount, perSource = PhysicalLensOptics::PathsPerSource;
						constexpr uint32_t rayCount = 6 * perSource * 3 * PhysicalLensOptics::Vertices;
						const auto add = [&](std::shared_ptr<org::Buffer>& a_buffer, uint32_t a_count, uint32_t a_stride, const char* a_name) {
							a_buffer = MakeStructured(a_count, a_stride, a_name);
							flareBuffers.emplace_back(a_name, a_buffer);
						};
						add(candidates, PhysicalLens::CandidateCount, 32, "PostProcessing::PhysicalLens::Candidates");
						add(regions, PhysicalLens::TileCount, 16, std::string(Published::kPhysicalLensWaveRegions).c_str());
						add(sources, sourceCount, 32, "PostProcessing::PhysicalLens::Sources");
						add(waveKernel, 12, 16, std::string(Published::kPhysicalLensWaveKernel).c_str());
						add(waveSources, sourceCount * 7, 16, std::string(Published::kPhysicalLensWaveSources).c_str());
						add(jobs, sourceCount * perSource, 32, "PostProcessing::PhysicalLens::VisibleJobs");
						add(bundles, sourceCount * perSource * 3, 32, "PostProcessing::PhysicalLens::Bundles");
						add(rays, rayCount, 16, "PostProcessing::PhysicalLens::Rays");
						add(pupils, sourceCount * perSource * PhysicalLensOptics::Grid, 8, "PostProcessing::PhysicalLens::PupilCoordinates");
						add(planTable, kPlanCount * 6, 16, "PostProcessing::PhysicalLens::PlanningTable");
						add(planStatus, kPlanCount, 4, "PostProcessing::PhysicalLens::PlanningValidity");
						add(planRequests, 4 * perSource, 4, "PostProcessing::PhysicalLens::PlanningRequests");
						waveDispatch = MakeRaw(16, "PostProcessing::PhysicalLens::DiffractionDispatch");
						ghostArguments = MakeRaw(128, "PostProcessing::PhysicalLens::IndirectArguments");
						planArguments = MakeRaw(16, "PostProcessing::PhysicalLens::PlanningDispatch");
						flareBuffers.emplace_back("PostProcessing::PhysicalLens::DiffractionDispatch", waveDispatch);
						flareBuffers.emplace_back("PostProcessing::PhysicalLens::IndirectArguments", ghostArguments);
						flareBuffers.emplace_back("PostProcessing::PhysicalLens::PlanningDispatch", planArguments);
						for (uint32_t i = 0; i < 3; ++i)
							waveTextures[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(i == 1 ? 512 : 1024, i == 1 ? 512 : 1024, 3, DXGI_FORMAT_R32G32B32A32_FLOAT),
								std::format("PostProcessing::PhysicalLens::WaveScratch{}", i), persistent);
						waveMax = std::make_unique<GraphResources::Texture2D>(TextureDesc(64, 64, 3, DXGI_FORMAT_R32_FLOAT, 7), "PostProcessing::PhysicalLens::WaveMaxDensity", persistent);
						atlas = std::make_unique<GraphResources::Texture2D>(TextureDesc(PhysicalLensGhostWave::Size, PhysicalLensGhostWave::Size, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, kAtlasMips),
							"PostProcessing::PhysicalLens::GhostWaveAtlas", persistent);
						material = std::make_unique<GraphResources::Texture2D>(TextureDesc(PhysicalLensGhostWave::Size, PhysicalLensGhostWave::Size, 1, DXGI_FORMAT_R32G32B32A32_FLOAT),
							"PostProcessing::PhysicalLens::GhostPupilMaterial", persistent);
						paths = std::make_unique<StagedBuffer>("PostProcessing::PhysicalLens::Paths", PhysicalLensOptics::MaxPaths,
							static_cast<uint32_t>(sizeof(PhysicalLensOptics::GhostPath)));
						indices = std::make_unique<StagedBuffer>("PostProcessing::PhysicalLens::BeamIndices", PhysicalLensOptics::BeamDrawVertices, 4u);
						// The beam's 16-bit index list as 32-bit words, for the vertex stage's pull.
						const auto list = PhysicalLensOptics::BeamIndices<std::uint32_t>();
						std::vector<std::byte> bytes(sizeof(list));
						std::memcpy(bytes.data(), list.data(), bytes.size());
						indices->Publish(std::move(bytes), std::make_shared<const int>(0));
						stagedPaths = 0;
						madeWave.reset();
						madeGhostWave.reset();
						madePlan.reset();
					}
					if (!waveRadiance || waveRadiance->Desc().Width != a_width || waveRadiance->Desc().Height != a_height) {
						// As SetupResources and UpdateDiffraction: sized by the chain's input.
						const uint32_t tiles = ((a_width + 15) / 16) * ((a_height + 15) / 16);
						std::erase_if(flareBuffers, [](const auto& a_entry) { return a_entry.first.ends_with("DiffractionTiles") || a_entry.first.ends_with("ActiveDiffractionTiles"); });
						waveTiles = MakeStructured(tiles, 4, std::string(Published::kPhysicalLensWaveTiles).c_str());
						waveWorkTiles = MakeStructured(tiles, 4, "PostProcessing::PhysicalLens::ActiveDiffractionTiles");
						flareBuffers.emplace_back(std::string(Published::kPhysicalLensWaveTiles), waveTiles);
						flareBuffers.emplace_back("PostProcessing::PhysicalLens::ActiveDiffractionTiles", waveWorkTiles);
						waveRadiance = std::make_unique<GraphResources::Texture2D>(TextureDesc(a_width, a_height, 3, DXGI_FORMAT_R32_FLOAT), std::string(Published::kPhysicalLensWaveTail), persistent);
						ghostSize = { (std::max)(1u, (a_width + 1) / 2), (std::max)(1u, (a_height + 1) / 2) };
						constexpr UINT targets = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
						ghosts = std::make_unique<GraphResources::Texture2D>(TextureDesc(ghostSize[0], ghostSize[1], 3, DXGI_FORMAT_R32_FLOAT, 1, targets | D3D11_BIND_UNORDERED_ACCESS),
							std::string(Published::kPhysicalLensGhosts), persistent);
						// Floating-point blending must not overflow on concentrated caustics.
						blueViolet = std::make_unique<GraphResources::Texture2D>(TextureDesc(ghostSize[0], ghostSize[1], 1, DXGI_FORMAT_R32G32_FLOAT, 1, targets),
							"PostProcessing::PhysicalLens::BlueViolet", persistent);
						for (uint32_t level = 0; level < 3; ++level)
							areaLayers[level] = std::make_unique<GraphResources::Texture2D>(
								TextureDesc((std::max)(1u, ghostSize[0] >> (2 + 2 * level)) + 8, (std::max)(1u, ghostSize[1] >> (2 + 2 * level)) + 8, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, targets),
								std::format("PostProcessing::PhysicalLens::AreaLayer{}", level), persistent);
					}
				} catch (const std::exception& e) {
					logger::error("[ORG] Physical Lens: its resources could not be made: {}", e.what());
					mapping.reset();
					candidates.reset();
					waveRadiance.reset();
					return false;
				}
				const auto made = [](const auto& a_twin) { return a_twin && *a_twin; };
				return made(mapping) && made(focus) && constants && imagingBuffer && tableBuffer && lensBuffer && sampler && made(waveTextures[0]) &&
				       made(waveTextures[1]) && made(waveTextures[2]) && made(waveMax) && made(atlas) && made(material) && made(waveRadiance) && made(ghosts) &&
				       made(blueViolet) && made(areaLayers[0]) && made(areaLayers[1]) && made(areaLayers[2]);
			}

			/** @brief The newest focus readback since the last reset, in metres, as ResolveCameraFocus maps its staging copy. */
			void TakeFocus()
			{
				std::uint32_t stamp = 0;
				const auto bytes = readback->Take(0, &stamp);
				if (bytes.size() < sizeof(float) || stamp < firstFocusStamp)
					return;
				float km = 0.f;
				std::memcpy(&km, bytes.data(), sizeof(km));
				const float metres = km * 1000.f;
				if (std::isfinite(metres) && metres > 0.f)
					resolvedFocusM = metres;
			}

			FrameAnchors::Texture depth;
			float aspect = 16.f / 9.f;
			uint32_t width = 0, height = 0;
			std::array<uint32_t, 2> ghostSize{ 1, 1 };
			bool built = false, waveOn = false, ghostsOn = false, hasExposure = false;

			// The persistent resources.
			std::unique_ptr<GraphResources::Texture2D> mapping;  // the image mapping, RGBA32F, 3 bands
			std::unique_ptr<GraphResources::Texture2D> focus;    // this effect's focus in km when DoF is not in the chain
			std::shared_ptr<org::LatchBlock> constants;          // the large blocks and the argument seeds, by run
			std::shared_ptr<org::Buffer> imagingBuffer, tableBuffer, lensBuffer;
			org::OwnedDescriptorBinding sampler;
			std::shared_ptr<org::Buffer> candidates, regions, sources, waveKernel, waveSources, waveTiles, waveWorkTiles, jobs, bundles, rays, pupils;
			std::shared_ptr<org::Buffer> planTable, planStatus, planRequests, waveDispatch, ghostArguments, planArguments;
			std::vector<std::pair<std::string, std::shared_ptr<org::Resource>>> flareBuffers;  // registered at each build
			std::array<std::unique_ptr<GraphResources::Texture2D>, 3> waveTextures;  // 1024, 512 and 1024 squared, 3 bands; 0 and 2 are scratch
			std::unique_ptr<GraphResources::Texture2D> waveMax, waveRadiance, ghosts, blueViolet, atlas, material;
			std::array<std::unique_ptr<GraphResources::Texture2D>, 3> areaLayers;
			std::unique_ptr<StagedBuffer> paths, indices;
			std::uint64_t stagedPaths = 0;
			std::shared_ptr<const GraphPasses::GenerateMipsProgram> generateMips;

			std::shared_ptr<const RenderGraph::RunGate> gate;
			std::shared_ptr<const org::LatchBlock> chainLatch;

			std::shared_ptr<RunReadback> readback;  // kept across builds
			std::uint32_t firstFocusStamp = 0;      // the stamp of the last reset's run
			bool ownFocus = true;
			unsigned lastFocusFrame = ~0u, lastGhostFrame = ~0u;
			const void* beamModel = nullptr;  // the paths snapshot the beams last traced
			float resolvedFocusM = 10.f;
			uint32_t focusReset = 0;
			bool lastMapped = false, lastConfigured = true;  // for the log line when either changes

			// What each cache was made from, and what this frame asked for until Executed.
			std::optional<Mapping> madeMapping, pendingMapping;
			std::optional<PhysicalLens::DiffractionInputs> madeWave, pendingWave;
			std::optional<PhysicalLensGhostWave::Parameters> madeGhostWave, pendingGhostWave;
			std::optional<PlanKey> madePlan, pendingPlan;

			LatchRegion<PhysicalLens::FocusInputs> focusConstants;
			LatchRegion<PhysicalLensImaging::DiffractionParameters> diffraction;
			LatchRegion<GhostWaveLatch> ghostWaveLatch;
			LatchRegion<MipGroups> mipGroups;
			GateEntry focusGate, mappingGate, imagingEntry, lensEntry, extractEntry, extractOneEntry, waveOneEntry, waveTilesEntry, waveTailEntry;
			GateEntry ghostEntry, jobsEntry, planClearEntry, resolveEntry, ghostMaterialEntry, ghostFieldEntry, ghostTransformEntry;
			std::vector<std::pair<GateEntry, std::array<uint32_t, 3>>> cacheEntries;
		};
	}

	std::unique_ptr<Effect> MakePhysicalLens() { return std::make_unique<PhysicalLensEffect>(); }
}

#endif
