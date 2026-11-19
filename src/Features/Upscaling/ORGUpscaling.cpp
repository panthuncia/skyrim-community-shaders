#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGUpscaling.h"
#	include "RenderGraph/Engine/Native/Native.h"

#	include "Features/HDRDisplay/ORGHDRDisplay.h"
#	include "Features/Upscaling.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/Backend.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/FrameStaging.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <array>
#	include <map>
#	include <mutex>
#	include <utility>
#	include <vector>

namespace
{
	using RenderGraph::AnchorPasses::Bind;

	constexpr const char* kClientId = "cs.upscaling";
	constexpr const char* kHudlessClientId = "cs.upscaling.hudless";
	// Frame generation's inputs, after the hudless copy in the same step.
	constexpr const char* kFrameGenInputsClientId = "cs.upscaling.fg-inputs";
	// On only while an SDK upscaler is the method, so the post stage's build leaves the pass out with TAA or no upscaler.
	constexpr const char* kEvaluateClientId = "cs.upscaling.evaluate";
	constexpr const char* kUpscaledName = "Upscaling::UpscaledTexture";
	constexpr const char* kHudlessName = "Upscaling::HudlessTexture";
	constexpr const char* kDepthShader = "Upscaling/DepthRefractionUpscalePS.hlsl";
	constexpr const char* kUnderwaterShader = "Upscaling/UnderwaterMaskUpscalePS.hlsl";
	// A plain Load and write, for the hudless copy and the upscaled colour's copy into kMAIN.
	constexpr const char* kCopyShader = "Upscaling/CopyHudlessPS.hlsl";
	constexpr std::int32_t kOrderBase = 100;

	// What a frame queues for the Upscale anchor.
	constexpr std::uint32_t kCopyToMain = 1u << 0;
	constexpr std::uint32_t kDepthUpscale = 1u << 1;
	constexpr std::uint32_t kEvaluate = 1u << 2;

	// The copy into kMAIN, and the depth upscale's two draws.
	constexpr std::uint32_t kCopyEntry = 0;
	constexpr std::uint32_t kDepthEntry = 1;
	constexpr std::uint32_t kUpscaleEntries = 2;

	// Push blocks in list order.
	struct DepthBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Handle refractionNormals, depth, linearSampler;
		ShaderBindings::Latch jitter;
	};

	struct UnderwaterBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Handle underwaterMask, linearSampler;
		ShaderBindings::Latch jitter;
	};

	struct CopyBlock
	{
		ShaderBindings::Handle colorInput;
	};

	static_assert(sizeof(Upscaling::JitterCB) == 16, "the latch struct is one 16-byte row, as the shaders' JitterConstants");

	rhi::Format FormatOf(ID3D11RenderTargetView* a_rtv)
	{
		D3D11_RENDER_TARGET_VIEW_DESC desc{};
		a_rtv->GetDesc(&desc);
		return rhi::helpers::ToRHI(desc.Format);
	}

	rhi::Format FormatOf(ID3D11DepthStencilView* a_dsv)
	{
		D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
		a_dsv->GetDesc(&desc);
		return rhi::helpers::ToRHI(desc.Format);
	}

	std::pair<std::uint32_t, std::uint32_t> SizeOf(ID3D11Texture2D* a_texture)
	{
		D3D11_TEXTURE2D_DESC desc{};
		a_texture->GetDesc(&desc);
		return { desc.Width, desc.Height };
	}

	/** @brief The engine targets the Upscale anchor's passes use. */
	struct DepthInputs
	{
		FrameAnchors::Texture main;
		rhi::Format mainFormat = rhi::Format::Unknown;  // the copy's target format
		FrameAnchors::Texture mainDepth, motion;        // read by the evaluate where it is a stage's pass
		FrameAnchors::Texture depth, depthCopy;
		FrameAnchors::Texture refraction, refractionCopy, saoCameraZ;
		FrameAnchors::Texture underwater, underwaterCopy;
		std::pair<std::uint32_t, std::uint32_t> depthSize, refractionSize, underwaterSize;  // the copies' sizes
		std::uint32_t width = 0, height = 0;  // the depth upscale's render area

		bool operator==(const DepthInputs&) const = default;

		bool DepthComplete() const
		{
			return depth && depthCopy && refraction && refractionCopy && saoCameraZ && underwater && underwaterCopy && width && height;
		}
	};

	/** @brief The engine's view formats. */
	struct DepthFormats
	{
		rhi::Format refraction = rhi::Format::Unknown, saoCameraZ = rhi::Format::Unknown, underwater = rhi::Format::Unknown;
		rhi::Format depth = rhi::Format::Unknown;

		bool operator==(const DepthFormats&) const = default;
	};

	/** @brief Whether a stage in this mode holds the Upscale anchor. True in native mode and now in guest mode too. */
	bool EvaluateInStage()
	{
		return FrameAnchors::StageStepsAt(FrameAnchors::Anchor::Upscale);
	}

	/** @brief Also the depth and motion vectors when the evaluate is a stage's pass. A null main when it cannot be imported. */
	void CurrentMain(DepthInputs& a_inputs)
	{
		auto* renderer = globals::game::renderer;
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		a_inputs.main = main.RTV ? FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN)) : nullptr;
		a_inputs.mainFormat = main.RTV ? FormatOf(main.RTV) : rhi::Format::Unknown;
		if (!EvaluateInStage())
			return;
		const auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		const auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		a_inputs.mainDepth = depth.texture ? FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN)) : nullptr;
		a_inputs.motion = motion.texture ? FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMOTION_VECTOR)) : nullptr;
	}

	/** @brief Empty depth fields when one is missing or a copy is its own source. The D3D11 path then draws. */
	void CurrentDepthInputs(DepthInputs& a_inputs, DepthFormats& a_formats)
	{
		auto* renderer = globals::game::renderer;
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
		auto& refraction = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
		auto& saoCameraZ = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSAO_CAMERAZ];
		auto& underwater = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kUNDERWATER_MASK];
		a_inputs.width = globals::game::graphicsState->screenWidth;
		a_inputs.height = globals::game::graphicsState->screenHeight;
		if (!depth.texture || !depth.views[0] || !depthCopy.texture || !depthCopy.depthSRV || depthCopy.texture == depth.texture ||
			!refraction.texture || !refraction.textureCopy || !refraction.SRVCopy || !refraction.RTV || refraction.textureCopy == refraction.texture ||
			!saoCameraZ.RTV || !underwater.texture || !underwater.textureCopy || !underwater.SRVCopy || !underwater.RTV ||
			underwater.textureCopy == underwater.texture)
			return;
		a_inputs.depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
		a_inputs.depthCopy = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY));
		a_inputs.refraction = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kREFRACTION_NORMALS));
		a_inputs.refractionCopy = FrameAnchors::D3D11Texture("Upscaling::RefractionNormalsCopy", refraction.textureCopy, refraction.SRVCopy, nullptr);
		a_inputs.saoCameraZ = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kSAO_CAMERAZ));
		a_inputs.underwater = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kUNDERWATER_MASK));
		a_inputs.underwaterCopy = FrameAnchors::D3D11Texture("Upscaling::UnderwaterMaskCopy", underwater.textureCopy, underwater.SRVCopy, nullptr);
		a_inputs.depthSize = SizeOf(depth.texture);
		a_inputs.refractionSize = SizeOf(refraction.texture);
		a_inputs.underwaterSize = SizeOf(underwater.texture);
		a_formats = { FormatOf(refraction.RTV), FormatOf(saoCameraZ.RTV), FormatOf(underwater.RTV), FormatOf(depth.views[0]) };
	}

	std::shared_ptr<const RasterProgram> LoadDepthProgram(rhi::Device a_device, const DepthFormats& a_formats)
	{
		// As the D3D11 states: no blending, depth written with the test always passing, no culling.
		return RasterProgram::Load(a_device, { .pixelSource = kDepthShader,
												 .constantWords = ShaderBindings::Words<DepthBlock>(),
												 .targets = { { .format = a_formats.refraction }, { .format = a_formats.saoCameraZ } },
												 .depthFormat = a_formats.depth,
												 .depth = { .depthEnable = true, .depthWrite = true, .depthFunc = rhi::CompareOp::Always } });
	}

	std::shared_ptr<const RasterProgram> LoadUnderwaterProgram(rhi::Device a_device, const DepthFormats& a_formats)
	{
		return RasterProgram::Load(a_device, { .pixelSource = kUnderwaterShader,
												 .constantWords = ShaderBindings::Words<UnderwaterBlock>(),
												 .targets = { { .format = a_formats.underwater } } });
	}

	std::shared_ptr<const RasterProgram> LoadCopyProgram(rhi::Device a_device, rhi::Format a_format)
	{
		return RasterProgram::Load(a_device, { .pixelSource = kCopyShader,
												 .constantWords = ShaderBindings::Words<CopyBlock>(),
												 .targets = { { .format = a_format } } });
	}

	struct EvaluateWork;
	struct EvaluateShared;

	/** @brief Default usage, no CPU access, no misc flags, as the graph twins require. */
	D3D11_TEXTURE2D_DESC Sanitized(D3D11_TEXTURE2D_DESC a_desc, UINT a_bindFlags)
	{
		a_desc.Usage = D3D11_USAGE_DEFAULT;
		a_desc.CPUAccessFlags = 0;
		a_desc.MiscFlags = 0;
		a_desc.BindFlags = a_bindFlags;
		return a_desc;
	}
}

