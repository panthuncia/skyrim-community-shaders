#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGUpscaling.h"

#	include "Features/Upscaling.h"
#	include "Globals.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <array>
#	include <atomic>
#	include <cstring>
#	include <map>
#	include <utility>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.upscaling";
	constexpr const char* kHudlessClientId = "cs.upscaling.hudless";
	constexpr const char* kUpscaledName = "Upscaling::UpscaledTexture";
	constexpr const char* kHudlessName = "Upscaling::HudlessTexture";
	constexpr const char* kDepthShader = "Upscaling/DepthRefractionUpscalePS.hlsl";
	constexpr const char* kUnderwaterShader = "Upscaling/UnderwaterMaskUpscalePS.hlsl";
	constexpr const char* kHudlessShader = "Upscaling/CopyHudlessPS.hlsl";

	// What a run of the anchors does, as bits of Shared::live: each pass checks its own.
	constexpr std::uint32_t kCopyToMain = 1u << 0;
	constexpr std::uint32_t kDepthUpscale = 1u << 1;
	constexpr std::uint32_t kHudlessCopy = 1u << 2;

	// No value prepared yet: differs from both states of a bit.
	constexpr std::uint8_t kNotPrepared = 0xFF;

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
	struct HudlessBlock
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

	/** @brief The engine targets the Upscale anchor's passes read and write, as graph textures. */
	struct Inputs
	{
		std::shared_ptr<org::ExternalTextureResource> main;
		std::shared_ptr<org::ExternalTextureResource> depth, depthCopy;
		std::shared_ptr<org::ExternalTextureResource> refraction, refractionCopy, saoCameraZ;
		std::shared_ptr<org::ExternalTextureResource> underwater, underwaterCopy;
		std::uint32_t width = 0, height = 0;  // the screen: the depth upscale's render area

		bool operator==(const Inputs&) const = default;

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

	/** @brief kMAIN, as the copy writes it. */
	std::shared_ptr<org::ExternalTextureResource> CurrentMain()
	{
		return FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
	}

	/**
	 * @brief The depth upscale's targets as UpscaleDepth binds them. Empty depth fields when one is missing or a copy is its own
	 * source: the D3D11 path then draws.
	 */
	void CurrentDepthInputs(Inputs& a_inputs, DepthFormats& a_formats)
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
		a_formats = { FormatOf(refraction.RTV), FormatOf(saoCameraZ.RTV), FormatOf(underwater.RTV), FormatOf(depth.views[0]) };
	}

	std::shared_ptr<const RasterProgram> LoadDepthProgram(rhi::Device a_device, const DepthFormats& a_formats)
	{
		// As the D3D11 states: no blending, depth written with the test always passing, no culling.
		return RasterProgram::Load(a_device, { .pixelSource = kDepthShader,
												 .constantWords = ShaderBindings::Words<DepthBlock>(),
												 .defines = { { L"PSHADER", L"" } },
												 .targets = { { .format = a_formats.refraction }, { .format = a_formats.saoCameraZ } },
												 .depthFormat = a_formats.depth,
												 .depth = { .depthEnable = true, .depthWrite = true, .depthFunc = rhi::CompareOp::Always } });
	}

	std::shared_ptr<const RasterProgram> LoadUnderwaterProgram(rhi::Device a_device, const DepthFormats& a_formats)
	{
		return RasterProgram::Load(a_device, { .pixelSource = kUnderwaterShader,
												 .constantWords = ShaderBindings::Words<UnderwaterBlock>(),
												 .defines = { { L"PSHADER", L"" } },
												 .targets = { { .format = a_formats.underwater } } });
	}

	std::shared_ptr<const RasterProgram> LoadHudlessProgram(rhi::Device a_device, rhi::Format a_format)
	{
		return RasterProgram::Load(a_device, { .pixelSource = kHudlessShader,
												 .constantWords = ShaderBindings::Words<HudlessBlock>(),
												 .defines = { { L"PSHADER", L"" } },
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
	std::map<rhi::Format, std::shared_ptr<const RasterProgram>> hudlessPrograms;
	org::OwnedDescriptorBinding linearSampler;
	std::shared_ptr<org::LatchBlock> latch;

	std::unique_ptr<GraphResources::Texture2D> upscaled, hudless;
	Inputs inputs;
	std::shared_ptr<org::ExternalTextureResource> hudlessSource;
	std::shared_ptr<const RasterProgram> hudlessProgram;  // for the hudless texture's format

	// Render thread: this frame's queued work, and the depth upscale's constants.
	std::uint32_t queued = 0;
	bool hudlessQueued = false;
	Upscaling::JitterCB jitter{};

	bool registered = false, hudlessRegistered = false;
	bool failed = false;

	// Published at the hook (the clients' active): what this run of the anchor does.
	std::atomic<std::uint32_t> live = 0;
};

namespace
{
	using Impl = ORGUpscaling::Impl;

	/**
	 * @brief A pass's per-run switch: whether this run of its anchor does its work. The recording is kept while the bit stays as it
	 * was prepared with; a ticket prepared before the hook published another value takes a value no later check repeats, so it is
	 * prepared again (as Cloud Shadows' copy does).
	 */
	class Gate
	{
	public:
		Gate(std::shared_ptr<const Impl> a_impl, std::uint32_t a_bit) :
			impl(std::move(a_impl)), bit(a_bit) {}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			const std::uint8_t used = prepared.load(std::memory_order_acquire);
			a_out.push_back(used);
			if (used != Live())
				a_out.push_back(mismatch.fetch_add(1, std::memory_order_relaxed) + 1);
		}

		bool Prepare()
		{
			const std::uint8_t live = Live();
			prepared.store(live, std::memory_order_release);
			return live != 0;
		}

	private:
		std::uint8_t Live() const { return (impl->live.load(std::memory_order_acquire) & bit) ? 1 : 0; }

		std::shared_ptr<const Impl> impl;
		std::uint32_t bit;
		std::atomic<std::uint8_t> prepared = kNotPrepared;
		mutable std::atomic<std::uint64_t> mismatch = 0;
	};

	struct CopyBindings
	{
		org::ResourceBindingToken from, to;
	};

	struct CopyFrame
	{
		bool run = false;
	};

	/** @brief One texture's mip 0 into another of the same size and format, as D3D11's CopyResource of a one-mip texture. */
	class CopyPass final : public org::TypedRenderGraphPass<CopyPass, CopyFrame, CopyBindings>
	{
	public:
		CopyPass(std::shared_ptr<const Impl> a_impl, std::uint32_t a_bit, std::shared_ptr<org::Resource> a_from, std::shared_ptr<org::Resource> a_to) :
			gate(std::move(a_impl), a_bit), from(std::move(a_from)), to(std::move(a_to)) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopySource(from), a_builder.CopyDestination(to) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(from.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(to.get()));
			gate.Revision(a_out);
		}

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) { return { gate.Prepare() }; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.run)
				return;
			// A zero size copies the source's whole mip.
			const rhi::TextureCopyRegion destination{ .texture = a_recording.Resolve(a_bindings.to).GetHandle() };
			const rhi::TextureCopyRegion source{ .texture = a_recording.Resolve(a_bindings.from).GetHandle() };
			a_recording.Commands().CopyTextureRegion(destination, source);
		}

	private:
		Gate gate;
		std::shared_ptr<org::Resource> from, to;
	};

	struct DepthBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken refractionCopy, depthCopy, refraction, saoCameraZ, depth;
	};

	struct DrawFrame
	{
		std::shared_ptr<const RasterProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t sampler = 0;
		std::uint32_t width = 0, height = 0;
	};

	/** @brief DepthRefractionUpscalePS: the refraction normals, kSAO_CAMERAZ and kMAIN's depth, from their render-size copies. */
	class DepthUpscalePass final : public org::TypedRenderGraphPass<DepthUpscalePass, DrawFrame, DepthBindings>
	{
	public:
		explicit DepthUpscalePass(const std::shared_ptr<const Impl>& a_impl) :
			gate(a_impl, kDepthUpscale),
			frame{ a_impl->depthProgram, a_impl->latch, a_impl->linearSampler.Index(), a_impl->inputs.width, a_impl->inputs.height },
			inputs(a_impl->inputs) {}

		DepthBindings Declare(org::PassBuilder& a_builder)
		{
			DepthBindings bindings{};
			bindings.shared = SharedConstants::Declare(a_builder);
			bindings.refractionCopy = a_builder.ShaderResource(inputs.refractionCopy).View();
			bindings.depthCopy = a_builder.ShaderResource(inputs.depthCopy).View();
			bindings.refraction = a_builder.RenderTarget(inputs.refraction).View();
			bindings.saoCameraZ = a_builder.RenderTarget(inputs.saoCameraZ).View();
			bindings.depth = a_builder.DepthReadWrite(inputs.depth).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.latch.get()));
			a_out.push_back(frame.sampler);
			a_out.push_back((std::uint64_t(frame.width) << 32) | frame.height);
			gate.Revision(a_out);
		}

		DrawFrame Prepare(const DepthBindings&, const org::PassPrepareContext&) { return gate.Prepare() ? frame : DrawFrame{}; }

		static void Record(const DepthBindings& a_bindings, const DrawFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			const DepthBlock block{ SharedConstants::Index(a_recording, a_bindings.shared), a_recording.Index(a_bindings.refractionCopy),
				a_recording.Index(a_bindings.depthCopy), a_frame.sampler, ShaderBindings::LatchOf(*a_frame.latch, a_recording.FrameSlot()) };
			const std::array<RasterProgram::Colour, 2> colours{ { { .view = &a_bindings.refraction }, { .view = &a_bindings.saoCameraZ } } };
			const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
			a_frame.program->Draw(a_recording, { .colours = colours,
												   .depth = &depth,
												   .viewport = { .width = a_frame.width, .height = a_frame.height },
												   .constants = ShaderBindings::Data(block),
												   .debugName = "Upscaling::DepthUpscale" });
		}

	private:
		Gate gate;
		DrawFrame frame;
		Inputs inputs;
	};

	struct UnderwaterBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken underwaterCopy, underwater;
	};

	/** @brief UnderwaterMaskUpscalePS at half the screen, as the D3D11 path's viewport. */
	class UnderwaterPass final : public org::TypedRenderGraphPass<UnderwaterPass, DrawFrame, UnderwaterBindings>
	{
	public:
		explicit UnderwaterPass(const std::shared_ptr<const Impl>& a_impl) :
			gate(a_impl, kDepthUpscale),
			frame{ a_impl->underwaterProgram, a_impl->latch, a_impl->linearSampler.Index(), a_impl->inputs.width / 2, a_impl->inputs.height / 2 },
			inputs(a_impl->inputs) {}

		UnderwaterBindings Declare(org::PassBuilder& a_builder)
		{
			UnderwaterBindings bindings{};
			bindings.shared = SharedConstants::Declare(a_builder);
			bindings.underwaterCopy = a_builder.ShaderResource(inputs.underwaterCopy).View();
			bindings.underwater = a_builder.RenderTarget(inputs.underwater).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.latch.get()));
			a_out.push_back(frame.sampler);
			a_out.push_back((std::uint64_t(frame.width) << 32) | frame.height);
			gate.Revision(a_out);
		}

		DrawFrame Prepare(const UnderwaterBindings&, const org::PassPrepareContext&) { return gate.Prepare() ? frame : DrawFrame{}; }

		static void Record(const UnderwaterBindings& a_bindings, const DrawFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			const UnderwaterBlock block{ SharedConstants::Index(a_recording, a_bindings.shared), a_recording.Index(a_bindings.underwaterCopy),
				a_frame.sampler, ShaderBindings::LatchOf(*a_frame.latch, a_recording.FrameSlot()) };
			const RasterProgram::Colour colour{ .view = &a_bindings.underwater };
			a_frame.program->Draw(a_recording, { .colours = { &colour, 1 },
												   .viewport = { .width = a_frame.width, .height = a_frame.height },
												   .constants = ShaderBindings::Data(block),
												   .debugName = "Upscaling::UnderwaterMaskUpscale" });
		}

	private:
		Gate gate;
		DrawFrame frame;
		Inputs inputs;
	};

	struct HudlessBindings
	{
		org::DeclaredViewToken source, target;
	};

	/** @brief CopyHudlessPS: the source into the hudless texture, converting its format. */
	class HudlessPass final : public org::TypedRenderGraphPass<HudlessPass, DrawFrame, HudlessBindings>
	{
	public:
		explicit HudlessPass(const std::shared_ptr<const Impl>& a_impl) :
			gate(a_impl, kHudlessCopy),
			frame{ a_impl->hudlessProgram, nullptr, 0, a_impl->hudless->Desc().Width, a_impl->hudless->Desc().Height },
			source(a_impl->hudlessSource),
			target(a_impl->hudless->Graph()) {}

		HudlessBindings Declare(org::PassBuilder& a_builder)
		{
			return { a_builder.ShaderResource(source).View(), a_builder.RenderTarget(target).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(source.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(target.get()));
			a_out.push_back((std::uint64_t(frame.width) << 32) | frame.height);
			gate.Revision(a_out);
		}

		DrawFrame Prepare(const HudlessBindings&, const org::PassPrepareContext&) { return gate.Prepare() ? frame : DrawFrame{}; }

		static void Record(const HudlessBindings& a_bindings, const DrawFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program)
				return;
			const HudlessBlock block{ a_recording.Index(a_bindings.source) };
			const RasterProgram::Colour colour{ .view = &a_bindings.target };
			a_frame.program->Draw(a_recording, { .colours = { &colour, 1 },
												   .viewport = { .width = a_frame.width, .height = a_frame.height },
												   .constants = ShaderBindings::Data(block),
												   .debugName = "Upscaling::CopyHudless" });
		}

	private:
		Gate gate;
		DrawFrame frame;
		std::shared_ptr<org::Resource> source, target;
	};

	void MarkFailed(const std::shared_ptr<Impl>& a_impl, const char* a_why)
	{
		if (!a_impl->failed)
			logger::error("[ORG] Upscaling returns to D3D11: {}", a_why);
		a_impl->failed = true;
	}

	bool RegisterUpscale(const std::shared_ptr<Impl>& a_impl)
	{
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.sharedConstants = true;
		const std::weak_ptr<Impl> weak = a_impl;
		const auto pass = [&](const char* a_name, std::int32_t a_order, FrameAnchors::PassKind a_kind,
							  std::function<std::shared_ptr<org::RenderPass>(const std::shared_ptr<Impl>&)> a_make) {
			client.passes.push_back({ .name = a_name,
				.anchor = FrameAnchors::Anchor::Upscale,
				.order = a_order,
				.kind = a_kind,
				.make = [weak, a_make = std::move(a_make)]() -> std::shared_ptr<org::RenderPass> {
					const auto impl = weak.lock();
					return impl ? a_make(impl) : nullptr;
				} });
		};
		using Kind = FrameAnchors::PassKind;
		pass("cs.upscaling.copy-to-main", 100, Kind::Copy, [](const std::shared_ptr<Impl>& a_impl) -> std::shared_ptr<org::RenderPass> {
			if (!a_impl->upscaled || !a_impl->inputs.main)
				return nullptr;
			return std::make_shared<CopyPass>(a_impl, kCopyToMain, a_impl->upscaled->Graph(), a_impl->inputs.main);
		});
		pass("cs.upscaling.depth-copy", 200, Kind::Copy, [](const std::shared_ptr<Impl>& a_impl) -> std::shared_ptr<org::RenderPass> {
			if (!a_impl->inputs.DepthComplete())
				return nullptr;
			return std::make_shared<CopyPass>(a_impl, kDepthUpscale, a_impl->inputs.depth, a_impl->inputs.depthCopy);
		});
		pass("cs.upscaling.refraction-normals-copy", 201, Kind::Copy, [](const std::shared_ptr<Impl>& a_impl) -> std::shared_ptr<org::RenderPass> {
			if (!a_impl->inputs.DepthComplete())
				return nullptr;
			return std::make_shared<CopyPass>(a_impl, kDepthUpscale, a_impl->inputs.refraction, a_impl->inputs.refractionCopy);
		});
		pass("cs.upscaling.depth-upscale", 202, Kind::Render, [](const std::shared_ptr<Impl>& a_impl) -> std::shared_ptr<org::RenderPass> {
			if (!a_impl->inputs.DepthComplete() || !a_impl->depthProgram)
				return nullptr;
			return std::make_shared<DepthUpscalePass>(a_impl);
		});
		pass("cs.upscaling.underwater-mask-copy", 203, Kind::Copy, [](const std::shared_ptr<Impl>& a_impl) -> std::shared_ptr<org::RenderPass> {
			if (!a_impl->inputs.DepthComplete())
				return nullptr;
			return std::make_shared<CopyPass>(a_impl, kDepthUpscale, a_impl->inputs.underwater, a_impl->inputs.underwaterCopy);
		});
		pass("cs.upscaling.underwater-mask-upscale", 204, Kind::Render, [](const std::shared_ptr<Impl>& a_impl) -> std::shared_ptr<org::RenderPass> {
			if (!a_impl->inputs.DepthComplete() || !a_impl->underwaterProgram)
				return nullptr;
			return std::make_shared<UnderwaterPass>(a_impl);
		});
		client.registerResources = [weak](org::RenderGraph& a_graph) {
			if (const auto impl = weak.lock(); impl && impl->upscaled)
				impl->upscaled->Register(a_graph);
		};
		client.commit = [weak](const FrameAnchors::Point&) {
			if (const auto impl = weak.lock(); impl && impl->latch)
				impl->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, impl->jitter);
		};
		client.active = [weak] {
			const auto impl = weak.lock();
			if (!impl)
				return false;
			const std::uint32_t queued = std::exchange(impl->queued, 0u);
			impl->live.store((impl->live.load(std::memory_order_relaxed) & kHudlessCopy) | queued, std::memory_order_release);
			return queued != 0;
		};
		client.onFailed = [weak] {
			if (const auto impl = weak.lock())
				MarkFailed(impl, "the render graph failed");
		};
		a_impl->registered = FrameAnchors::Register(std::move(client));
		return a_impl->registered;
	}

	bool RegisterHudless(const std::shared_ptr<Impl>& a_impl)
	{
		FrameAnchors::ClientDesc client{};
		client.id = kHudlessClientId;
		const std::weak_ptr<Impl> weak = a_impl;
		client.passes.push_back({ .name = "cs.upscaling.hudless-copy",
			.anchor = FrameAnchors::Anchor::FrameGeneration,
			.order = 100,
			.kind = FrameAnchors::PassKind::Render,
			.make = [weak]() -> std::shared_ptr<org::RenderPass> {
				const auto impl = weak.lock();
				if (!impl || !impl->hudless || !impl->hudlessSource || !impl->hudlessProgram)
					return nullptr;
				return std::make_shared<HudlessPass>(impl);
			} });
		client.registerResources = [weak](org::RenderGraph& a_graph) {
			if (const auto impl = weak.lock(); impl && impl->hudless)
				impl->hudless->Register(a_graph);
		};
		client.active = [weak] {
			const auto impl = weak.lock();
			if (!impl)
				return false;
			const bool queued = std::exchange(impl->hudlessQueued, false);
			const std::uint32_t others = impl->live.load(std::memory_order_relaxed) & ~kHudlessCopy;
			impl->live.store(others | (queued ? kHudlessCopy : 0u), std::memory_order_release);
			return queued;
		};
		client.onFailed = [weak] {
			if (const auto impl = weak.lock())
				MarkFailed(impl, "the render graph failed");
		};
		a_impl->hudlessRegistered = FrameAnchors::Register(std::move(client));
		return a_impl->hudlessRegistered;
	}
}

