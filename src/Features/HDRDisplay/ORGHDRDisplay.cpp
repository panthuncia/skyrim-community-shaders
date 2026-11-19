#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/HDRDisplay/ORGHDRDisplay.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"

bool ORGHDRDisplay::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
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
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "Util.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <utility>

namespace
{
	/** @brief A null entry is a D3D11 texture. */
	std::array<std::unique_ptr<GraphResources::Texture2D>, static_cast<std::size_t>(ORGHDRDisplay::Texture::Count)>& Twins()
	{
		static std::array<std::unique_ptr<GraphResources::Texture2D>, static_cast<std::size_t>(ORGHDRDisplay::Texture::Count)> s_twins;
		return s_twins;
	}

	constexpr const char* kClientId = "cs.hdr";
	constexpr const char* kShader = "HDRDisplay/HDROutputCS.hlsl";
	constexpr const char* kClearShader = "HDRDisplay/ClearSceneCS.hlsl";
	// ClearSceneCS's numthreads.
	constexpr std::uint32_t kClearGroupSize = 8;
	constexpr const char* kSceneName = "hdr.scene";
	constexpr const char* kOutputName = "hdr.output";
	constexpr const char* kUiName = "hdr.ui";
	constexpr const char* kBackBufferName = "hdr.back-buffer";
	// The scene clear, composite, back buffer copy and UI clear take order keys from here up.
	constexpr std::int32_t kOrderBase = 100;
	// A movie frame's scene clear.
	constexpr std::uint32_t kSceneClearEntry = 0;

	using ShaderBindings::Handle;

	// In HDR_OUTPUT_RESOURCES order.
	struct OutputBlock
	{
		SharedConstants::Handles shared;
		Handle scene, ui, output;
		ShaderBindings::Latch perFrame;
	};
	static_assert(sizeof(OutputBlock) == 36, "matches the shader's push block (check-dual-target-shaders reports 36 bytes)");
	static_assert(sizeof(HDRDisplay::HDRDataCB) == 48, "the latch region holds the constants as the D3D11 cbuffer does");

	struct ClearBlock
	{
		Handle scene;
	};

	/** @brief A change registers the client again. */
	struct Inputs
	{
		std::shared_ptr<const ComputeProgram> program;
		FrameAnchors::Texture scene, ui, output;
		FrameAnchors::Texture backBuffer;  // null means no copy pass
		// Null means no scene clear pass, and PrepareMovieDraw clears on D3D11.
		std::shared_ptr<const ComputeProgram> clearProgram;
		FrameAnchors::Texture clearTarget;
		std::uint32_t groupsX = 0, groupsY = 0;
		std::uint32_t width = 0, height = 0;  // the copy's extent

		bool operator==(const Inputs&) const = default;
	};

	ComputeProgram::Desc ProgramDesc()
	{
		// The program adds the D3D11 build's defines itself.
		return { .source = kShader, .constantWords = ShaderBindings::Words<OutputBlock>() };
	}

	/**
	* @brief The gated movie scene clear, the composite, the back buffer copy, then the UI clear after the composite read it.
	* Every pass but the gated clear runs at every run of the anchor.
	*/
	RenderGraph::AnchorPasses::Builder MakePasses(const Inputs& a_in, std::shared_ptr<const org::LatchBlock> a_latch, std::shared_ptr<const RenderGraph::RunGate> a_gate)
	{
		using RenderGraph::AnchorPasses::Bind;
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::HDROutput, kClientId, kOrderBase, std::move(a_latch), std::move(a_gate));
		if (a_in.clearTarget)
			builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = "scene-clear",
				.program = a_in.clearProgram,
				.bindings = { Bind::Uav(a_in.clearTarget) },
				.gate = kSceneClearEntry });
		builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = "output",
			.program = a_in.program,
			.bindings = { Bind::Shared(), Bind::Srv(a_in.scene), Bind::Srv(a_in.ui), Bind::Uav(a_in.output), Bind::Latch(0) },
			.groups = { a_in.groupsX, a_in.groupsY, 1 } });
		if (a_in.backBuffer)
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "back-buffer", .source = a_in.output, .target = a_in.backBuffer, .width = a_in.width, .height = a_in.height });
		builder.Add(RenderGraph::AnchorPasses::Clear{ .name = "ui-clear", .target = a_in.ui }, false);
		return builder;
	}

	/** @brief Only one back buffer is allowed. With several, DXVK rotates their images at present on its own thread, which a single import cannot follow. */
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

	// Null program means it did not build and PrepareMovieDraw clears on D3D11.
	std::shared_ptr<const ComputeProgram> clearProgram;
	std::shared_ptr<RenderGraph::RunGate> gate;
	// Zero for none.
	std::uint32_t clearGroupsX = 0, clearGroupsY = 0;

	winrt::com_ptr<ID3D11Texture2D> backBufferTexture;
	FrameAnchors::Texture backBuffer;

	// Null before the first registration.
	std::shared_ptr<const Inputs> registered;

	HDRDisplay::HDRDataCB constants{};
};
#	pragma warning(pop)