struct ORGUpscaling::Impl
{
	std::shared_ptr<const RasterProgram> depthProgram, underwaterProgram;
	DepthFormats formats;
	std::map<rhi::Format, std::shared_ptr<const RasterProgram>> copyPrograms;  // by target format
	org::OwnedDescriptorBinding linearSampler;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> upscaleGate, hudlessGate;

	std::unique_ptr<GraphResources::Texture2D> upscaled, hudless;
	DepthInputs inputs;
	FrameAnchors::Texture hudlessSource;

	// Render thread.
	std::uint32_t queued = 0;
	Upscaling::JitterCB jitter{};
	// Native mode: each run's evaluate, stored by the commit.
	std::shared_ptr<const EvaluateShared> evaluateShared;
	std::shared_ptr<const EvaluateWork> evaluate;
	// Frame generation's inputs: each run's prepare, stored by the commit, and the engine targets the pass imports.
	std::shared_ptr<const EvaluateShared> inputsShared;
	std::shared_ptr<const EvaluateWork> inputsWork;
	FrameAnchors::Texture inputsDepth, inputsMotion;
	bool inputsRegistered = false, inputsArmed = false;

	bool registered = false, hudlessRegistered = false;
	// Set only around CopyHudless's tap, so the clean scene's tap of the same step does not copy the hudless.
	bool hudlessArmed = false;
	bool depthRegistered = false;
	bool failed = false;
};

namespace
{
	using Impl = ORGUpscaling::Impl;

	/** @brief Immutable once built. */
	struct DepthSpec
	{
		std::shared_ptr<const RasterProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t sampler = 0;
		DepthInputs inputs;
	};

