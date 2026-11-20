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
#	include "RenderGraph/Frame/SegmentId.h"
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

#	include "RenderGraph/Frame/Declare.h"

#	include <array>
#	include <optional>
#	include <utility>
#	include <vector>

namespace
{
	namespace Vars = RenderGraph::Frame::Vars;

	constexpr std::size_t kTextureCount = static_cast<std::size_t>(ORGHDRDisplay::Texture::Count);

	/** @brief The declared twins. A null entry is a D3D11 texture. */
	struct DeclaredTextures
	{
		std::array<RenderGraph::Frame::DeclaredTexture2D, kTextureCount> twins;
		// The D3D11 texture HDR Display wraps now. It can be the last version's for a few calls after an adoption.
		std::array<ID3D11Texture2D*, kTextureCount> wrapped{};
		Vars::Extent2D setup{};  // the size the textures were made at, until a display size is written
	};

	DeclaredTextures& Declared()
	{
		static DeclaredTextures s_declared;
		return s_declared;
	}

	/** @brief The display size, or the setup size before one was written. */
	Vars::Extent2D DisplayOr(Vars::Extent2D a_display, Vars::Extent2D a_setup)
	{
		return a_display.width && a_display.height ? a_display : a_setup;
	}

	/** @brief A declared twin as passes declare it: its slot. */
	FrameAnchors::Texture SlotOf(const RenderGraph::Frame::DeclaredTexture2D& a_texture)
	{
		return a_texture && a_texture->Versioned() ? FrameAnchors::Texture(a_texture->Name(), a_texture->Versioned()) : FrameAnchors::Texture();
	}

	constexpr const char* kClientId = "cs.hdr";
	constexpr const char* kShader = "HDRDisplay/HDROutputCS.hlsl";
	constexpr const char* kClearShader = "HDRDisplay/ClearSceneCS.hlsl";
	// ClearSceneCS's and HDROutputCS's numthreads.
	constexpr std::uint32_t kClearGroupSize = 8;
	constexpr std::uint32_t kOutputGroupSize = 8;
	constexpr const char* kSceneName = "hdr.scene";
	constexpr const char* kOutputName = "hdr.output";
	constexpr const char* kUiName = "hdr.ui";
	constexpr const char* kBackBufferName = "hdr.back-buffer";
	// The scene clear, composite, back buffer copy and UI clear take order keys from here up.
	constexpr std::int32_t kOrderBase = 100;
	// The run gate's entries. Every pass is gated: the UI stage runs them on every frame, also when ApplyHDR did not tap.
	constexpr std::uint32_t kSceneClearEntry = 0;  // a movie frame's scene clear
	constexpr std::uint32_t kOutputEntry = 1;
	constexpr std::uint32_t kCopyEntry = 2;
	constexpr std::uint32_t kUiClearEntry = 3;
	constexpr std::uint32_t kGateEntries = 4;

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

	/** @brief What the client's registration depends on: variables, and the back buffer Compose saw last. */
	struct CompositeKey
	{
		Vars::Extent2D display{};
		std::uint64_t backBuffer = 0;  // Impl::backBufferSerial
		bool operator==(const CompositeKey&) const = default;
	};

	/** @brief What one registration was made from. The commit reads the adopted one's. */
	struct Inputs
	{
		std::shared_ptr<const ComputeProgram> program;
		FrameAnchors::Texture scene, ui, output;
		FrameAnchors::Texture backBuffer;                    // null means no copy pass
		const ID3D11Texture2D* backBufferTexture = nullptr;  // the back buffer it was made for, compared only
		// Null means no scene clear pass, and PrepareMovieDraw clears on D3D11.
		std::shared_ptr<const ComputeProgram> clearProgram;
		FrameAnchors::Texture clearTarget;
		// ClearSceneCS clears any float4 UAV, so it clears the UI texture too. Null means an ungated clear.
		std::shared_ptr<const ComputeProgram> uiClearProgram;
		std::uint32_t uiGroupsX = 0, uiGroupsY = 0;
		std::uint32_t groupsX = 0, groupsY = 0;
		std::uint32_t width = 0, height = 0;  // the display size: the textures' and the copy's extent
	};

	ComputeProgram::Desc ProgramDesc()
	{
		// The program adds the D3D11 build's defines itself.
		return { .source = kShader, .constantWords = ShaderBindings::Words<OutputBlock>() };
	}

