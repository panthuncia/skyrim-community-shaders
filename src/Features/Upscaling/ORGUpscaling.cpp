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
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/Variables.h"
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
#	include <Resources/VersionedTexture.h>
#	include <rhi_helpers.h>

#	include <array>
#	include <map>
#	include <mutex>
#	include <optional>
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
	// One texture per format, in ORGUpscaling::kHudlessFormats' order.
	constexpr std::array<const char*, ORGUpscaling::kHudlessFormats.size()> kHudlessNames{ "Upscaling::HudlessTexture.RGBA8", "Upscaling::HudlessTexture.BGRA8",
		"Upscaling::HudlessTexture.RGB10A2" };
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

	// Declared on variables (frame revision plan 6.14). Their extents change only at adoption.
	RenderGraph::Frame::DeclaredTexture2D upscaled;
	std::array<RenderGraph::Frame::DeclaredTexture2D, kHudlessFormats.size()> hudless;
	RenderGraph::Frame::DeclaredClient upscaleClient, hudlessClient;
	// Frame generation's inputs, made again when a revision starts after QueueFrameGenInputs saw new textures (observed, an interim).
	RenderGraph::Frame::DeclaredClient inputsClient;
	// The display size before the first DisplaySize event: kMAIN's at setup.
	RenderGraph::Frame::Vars::Extent2D setupDisplay{};

	// The engine targets the Upscale anchor imports, as the last frame found them. A change bumps the serial, which the Upscale
	// client's key holds. They are ORG's engine targets, whose slots never change, so the frame may look them up.
	DepthInputs inputs;
	std::uint64_t inputsSerial = 0;
	// The hudless copy's source texture and view, as the last copy found them. Not a variable yet, so a change invalidates the
	// hudless client. Its key imports them, so the copy makes no import mid-frame.
	winrt::com_ptr<ID3D11Texture2D> hudlessSourceTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> hudlessSourceView;

	// Render thread.
	std::uint32_t queued = 0;
	Upscaling::JitterCB jitter{};
	// Native mode: each run's evaluate, stored by the commit.
	std::shared_ptr<const EvaluateShared> evaluateShared;
	std::shared_ptr<const EvaluateWork> evaluate;
	// Frame generation's inputs: each run's prepare, stored by the commit, and the engine targets the pass imports.
	std::shared_ptr<const EvaluateShared> inputsShared;
	std::shared_ptr<const EvaluateWork> inputsWork;
	FrameAnchors::Texture inputsDepth, inputsMotion, inputsHudless;
	bool inputsArmed = false;
	// Raised for new input textures; the registration made for them sets inputsMade.
	std::uint64_t inputsWanted = 0, inputsMade = 0;

	// Set only around CopyHudless's tap, so the clean scene's tap of the same step does not copy the hudless.
	bool hudlessArmed = false;
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
		EvaluatePass(std::shared_ptr<const EvaluateShared> a_shared, DepthInputs a_inputs, FrameAnchors::Texture a_output) :
			shared(std::move(a_shared)), inputs(std::move(a_inputs)), output(std::move(a_output)) {}

		EvaluateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			EvaluateBindings bindings{};
			bindings.colorIn = a_builder.LegacyInteropUse(inputs.main).Resource();
			bindings.colorOut = a_builder.LegacyInteropUse(output).Resource();
			bindings.depth = a_builder.LegacyInteropUse(inputs.mainDepth).Resource();
			bindings.motion = a_builder.LegacyInteropUse(inputs.motion).Resource();
			// The upscaled texture is declared through its slot, so a new version needs no build.
			if (RenderGraph::Backend::IsD3D12() && output.Slot())
				a_builder.InternalTransition(org::ResourceResolverAndRange(*output.Slot()),
					org::ResourceState{ rhi::ResourceAccessType::Common, rhi::ResourceLayout::Common, rhi::ResourceSyncState::All });
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
		FrameAnchors::Texture output;
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
			FrameAnchors::Texture a_hudless) :
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
		FrameAnchors::Texture hudless;
	};

	void MarkFailed(const std::shared_ptr<Impl>& a_impl, const char* a_why)
	{
		if (!a_impl->failed)
			logger::error("[ORG] Upscaling returns to D3D11: {}", a_why);
		a_impl->failed = true;
	}

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

	namespace Vars = RenderGraph::Frame::Vars;
	constexpr const auto& kHudlessFormats = ORGUpscaling::kHudlessFormats;

	bool SdkMethod(Vars::Upscaler a_method)
	{
		return a_method == Vars::Upscaler::FSR || a_method == Vars::Upscaler::DLSS || a_method == Vars::Upscaler::XeSS;
	}

	/** @brief The display size, or a_setup before the first DisplaySize event. */
	Vars::Extent2D DisplayOr(Vars::Extent2D a_display, Vars::Extent2D a_setup)
	{
		return a_display.width && a_display.height ? a_display : a_setup;
	}

	/** @brief Which hudless texture a revision's values choose: none without frame generation. Inputs or the adopted revision. */
	template <class Values>
	std::optional<std::uint32_t> HudlessIndexIn(const Values& a_values)
	{
		if (!a_values.Get(globals::features::upscaling.settings.frameGeneration))
			return std::nullopt;
		const auto surface = a_values.Get(Vars::PresenterFormat());
		return ORGUpscaling::HudlessFormatIndex(a_values.Get(Vars::HdrEnabled()), static_cast<DXGI_FORMAT>(surface.format), surface.hdr10);
	}

	/** @brief What the Upscale client's registration depends on. */
	struct UpscaleKey
	{
		Vars::Extent2D display{};  // kMAIN's, the copy's render area
		bool depth = false;        // the depth upscale is in the registration: an SDK upscaler. The frame opens it below native.
		std::uint64_t inputs = 0;  // Impl::inputsSerial
		bool operator==(const UpscaleKey&) const = default;
	};

	/** @brief What the hudless client's registration depends on. */
	struct HudlessKey
	{
		std::uint32_t format = 0;  // the index in kHudlessFormats
		Vars::Extent2D display{};
		FrameAnchors::Texture source;  // the copy's source, imported by the key
		bool operator==(const HudlessKey&) const = default;
	};

	/**
	* @brief The hudless copy's source as the graph reads it: the back buffer, HDR's twin, or an import of the D3D11 texture.
	* Call it only from the hudless client's key, where a new texture joins the revision.
	*/
	FrameAnchors::Texture HudlessSourceOf(ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_view)
	{
		// HDR's clean composite is its output twin with CS_ORG_HDR_DISPLAY, ordered after its writes, else an imported D3D11 texture.
		const auto& framebuffer = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
		if (framebuffer.texture == a_texture)
			return FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kFRAMEBUFFER));
		if (auto slot = ORGHDRDisplay::Get().GraphOf(a_texture))
			return slot;
		return FrameAnchors::D3D11Texture("Upscaling::HudlessSource", a_texture, a_view, nullptr);
	}

	/** @brief A declared twin as passes declare it: its slot. */
	FrameAnchors::Texture SlotOf(const RenderGraph::Frame::DeclaredTexture2D& a_texture)
	{
		return a_texture ? FrameAnchors::Texture(a_texture->Name(), a_texture->Versioned()) : FrameAnchors::Texture();
	}

	/**
	* @brief The Upscale client for a_key: the copy into kMAIN, plus the depth upscale when the key has it. Registers the evaluate
	* client beside it. False, logged, on failure.
	*/
	bool MakeUpscaleClient(const std::shared_ptr<Impl>& a_impl, const UpscaleKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		auto& impl = *a_impl;
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::Upscale, kClientId, kOrderBase, impl.latch, impl.upscaleGate);
		if (impl.upscaled && impl.inputs.main) {
			builder.Add(RenderGraph::AnchorPasses::Draw{ .name = "copy-to-main",
				.program = CopyProgram(impl, impl.inputs.mainFormat),
				.bindings = { Bind::Srv(*impl.upscaled) },
				.target = impl.inputs.main,
				.viewport = { .width = a_key.display.width, .height = a_key.display.height },
				.gate = kCopyEntry });
		}
		const bool depth = a_key.depth && impl.inputs.DepthComplete();
		if (depth) {
			const auto& inputs = impl.inputs;
			// The copies run with the depth upscale, so a frame at native quality skips them, as D3D11 does.
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "depth-copy",
				.source = inputs.depth,
				.target = inputs.depthCopy,
				.width = inputs.depthSize.first,
				.height = inputs.depthSize.second,
				.gate = kDepthEntry });
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "refraction-normals-copy",
				.source = inputs.refraction,
				.target = inputs.refractionCopy,
				.width = inputs.refractionSize.first,
				.height = inputs.refractionSize.second,
				.gate = kDepthEntry });
			if (!impl.depthProgram)
				builder.Fail("the depth upscale's program is missing");
			auto spec = std::make_shared<const DepthSpec>(DepthSpec{ impl.depthProgram, impl.latch, impl.upscaleGate, impl.linearSampler.Index(), inputs });
			builder.Add("depth-upscale", FrameAnchors::PassKind::Render,
				[spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<DepthUpscalePass>(spec); });
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "underwater-mask-copy",
				.source = inputs.underwater,
				.target = inputs.underwaterCopy,
				.width = inputs.underwaterSize.first,
				.height = inputs.underwaterSize.second,
				.gate = kDepthEntry });
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

		// The evaluate first, recorded at execution and not gated. Its own client, declared while an SDK upscaler is the method.
		const std::weak_ptr<Impl> weak = a_impl;
		if (impl.evaluateShared && impl.upscaled && impl.inputs.main && impl.inputs.mainDepth && impl.inputs.motion) {
			auto shared = impl.evaluateShared;
			auto inputs = impl.inputs;
			auto output = SlotOf(impl.upscaled);
			FrameAnchors::ClientDesc evaluate{};
			evaluate.id = kEvaluateClientId;
			evaluate.passes.push_back({ .name = std::string(kClientId) + ".evaluate",
				.anchor = FrameAnchors::Anchor::Upscale,
				.order = kOrderBase - 1,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [shared, inputs, output]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<EvaluatePass>(shared, inputs, output); },
				.atExecution = true });
			evaluate.declared = [] { return RenderGraph::Frame::Driver::Current().post.upscaler; };
			evaluate.active = [weak] {
				const auto current = weak.lock();
				return current && (current->queued & kEvaluate) != 0;
			};
			if (!FrameAnchors::Register(std::move(evaluate))) {
				logger::error("[ORG] Upscaling: the evaluate client was refused");
				return false;
			}
		} else
			FrameAnchors::Unregister(kEvaluateClientId);
		for (auto& pass : builder.Take())
			a_client.passes.push_back(std::move(pass));
		// The depth upscale's own pass reads the shared constants too.
		a_client.sharedConstants = depth;
		a_client.gates = { impl.upscaleGate };
		a_client.registerResources = [weak](org::RenderGraph& a_graph) {
			if (const auto impl = weak.lock(); impl && impl->upscaled)
				impl->upscaled->Register(a_graph);
		};
		a_client.commit = [weak](const FrameAnchors::Point&) {
			const auto impl = weak.lock();
			if (!impl)
				return;
			const auto run = RenderGraphRuntime::Get().Host()->CurrentRun();
			impl->latch->WriteValue(run, 0, impl->jitter);
			if (impl->evaluateShared)
				impl->evaluateShared->runs.Store(run, (impl->queued & kEvaluate) ? impl->evaluate : nullptr);
			// The evaluate is in the post stage's build only while its keyed build for an SDK upscaler has switched in.
			// Until then its output was not written, so do not copy it.
			const bool evaluated = !(impl->queued & kEvaluate) || !impl->evaluateShared || FrameAnchors::Scheduled(kEvaluateClientId, FrameAnchors::Anchor::Upscale);
			if ((impl->queued & kCopyToMain) && evaluated)
				impl->upscaleGate->OpenDraw(kCopyEntry);
			if (impl->queued & kDepthUpscale)
				impl->upscaleGate->OpenDraw(kDepthEntry);
		};
		a_client.active = [weak] {
			const auto impl = weak.lock();
			return impl && impl->queued != 0;
		};
		return true;
	}

	/** @brief The hudless client for a_key: the copy into the key's format's texture. False, logged, on failure. */
	bool MakeHudlessClient(const std::shared_ptr<Impl>& a_impl, const HudlessKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		auto& impl = *a_impl;
		const auto index = a_key.format;
		if (index >= impl.hudless.size() || !impl.hudless[index] || !a_key.source) {
			logger::error("[ORG] Upscaling: the hudless copy has no texture or no source for format index {}", index);
			return false;
		}
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::FrameGeneration, kClientId, kOrderBase, nullptr, impl.hudlessGate);
		builder.Add(RenderGraph::AnchorPasses::Draw{ .name = "hudless-copy",
			.program = CopyProgram(impl, rhi::helpers::ToRHI(kHudlessFormats[index])),
			.bindings = { Bind::Srv(a_key.source) },
			.target = SlotOf(impl.hudless[index]),
			.viewport = { .width = a_key.display.width, .height = a_key.display.height },
			.gate = 0 });
		if (builder.Failed()) {
			logger::error("[ORG] Upscaling: {}", builder.Failure());
			return false;
		}

		a_client.passes = builder.Take();
		a_client.gates = { impl.hudlessGate };
		const std::weak_ptr<Impl> weak = a_impl;
		a_client.registerResources = [weak, index](org::RenderGraph& a_graph) {
			if (const auto impl = weak.lock(); impl && impl->hudless[index])
				impl->hudless[index]->Register(a_graph);
		};
		a_client.active = [weak] {
			const auto impl = weak.lock();
			return impl && impl->hudlessArmed;
		};
		a_client.commit = [weak](const FrameAnchors::Point&) {
			if (const auto impl = weak.lock())
				impl->hudlessGate->OpenDraw(0);
		};
		// The UI stage's frame generation step also takes the clean scene's tap in the same frame.
		a_client.repeats = true;
		return true;
	}

	/** @brief The textures, declared on the variables they follow. False, logged, when one could not be made. */
	bool DeclareTextures(const std::shared_ptr<Impl>& a_impl, const D3D11_TEXTURE2D_DESC& a_main)
	{
		using namespace RenderGraph::Frame;
		const auto& settings = globals::features::upscaling.settings;
		const auto setup = a_impl->setupDisplay;
		// Streamline writes and reads these through their D3D11 images outside the graph, so they are exported and kept.
		const auto upscaledDesc = Sanitized(a_main, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		a_impl->upscaled = DeclareTexture2D(
			kUpscaledName, DependsOn(Vars::DisplaySize(), Vars::UpscaleMethod(), settings.frameGeneration),
			[upscaledDesc, setup](const Inputs& a_in) {
				// An SDK upscaler evaluates into it, and so does guest DLSS-G's evaluate without one. One texel otherwise.
				const bool used = SdkMethod(a_in.Get(Vars::UpscaleMethod())) || a_in.Get(globals::features::upscaling.settings.frameGeneration);
				const auto display = DisplayOr(a_in.Get(Vars::DisplaySize()), setup);
				auto desc = upscaledDesc;
				desc.Width = used ? display.width : 1;
				desc.Height = used ? display.height : 1;
				return desc;
			},
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV });
		if (!a_impl->upscaled || !a_impl->upscaled->D3D11() || !a_impl->upscaled->UAV() || !a_impl->upscaled->Versioned()) {
			logger::error("[ORG] Upscaling: the upscaled texture could not be declared");
			return false;
		}
		for (std::uint32_t i = 0; i < kHudlessFormats.size(); ++i) {
			const D3D11_TEXTURE2D_DESC hudlessDesc{ .Width = 1,
				.Height = 1,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = kHudlessFormats[i],
				.SampleDesc = { .Count = 1 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET };
			// FFX derives the UI as the back buffer minus the hudless colour, so the two agree in format and the copy draws through an RTV.
			a_impl->hudless[i] = DeclareTexture2D(
				kHudlessNames[i], DependsOn(Vars::DisplaySize(), Vars::HdrEnabled(), Vars::PresenterFormat(), settings.frameGeneration),
				[hudlessDesc, setup, i](const Inputs& a_in) {
					auto desc = hudlessDesc;
					if (HudlessIndexIn(a_in) == i) {
						const auto display = DisplayOr(a_in.Get(Vars::DisplaySize()), setup);
						desc.Width = display.width;
						desc.Height = display.height;
					}
					return desc;
				},
				GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::RTV });
			if (!a_impl->hudless[i] || !a_impl->hudless[i]->D3D11() || !a_impl->hudless[i]->Versioned()) {
				logger::error("[ORG] Upscaling: the hudless texture for format {} could not be declared", static_cast<int>(kHudlessFormats[i]));
				return false;
			}
		}
		return true;
	}

	/** @brief What the frame generation inputs' registration depends on: the textures QueueFrameGenInputs last saw. */
	struct InputsKey
	{
		std::uint64_t serial = 0;
		bool operator==(const InputsKey&) const = default;
	};

	/** @brief Frame generation's inputs pass for the textures QueueFrameGenInputs last saw, made when a revision starts. */
	bool MakeFrameGenInputsClient(const std::shared_ptr<Impl>& a_impl, const InputsKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		auto& impl = *a_impl;
		auto shared = impl.inputsShared;
		auto depth = impl.inputsDepth;
		auto motion = impl.inputsMotion;
		auto hudless = impl.inputsHudless;
		auto& client = a_client;
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
		impl.inputsMade = a_key.serial;
		return true;
	}

	/** @brief The clients, declared on the variables their registrations read. False, logged, when one cannot be declared. */
	bool DeclareClients(const std::shared_ptr<Impl>& a_impl)
	{
		using namespace RenderGraph::Frame;
		const auto& settings = globals::features::upscaling.settings;
		const std::weak_ptr<Impl> weak = a_impl;
		a_impl->upscaleClient = DeclareClient<UpscaleKey>(
			kClientId, DependsOn(Vars::DisplaySize(), Vars::UpscaleMethod()),
			[weak](const Inputs& a_in) -> std::optional<UpscaleKey> {
				const auto impl = weak.lock();
				if (!impl || impl->failed)
					return std::nullopt;
				// The quality mode is no dependency (decision 6a): the depth upscale is gated, and the frame opens it below native
				// only, as Upscaling::IsUpscalingActive says. So a quality change builds nothing.
				const bool depth = SdkMethod(a_in.Get(Vars::UpscaleMethod()));
				return UpscaleKey{ DisplayOr(a_in.Get(Vars::DisplaySize()), impl->setupDisplay), depth, impl->inputsSerial };
			},
			[weak](const UpscaleKey& a_key, FrameAnchors::ClientDesc& a_client) {
				const auto impl = weak.lock();
				return impl && MakeUpscaleClient(impl, a_key, a_client);
			});
		a_impl->hudlessClient = DeclareClient<HudlessKey>(
			kHudlessClientId, DependsOn(Vars::DisplaySize(), Vars::HdrEnabled(), Vars::PresenterFormat(), settings.frameGeneration),
			[weak](const Inputs& a_in) -> std::optional<HudlessKey> {
				const auto impl = weak.lock();
				if (!impl || impl->failed)
					return std::nullopt;
				const auto index = HudlessIndexIn(a_in);
				// The source comes from the last copy that named one. Before one did, the client waits for it.
				if (!index || !impl->hudlessSourceTexture)
					return std::nullopt;
				// Imported here, when the revision starts, so a new source texture joins the revision.
				auto source = HudlessSourceOf(impl->hudlessSourceTexture.get(), impl->hudlessSourceView.get());
				if (!source) {
					logger::error("[ORG] Upscaling: the hudless copy's source cannot be imported; the frame has no hudless colour");
					return std::nullopt;
				}
				return HudlessKey{ *index, DisplayOr(a_in.Get(Vars::DisplaySize()), impl->setupDisplay), std::move(source) };
			},
			[weak](const HudlessKey& a_key, FrameAnchors::ClientDesc& a_client) {
				const auto impl = weak.lock();
				return impl && MakeHudlessClient(impl, a_key, a_client);
			});
		a_impl->inputsClient = DeclareClient<InputsKey>(
			kFrameGenInputsClientId, {},
			[weak](const Inputs&) -> std::optional<InputsKey> {
				const auto impl = weak.lock();
				if (!impl || impl->failed || !impl->inputsShared || !impl->inputsHudless || !impl->inputsDepth || !impl->inputsMotion)
					return std::nullopt;
				return InputsKey{ impl->inputsWanted };
			},
			[weak](const InputsKey& a_key, FrameAnchors::ClientDesc& a_client) {
				const auto impl = weak.lock();
				return impl && MakeFrameGenInputsClient(impl, a_key, a_client);
			});
		if (!a_impl->upscaleClient || !a_impl->hudlessClient || !a_impl->inputsClient) {
			logger::error("[ORG] Upscaling: its clients could not be declared");
			return false;
		}
		return true;
	}

	/** @brief The hudless texture of the adopted revision, as passes declare it. Empty while that revision has none. */
	FrameAnchors::Texture AdoptedHudless(const Impl& a_impl)
	{
		const auto index = HudlessIndexIn(RenderGraph::Frame::Revision::Adopted());
		return index ? SlotOf(a_impl.hudless[*index]) : FrameAnchors::Texture();
	}

	/** @brief The Upscale anchor's engine inputs changed: the Upscale client is registered again when the next revision starts. */
	void UpscaleInputsChanged(Impl& a_impl)
	{
		++a_impl.inputsSerial;
		a_impl.upscaleClient.Invalidate();
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
		// kMAIN's description and size, for the upscaled texture and as the display size before its first event.
		auto* mainTexture = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture;
		if (!mainTexture) {
			logger::warn("[ORG] Upscaling stays on D3D11: kMAIN is missing");
			return false;
		}
		D3D11_TEXTURE2D_DESC mainDesc{};
		mainTexture->GetDesc(&mainDesc);
		next->setupDisplay = { mainDesc.Width, mainDesc.Height };
		if (!DeclareTextures(next, mainDesc) || !DeclareClients(next)) {
			logger::warn("[ORG] Upscaling stays on D3D11: its textures or clients could not be declared");
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
	const bool heldUpscaled = static_cast<bool>(impl->upscaled);
	FrameAnchors::Unregister(kClientId);
	FrameAnchors::Unregister(kEvaluateClientId);
	FrameAnchors::Unregister(kHudlessClientId);
	FrameAnchors::Unregister(kFrameGenInputsClientId);
	// The declarations go with impl, so no revision registers the clients again.
	impl.reset();
	logger::warn("[ORG] Upscaling: its own passes draw with D3D11 from now on");
	return heldUpscaled;
}

bool ORGUpscaling::Holds(Target a_target) const
{
	if (!IsActive())
		return false;
	return a_target == Target::Hudless || static_cast<bool>(impl->upscaled);
}

ID3D11Texture2D* ORGUpscaling::Texture(Target a_target, ID3D11Texture2D* a_d3d11) const
{
	if (!Holds(a_target))
		return a_d3d11;
	if (a_target == Target::Upscaled)
		return impl->upscaled->D3D11();
	const auto index = HudlessIndexIn(RenderGraph::Frame::Revision::Adopted());
	return index ? impl->hudless[*index]->D3D11() : nullptr;
}

bool ORGUpscaling::QueueEvaluate(std::string a_label, EvaluateRecord a_record)
{
	if (!IsActive() || !impl->upscaled || !impl->evaluateShared)
		return false;
	// New engine targets register the client again (an interim until engine imports are declared, plan step 9b).
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
		UpscaleInputsChanged(*impl);
	}
	impl->evaluate = std::make_shared<const EvaluateWork>(EvaluateWork{ std::move(a_label), std::move(a_record) });
	impl->queued |= kEvaluate;
	return true;
}