	struct DepthBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken refractionCopy, depthCopy, refraction, saoCameraZ, depth;
	};

	struct DepthFrame
	{
		std::shared_ptr<const DepthSpec> spec;
	};

	/** @brief Writes two colour targets and depth, which AnchorPasses::Draw does not cover, so it is its own pass. Gated by the depth entry. */
	class DepthUpscalePass final : public org::TypedRenderGraphPass<DepthUpscalePass, DepthFrame, DepthBindings>
	{
	public:
		explicit DepthUpscalePass(std::shared_ptr<const DepthSpec> a_spec) :
			spec(std::move(a_spec)) {}

		DepthBindings Declare(org::PassBuilder& a_builder)
		{
			const auto& inputs = spec->inputs;
			DepthBindings bindings{};
			bindings.shared = SharedConstants::Declare(a_builder);
			bindings.refractionCopy = a_builder.ShaderResource(inputs.refractionCopy).View();
			bindings.depthCopy = a_builder.ShaderResource(inputs.depthCopy).View();
			bindings.refraction = a_builder.RenderTarget(inputs.refraction).View();
			bindings.saoCameraZ = a_builder.RenderTarget(inputs.saoCameraZ).View();
			bindings.depth = a_builder.DepthReadWrite(inputs.depth).View();
			return bindings;
		}

		// The recording depends only on what the pass was made with.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

		DepthFrame Prepare(const DepthBindings&, const org::PassPrepareContext&) const { return { spec }; }

		static void Record(const DepthBindings& a_bindings, const DepthFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& spec = *a_frame.spec;
			const DepthBlock block{ SharedConstants::Index(a_recording, a_bindings.shared), a_recording.Index(a_bindings.refractionCopy),
				a_recording.Index(a_bindings.depthCopy), spec.sampler, ShaderBindings::LatchOf(*spec.latch, a_recording.Run()) };
			const std::array<RasterProgram::Colour, 2> colours{ { { .view = &a_bindings.refraction }, { .view = &a_bindings.saoCameraZ } } };
			const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
			spec.program->Draw(a_recording, { .colours = colours,
												.depth = &depth,
												.viewport = { .width = spec.inputs.width, .height = spec.inputs.height },
												.constants = ShaderBindings::Data(block),
												.debugName = "Upscaling::DepthUpscale",
												.indirect = &spec.gate->Latch(),
												.indirectOffset = spec.gate->Offset(kDepthEntry) });
		}

	private:
		std::shared_ptr<const DepthSpec> spec;
	};


	struct EvaluateWork
	{
		std::string label;
		ORGUpscaling::EvaluateRecord record;
	};

	/** @brief Each run's evaluate, stored by the Upscale client's commit. */
	struct EvaluateShared
	{
		RenderGraph::RunSlots<EvaluateWork> runs;
	};

	struct EvaluateBindings
	{
		org::ResourceBindingToken colorIn, colorOut, depth, motion;
	};

	struct EvaluateFrame
	{
		std::shared_ptr<const EvaluateShared> shared;
	};

	/**
	* @brief Records Streamline's tags and evaluate on this pass's command list. Its four images are legacy interop uses in the
	* common state, which the tags name. Recorded at execution from the run's stored evaluate. Rebinds the graph's heaps after.
	*/
	class EvaluatePass final : public org::TypedRenderGraphPass<EvaluatePass, EvaluateFrame, EvaluateBindings>
	{
	public:
		EvaluatePass(std::shared_ptr<const EvaluateShared> a_shared, DepthInputs a_inputs, std::shared_ptr<org::Resource> a_output) :
			shared(std::move(a_shared)), inputs(std::move(a_inputs)), output(std::move(a_output)) {}

		EvaluateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			EvaluateBindings bindings{};
			bindings.colorIn = a_builder.LegacyInteropUse(inputs.main).Resource();
			bindings.colorOut = a_builder.LegacyInteropUse(output).Resource();
			bindings.depth = a_builder.LegacyInteropUse(inputs.mainDepth).Resource();
			bindings.motion = a_builder.LegacyInteropUse(inputs.motion).Resource();
			if (RenderGraph::Backend::IsD3D12())
				a_builder.InternalTransition(output, org::ResourceState{ rhi::ResourceAccessType::Common, rhi::ResourceLayout::Common, rhi::ResourceSyncState::All });
			return bindings;
		}

		// Recorded at execution from the run's evaluate, so nothing here prevents reuse.
		void Revision(std::vector<std::uint64_t>&) const {}

		EvaluateFrame Prepare(const EvaluateBindings&, const org::PassPrepareContext&) const { return { shared }; }

		static bool SkipsAtExecution(const EvaluateBindings&, const EvaluateFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return !a_frame.shared || !a_frame.shared->runs.Find(a_recording.Run());
		}

		static void Record(const EvaluateBindings& a_bindings, const EvaluateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto work = a_frame.shared ? a_frame.shared->runs.Take(a_recording.Run()) : nullptr;
			if (!work || !work->record)
				return;
			const std::array<rhi::Resource, 4> images{ a_recording.Resolve(a_bindings.colorIn), a_recording.Resolve(a_bindings.colorOut),
				a_recording.Resolve(a_bindings.depth), a_recording.Resolve(a_bindings.motion) };
			void* nativeList = nullptr;
			std::vector<RenderGraph::Engine::Native::ExternalImageInfo> infos;
			std::shared_ptr<const void> views;
			std::string why;
			const bool described = RenderGraph::Engine::Native::DescribeForCommands(a_recording.Commands(), images, nativeList, infos, views, why);
			if (views)
				a_recording.Retain(std::move(views));
			if (!described) {
				static std::once_flag logged;
				std::call_once(logged, [&] { logger::error("[ORG] Upscaling: {} cannot be recorded ({}); the frame keeps the previous output (logged once)", work->label, why); });
				return;
			}
			const bool evaluated = work->record(nativeList, infos);
			// Streamline may bind its own descriptor heaps, so restore the frame's.
			a_recording.RestoreDescriptorHeaps();
			if (!evaluated) {
				static std::once_flag logged;
				std::call_once(logged, [&] { logger::error("[ORG] Upscaling: {} was refused by the SDK; the frame keeps the previous output (logged once)", work->label); });
			}
		}

	private:
		std::shared_ptr<const EvaluateShared> shared;
		DepthInputs inputs;
		std::shared_ptr<org::Resource> output;
	};

	struct InputsBindings
	{
		org::ResourceBindingToken depth, motion, hudless;
	};

	/**
	* @brief Records FSR-FG's tags and prepare on this pass's command list, after the hudless copy. Its three images are legacy interop
	* uses in the common state, which the tags name, and the hudless stays in that state until the present reads it. Recorded at
	* execution from the run's stored prepare, so a replayed recording records it again. Rebinds the graph's heaps after.
	*/
	class FrameGenInputsPass final : public org::TypedRenderGraphPass<FrameGenInputsPass, EvaluateFrame, InputsBindings>
	{
	public:
		FrameGenInputsPass(std::shared_ptr<const EvaluateShared> a_shared, FrameAnchors::Texture a_depth, FrameAnchors::Texture a_motion,
			std::shared_ptr<org::Resource> a_hudless) :
			shared(std::move(a_shared)), depth(std::move(a_depth)), motion(std::move(a_motion)), hudless(std::move(a_hudless)) {}

		InputsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			InputsBindings bindings{};
			bindings.depth = a_builder.LegacyInteropUse(depth).Resource();
			bindings.motion = a_builder.LegacyInteropUse(motion).Resource();
			bindings.hudless = a_builder.LegacyInteropUse(hudless).Resource();
			return bindings;
		}

		// Recorded at execution from the run's prepare, so nothing here prevents reuse.
		void Revision(std::vector<std::uint64_t>&) const {}

		EvaluateFrame Prepare(const InputsBindings&, const org::PassPrepareContext&) const { return { shared }; }

		static bool SkipsAtExecution(const InputsBindings&, const EvaluateFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return !a_frame.shared || !a_frame.shared->runs.Find(a_recording.Run());
		}

		static void Record(const InputsBindings& a_bindings, const EvaluateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto work = a_frame.shared ? a_frame.shared->runs.Take(a_recording.Run()) : nullptr;
			if (!work || !work->record)
				return;
			const std::array<rhi::Resource, 3> images{ a_recording.Resolve(a_bindings.depth), a_recording.Resolve(a_bindings.motion),
				a_recording.Resolve(a_bindings.hudless) };
			void* nativeList = nullptr;
			std::vector<RenderGraph::Engine::Native::ExternalImageInfo> infos;
			std::shared_ptr<const void> views;
			std::string why;
			const bool described = RenderGraph::Engine::Native::DescribeForCommands(a_recording.Commands(), images, nativeList, infos, views, why);
			if (views)
				a_recording.Retain(std::move(views));
			if (!described) {
				static std::once_flag logged;
				std::call_once(logged, [&] { logger::error("[ORG] Upscaling: {} cannot be recorded ({}); the frame is presented without generation (logged once)", work->label, why); });
				return;
			}
			const bool prepared = work->record(nativeList, infos);
			// Streamline may bind its own descriptor heaps, so restore the frame's.
			a_recording.RestoreDescriptorHeaps();
			if (!prepared) {
				static std::once_flag logged;
				std::call_once(logged, [&] { logger::error("[ORG] Upscaling: {} was refused by the SDK; the frame is presented without generation (logged once)", work->label); });
			}
		}

	private:
		std::shared_ptr<const EvaluateShared> shared;
		FrameAnchors::Texture depth, motion;
		std::shared_ptr<org::Resource> hudless;
	};

	void MarkFailed(const std::shared_ptr<Impl>& a_impl, const char* a_why)
	{
		if (!a_impl->failed)
			logger::error("[ORG] Upscaling returns to D3D11: {}", a_why);
		a_impl->failed = true;
	}

	/** @brief Every format Upscaling::CreateHudlessTexture can produce, built at setup. */
	constexpr std::array kHudlessFormats{ DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM };

	/** @brief A format not built at setup, such as a new kMAIN format, is built at first sight and logged. */
	std::shared_ptr<const RasterProgram> CopyProgram(Impl& a_impl, rhi::Format a_format, bool a_atSetup = false)
	{
		if (const auto found = a_impl.copyPrograms.find(a_format); found != a_impl.copyPrograms.end())
			return found->second;
		if (!a_atSetup)
			logger::info("[ORG] Upscaling: the copy into format {} was not built at setup; building it now", static_cast<int>(a_format));
		auto program = LoadCopyProgram(RenderGraphRuntime::Get().Host()->Device(), a_format);
		if (program)
			a_impl.copyPrograms.emplace(a_format, program);
		return program;
	}

	/**
	* @brief The copy into kMAIN while the graph holds the upscaled texture, plus the depth upscale when a_depth.
	* Registering again replaces the client. False, logged, on failure.
	*/
	bool RegisterUpscale(const std::shared_ptr<Impl>& a_impl, bool a_depth)
	{
		auto& impl = *a_impl;
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::Upscale, kClientId, kOrderBase, impl.latch, impl.upscaleGate);
		if (impl.upscaled && impl.inputs.main) {
			const auto& desc = impl.upscaled->Desc();
			builder.Add(RenderGraph::AnchorPasses::Draw{ .name = "copy-to-main",
				.program = CopyProgram(impl, impl.inputs.mainFormat),
				.bindings = { Bind::Srv(impl.upscaled->Graph()) },
				.target = impl.inputs.main,
				.viewport = { .width = desc.Width, .height = desc.Height },
				.gate = kCopyEntry });
		}
		const bool depth = a_depth && impl.inputs.DepthComplete();
		if (depth) {
			const auto& inputs = impl.inputs;
			builder.Add(RenderGraph::AnchorPasses::Copy{
				.name = "depth-copy", .source = inputs.depth, .target = inputs.depthCopy, .width = inputs.depthSize.first, .height = inputs.depthSize.second });
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "refraction-normals-copy",
				.source = inputs.refraction,
				.target = inputs.refractionCopy,
				.width = inputs.refractionSize.first,
				.height = inputs.refractionSize.second });
			if (!impl.depthProgram)
				builder.Fail("the depth upscale's program is missing");
			auto spec = std::make_shared<const DepthSpec>(DepthSpec{ impl.depthProgram, impl.latch, impl.upscaleGate, impl.linearSampler.Index(), inputs });
			builder.Add("depth-upscale", FrameAnchors::PassKind::Render,
				[spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<DepthUpscalePass>(spec); });
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "underwater-mask-copy",
				.source = inputs.underwater,
				.target = inputs.underwaterCopy,
				.width = inputs.underwaterSize.first,
				.height = inputs.underwaterSize.second });
			// At half the screen, as the D3D11 path's viewport.
			builder.Add(RenderGraph::AnchorPasses::Draw{ .name = "underwater-mask-upscale",
				.program = impl.underwaterProgram,
				.bindings = { Bind::Shared(), Bind::Srv(inputs.underwaterCopy), Bind::Handle(impl.linearSampler.Index()), Bind::Latch(0) },
				.target = inputs.underwater,
				.viewport = { .width = inputs.width / 2, .height = inputs.height / 2 },
				.gate = kDepthEntry });
		}
		if (builder.Failed()) {
			logger::error("[ORG] Upscaling: {}", builder.Failure());
			return false;
		}

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		// The evaluate first, recorded at execution and not gated. Its own client, declared while an SDK upscaler is the method.
		const std::weak_ptr<Impl> weakImpl = a_impl;
		if (impl.evaluateShared && impl.upscaled && impl.inputs.main && impl.inputs.mainDepth && impl.inputs.motion) {
			auto shared = impl.evaluateShared;
			auto inputs = impl.inputs;
			auto output = std::static_pointer_cast<org::Resource>(impl.upscaled->Graph());
			FrameAnchors::ClientDesc evaluate{};
			evaluate.id = kEvaluateClientId;
			evaluate.passes.push_back({ .name = std::string(kClientId) + ".evaluate",
				.anchor = FrameAnchors::Anchor::Upscale,
				.order = kOrderBase - 1,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [shared, inputs, output]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<EvaluatePass>(shared, inputs, output); },
				.atExecution = true });
			evaluate.declared = [] { return RenderGraph::Frame::Driver::Current().post.upscaler; };
			evaluate.active = [weakImpl] {
				const auto current = weakImpl.lock();
				return current && (current->queued & kEvaluate) != 0;
			};
			if (!FrameAnchors::Register(std::move(evaluate))) {
				logger::error("[ORG] Upscaling: the evaluate client was refused");
				return false;
			}
		} else
			FrameAnchors::Unregister(kEvaluateClientId);
		for (auto& pass : builder.Take())
			client.passes.push_back(std::move(pass));
		// The depth upscale's own pass reads the shared constants too.
		client.sharedConstants = depth;
		client.gates = { impl.upscaleGate };
		const std::weak_ptr<Impl> weak = a_impl;
		client.registerResources = [weak](org::RenderGraph& a_graph) {
			if (const auto impl = weak.lock(); impl && impl->upscaled)
				impl->upscaled->Register(a_graph);
		};
		client.commit = [weak](const FrameAnchors::Point&) {
			const auto impl = weak.lock();
			if (!impl)
				return;
			const auto run = RenderGraphRuntime::Get().Host()->CurrentRun();
			impl->latch->WriteValue(run, 0, impl->jitter);
			if (impl->evaluateShared)
				impl->evaluateShared->runs.Store(run, (impl->queued & kEvaluate) ? impl->evaluate : nullptr);
			// While a newly chosen upscaler's build has not switched in, its output was not written, so do not copy it.
			const bool evaluated = !(impl->queued & kEvaluate) || !impl->evaluateShared || FrameAnchors::Scheduled(kEvaluateClientId, FrameAnchors::Anchor::Upscale);
			if ((impl->queued & kCopyToMain) && evaluated)
				impl->upscaleGate->OpenDraw(kCopyEntry);
			if (impl->queued & kDepthUpscale)
				impl->upscaleGate->OpenDraw(kDepthEntry);
		};
		client.active = [weak] {
			const auto impl = weak.lock();
			return impl && impl->queued != 0;
		};
		impl.registered = FrameAnchors::Register(std::move(client));
		impl.depthRegistered = impl.registered && depth;
		return impl.registered;
	}

	/** @brief Registering again replaces it. False, logged, on failure. */
	bool RegisterHudless(const std::shared_ptr<Impl>& a_impl)
	{
		auto& impl = *a_impl;
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::FrameGeneration, kClientId, kOrderBase, nullptr, impl.hudlessGate);
		if (impl.hudless && impl.hudlessSource) {
			const auto& desc = impl.hudless->Desc();
			builder.Add(RenderGraph::AnchorPasses::Draw{ .name = "hudless-copy",
				.program = CopyProgram(impl, rhi::helpers::ToRHI(desc.Format)),
				.bindings = { Bind::Srv(impl.hudlessSource) },
				.target = impl.hudless->Graph(),
				.viewport = { .width = desc.Width, .height = desc.Height },
				.gate = 0 });
		}
		if (builder.Failed()) {
			logger::error("[ORG] Upscaling: {}", builder.Failure());
			return false;
		}

		FrameAnchors::ClientDesc client{};
		client.id = kHudlessClientId;
		client.passes = builder.Take();
		client.gates = { impl.hudlessGate };
		const std::weak_ptr<Impl> weak = a_impl;
		client.registerResources = [weak](org::RenderGraph& a_graph) {
			if (const auto impl = weak.lock(); impl && impl->hudless)
				impl->hudless->Register(a_graph);
		};
		client.active = [weak] {
			const auto impl = weak.lock();
			return impl && impl->hudlessArmed;
		};
		client.commit = [weak](const FrameAnchors::Point&) {
			if (const auto impl = weak.lock())
				impl->hudlessGate->OpenDraw(0);
		};
		// The UI stage's frame generation step also takes the clean scene's tap in the same frame.
		client.repeats = true;
		impl.hudlessRegistered = FrameAnchors::Register(std::move(client));
		return impl.hudlessRegistered;
	}

	/** @brief Frame generation's inputs pass. Registering again replaces it. False, logged, on failure. */
	bool RegisterFrameGenInputs(const std::shared_ptr<Impl>& a_impl)
	{
		auto& impl = *a_impl;
		if (!impl.inputsShared || !impl.hudless || !impl.inputsDepth || !impl.inputsMotion) {
			FrameAnchors::Unregister(kFrameGenInputsClientId);
			impl.inputsRegistered = false;
			return false;
		}
		auto shared = impl.inputsShared;
		auto depth = impl.inputsDepth;
		auto motion = impl.inputsMotion;
		auto hudless = impl.hudless->Graph();
		FrameAnchors::ClientDesc client{};
		client.id = kFrameGenInputsClientId;
		client.passes.push_back({ .name = kFrameGenInputsClientId,
			.anchor = FrameAnchors::Anchor::FrameGeneration,
			.order = kOrderBase + 1,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [shared, depth, motion, hudless]() -> std::shared_ptr<org::RenderPass> {
				return std::make_shared<FrameGenInputsPass>(shared, depth, motion, hudless);
			},
			.atExecution = true });
		const std::weak_ptr<Impl> weak = a_impl;
		client.active = [weak] {
			const auto impl = weak.lock();
			return impl && impl->inputsArmed;
		};
		client.commit = [weak](const FrameAnchors::Point&) {
			const auto impl = weak.lock();
			if (!impl || !impl->inputsShared)
				return;
			impl->inputsShared->runs.Store(RenderGraphRuntime::Get().Host()->CurrentRun(), impl->inputsWork);
		};
		// The third tap of the UI stage's frame generation step, after the clean scene's and the hudless copy's.
		client.repeats = true;
		impl.inputsRegistered = FrameAnchors::Register(std::move(client));
		if (!impl.inputsRegistered)
			logger::error("[ORG] Upscaling: the frame generation inputs client was refused");
		return impl.inputsRegistered;
	}

	/** @brief Remakes the client after a new texture or engine target. */
	void UpscaleChanged(const std::shared_ptr<Impl>& a_impl)
	{
		if (a_impl->registered && !RegisterUpscale(a_impl, a_impl->depthRegistered))
			MarkFailed(a_impl, "its Upscale client could not be registered again");
	}

	/** @brief Remakes the client after a new texture or source. */
	void HudlessChanged(const std::shared_ptr<Impl>& a_impl)
	{
		if (a_impl->hudlessRegistered && !RegisterHudless(a_impl))
			MarkFailed(a_impl, "its FrameGeneration client could not be registered again");
		// The inputs pass reads the hudless, so a new one remakes it.
		if (a_impl->inputsRegistered && !RegisterFrameGenInputs(a_impl))
			MarkFailed(a_impl, "its frame generation inputs client could not be registered again");
	}
}