	/**
	* @brief The movie scene clear, the composite, the back buffer copy, then the UI clear after the composite read it.
	* Each pass runs only on runs whose commit opened its gate entry. Without the clear program the UI clear is ungated.
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
			.gate = kOutputEntry });
		if (a_in.backBuffer)
			builder.Add(RenderGraph::AnchorPasses::Copy{
				.name = "back-buffer", .source = a_in.output, .target = a_in.backBuffer, .width = a_in.width, .height = a_in.height, .gate = kCopyEntry });
		if (a_in.uiClearProgram)
			builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = "ui-clear", .program = a_in.uiClearProgram, .bindings = { Bind::Uav(a_in.ui) }, .gate = kUiClearEntry });
		else
			builder.Add(RenderGraph::AnchorPasses::Clear{ .name = "ui-clear", .target = a_in.ui }, false);
		return builder;
	}

	/**
	* @brief Whether the back buffer has the output's shape. Only one back buffer is allowed. With several, DXVK rotates their
	* images at present on its own thread, which a single import cannot follow.
	*/
	bool CanCopyInto(ID3D11Texture2D* a_backBuffer, std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format)
	{
		D3D11_TEXTURE2D_DESC desc{};
		a_backBuffer->GetDesc(&desc);
		if (desc.Format != a_format || desc.Width != a_width || desc.Height != a_height)
			return false;
		DXGI_SWAP_CHAIN_DESC swapChain{};
		if (!globals::d3d::swapChain || FAILED(globals::d3d::swapChain->GetDesc(&swapChain)))
			return false;
		const bool sequential = swapChain.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL || swapChain.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		return !sequential || swapChain.BufferCount <= 1;
	}

	/** @brief A graph twin's slot, else an import of the D3D11 texture. */
	FrameAnchors::Texture SlotOrImport(const Texture2D& a_texture, const char* a_name, bool a_rtv)
	{
		if (auto slot = ORGHDRDisplay::Get().GraphOf(a_texture.resource.get()))
			return slot;
		return FrameAnchors::D3D11Texture(a_name, a_texture.resource.get(), a_texture.srv.get(), a_texture.uav.get(), a_rtv ? a_texture.rtv.get() : nullptr);
	}
}

// C4324: the aligned HDR constants pad the struct.
#	pragma warning(push)
#	pragma warning(disable: 4324)
struct ORGHDRDisplay::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	std::shared_ptr<org::LatchBlock> latch;

	// Null program means it did not build and PrepareMovieDraw clears on D3D11.
	std::shared_ptr<const ComputeProgram> clearProgram;
	std::shared_ptr<RenderGraph::RunGate> gate;
	// Zero for none.
	std::uint32_t clearGroupsX = 0, clearGroupsY = 0;

	// The back buffer Compose saw last. A new one bumps the serial and invalidates the client.
	winrt::com_ptr<ID3D11Texture2D> backBufferTexture;
	std::uint64_t backBufferSerial = 0;

	RenderGraph::Frame::DeclaredClient client;
	// Registrations made and not yet adopted, oldest first. The adopted call takes its own and drops older ones.
	std::vector<std::pair<CompositeKey, std::shared_ptr<const Inputs>>> made;
	// The registration the adopted revision holds. Null before the first, and once a revision drops the client.
	std::shared_ptr<const Inputs> held;

	HDRDisplay::HDRDataCB constants{};
};
#	pragma warning(pop)

ORGHDRDisplay::~ORGHDRDisplay() = default;

void ORGHDRDisplay::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::HDROutput))
		logger::warn("[ORG] HDR Display stays on D3D11: the HDROutput anchor could not be reserved");
}