ORGHDRDisplay::~ORGHDRDisplay() = default;

void ORGHDRDisplay::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::HDROutput))
		logger::warn("[ORG] HDR Display stays on D3D11: the HDROutput anchor could not be reserved");
}

bool ORGHDRDisplay::Setup()
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
	state->latch = std::make_shared<org::LatchBlock>("cs.hdr.latch", static_cast<std::uint32_t>(sizeof(HDRDisplay::HDRDataCB)));
	state->gate = RenderGraph::RunGate::Create("cs.hdr.gate");
	if (!state->gate) {
		Fail("its run gate could not be made");
		return false;
	}
	// Optional. Without it a movie frame's scene clear stays on D3D11.
	state->clearProgram = ComputeProgram::Load(host->Device(), { .source = kClearShader, .constantWords = ShaderBindings::Words<ClearBlock>(), .commonDefines = false });
	if (!state->clearProgram)
		logger::warn("[ORG] HDR Display: {} could not be built; a movie frame's scene clear stays on D3D11", kClearShader);
	impl = std::move(state);
	return true;
}

void ORGHDRDisplay::Fail(const char* a_why)
{
	if (!failed)
		logger::error("[ORG] HDR Display returns to D3D11: {}", a_why);
	failed = true;
	FrameAnchors::Unregister(kClientId);
	impl.reset();
}

void ORGHDRDisplay::ClearShaderCache()
{
	if (impl)
		impl->programDirty = true;
}