bool ORGUpscaling::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_UPSCALING", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
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

void ORGUpscaling::Setup()
{
	impl.reset();
	if (!Requested())
		return;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Upscale) ||
		!FrameAnchors::HasEpoch(FrameAnchors::Anchor::FrameGeneration)) {
		logger::warn("[ORG] Upscaling stays on D3D11: the render graph or its Upscale and FrameGeneration anchors are not available");
		return;
	}
	try {
		auto next = std::make_shared<Impl>();
		// The D3D11 path's Deferred::LinearSampler: linear, clamped.
		next->linearSampler = ShaderBindings::CreateSampler({});
		next->latch = std::make_shared<org::LatchBlock>("cs.upscaling.jitter", static_cast<std::uint32_t>(sizeof(Upscaling::JitterCB)), host->FrameSlots());
		CurrentDepthInputs(next->inputs, next->formats);
		if (next->inputs.DepthComplete()) {
			next->depthProgram = LoadDepthProgram(host->Device(), next->formats);
			next->underwaterProgram = LoadUnderwaterProgram(host->Device(), next->formats);
		}
		next->inputs.main = CurrentMain();
		if (!next->linearSampler || !next->latch || !next->inputs.main || !next->depthProgram || !next->underwaterProgram) {
			logger::warn("[ORG] Upscaling stays on D3D11: its sampler, latch, programs or engine targets could not be made");
			return;
		}
		impl = std::move(next);
		logger::info("[ORG] Upscaling's own passes run on the render graph ({}x{}); the upscalers stay in the interop command ring",
			impl->inputs.width, impl->inputs.height);
	} catch (const std::exception& e) {
		logger::error("[ORG] Upscaling setup failed, staying on D3D11: {}", e.what());
		impl.reset();
	}
}