namespace
{
	/** @brief The registration for a_key. False is fatal (Declare.h). */
	bool MakeComposite(ORGHDRDisplay::Impl& a_state, const CompositeKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		auto& hdr = globals::features::hdrDisplay;
		if (!hdr.hdrTexture || !hdr.hdrTexture->srv || !hdr.outputTexture || !hdr.outputTexture->uav || !hdr.outputTexture->srv || !hdr.uiTexture ||
			!hdr.uiTexture->srv || !hdr.uiTexture->uav) {
			logger::error("[ORG] HDR Display: the composite has no scene, output or UI texture with its views");
			return false;
		}
		auto inputs = std::make_shared<Inputs>();
		inputs->program = a_state.program;
		inputs->scene = SlotOrImport(*hdr.hdrTexture, kSceneName, false);
		inputs->output = SlotOrImport(*hdr.outputTexture, kOutputName, false);
		// Bink's graph path draws into it through the same import, and a name's first import decides its views, so include the RTV.
		inputs->ui = SlotOrImport(*hdr.uiTexture, kUiName, true);
		if (!inputs->scene || !inputs->output || !inputs->ui) {
			logger::error("[ORG] HDR Display: the {} cannot be imported", !inputs->ui ? "UI texture" : !inputs->scene ? "scene" : "output texture");
			return false;
		}
		inputs->width = a_key.display.width;
		inputs->height = a_key.display.height;
		auto* backBuffer = a_state.backBufferTexture.get();
		inputs->backBufferTexture = backBuffer;
		if (backBuffer && CanCopyInto(backBuffer, inputs->width, inputs->height, hdr.outputTexture->desc.Format))
			inputs->backBuffer = FrameAnchors::D3D11Texture(kBackBufferName, backBuffer, nullptr, nullptr);
		if (a_state.clearProgram) {
			inputs->clearTarget = ORGHDRDisplay::Get().GraphOf(hdr.hdrTexture->resource.get());
			if (inputs->clearTarget)
				inputs->clearProgram = a_state.clearProgram;
		}
		inputs->groupsX = (inputs->width + kOutputGroupSize - 1) / kOutputGroupSize;
		inputs->groupsY = (inputs->height + kOutputGroupSize - 1) / kOutputGroupSize;
		inputs->uiClearProgram = a_state.clearProgram;
		inputs->uiGroupsX = (inputs->width + kClearGroupSize - 1) / kClearGroupSize;
		inputs->uiGroupsY = (inputs->height + kClearGroupSize - 1) / kClearGroupSize;

		auto builder = MakePasses(*inputs, a_state.latch, a_state.gate);
		if (builder.Failed()) {
			logger::error("[ORG] HDR Display: {}", builder.Failure());
			return false;
		}
		a_client.sharedConstants = builder.ReadsSharedConstants();
		a_client.passes = builder.Take();
		a_client.gates = { a_state.gate };
		// The newest registration's commit runs for the one the schedule holds, so the commit reads the adopted registration's inputs.
		a_client.matchesById = true;
		logger::info("[ORG] HDR Display: the composite is registered for the next revision ({}x{}, back buffer copy {})", inputs->width, inputs->height,
			inputs->backBuffer ? "on the graph" : "not on the graph");
		a_state.made.emplace_back(a_key, std::move(inputs));
		return true;
	}
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
	// Hot reload swaps the pipeline in place (RenderGraphRuntime::RequestShaderReload), so the program is loaded once.
	auto program = ComputeProgram::Load(host->Device(), ProgramDesc());
	if (!program) {
		Fail("HDROutputCS could not be built for the graph");
		return false;
	}
	auto state = std::make_unique<Impl>();
	state->program = std::move(program);
	state->latch = std::make_shared<org::LatchBlock>("cs.hdr.latch", static_cast<std::uint32_t>(sizeof(HDRDisplay::HDRDataCB)));
	state->gate = RenderGraph::RunGate::Create("cs.hdr.gate", kGateEntries);
	if (!state->gate) {
		Fail("its run gate could not be made");
		return false;
	}
	// Optional. Without it a movie frame's scene clear stays on D3D11.
	state->clearProgram = ComputeProgram::Load(host->Device(), { .source = kClearShader, .constantWords = ShaderBindings::Words<ClearBlock>(), .commonDefines = false });
	if (!state->clearProgram)
		logger::warn("[ORG] HDR Display: {} could not be built; a movie frame's scene clear stays on D3D11", kClearShader);
	// The swap chain's back buffer, so the first registration can copy into it. Compose follows a new one.
	if (globals::d3d::swapChain)
		globals::d3d::swapChain->GetBuffer(0, IID_PPV_ARGS(state->backBufferTexture.put()));
	impl = std::move(state);