Texture2D* ORGHDRDisplay::MakeTexture(Texture a_texture, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name)
{
	if (a_texture >= Texture::Count || !Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& twin = Twins()[static_cast<std::size_t>(a_texture)];
	if (twin)
		return nullptr;
	// D3D11 writes and reads it too, so exported and never aliased. HDR Display makes its own views.
	auto texture = std::make_unique<GraphResources::Texture2D>(a_desc, std::string(a_name),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!*texture || !texture->D3D11() || !std::dynamic_pointer_cast<org::ExternalTextureResource>(texture->Graph())) {
		logger::warn("[ORG] HDR Display: {} could not be made on the render graph; it is a D3D11 texture", a_name);
		return nullptr;
	}
	// The composite, Bink, Scaleform and Upscaling's hudless copy declare it in their graphs.
	if (!PublishedResources::Publish(*texture)) {
		logger::warn("[ORG] HDR Display: {} could not be published; it is a D3D11 texture", a_name);
		return nullptr;
	}
	// The twin keeps the image, the wrapper keeps the D3D11 texture and its views.
	auto* d3d11 = texture->D3D11();
	d3d11->AddRef();
	auto* wrapper = new Texture2D(d3d11, a_name);
	logger::info("[ORG] HDR Display: {} is a graph texture ({}x{}, DXGI format {})", a_name, a_desc.Width, a_desc.Height, static_cast<int>(a_desc.Format));
	twin = std::move(texture);
	return wrapper;
}

void ORGHDRDisplay::ReleaseTextures()
{
	for (auto& twin : Twins())
		twin.reset();
}

std::shared_ptr<org::ExternalTextureResource> ORGHDRDisplay::GraphOf(ID3D11Texture2D* a_texture) const
{
	if (!a_texture || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	for (const auto& twin : Twins())
		if (twin && twin->D3D11() == a_texture)
			return std::dynamic_pointer_cast<org::ExternalTextureResource>(twin->Graph());
	return nullptr;
}

FrameAnchors::Texture ORGHDRDisplay::ImportOf(ID3D11Texture2D* a_texture) const
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

bool ORGHDRDisplay::CanClearScene() const
{
	const auto& hdr = globals::features::hdrDisplay;
	return IsActive() && impl->clearProgram && hdr.hdrTexture && GraphOf(hdr.hdrTexture->resource.get());
}

ORGHDRDisplay::Composed ORGHDRDisplay::Compose(const HDRDisplay::HDRDataCB& a_constants, ID3D11Texture2D* a_backBuffer, bool a_clearScene)
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
		// A failed reload keeps the old program, as the D3D11 shader cache does.
		if (auto program = ComputeProgram::Load(RenderGraphRuntime::Get().Host()->Device(), ProgramDesc()))
			state.program = std::move(program);
		else
			logger::warn("[ORG] HDR Display: HDROutputCS failed to build again; the graph keeps the last program");
	}

	// HDR's float texture, else the framebuffer, as ApplyHDR picks it.
	const bool hdrScene = hdr.IsHDREnabledForFrame() && hdr.hdrTexture && hdr.hdrTexture->srv;
	FrameAnchors::Texture scene = !hdrScene ? FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER)) : FrameAnchors::Texture(GraphOf(hdr.hdrTexture->resource.get()));
	if (hdrScene && !scene)
		scene = FrameAnchors::D3D11Texture(kSceneName, hdr.hdrTexture->resource.get(), hdr.hdrTexture->srv.get(), nullptr);
	FrameAnchors::Texture output = GraphOf(hdr.outputTexture->resource.get());
	if (!output)
		output = FrameAnchors::D3D11Texture(kOutputName, hdr.outputTexture->resource.get(), hdr.outputTexture->srv.get(), hdr.outputTexture->uav.get());
	// Bink's graph path draws into it through the same import, and a name's first import decides its views, so include the RTV.
	FrameAnchors::Texture ui = GraphOf(hdr.uiTexture->resource.get());
	if (!ui)
		ui = FrameAnchors::D3D11Texture(kUiName, hdr.uiTexture->resource.get(), hdr.uiTexture->srv.get(), hdr.uiTexture->uav.get(), hdr.uiTexture->rtv.get());
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
	if (state.clearProgram && hdr.hdrTexture) {
		inputs->clearTarget = GraphOf(hdr.hdrTexture->resource.get());
		if (inputs->clearTarget)
			inputs->clearProgram = state.clearProgram;
	}
	// A clear asked for with no pass to make it, so the caller clears and composes with D3D11.
	if (a_clearScene && !inputs->clearTarget)
		return Composed::None;
	const auto groups = Util::GetScreenDispatchCount(false);
	inputs->groupsX = groups.x;
	inputs->groupsY = groups.y;
	inputs->width = outputDesc.Width;
	inputs->height = outputDesc.Height;

	if (!state.registered || !(*state.registered == *inputs)) {
		auto builder = MakePasses(*inputs, state.latch, state.gate);
		if (builder.Failed()) {
			Fail(builder.Failure().c_str());
			return Composed::None;
		}
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.sharedConstants = builder.ReadsSharedConstants();
		client.passes = builder.Take();
		client.gates = { state.gate };
		client.commit = [](const FrameAnchors::Point&) {
			auto& self = Get();
			if (!self.impl)
				return;
			self.impl->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, self.impl->constants);
			// Every run closes the gate first, so only a movie frame's run clears.
			if (self.impl->clearGroupsX)
				self.impl->gate->OpenDispatch(kSceneClearEntry, self.impl->clearGroupsX, self.impl->clearGroupsY, 1);
		};
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
	const D3D11_TEXTURE2D_DESC& sceneDesc = hdr.hdrTexture ? hdr.hdrTexture->desc : outputDesc;
	state.clearGroupsX = a_clearScene ? (sceneDesc.Width + kClearGroupSize - 1) / kClearGroupSize : 0;
	state.clearGroupsY = a_clearScene ? (sceneDesc.Height + kClearGroupSize - 1) / kClearGroupSize : 0;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::HDROutput);
	state.clearGroupsX = state.clearGroupsY = 0;
	if (!ran)
		return Composed::None;
	uiCleared = true;
	return state.registered->backBuffer ? Composed::OutputAndBackBuffer : Composed::Output;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph HDR Display always composes with D3D11.
struct ORGHDRDisplay::Impl
{};

ORGHDRDisplay::~ORGHDRDisplay() = default;
void ORGHDRDisplay::Reserve() {}
bool ORGHDRDisplay::Setup() { return false; }
void ORGHDRDisplay::Fail(const char*) {}
void ORGHDRDisplay::ClearShaderCache() {}
FrameAnchors::Texture ORGHDRDisplay::ImportOf(ID3D11Texture2D*) const { return nullptr; }
Texture2D* ORGHDRDisplay::MakeTexture(Texture, const D3D11_TEXTURE2D_DESC&, const char*) { return nullptr; }
void ORGHDRDisplay::ReleaseTextures() {}
std::shared_ptr<org::ExternalTextureResource> ORGHDRDisplay::GraphOf(ID3D11Texture2D*) const { return nullptr; }
bool ORGHDRDisplay::TakeUIClear() { return false; }
bool ORGHDRDisplay::CanClearScene() const { return false; }
ORGHDRDisplay::Composed ORGHDRDisplay::Compose(const HDRDisplay::HDRDataCB&, ID3D11Texture2D*, bool) { return Composed::None; }

#endif