bool ORGUpscaling::Running() const
{
	return impl && !impl->failed && RenderGraphRuntime::Get().IsActive();
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

bool ORGUpscaling::CreateUpscaledTexture(const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (!Running())
		return false;
	if (impl->upscaled)
		return true;
	// Streamline writes it through its D3D11 image (the UAV) every upscaled frame, outside the graph: exported, kept.
	auto texture = std::make_unique<GraphResources::Texture2D>(Sanitized(a_desc, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS), kUpscaledName,
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV });
	if (!*texture || !texture->D3D11() || !texture->UAV()) {
		logger::error("[ORG] Upscaling: the upscaled texture could not be made on the graph; it stays a D3D11 texture");
		return false;
	}
	impl->upscaled = std::move(texture);
	if (impl->registered)
		FrameAnchors::Refresh(kClientId);
	logger::info("[ORG] Upscaling: created the upscaled texture on the graph ({}x{}, format={})", a_desc.Width, a_desc.Height,
		static_cast<int>(a_desc.Format));
	return true;
}

void ORGUpscaling::DestroyUpscaledTexture()
{
	if (!impl || !impl->upscaled)
		return;
	impl->upscaled.reset();
	if (impl->registered)
		FrameAnchors::Refresh(kClientId);
}

ID3D11Texture2D* ORGUpscaling::UpscaledTexture() const
{
	return impl && impl->upscaled ? impl->upscaled->D3D11() : nullptr;
}