	using RenderGraph::Frame::DependsOn;
	impl->client = RenderGraph::Frame::DeclareClient<CompositeKey>(
		kClientId, DependsOn(Vars::HdrEnabled(), Vars::DisplaySize()),
		[](const RenderGraph::Frame::Inputs& a_in) -> std::optional<CompositeKey> {
			auto& self = Get();
			if (!self.impl)
				return std::nullopt;
			// The composite runs only for an HDR frame. Without a client, Compose has no registration to tap.
			if (!a_in.Get(Vars::HdrEnabled())) {
				self.impl->held.reset();
				return std::nullopt;
			}
			return CompositeKey{ DisplayOr(a_in.Get(Vars::DisplaySize()), Declared().setup), self.impl->backBufferSerial };
		},
		[](const CompositeKey& a_key, FrameAnchors::ClientDesc& a_client) {
			auto& self = Get();
			if (!self.impl || !MakeComposite(*self.impl, a_key, a_client))
				return false;
			a_client.commit = [](const FrameAnchors::Point&) {
				auto& owner = Get();
				if (!owner.impl || !owner.impl->held)
					return;
				auto& current = *owner.impl;
				current.latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, current.constants);
				// Every run closes the gate first, so only a movie frame's run clears.
				if (current.clearGroupsX)
					current.gate->OpenDispatch(kSceneClearEntry, current.clearGroupsX, current.clearGroupsY, 1);
				const auto& in = *current.held;
				current.gate->OpenDispatch(kOutputEntry, in.groupsX, in.groupsY, 1);
				if (in.backBuffer)
					current.gate->OpenDraw(kCopyEntry);
				if (in.uiClearProgram)
					current.gate->OpenDispatch(kUiClearEntry, in.uiGroupsX, in.uiGroupsY, 1);
			};
			return true;
		},
		[](const CompositeKey& a_key) {
			auto& self = Get();
			if (!self.impl)
				return;
			auto& made = self.impl->made;
			for (std::size_t i = made.size(); i-- > 0;)
				if (made[i].first == a_key) {
					self.impl->held = made[i].second;
					made.erase(made.begin(), made.begin() + static_cast<std::ptrdiff_t>(i) + 1);
					logger::info("[ORG] HDR Display composes on the render graph from this frame ({}x{}, back buffer copy {})", a_key.display.width,
						a_key.display.height, self.impl->held->backBuffer ? "on the graph" : "not on the graph");
					return;
				}
			stl::report_and_fail("[ORG] HDR Display: a revision adopted a composite registration that HDR Display did not make");
		});
	if (!impl->client) {
		Fail("its client could not be declared");
		return false;
	}
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