bool ORGUpscaling::Requested()
{
	return RenderGraph::Engine::Native::GraphMode();
}

ORGUpscaling& ORGUpscaling::Get()
{
	static ORGUpscaling s_upscaling;
	return s_upscaling;
}

ORGUpscaling::~ORGUpscaling() = default;

void ORGUpscaling::Reserve()
{
	if (!Requested())
		return;
	FrameAnchors::Reserve(FrameAnchors::Anchor::Upscale);
	FrameAnchors::Reserve(FrameAnchors::Anchor::FrameGeneration);
}

bool ORGUpscaling::Setup()
{
	impl.reset();
	setupFailed = false;
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Upscale) ||
		!FrameAnchors::HasEpoch(FrameAnchors::Anchor::FrameGeneration)) {
		logger::warn("[ORG] Upscaling stays on D3D11: the render graph or its Upscale and FrameGeneration anchors are not available");
		return false;
	}
	setupFailed = true;
	try {
		auto next = std::make_shared<Impl>();
		// The D3D11 path's Deferred::LinearSampler.
		next->linearSampler = ShaderBindings::CreateSampler({});
		next->latch = std::make_shared<org::LatchBlock>("cs.upscaling.jitter", static_cast<std::uint32_t>(sizeof(Upscaling::JitterCB)));
		next->upscaleGate = RenderGraph::RunGate::Create("cs.upscaling.gate", kUpscaleEntries);
		next->hudlessGate = RenderGraph::RunGate::Create("cs.upscaling.hudless.gate");
		if (EvaluateInStage())
			next->evaluateShared = std::make_shared<const EvaluateShared>();
		if (FrameAnchors::StageStepsAt(FrameAnchors::Anchor::FrameGeneration))
			next->inputsShared = std::make_shared<const EvaluateShared>();
		CurrentDepthInputs(next->inputs, next->formats);
		if (next->inputs.DepthComplete()) {
			next->depthProgram = LoadDepthProgram(host->Device(), next->formats);
			next->underwaterProgram = LoadUnderwaterProgram(host->Device(), next->formats);
		}
		CurrentMain(next->inputs);
		const auto mainProgram = next->inputs.main ? CopyProgram(*next, next->inputs.mainFormat, true) : nullptr;
		bool hudlessPrograms = true;
		for (const auto format : kHudlessFormats)
			hudlessPrograms &= CopyProgram(*next, rhi::helpers::ToRHI(format), true) != nullptr;
		if (!next->linearSampler || !next->latch || !next->upscaleGate || !next->hudlessGate || !mainProgram || !hudlessPrograms ||
			!next->depthProgram || !next->underwaterProgram) {
			logger::warn("[ORG] Upscaling stays on D3D11: its sampler, latch, gates, programs or engine targets could not be made");
			return false;
		}
		impl = std::move(next);
		setupFailed = false;
		logger::info("[ORG] Upscaling's own passes run on the render graph ({}x{}); {}", impl->inputs.width, impl->inputs.height,
			impl->evaluateShared ? "the upscaler's evaluate is the post stage's pass" : "the upscalers stay in the interop command ring");
		return true;
	} catch (const std::exception& e) {
		logger::error("[ORG] Upscaling setup failed, staying on D3D11: {}", e.what());
		impl.reset();
		return false;
	}
}