bool ORGUpscaling::CreateHudlessTexture(const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (!Running())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	const rhi::Format format = rhi::helpers::ToRHI(a_desc.Format);
	auto& program = impl->hudlessPrograms[format];
	if (!program) {
		// Once per format the presenter uses: the D3D11 path compiles its shader on first use too.
		program = LoadHudlessProgram(runtime.Host()->Device(), format);
		if (!program) {
			impl->hudlessPrograms.erase(format);
			logger::error("[ORG] Upscaling: the hudless copy could not be built for format {}; it stays a D3D11 texture", static_cast<int>(a_desc.Format));
			return false;
		}
	}
	// Streamline reads it through its D3D11 image (the SRV): exported, kept.
	auto texture = std::make_unique<GraphResources::Texture2D>(Sanitized(a_desc, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET), kHudlessName,
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::RTV });
	if (!*texture || !texture->D3D11()) {
		logger::error("[ORG] Upscaling: the hudless texture could not be made on the graph; it stays a D3D11 texture");
		return false;
	}
	impl->hudless = std::move(texture);
	impl->hudlessProgram = program;
	if (impl->hudlessRegistered)
		FrameAnchors::Refresh(kHudlessClientId);
	logger::info("[ORG] Upscaling: created the hudless texture on the graph ({}x{}, format={})", a_desc.Width, a_desc.Height,
		static_cast<int>(a_desc.Format));
	return true;
}