Texture2D* ORGHDRDisplay::MakeTexture(Texture a_texture, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name)
{
	if (a_texture >= Texture::Count || !Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& declared = Declared();
	const auto index = static_cast<std::size_t>(a_texture);
	if (declared.twins[index])
		return nullptr;
	declared.setup = { a_desc.Width, a_desc.Height };
	using RenderGraph::Frame::DependsOn;
	// D3D11 writes and reads it too, so exported and never aliased. HDR Display makes its own views.
	auto texture = RenderGraph::Frame::DeclareTexture2D(
		a_name, DependsOn(Vars::DisplaySize()),
		[a_desc](const RenderGraph::Frame::Inputs& a_in) {
			const auto display = DisplayOr(a_in.Get(Vars::DisplaySize()), { a_desc.Width, a_desc.Height });
			auto desc = a_desc;
			desc.Width = display.width;
			desc.Height = display.height;
			return desc;
		},
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!texture || !texture->D3D11() || !texture->Versioned()) {
		logger::warn("[ORG] HDR Display: {} could not be declared on the render graph; it is a D3D11 texture", a_name);
		return nullptr;
	}
	// The composite, Bink, Scaleform and Upscaling's hudless copy declare it in their graphs, by its slot.
	if (!PublishedResources::Publish(*texture)) {
		logger::warn("[ORG] HDR Display: {} could not be published; it is a D3D11 texture", a_name);
		return nullptr;
	}
	// The twin keeps the image, the wrapper keeps the D3D11 texture and its views.
	auto* d3d11 = texture->D3D11();
	d3d11->AddRef();
	auto* wrapper = new Texture2D(d3d11, a_name);
	const auto& desc = texture->Desc();
	logger::info("[ORG] HDR Display: {} is a graph texture ({}x{}, DXGI format {}), declared on the display size", a_name, desc.Width, desc.Height,
		static_cast<int>(desc.Format));
	declared.twins[index] = std::move(texture);
	declared.wrapped[index] = d3d11;
	return wrapper;
}

ID3D11Texture2D* ORGHDRDisplay::NewExport(Texture a_texture, ID3D11Texture2D* a_current)
{
	if (a_texture >= Texture::Count)
		return nullptr;
	auto& declared = Declared();
	const auto index = static_cast<std::size_t>(a_texture);
	const auto& twin = declared.twins[index];
	// The export changes only when a revision adopts a new version.
	if (!twin || !twin->D3D11() || twin->D3D11() == a_current)
		return nullptr;
	auto* d3d11 = twin->D3D11();
	d3d11->AddRef();
	declared.wrapped[index] = d3d11;
	logger::info("[ORG] HDR Display: {} follows its adopted version ({}x{})", twin->Name(), twin->Desc().Width, twin->Desc().Height);
	return d3d11;
}

void ORGHDRDisplay::ReleaseTextures()
{
	auto& declared = Declared();
	for (auto& twin : declared.twins)
		twin = {};
	declared.wrapped.fill(nullptr);
}

FrameAnchors::Texture ORGHDRDisplay::GraphOf(ID3D11Texture2D* a_texture) const
{
	if (!a_texture || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	// The last wrapped texture counts too: a revision's key may read the wrappers before HDR Display wraps the adopted version.
	const auto& declared = Declared();
	for (std::size_t i = 0; i < kTextureCount; ++i)
		if (const auto& twin = declared.twins[i]; twin && (twin->D3D11() == a_texture || declared.wrapped[i] == a_texture))
			return SlotOf(twin);
	return nullptr;
}

FrameAnchors::Texture ORGHDRDisplay::ImportOf(ID3D11Texture2D* a_texture) const
{
	if (!impl || !impl->held || !a_texture)
		return nullptr;
	const auto& inputs = *impl->held;
	if (inputs.backBuffer && a_texture == inputs.backBufferTexture)
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

void ORGHDRDisplay::FollowWithD3D11()
{
	// The stage's end point, as the present would tap it. A capture after it uses D3D11.
	(void)FrameAnchors::TapPoint(RenderGraph::Frame::SegmentId::Ui);
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

	auto& state = *impl;
	// The back buffer is not a variable yet. A new one registers the client again when the next revision starts.
	if (a_backBuffer != state.backBufferTexture.get()) {
		state.backBufferTexture.copy_from(a_backBuffer);
		++state.backBufferSerial;
		state.client.Invalidate();
		logger::info("[ORG] HDR Display: a new back buffer; the composite follows it with the next revision");
	}
	// Until a revision adopts a registration made for this back buffer, ApplyHDR composes with D3D11.
	if (!state.held || state.held->backBufferTexture != a_backBuffer)
		return Composed::None;
	// A clear asked for with no pass to make it, so the caller clears and composes with D3D11.
	if (a_clearScene && !state.held->clearTarget)
		return Composed::None;
	state.constants = a_constants;
	state.clearGroupsX = a_clearScene ? (state.held->width + kClearGroupSize - 1) / kClearGroupSize : 0;
	state.clearGroupsY = a_clearScene ? (state.held->height + kClearGroupSize - 1) / kClearGroupSize : 0;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::HDROutput);
	state.clearGroupsX = state.clearGroupsY = 0;
	if (!ran)
		return Composed::None;
	uiCleared = true;
	return state.held->backBuffer ? Composed::OutputAndBackBuffer : Composed::Output;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph HDR Display always composes with D3D11.
struct ORGHDRDisplay::Impl
{};

ORGHDRDisplay::~ORGHDRDisplay() = default;
void ORGHDRDisplay::Reserve() {}
bool ORGHDRDisplay::Setup() { return false; }
void ORGHDRDisplay::Fail(const char*) {}
FrameAnchors::Texture ORGHDRDisplay::ImportOf(ID3D11Texture2D*) const { return nullptr; }
Texture2D* ORGHDRDisplay::MakeTexture(Texture, const D3D11_TEXTURE2D_DESC&, const char*) { return nullptr; }
void ORGHDRDisplay::ReleaseTextures() {}
ID3D11Texture2D* ORGHDRDisplay::NewExport(Texture, ID3D11Texture2D*) { return nullptr; }
FrameAnchors::Texture ORGHDRDisplay::GraphOf(ID3D11Texture2D*) const { return nullptr; }
void ORGHDRDisplay::FollowWithD3D11() {}
bool ORGHDRDisplay::TakeUIClear() { return false; }
bool ORGHDRDisplay::CanClearScene() const { return false; }
ORGHDRDisplay::Composed ORGHDRDisplay::Compose(const HDRDisplay::HDRDataCB&, ID3D11Texture2D*, bool) { return Composed::None; }

#endif
