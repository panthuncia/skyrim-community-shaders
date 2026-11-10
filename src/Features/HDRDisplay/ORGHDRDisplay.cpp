#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/HDRDisplay/ORGHDRDisplay.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"

bool ORGHDRDisplay::Requested()
{
	static const bool requested = Switches::On("CS_ORG_HDR_DISPLAY");
	return requested;
}

ORGHDRDisplay& ORGHDRDisplay::Get()
{
	static ORGHDRDisplay s_hdrDisplay;
	return s_hdrDisplay;
}

bool ORGHDRDisplay::IsActive() const
{
	return impl && !failed && RenderGraphRuntime::Get().IsActive();
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "Util.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Resources/ExternalTextureResource.h>

#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.hdr";
	constexpr const char* kShader = "HDRDisplay/HDROutputCS.hlsl";
	constexpr const char* kSceneName = "hdr.scene";
	constexpr const char* kOutputName = "hdr.output";
	constexpr const char* kUiName = "hdr.ui";
	constexpr const char* kBackBufferName = "hdr.back-buffer";
	// The composite, the back buffer copy and the UI clear take the order keys from here up.
	constexpr std::int32_t kOrderBase = 100;

	using ShaderBindings::Handle;

	// HDROutputCS's push block under ORG, in its HDR_OUTPUT_RESOURCES order.
	struct OutputBlock
	{
		SharedConstants::Handles shared;
		Handle scene, ui, output;
		ShaderBindings::Latch perFrame;
	};
	static_assert(sizeof(OutputBlock) == 36, "matches the shader's push block (check-dual-target-shaders reports 36 bytes)");
	static_assert(sizeof(HDRDisplay::HDRDataCB) == 48, "the latch region holds the constants as the D3D11 cbuffer does");

	/** @brief What the passes are made from. A change registers the client again. */
	struct Inputs
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<org::ExternalTextureResource> scene, ui, output;
		std::shared_ptr<org::ExternalTextureResource> backBuffer;  // null: no copy pass
		std::uint32_t groupsX = 0, groupsY = 0;
		std::uint32_t width = 0, height = 0;  // the output's size: the copy's extent

		bool operator==(const Inputs&) const = default;
	};

	ComputeProgram::Desc ProgramDesc()
	{
		// The program adds the D3D11 build's defines itself (ComputeProgram::Desc::commonDefines).
		return { .source = kShader, .constantWords = ShaderBindings::Words<OutputBlock>() };
	}

	/**
	 * @brief The client's passes: HDROutputCS (the scene and the UI into the output, as DispatchHDROutput), the output into the
	 * back buffer when there is one (ApplyHDR's CopyResource), then the UI texture cleared to transparent black after the composite
	 * read it (ClearUIBuffer's clear, through the UI texture's UAV). Every pass runs at every run of the anchor.
	 */
	RenderGraph::AnchorPasses::Builder MakePasses(const Inputs& a_in, std::shared_ptr<const org::LatchBlock> a_latch)
	{
		using RenderGraph::AnchorPasses::Bind;
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::HDROutput, kClientId, kOrderBase, std::move(a_latch));
		builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = "output",
			.program = a_in.program,
			.bindings = { Bind::Shared(), Bind::Srv(a_in.scene), Bind::Srv(a_in.ui), Bind::Uav(a_in.output), Bind::Latch(0) },
			.groups = { a_in.groupsX, a_in.groupsY, 1 } });
		if (a_in.backBuffer)
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "back-buffer", .source = a_in.output, .target = a_in.backBuffer, .width = a_in.width, .height = a_in.height });
		builder.Add(RenderGraph::AnchorPasses::Clear{ .name = "ui-clear", .target = a_in.ui }, false);
		return builder;
	}

	/**
	 * @brief Whether the graph may copy the output into this back buffer: the formats and sizes CopyResource needs, and one back
	 * buffer only. With several, DXVK rotates their images at each present on its own thread, which an import made once cannot follow.
	 */
	bool CanCopyInto(ID3D11Texture2D* a_backBuffer, const D3D11_TEXTURE2D_DESC& a_output)
	{
		D3D11_TEXTURE2D_DESC desc{};
		a_backBuffer->GetDesc(&desc);
		if (desc.Format != a_output.Format || desc.Width != a_output.Width || desc.Height != a_output.Height)
			return false;
		DXGI_SWAP_CHAIN_DESC swapChain{};
		if (!globals::d3d::swapChain || FAILED(globals::d3d::swapChain->GetDesc(&swapChain)))
			return false;
		const bool sequential = swapChain.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL || swapChain.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		return !sequential || swapChain.BufferCount <= 1;
	}
}