bool ORGUpscaling::IsActive() const
{
	return impl && !impl->failed && RenderGraphRuntime::Get().IsActive();
}

bool ORGUpscaling::EvaluatesInStage() const
{
	return IsActive() && impl->evaluateShared;
}

void ORGUpscaling::ClearShaderCache()
{
	// Built programs reload through the pipeline registry. A failed setup is tried again, and the copies stay on D3D11 until the textures are remade.
	if (!impl && setupFailed)
		(void)Setup();
}

bool ORGUpscaling::Failed() const
{
	return impl && (impl->failed || !RenderGraphRuntime::Get().IsActive());
}

bool ORGUpscaling::ReleaseAfterFailure()
{
	if (!impl)
		return false;
	const bool heldUpscaled = impl->upscaled != nullptr;
	FrameAnchors::Unregister(kClientId);
	FrameAnchors::Unregister(kEvaluateClientId);
	FrameAnchors::Unregister(kHudlessClientId);
	FrameAnchors::Unregister(kFrameGenInputsClientId);
	impl->upscaled.reset();
	impl->hudless.reset();
	impl.reset();
	logger::warn("[ORG] Upscaling: its own passes draw with D3D11 from now on");
	return heldUpscaled;
}

bool ORGUpscaling::CreateTexture(Target a_target, const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (!IsActive())
		return false;
	const bool upscaled = a_target == Target::Upscaled;
	auto& slot = upscaled ? impl->upscaled : impl->hudless;
	if (slot)
		return true;
	if (!upscaled && !CopyProgram(*impl, rhi::helpers::ToRHI(a_desc.Format))) {
		logger::error("[ORG] Upscaling: the hudless copy could not be built for format {}; it stays a D3D11 texture", static_cast<int>(a_desc.Format));
		return false;
	}
	// Streamline writes and reads these through their D3D11 images outside the graph, so they are exported and kept.
	const UINT bindFlags = D3D11_BIND_SHADER_RESOURCE | (upscaled ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET);
	const auto exports = GraphResources::Export::SRV | (upscaled ? GraphResources::Export::UAV : GraphResources::Export::RTV);
	auto texture = std::make_unique<GraphResources::Texture2D>(Sanitized(a_desc, bindFlags), upscaled ? kUpscaledName : kHudlessName,
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = exports });
	if (!*texture || !texture->D3D11() || (upscaled && !texture->UAV())) {
		logger::error("[ORG] Upscaling: the {} texture could not be made on the graph; it stays a D3D11 texture", upscaled ? "upscaled" : "hudless");
		return false;
	}
	slot = std::move(texture);
	if (upscaled)
		UpscaleChanged(impl);
	else
		HudlessChanged(impl);
	logger::info("[ORG] Upscaling: created the {} texture on the graph ({}x{}, format={})", upscaled ? "upscaled" : "hudless", a_desc.Width,
		a_desc.Height, static_cast<int>(a_desc.Format));
	return true;
}