void ORGUpscaling::DestroyHudlessTexture()
{
	if (!impl || !impl->hudless)
		return;
	impl->hudless.reset();
	impl->hudlessProgram.reset();
	if (impl->hudlessRegistered)
		FrameAnchors::Refresh(kHudlessClientId);
}

ID3D11Texture2D* ORGUpscaling::HudlessTexture() const
{
	return impl && impl->hudless ? impl->hudless->D3D11() : nullptr;
}

const D3D11_TEXTURE2D_DESC* ORGUpscaling::HudlessDesc() const
{
	return impl && impl->hudless ? &impl->hudless->Desc() : nullptr;
}

bool ORGUpscaling::QueueCopyToMain()
{
	if (!Running() || !impl->upscaled)
		return false;
	// A new kMAIN (a resolution change): the passes are made again with it.
	if (auto main = CurrentMain(); main != impl->inputs.main) {
		if (!main)
			return false;
		impl->inputs.main = std::move(main);
		if (impl->registered)
			FrameAnchors::Refresh(kClientId);
	}
	impl->queued |= kCopyToMain;
	return true;
}

bool ORGUpscaling::QueueDepthUpscale(float2 a_jitter, float a_useWideKernel)
{
	if (!Running())
		return false;
	Inputs inputs{};
	DepthFormats formats{};
	CurrentDepthInputs(inputs, formats);
	if (!inputs.DepthComplete())
		return false;
	inputs.main = impl->inputs.main;
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
		if (impl->registered)
			FrameAnchors::Refresh(kClientId);
	}
	impl->jitter.jitter = a_jitter;
	impl->jitter.useWideKernel = a_useWideKernel;
	impl->jitter.pad0 = 0.0f;
	impl->queued |= kDepthUpscale;
	return true;
}