// C4324: the aligned HDR constants pad the struct.
#	pragma warning(push)
#	pragma warning(disable: 4324)
struct ORGHDRDisplay::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	bool programDirty = false;
	std::shared_ptr<org::LatchBlock> latch;
	std::function<void()> onFailed;

	// The last back buffer asked about, and whether the graph copies into it (CanCopyInto and the import).
	winrt::com_ptr<ID3D11Texture2D> backBufferTexture;
	std::shared_ptr<org::ExternalTextureResource> backBuffer;

	// What the registered passes were made from; null before the first registration.
	std::shared_ptr<const Inputs> registered;

	// This frame's constants, for the commit.
	HDRDisplay::HDRDataCB constants{};
};
#	pragma warning(pop)

ORGHDRDisplay::~ORGHDRDisplay() = default;

void ORGHDRDisplay::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::HDROutput))
		logger::warn("[ORG] HDR Display stays on D3D11: the HDROutput anchor could not be reserved");
}

bool ORGHDRDisplay::Setup(std::function<void()> a_onFailed)
{
	if (impl)
		return true;
	if (!Requested() || failed)
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::HDROutput)) {
		Fail("the render graph or its HDROutput anchor is not available");
		return false;
	}
	auto program = ComputeProgram::Load(host->Device(), ProgramDesc());
	if (!program) {
		Fail("HDROutputCS could not be built for the graph");
		return false;
	}
	auto state = std::make_unique<Impl>();
	state->program = std::move(program);
	state->latch = std::make_shared<org::LatchBlock>("cs.hdr.latch", static_cast<std::uint32_t>(sizeof(HDRDisplay::HDRDataCB)), host->FrameSlots());
	state->onFailed = std::move(a_onFailed);
	impl = std::move(state);
	return true;
}

void ORGHDRDisplay::Fail(const char* a_why)
{
	if (!failed)
		logger::error("[ORG] HDR Display returns to D3D11: {}", a_why);
	failed = true;
	FrameAnchors::Unregister(kClientId);
	auto onFailed = impl ? std::move(impl->onFailed) : std::function<void()>();
	impl.reset();
	if (onFailed)
		onFailed();
}

void ORGHDRDisplay::ClearShaderCache()
{
	if (impl)
		impl->programDirty = true;
}

std::shared_ptr<org::ExternalTextureResource> ORGHDRDisplay::ImportOf(ID3D11Texture2D* a_texture) const
{
	if (!impl || !impl->registered || !a_texture)
		return nullptr;
	const auto& inputs = *impl->registered;
	if (inputs.backBuffer && a_texture == impl->backBufferTexture.get())
		return inputs.backBuffer;
	const auto& hdr = globals::features::hdrDisplay;
	if (inputs.output && hdr.outputTexture && a_texture == hdr.outputTexture->resource.get())
		return inputs.output;
	return nullptr;
}

bool ORGHDRDisplay::TakeUIClear()
{
	return std::exchange(uiCleared, false);
}

