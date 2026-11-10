#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGUpscaling.h"

#	include "Features/Upscaling.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/FrameAnchors.h"
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
#	include <utility>
#	include <vector>

namespace
{
	using RenderGraph::AnchorPasses::Bind;

	constexpr const char* kClientId = "cs.upscaling";
	constexpr const char* kHudlessClientId = "cs.upscaling.hudless";
	constexpr const char* kUpscaledName = "Upscaling::UpscaledTexture";
	constexpr const char* kHudlessName = "Upscaling::HudlessTexture";
	constexpr const char* kDepthShader = "Upscaling/DepthRefractionUpscalePS.hlsl";
	constexpr const char* kUnderwaterShader = "Upscaling/UnderwaterMaskUpscalePS.hlsl";
	// A plain Load and write: the hudless copy, and the upscaled colour's copy into kMAIN.
	constexpr const char* kCopyShader = "Upscaling/CopyHudlessPS.hlsl";
	constexpr std::int32_t kOrderBase = 100;

	// What a frame queues for the Upscale anchor.
	constexpr std::uint32_t kCopyToMain = 1u << 0;
	constexpr std::uint32_t kDepthUpscale = 1u << 1;

	// The Upscale client's run gate: the copy into kMAIN, and the depth upscale's two draws.
	constexpr std::uint32_t kCopyEntry = 0;
	constexpr std::uint32_t kDepthEntry = 1;
	constexpr std::uint32_t kUpscaleEntries = 2;

	// DepthRefractionUpscalePS's push block under ORG, in list order.
	struct DepthBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Handle refractionNormals, depth, linearSampler;
		ShaderBindings::Latch jitter;
	};

	// UnderwaterMaskUpscalePS's.
	struct UnderwaterBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Handle underwaterMask, linearSampler;
		ShaderBindings::Latch jitter;
	};

	// CopyHudlessPS's.
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

	/** @brief A texture's width and height, for a whole-texture copy. */
	std::pair<std::uint32_t, std::uint32_t> SizeOf(ID3D11Texture2D* a_texture)
	{
		D3D11_TEXTURE2D_DESC desc{};
		a_texture->GetDesc(&desc);
		return { desc.Width, desc.Height };
	}

	/** @brief The engine targets the Upscale anchor's passes read and write, as graph textures. */
	struct DepthInputs
	{
		std::shared_ptr<org::ExternalTextureResource> main;
		rhi::Format mainFormat = rhi::Format::Unknown;  // kMAIN's view: the copy's target format
		std::shared_ptr<org::ExternalTextureResource> depth, depthCopy;
		std::shared_ptr<org::ExternalTextureResource> refraction, refractionCopy, saoCameraZ;
		std::shared_ptr<org::ExternalTextureResource> underwater, underwaterCopy;
		std::pair<std::uint32_t, std::uint32_t> depthSize, refractionSize, underwaterSize;  // the copies' sizes
		std::uint32_t width = 0, height = 0;  // the screen: the depth upscale's render area

		bool operator==(const DepthInputs&) const = default;

		bool DepthComplete() const
		{
			return depth && depthCopy && refraction && refractionCopy && saoCameraZ && underwater && underwaterCopy && width && height;
		}
	};

	/** @brief The formats the depth upscale's programs are built for: the engine's views. */
	struct DepthFormats
	{
		rhi::Format refraction = rhi::Format::Unknown, saoCameraZ = rhi::Format::Unknown, underwater = rhi::Format::Unknown;
		rhi::Format depth = rhi::Format::Unknown;

		bool operator==(const DepthFormats&) const = default;
	};

	/** @brief kMAIN as the copy writes it, with its view's format. A null main when it cannot be imported. */
	void CurrentMain(DepthInputs& a_inputs)
	{
		auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		a_inputs.main = main.RTV ? FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN)) : nullptr;
		a_inputs.mainFormat = main.RTV ? FormatOf(main.RTV) : rhi::Format::Unknown;
	}

	/**
	 * @brief The depth upscale's targets as UpscaleDepth binds them. Empty depth fields when one is missing or a copy is its own
	 * source: the D3D11 path then draws.
	 */
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

	/** @brief A texture description the graph twins accept: default usage, no CPU access, no misc flags. */
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
	std::map<rhi::Format, std::shared_ptr<const RasterProgram>> copyPrograms;  // CopyHudlessPS, by target format
	org::OwnedDescriptorBinding linearSampler;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> upscaleGate, hudlessGate;

	std::unique_ptr<GraphResources::Texture2D> upscaled, hudless;
	DepthInputs inputs;
	std::shared_ptr<org::ExternalTextureResource> hudlessSource;

	// Render thread: this frame's queued work, and the depth upscale's constants.
	std::uint32_t queued = 0;
	Upscaling::JitterCB jitter{};

	// Whether each client is registered, and whether the Upscale client holds the depth upscale's passes.
	bool registered = false, hudlessRegistered = false;
	bool depthRegistered = false;
	bool failed = false;
};