void ORGUpscaling::RunUpscale()
{
	if (!impl)
		return;
	if (!impl->queued)
		return;
	if (!Running()) {
		impl->queued = 0;
		return;
	}
	// Registered at the first run: the shared constants exist once State has uploaded them.
	if (!impl->registered && !RegisterUpscale(impl)) {
		impl->queued = 0;
		MarkFailed(impl, "its Upscale client could not be registered");
		return;
	}
	(void)FrameAnchors::Run(FrameAnchors::Anchor::Upscale);
}

bool ORGUpscaling::CopyHudless(ID3D11ShaderResourceView* a_source)
{
	if (!Running() || !impl->hudless || !impl->hudlessProgram || !a_source)
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
		if (impl->hudlessRegistered)
			FrameAnchors::Refresh(kHudlessClientId);
	}
	if (!impl->hudlessRegistered && !RegisterHudless(impl)) {
		MarkFailed(impl, "its FrameGeneration client could not be registered");
		return false;
	}
	impl->hudlessQueued = true;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::FrameGeneration);
	impl->hudlessQueued = false;
	return ran && Running();
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
void ORGUpscaling::Setup() {}
bool ORGUpscaling::Running() const { return false; }
bool ORGUpscaling::Failed() const { return false; }
bool ORGUpscaling::ReleaseAfterFailure() { return false; }
bool ORGUpscaling::CreateUpscaledTexture(const D3D11_TEXTURE2D_DESC&) { return false; }
void ORGUpscaling::DestroyUpscaledTexture() {}
ID3D11Texture2D* ORGUpscaling::UpscaledTexture() const { return nullptr; }
bool ORGUpscaling::CreateHudlessTexture(const D3D11_TEXTURE2D_DESC&) { return false; }
void ORGUpscaling::DestroyHudlessTexture() {}
ID3D11Texture2D* ORGUpscaling::HudlessTexture() const { return nullptr; }
const D3D11_TEXTURE2D_DESC* ORGUpscaling::HudlessDesc() const { return nullptr; }
bool ORGUpscaling::QueueCopyToMain() { return false; }
bool ORGUpscaling::QueueDepthUpscale(float2, float) { return false; }
void ORGUpscaling::RunUpscale() {}
bool ORGUpscaling::CopyHudless(ID3D11ShaderResourceView*) { return false; }

#endif