void ORGUpscaling::DestroyTexture(Target a_target)
{
	if (!Holds(a_target))
		return;
	if (a_target == Target::Upscaled) {
		impl->upscaled.reset();
		UpscaleChanged(impl);
	} else {
		impl->hudless.reset();
		HudlessChanged(impl);
	}
}

bool ORGUpscaling::Holds(Target a_target) const
{
	return impl && (a_target == Target::Upscaled ? impl->upscaled : impl->hudless) != nullptr;
}

ID3D11Texture2D* ORGUpscaling::Texture(Target a_target, ID3D11Texture2D* a_d3d11) const
{
	if (!Holds(a_target))
		return a_d3d11;
	return (a_target == Target::Upscaled ? impl->upscaled : impl->hudless)->D3D11();
}

const D3D11_TEXTURE2D_DESC* ORGUpscaling::Desc(Target a_target, const D3D11_TEXTURE2D_DESC* a_d3d11) const
{
	if (!Holds(a_target))
		return a_d3d11;
	return &(a_target == Target::Upscaled ? impl->upscaled : impl->hudless)->Desc();
}

bool ORGUpscaling::QueueEvaluate(std::string a_label, EvaluateRecord a_record)
{
	if (!IsActive() || !impl->upscaled || !impl->evaluateShared)
		return false;
	// A resolution change remakes the client.
	DepthInputs current{};
	CurrentMain(current);
	if (!current.main || !current.mainDepth || !current.motion)
		return false;
	if (current.main != impl->inputs.main || current.mainFormat != impl->inputs.mainFormat || current.mainDepth != impl->inputs.mainDepth ||
		current.motion != impl->inputs.motion) {
		if (!CopyProgram(*impl, current.mainFormat))
			return false;
		impl->inputs.main = std::move(current.main);
		impl->inputs.mainFormat = current.mainFormat;
		impl->inputs.mainDepth = std::move(current.mainDepth);
		impl->inputs.motion = std::move(current.motion);
		UpscaleChanged(impl);
	}
	impl->evaluate = std::make_shared<const EvaluateWork>(EvaluateWork{ std::move(a_label), std::move(a_record) });
	impl->queued |= kEvaluate;
	return true;
}