namespace
{
	using Impl = ORGUpscaling::Impl;

	/** @brief What the depth upscale's draw is made from. Immutable once built. */
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

	/**
	 * @brief DepthRefractionUpscalePS: the refraction normals, kSAO_CAMERAZ and kMAIN's depth, from their render-size copies. It
	 * writes two colour targets and the depth, which AnchorPasses::Draw does not cover, so it is a pass of its own. It draws
	 * through the run gate's depth entry.
	 */
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
				a_recording.Index(a_bindings.depthCopy), spec.sampler, ShaderBindings::LatchOf(*spec.latch, a_recording.FrameSlot()) };
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

	void MarkFailed(const std::shared_ptr<Impl>& a_impl, const char* a_why)
	{
		if (!a_impl->failed)
			logger::error("[ORG] Upscaling returns to D3D11: {}", a_why);
		a_impl->failed = true;
	}

	/** @brief The copy program for a_format, made once per format (the D3D11 path compiles its shader on first use too). */
	std::shared_ptr<const RasterProgram> CopyProgram(Impl& a_impl, rhi::Format a_format)
	{
		if (const auto found = a_impl.copyPrograms.find(a_format); found != a_impl.copyPrograms.end())
			return found->second;
		auto program = LoadCopyProgram(RenderGraphRuntime::Get().Host()->Device(), a_format);
		if (program)
			a_impl.copyPrograms.emplace(a_format, program);
		return program;
	}

	/**
	 * @brief The Upscale client as the frame needs it now: the copy into kMAIN while the graph holds the upscaled texture, and the
	 * depth upscale's copies and draws when a_depth (the depth upscale runs on every run of this registration). Registering again
	 * replaces the client. False, logged, when a pass cannot be made or the anchors refuse the client.
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
		client.passes = builder.Take();
		// The depth upscale's own pass reads the shared constants too, beside the underwater draw the builder saw.
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
			impl->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, impl->jitter);
			if (impl->queued & kCopyToMain)
				impl->upscaleGate->OpenDraw(kCopyEntry);
			if (impl->queued & kDepthUpscale)
				impl->upscaleGate->OpenDraw(kDepthEntry);
		};
		client.active = [weak] {
			const auto impl = weak.lock();
			return impl && impl->queued != 0;
		};
		client.onFailed = [weak] {
			if (const auto impl = weak.lock())
				MarkFailed(impl, "the render graph failed");
		};
		impl.registered = FrameAnchors::Register(std::move(client));
		impl.depthRegistered = impl.registered && depth;
		return impl.registered;
	}

	/**
	 * @brief The FrameGeneration client: CopyHudlessPS from the current source into the hudless texture, through the run gate.
	 * Registering again replaces it. False, logged, when the pass cannot be made or the anchors refuse the client.
	 */
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
		// CopyHudless runs the anchor for this copy alone, so every run that commits draws it.
		client.commit = [weak](const FrameAnchors::Point&) {
			if (const auto impl = weak.lock())
				impl->hudlessGate->OpenDraw(0);
		};
		client.onFailed = [weak] {
			if (const auto impl = weak.lock())
				MarkFailed(impl, "the render graph failed");
		};
		impl.hudlessRegistered = FrameAnchors::Register(std::move(client));
		return impl.hudlessRegistered;
	}

	/** @brief A registered Upscale client made again with what it holds now: a new texture or engine target. */
	void UpscaleChanged(const std::shared_ptr<Impl>& a_impl)
	{
		if (a_impl->registered && !RegisterUpscale(a_impl, a_impl->depthRegistered))
			MarkFailed(a_impl, "its Upscale client could not be registered again");
	}

	/** @brief A registered FrameGeneration client made again with what it holds now: a new texture or source. */
	void HudlessChanged(const std::shared_ptr<Impl>& a_impl)
	{
		if (a_impl->hudlessRegistered && !RegisterHudless(a_impl))
			MarkFailed(a_impl, "its FrameGeneration client could not be registered again");
	}
}