ORGHDRDisplay::Composed ORGHDRDisplay::Compose(const HDRDisplay::HDRDataCB& a_constants, ID3D11Texture2D* a_backBuffer)
{
	uiCleared = false;
	if (!IsActive())
		return Composed::None;

	auto& hdr = globals::features::hdrDisplay;
	if (!hdr.outputTexture || !hdr.outputTexture->uav || !hdr.outputTexture->srv || !hdr.uiTexture || !hdr.uiTexture->srv || !hdr.uiTexture->uav)
		return Composed::None;

	auto& state = *impl;
	if (state.programDirty) {
		state.programDirty = false;
		// A reload that fails keeps the program it had, as the D3D11 shader cache does.
		if (auto program = ComputeProgram::Load(RenderGraphRuntime::Get().Host()->Device(), ProgramDesc()))
			state.program = std::move(program);
		else
			logger::warn("[ORG] HDR Display: HDROutputCS failed to build again; the graph keeps the last program");
	}

	// The scene as ApplyHDR picks it: HDR's float texture, else the framebuffer.
	const bool hdrScene = hdr.IsHDREnabledForFrame() && hdr.hdrTexture && hdr.hdrTexture->srv;
	auto scene = hdrScene ? FrameAnchors::D3D11Texture(kSceneName, hdr.hdrTexture->resource.get(), hdr.hdrTexture->srv.get(), nullptr) :
	                        FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER));
	auto output = FrameAnchors::D3D11Texture(kOutputName, hdr.outputTexture->resource.get(), hdr.outputTexture->srv.get(), hdr.outputTexture->uav.get());
	auto ui = FrameAnchors::D3D11Texture(kUiName, hdr.uiTexture->resource.get(), hdr.uiTexture->srv.get(), hdr.uiTexture->uav.get());
	if (!scene || !output || !ui) {
		Fail(!ui ? "the UI texture cannot be imported" : !scene ? "the scene cannot be imported" :
																"the output texture cannot be imported");
		return Composed::None;
	}

	const D3D11_TEXTURE2D_DESC& outputDesc = hdr.outputTexture->desc;
	if (a_backBuffer != state.backBufferTexture.get()) {
		state.backBufferTexture.copy_from(a_backBuffer);
		state.backBuffer = a_backBuffer && CanCopyInto(a_backBuffer, outputDesc) ? FrameAnchors::D3D11Texture(kBackBufferName, a_backBuffer, nullptr, nullptr) : nullptr;
		if (a_backBuffer)
			logger::info("[ORG] HDR Display: the output goes into the back buffer {}", state.backBuffer ? "on the graph" : "with D3D11 (it cannot be copied on the graph)");
	}

	auto inputs = std::make_shared<Inputs>();
	inputs->program = state.program;
	inputs->scene = std::move(scene);
	inputs->ui = std::move(ui);
	inputs->output = std::move(output);
	inputs->backBuffer = state.backBuffer;
	const auto groups = Util::GetScreenDispatchCount(false);
	inputs->groupsX = groups.x;
	inputs->groupsY = groups.y;
	inputs->width = outputDesc.Width;
	inputs->height = outputDesc.Height;

	if (!state.registered || !(*state.registered == *inputs)) {
		auto builder = MakePasses(*inputs, state.latch);
		if (builder.Failed()) {
			Fail(builder.Failure().c_str());
			return Composed::None;
		}
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.sharedConstants = builder.ReadsSharedConstants();
		client.passes = builder.Take();
		client.commit = [](const FrameAnchors::Point&) {
			auto& self = Get();
			if (self.impl)
				self.impl->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, self.impl->constants);
		};
		// Run unregisters the clients before this; Compose stops the path once Run returns.
		client.onFailed = [] { Get().graphFailed = true; };
		if (!FrameAnchors::Register(std::move(client))) {
			Fail("its client could not be registered on the HDROutput anchor");
			return Composed::None;
		}
		const bool first = !state.registered;
		state.registered = std::move(inputs);
		logger::info("[ORG] HDR Display {} on the render graph ({}x{}, scene from {}, back buffer copy {})", first ? "runs" : "registered again", outputDesc.Width,
			outputDesc.Height, hdrScene ? "the HDR texture" : "the framebuffer", state.registered->backBuffer ? "on the graph" : "not on the graph");
	}

	state.constants = a_constants;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::HDROutput);
	if (graphFailed) {
		Fail("the render graph failed");
		return Composed::None;
	}
	if (!ran)
		return Composed::None;
	uiCleared = true;
	return state.registered->backBuffer ? Composed::OutputAndBackBuffer : Composed::Output;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: HDR Display always composes with D3D11.
struct ORGHDRDisplay::Impl
{};

ORGHDRDisplay::~ORGHDRDisplay() = default;
void ORGHDRDisplay::Reserve() {}
bool ORGHDRDisplay::Setup(std::function<void()>) { return false; }
void ORGHDRDisplay::Fail(const char*) {}
void ORGHDRDisplay::ClearShaderCache() {}
std::shared_ptr<org::ExternalTextureResource> ORGHDRDisplay::ImportOf(ID3D11Texture2D*) const { return nullptr; }
bool ORGHDRDisplay::TakeUIClear() { return false; }
ORGHDRDisplay::Composed ORGHDRDisplay::Compose(const HDRDisplay::HDRDataCB&, ID3D11Texture2D*) { return Composed::None; }

#endif