bool ORGUpscaling::RecordsFrameGenInputs() const
{
	return IsActive() && impl->hudless && impl->inputsShared;
}

bool ORGUpscaling::QueueFrameGenInputs(std::string a_label, EvaluateRecord a_record, bool a_depthCopy)
{
	if (!RecordsFrameGenInputs())
		return false;
	auto* renderer = globals::game::renderer;
	const auto depthId = a_depthCopy ? RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY : RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN;
	const auto& depth = renderer->GetDepthStencilData().depthStencils[depthId];
	const auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	if (!depth.texture || !motion.texture)
		return false;
	auto depthTexture = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(depthId));
	auto motionTexture = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMOTION_VECTOR));
	if (!depthTexture || !motionTexture)
		return false;
	// A new engine target or a switch between kMAIN's depth and its copy remakes the client.
	if (!impl->inputsRegistered || depthTexture != impl->inputsDepth || motionTexture != impl->inputsMotion) {
		impl->inputsDepth = std::move(depthTexture);
		impl->inputsMotion = std::move(motionTexture);
		if (!RegisterFrameGenInputs(impl)) {
			MarkFailed(impl, "its frame generation inputs client could not be registered");
			return false;
		}
	}
	// A new registration prepares once the UI stage's build holds it. Until then the frame is presented without generation.
	if (!FrameAnchors::Scheduled(kFrameGenInputsClientId, FrameAnchors::Anchor::FrameGeneration))
		return false;
	impl->inputsWork = std::make_shared<const EvaluateWork>(EvaluateWork{ std::move(a_label), std::move(a_record) });
	impl->inputsArmed = true;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::FrameGeneration);
	impl->inputsArmed = false;
	impl->inputsWork.reset();
	return ran && IsActive();
}