bool ORGUpscaling::Requested()
{
	return Switches::On("CS_ORG_UPSCALING");
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
		// The D3D11 path's Deferred::LinearSampler: linear, clamped.
		next->linearSampler = ShaderBindings::CreateSampler({});
		next->latch = std::make_shared<org::LatchBlock>("cs.upscaling.jitter", static_cast<std::uint32_t>(sizeof(Upscaling::JitterCB)), host->FrameSlots());
		next->upscaleGate = RenderGraph::RunGate::Create("cs.upscaling.gate", kUpscaleEntries);
		next->hudlessGate = RenderGraph::RunGate::Create("cs.upscaling.hudless.gate");
		CurrentDepthInputs(next->inputs, next->formats);
		if (next->inputs.DepthComplete()) {
			next->depthProgram = LoadDepthProgram(host->Device(), next->formats);
			next->underwaterProgram = LoadUnderwaterProgram(host->Device(), next->formats);
		}
		CurrentMain(next->inputs);
		const auto mainProgram = next->inputs.main ? CopyProgram(*next, next->inputs.mainFormat) : nullptr;
		if (!next->linearSampler || !next->latch || !next->upscaleGate || !next->hudlessGate || !mainProgram || !next->depthProgram ||
			!next->underwaterProgram) {
			logger::warn("[ORG] Upscaling stays on D3D11: its sampler, latch, gates, programs or engine targets could not be made");
			return false;
		}
		impl = std::move(next);
		setupFailed = false;
		logger::info("[ORG] Upscaling's own passes run on the render graph ({}x{}); the upscalers stay in the interop command ring",
			impl->inputs.width, impl->inputs.height);
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

void ORGUpscaling::ClearShaderCache()
{
	// Programs that built reload through the pipeline registry. A setup that failed (a shader that did not compile) is tried
	// again; until the textures are made again the copy to kMAIN and the hudless copy stay on D3D11.
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
	FrameAnchors::Unregister(kHudlessClientId);
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
	// Streamline writes the upscaled texture (its UAV) and reads the hudless one (its SRV) through their D3D11 images, outside the
	// graph: exported, kept.
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

bool ORGUpscaling::QueueCopyToMain()
{
	if (!IsActive() || !impl->upscaled)
		return false;
	// A new kMAIN (a resolution change): the client is made again with it.
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
	if (!(inputs == impl->inputs)) {
		if (!(formats == impl->formats)) {
			// New engine targets in other formats: the programs follow them.
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
	// The depth upscale's copies have no per-run switch, so the client holds them exactly while the depth upscale runs and is
	// made again when it starts or stops (an upscaler at full resolution queues only the copy into kMAIN).
	const bool depth = (impl->queued & kDepthUpscale) != 0;
	if ((!impl->registered || depth != impl->depthRegistered) && !RegisterUpscale(impl, depth)) {
		impl->queued = 0;
		MarkFailed(impl, "its Upscale client could not be registered");
		return;
	}
	(void)FrameAnchors::Run(FrameAnchors::Anchor::Upscale);
	impl->queued = 0;
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
	// The framebuffer is the engine's target (the overlay imports it too); HDR's clean composite is a D3D11 texture of its own.
	auto& framebuffer = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
	auto source = framebuffer.texture == texture.get() ? FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kFRAMEBUFFER)) :
	                                                     FrameAnchors::D3D11Texture("Upscaling::HudlessSource", texture.get(), a_source, nullptr);
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
	return FrameAnchors::Run(FrameAnchors::Anchor::FrameGeneration) && IsActive();
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Upscaling's own passes always draw with D3D11.
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
bool ORGUpscaling::QueueCopyToMain() { return false; }
bool ORGUpscaling::QueueDepthUpscale(float2, float) { return false; }
void ORGUpscaling::RunUpscale() {}
bool ORGUpscaling::CopyHudless(ID3D11ShaderResourceView*) { return false; }

#endif