bool ORGUpscaling::RecordsFrameGenInputs() const
{
	return IsActive() && impl->inputsShared;
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
	// The adopted revision's hudless texture. Its format follows HDR and the presenter.
	auto hudlessTexture = AdoptedHudless(*impl);
	if (!depthTexture || !motionTexture || !hudlessTexture)
		return false;
	// A new engine target, a switch between kMAIN's depth and its copy, or another hudless format remakes the client when the
	// next revision starts.
	if (depthTexture != impl->inputsDepth || motionTexture != impl->inputsMotion || hudlessTexture != impl->inputsHudless) {
		impl->inputsDepth = std::move(depthTexture);
		impl->inputsMotion = std::move(motionTexture);
		impl->inputsHudless = std::move(hudlessTexture);
		++impl->inputsWanted;
		impl->inputsClient.Invalidate();
	}
	// A new registration prepares once it is made and the UI stage's build holds it. Until then the frame is presented without generation.
	if (impl->inputsMade != impl->inputsWanted || !FrameAnchors::Scheduled(kFrameGenInputsClientId, FrameAnchors::Anchor::FrameGeneration))
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
	// New engine targets register the client again, as in QueueEvaluate.
	DepthInputs current{};
	CurrentMain(current);
	if (current.main != impl->inputs.main || current.mainFormat != impl->inputs.mainFormat) {
		if (!current.main || !CopyProgram(*impl, current.mainFormat))
			return false;
		impl->inputs.main = std::move(current.main);
		impl->inputs.mainFormat = current.mainFormat;
		UpscaleInputsChanged(*impl);
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
		UpscaleInputsChanged(*impl);
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
	// The copies have no per-run switch. The client's key holds the depth upscale exactly while the adopted method and quality
	// run it, so the registration the frame's build holds matches what was queued.
	(void)FrameAnchors::Run(FrameAnchors::Anchor::Upscale);
	impl->queued = 0;
	impl->evaluate.reset();
}

bool ORGUpscaling::CopyHudless(ID3D11ShaderResourceView* a_source)
{
	if (!IsActive() || !a_source)
		return false;
	winrt::com_ptr<ID3D11Resource> resource;
	a_source->GetResource(resource.put());
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))) || !texture)
		return false;
	// A new source registers the client again when the next revision starts, whose key imports it. No import is made here.
	if (texture.get() != impl->hudlessSourceTexture.get() || a_source != impl->hudlessSourceView.get()) {
		impl->hudlessSourceTexture = std::move(texture);
		impl->hudlessSourceView.copy_from(a_source);
		impl->hudlessClient.Invalidate();
	}
	// The adopted revision has no hudless texture while frame generation is off or the presenter has no format for it.
	if (!AdoptedHudless(*impl))
		return false;
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
bool ORGUpscaling::Holds(Target) const { return false; }
ID3D11Texture2D* ORGUpscaling::Texture(Target, ID3D11Texture2D* a_d3d11) const { return a_d3d11; }
bool ORGUpscaling::QueueEvaluate(std::string, EvaluateRecord) { return false; }
bool ORGUpscaling::RecordsFrameGenInputs() const { return false; }
bool ORGUpscaling::QueueFrameGenInputs(std::string, EvaluateRecord, bool) { return false; }
bool ORGUpscaling::QueueCopyToMain() { return false; }
bool ORGUpscaling::QueueDepthUpscale(float2, float) { return false; }
void ORGUpscaling::RunUpscale() {}
bool ORGUpscaling::CopyHudless(ID3D11ShaderResourceView*) { return false; }

#endif