bool ORGUpscaling::QueueCopyToMain()
{
	if (!IsActive() || !impl->upscaled)
		return false;
	// A resolution change remakes the client.
	DepthInputs current{};
	CurrentMain(current);
	if (current.main != impl->inputs.main || current.mainFormat != impl->inputs.mainFormat) {
		if (!current.main || !CopyProgram(*impl, current.mainFormat))
			return false;
		impl->inputs.main = std::move(current.main);
		impl->inputs.mainFormat = current.mainFormat;
		UpscaleChanged(impl);
	}
	impl->queued |= kCopyToMain;
	return true;
}

bool ORGUpscaling::QueueDepthUpscale(float2 a_jitter, float a_useWideKernel)
{
	if (!IsActive())
		return false;
	DepthInputs inputs{};
	DepthFormats formats{};
	CurrentDepthInputs(inputs, formats);
	if (!inputs.DepthComplete())
		return false;
	inputs.main = impl->inputs.main;
	inputs.mainFormat = impl->inputs.mainFormat;
	inputs.mainDepth = impl->inputs.mainDepth;
	inputs.motion = impl->inputs.motion;
	if (!(inputs == impl->inputs)) {
		if (!(formats == impl->formats)) {
			logger::info("[ORG] Upscaling: the engine's depth upscale targets changed format; building their programs now");
			auto* host = RenderGraphRuntime::Get().Host();
			auto depthProgram = LoadDepthProgram(host->Device(), formats);
			auto underwaterProgram = LoadUnderwaterProgram(host->Device(), formats);
			if (!depthProgram || !underwaterProgram) {
				MarkFailed(impl, "the depth upscale could not be built for the engine's new targets");
				return false;
			}
			impl->depthProgram = std::move(depthProgram);
			impl->underwaterProgram = std::move(underwaterProgram);
			impl->formats = formats;
		}
		impl->inputs = std::move(inputs);
		UpscaleChanged(impl);
	}
	impl->jitter.jitter = a_jitter;
	impl->jitter.useWideKernel = a_useWideKernel;
	impl->jitter.pad0 = 0.0f;
	impl->queued |= kDepthUpscale;
	return true;
}

void ORGUpscaling::RunUpscale()
{
	if (!impl || !impl->queued)
		return;
	if (!IsActive()) {
		impl->queued = 0;
		return;
	}
	// The copies have no per-run switch, so the client holds them exactly while the depth upscale runs and is remade when it starts or stops.
	const bool depth = (impl->queued & kDepthUpscale) != 0;
	if ((!impl->registered || depth != impl->depthRegistered) && !RegisterUpscale(impl, depth)) {
		impl->queued = 0;
		MarkFailed(impl, "its Upscale client could not be registered");
		return;
	}
	(void)FrameAnchors::Run(FrameAnchors::Anchor::Upscale);
	impl->queued = 0;
	impl->evaluate.reset();
}

bool ORGUpscaling::CopyHudless(ID3D11ShaderResourceView* a_source)
{
	if (!IsActive() || !impl->hudless || !a_source)
		return false;
	winrt::com_ptr<ID3D11Resource> resource;
	a_source->GetResource(resource.put());
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))) || !texture)
		return false;
	// HDR's clean composite is its output twin with CS_ORG_HDR_DISPLAY, ordered after its writes, else an imported D3D11 texture.
	auto& framebuffer = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
	FrameAnchors::Texture source;
	if (framebuffer.texture == texture.get())
		source = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kFRAMEBUFFER));
	else if (!(source = ORGHDRDisplay::Get().GraphOf(texture.get())))
		source = FrameAnchors::D3D11Texture("Upscaling::HudlessSource", texture.get(), a_source, nullptr);
	if (!source)
		return false;
	if (source != impl->hudlessSource) {
		impl->hudlessSource = std::move(source);
		HudlessChanged(impl);
	}
	if (!impl->hudlessRegistered && !RegisterHudless(impl)) {
		MarkFailed(impl, "its FrameGeneration client could not be registered");
		return false;
	}
	// A new registration copies once the UI stage's build holds it. Until then the frame has no hudless.
	if (!FrameAnchors::Scheduled(kHudlessClientId, FrameAnchors::Anchor::FrameGeneration))
		return false;
	impl->hudlessArmed = true;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::FrameGeneration);
	impl->hudlessArmed = false;
	return ran && IsActive();
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph Upscaling's own passes always draw with D3D11.
#	include "ORGUpscaling.h"

struct ORGUpscaling::Impl
{};

bool ORGUpscaling::Requested() { return false; }

ORGUpscaling& ORGUpscaling::Get()
{
	static ORGUpscaling s_upscaling;
	return s_upscaling;
}

ORGUpscaling::~ORGUpscaling() = default;
void ORGUpscaling::Reserve() {}
bool ORGUpscaling::Setup() { return false; }
void ORGUpscaling::ClearShaderCache() {}
bool ORGUpscaling::IsActive() const { return false; }
bool ORGUpscaling::Failed() const { return false; }
bool ORGUpscaling::ReleaseAfterFailure() { return false; }
bool ORGUpscaling::CreateTexture(Target, const D3D11_TEXTURE2D_DESC&) { return false; }
void ORGUpscaling::DestroyTexture(Target) {}
bool ORGUpscaling::Holds(Target) const { return false; }
ID3D11Texture2D* ORGUpscaling::Texture(Target, ID3D11Texture2D* a_d3d11) const { return a_d3d11; }
const D3D11_TEXTURE2D_DESC* ORGUpscaling::Desc(Target, const D3D11_TEXTURE2D_DESC* a_d3d11) const { return a_d3d11; }
bool ORGUpscaling::QueueEvaluate(std::string, EvaluateRecord) { return false; }
bool ORGUpscaling::RecordsFrameGenInputs() const { return false; }
bool ORGUpscaling::QueueFrameGenInputs(std::string, EvaluateRecord, bool) { return false; }
bool ORGUpscaling::QueueCopyToMain() { return false; }
bool ORGUpscaling::QueueDepthUpscale(float2, float) { return false; }
void ORGUpscaling::RunUpscale() {}
bool ORGUpscaling::CopyHudless(ID3D11ShaderResourceView*) { return false; }

#endif
