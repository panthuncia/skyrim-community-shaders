// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/HDRDisplay/ORGHDRDisplay.h"

#include <winrt/base.h>

#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"

#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/Engine/UiTargets.h"
#include "RenderGraph/Frame/SegmentId.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "Util.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <rhi_helpers.h>

#include "RenderGraph/Frame/Declare.h"

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace
{
	namespace Vars = RenderGraph::Frame::Vars;

	constexpr std::size_t kTextureCount = static_cast<std::size_t>(ORGHDRDisplay::Texture::Count);

	/** @brief The declared twins, kept for the session: their ids are published once. A null entry is a D3D11 texture. */
	struct DeclaredTextures
	{
		std::array<RenderGraph::Frame::DeclaredTexture2D, kTextureCount> twins;
		// The D3D11 texture HDR Display wraps now. It can be the last version's for a few calls after an adoption.
		std::array<ID3D11Texture2D*, kTextureCount> wrapped{};
	};

	DeclaredTextures& Declared()
	{
		static DeclaredTextures s_declared;
		return s_declared;
	}

	/** @brief A wrapper of a twin's bound export. HDR Display makes the views. */
	Texture2D* HdrWrapTwin(const RenderGraph::Frame::DeclaredTexture2D& a_twin, const char* a_name)
	{
		auto* d3d11 = a_twin->D3D11();
		d3d11->AddRef();
		return new Texture2D(d3d11, a_name);
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

	/** @brief What the client's registration depends on: variables, and kFRAMEBUFFER's slot (stable once the target set has it). */
	struct CompositeKey
	{
		Vars::Extent2D display{};
		FrameAnchors::Texture backBuffer;  // empty before the target set took the back buffer: no copy pass
		bool operator==(const CompositeKey&) const = default;
	};

	/** @brief What one registration was made from. The commit reads the adopted one's. */
	struct Inputs
	{
		std::shared_ptr<const ComputeProgram> program;
		FrameAnchors::Texture scene, ui, output;
		FrameAnchors::Texture backBuffer;  // kFRAMEBUFFER's slot; null means no copy pass
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
			builder.Add(RenderGraph::AnchorPasses::Clear{ .name = "ui-clear", .target = a_in.ui });
		return builder;
	}

	/** @brief A declared twin's slot. In graph mode HDR Display's textures are always twins (MakeTexture), so there is no import. */
	FrameAnchors::Texture TwinSlot(const Texture2D& a_texture)
	{
		return ORGHDRDisplay::Get().GraphOf(a_texture.resource.get());
	}
}

// C4324: the aligned HDR constants pad the struct.
#pragma warning(push)
#pragma warning(disable: 4324)
struct ORGHDRDisplay::Impl
{
	// Queued at setup.
	RenderGraph::Programs::Fixed<ComputeProgram> program;
	std::shared_ptr<org::LatchBlock> latch;

	RenderGraph::Programs::Fixed<ComputeProgram> clearProgram;
	std::shared_ptr<RenderGraph::RunGate> gate;
	// Zero for none.
	std::uint32_t clearGroupsX = 0, clearGroupsY = 0;

	// Declare keeps each registration's inputs. The commit reads the adopted one's (Adopted()): null before the first, and once a
	// revision drops the client.
	RenderGraph::Frame::DeclaredClientOf<const Inputs> client;

	HDRDisplay::HDRDataCB constants{};
	// Why a frame composed nothing, each reason logged once.
	Util::ReasonLog lagReasons{ "[ORG] HDR Display" };
};
#pragma warning(pop)

ORGHDRDisplay::ORGHDRDisplay() = default;
ORGHDRDisplay::~ORGHDRDisplay() = default;

namespace
{
	/** @brief The registration for a_key and its inputs. Null is fatal (Declare.h). */
	std::shared_ptr<const Inputs> MakeComposite(ORGHDRDisplay::Impl& a_state, const CompositeKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		auto& hdr = globals::features::hdrDisplay;
		if (!hdr.hdrTexture || !hdr.hdrTexture->srv || !hdr.outputTexture || !hdr.outputTexture->uav || !hdr.outputTexture->srv || !hdr.uiTexture ||
			!hdr.uiTexture->srv || !hdr.uiTexture->uav) {
			logger::error("[ORG] HDR Display: the composite has no scene, output or UI texture with its views");
			return nullptr;
		}
		auto inputs = std::make_shared<Inputs>();
		inputs->program = a_state.program.Get();
		inputs->scene = TwinSlot(*hdr.hdrTexture);
		inputs->output = TwinSlot(*hdr.outputTexture);
		// Scaleform, Bink and the ImGui overlay draw into the same slot (Engine::UiTargets).
		inputs->ui = TwinSlot(*hdr.uiTexture);
		if (!inputs->scene || !inputs->output || !inputs->ui) {
			logger::error("[ORG] HDR Display: the {} is not a graph texture", !inputs->ui ? "UI texture" : !inputs->scene ? "scene" : "output texture");
			return nullptr;
		}
		inputs->width = a_key.display.width;
		inputs->height = a_key.display.height;
		// The composite goes into kFRAMEBUFFER's slot, which the present copies into the swap chain. Both have the swap chain's format.
		if (a_key.backBuffer) {
			if (a_key.backBuffer.Format() != rhi::helpers::ToRHI(hdr.outputTexture->desc.Format)) {
				logger::error("[ORG] HDR Display: the back buffer (RHI format {}) and the output (DXGI format {}) differ in format",
					static_cast<int>(a_key.backBuffer.Format()), static_cast<int>(hdr.outputTexture->desc.Format));
				return nullptr;
			}
			inputs->backBuffer = a_key.backBuffer;
		}
		if (a_state.clearProgram) {
			inputs->clearTarget = ORGHDRDisplay::Get().GraphOf(hdr.hdrTexture->resource.get());
			if (inputs->clearTarget)
				inputs->clearProgram = a_state.clearProgram.Get();
		}
		inputs->groupsX = (inputs->width + kOutputGroupSize - 1) / kOutputGroupSize;
		inputs->groupsY = (inputs->height + kOutputGroupSize - 1) / kOutputGroupSize;
		inputs->uiClearProgram = a_state.clearProgram.Get();
		inputs->uiGroupsX = (inputs->width + kClearGroupSize - 1) / kClearGroupSize;
		inputs->uiGroupsY = (inputs->height + kClearGroupSize - 1) / kClearGroupSize;

		auto builder = MakePasses(*inputs, a_state.latch, a_state.gate);
		if (builder.Failed()) {
			logger::error("[ORG] HDR Display: {}", builder.Failure());
			return nullptr;
		}
		a_client.sharedConstants = builder.ReadsSharedConstants();
		a_client.passes = builder.Take();
		a_client.gates = { a_state.gate };
		// The newest registration's commit runs for the one the schedule holds (it taps by id), so the commit reads the adopted inputs.
		logger::info("[ORG] HDR Display: the composite is registered for the next revision ({}x{}, back buffer copy {})", inputs->width, inputs->height,
			inputs->backBuffer ? "on the graph" : "not on the graph");
		return inputs;
	}
}

bool ORGHDRDisplay::Setup()
{
	if (!BeginSetup())
		return IsActive();
	// Hot reload swaps the pipeline in place (RenderGraphRuntime::RequestShaderReload), so the program is queued once.
	auto state = std::make_unique<Impl>();
	state->program = RenderGraph::Programs::Queue("HDRDisplay", ProgramDesc());
	state->latch = std::make_shared<org::LatchBlock>("cs.hdr.latch", static_cast<std::uint32_t>(sizeof(HDRDisplay::HDRDataCB)));
	state->gate = RenderGraph::RunGate::Create("cs.hdr.gate", kGateEntries);
	state->clearProgram = RenderGraph::Programs::Queue("HDRDisplay",
		ComputeProgram::Desc{ .source = kClearShader, .constantWords = ShaderBindings::Words<ClearBlock>(), .commonDefines = false });
	impl = std::move(state);

	using RenderGraph::Frame::DependsOn;
	impl->client = RenderGraph::Frame::DeclareClient<CompositeKey, const Inputs>(
		kClientId, DependsOn(Vars::DisplaySize()),
		[](const RenderGraph::Frame::Inputs& a_in) -> std::optional<CompositeKey> {
			auto& self = Get();
			if (!self.impl)
				return std::nullopt;
			// Registered whatever the HDR state, so an HDR switch never leaves a frame without its registration. Only an HDR frame
			// (ApplyHDR) runs the anchor, so an SDR frame commits nothing.
			// kFRAMEBUFFER's slot: the target set declares it at Engine::Setup, before the setup evaluation. Guest mode installs
			// the field handles later, so the key reads the slot, not the handle. Compose is fatal without it.
			FrameAnchors::Texture backBuffer = RenderGraph::Engine::UiTargets::Current(a_in).backBuffer;
			return CompositeKey{ a_in.Get(Vars::DisplaySize()), std::move(backBuffer) };
		},
		[](const CompositeKey& a_key, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<const Inputs> {
			auto& self = Get();
			auto inputs = self.impl ? MakeComposite(*self.impl, a_key, a_client) : nullptr;
			if (!inputs)
				return nullptr;
			a_client.commit = [](const FrameAnchors::Point&) {
				auto& owner = Get();
				if (!owner.impl)
					return;
				auto& current = *owner.impl;
				const auto held = current.client.Adopted();
				if (!held)
					return;
				current.latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, current.constants);
				// Every run closes the gate first, so only a movie frame's run clears.
				if (current.clearGroupsX)
					current.gate->OpenDispatch(kSceneClearEntry, current.clearGroupsX, current.clearGroupsY, 1);
				const auto& in = *held;
				current.gate->OpenDispatch(kOutputEntry, in.groupsX, in.groupsY, 1);
				if (in.backBuffer)
					current.gate->OpenDraw(kCopyEntry);
				if (in.uiClearProgram)
					current.gate->OpenDispatch(kUiClearEntry, in.uiGroupsX, in.uiGroupsY, 1);
			};
			return inputs;
		},
		[](const CompositeKey& a_key, const Inputs& a_inputs) {
			logger::info("[ORG] HDR Display composes on the render graph from this frame ({}x{}, back buffer copy {})", a_key.display.width,
				a_key.display.height, a_inputs.backBuffer ? "on the graph" : "not on the graph");
		});
	return true;
}

Texture2D* ORGHDRDisplay::MakeTexture(Texture a_texture, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name)
{
	if (a_texture >= Texture::Count || !On())
		return nullptr;
	auto& declared = Declared();
	const auto index = static_cast<std::size_t>(a_texture);
	// A rebuild of HDR Display's resources wraps the twin it declared before: its id is published once, for the session.
	if (const auto& kept = declared.twins[index]) {
		if (kept->Desc().Format != a_desc.Format)
			Fatal(fmt::format("{} was declared in format {} and is asked for in format {}; a declared texture keeps its format", a_name,
				static_cast<int>(kept->Desc().Format), static_cast<int>(a_desc.Format)));
		declared.wrapped[index] = kept->D3D11();
		return HdrWrapTwin(kept, a_name);
	}
	// D3D11 writes and reads it too, so exported and never aliased. HDR Display makes its own views.
	auto texture = RenderGraph::Frame::DeclareTexture2D(a_name, RenderGraph::Frame::DisplaySized(a_desc),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!texture->D3D11() || !texture->Versioned())
		Fatal(fmt::format("{} could not be declared on the render graph", a_name));
	// The composite, Bink, Scaleform and Upscaling's hudless copy declare it in their graphs, by its slot.
	if (!PublishedResources::Publish(*texture))
		Fatal(fmt::format("{} could not be published", a_name));
	// The twin keeps the image, the wrapper keeps the D3D11 texture and its views.
	auto* d3d11 = texture->D3D11();
	auto* wrapper = HdrWrapTwin(texture, a_name);
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
	// The twins stay: their ids are published for the session, and the next MakeTexture wraps them again.
	Declared().wrapped.fill(nullptr);
}

FrameAnchors::Texture ORGHDRDisplay::GraphOf(ID3D11Texture2D* a_texture) const
{
	if (!a_texture)
		return nullptr;
	// The last wrapped texture counts too: a revision's key may read the wrappers before HDR Display wraps the adopted version.
	const auto& declared = Declared();
	for (std::size_t i = 0; i < kTextureCount; ++i)
		if (const auto& twin = declared.twins[i]; twin && (twin->D3D11() == a_texture || declared.wrapped[i] == a_texture))
			return SlotOf(twin);
	return nullptr;
}

FrameAnchors::Texture ORGHDRDisplay::Slot(Texture a_texture) const
{
	if (a_texture >= Texture::Count)
		return nullptr;
	return SlotOf(Declared().twins[static_cast<std::size_t>(a_texture)]);
}

FrameAnchors::Texture ORGHDRDisplay::ImportOf(ID3D11Texture2D* a_texture) const
{
	const auto held = impl ? impl->client.Adopted() : nullptr;
	if (!held || !a_texture)
		return nullptr;
	const auto& inputs = *held;
	// The back buffer is kFRAMEBUFFER's slot, which a capture reaches as an engine target.
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

ORGHDRDisplay::Composed ORGHDRDisplay::Compose(const HDRDisplay::HDRDataCB& a_constants, ID3D11Texture2D*, bool a_clearScene)
{
	uiCleared = false;
	if (!IsActive())
		return Composed::None;

	auto& state = *impl;
	// The setup build holds the registration, adopted at setup, so every frame has one. D3D11 never composes in graph mode.
	const auto held = state.client.Adopted();
	if (!held)
		Fatal("an HDR frame has no adopted composite");
	if (!held->backBuffer)
		Fatal("the composite has no kFRAMEBUFFER slot to copy into");
	if (a_clearScene && !held->clearTarget)
		Fatal("a movie frame's scene clear has no pass");
	// A stage that runs an older schedule than the client lags: this frame composes nothing. One that never will is fatal.
	const auto place = FrameAnchors::Place(kClientId, FrameAnchors::Anchor::HDROutput);
	if (!place.draws) {
		if (!place.MayLag())
			Fatal(fmt::format("the UI stage will never hold the composite ({})", FrameAnchors::ReasonName(place.reason)));
		state.lagReasons.Report(fmt::format("this frame composes nothing (the UI stage lags: {})", FrameAnchors::ReasonName(place.reason)));
		return Composed::Nothing;
	}
	state.constants = a_constants;
	state.clearGroupsX = a_clearScene ? (held->width + kClearGroupSize - 1) / kClearGroupSize : 0;
	state.clearGroupsY = a_clearScene ? (held->height + kClearGroupSize - 1) / kClearGroupSize : 0;
	const auto ran = FrameAnchors::Run(FrameAnchors::Anchor::HDROutput);
	state.clearGroupsX = state.clearGroupsY = 0;
	if (!ran)
		Fatal(fmt::format("the HDROutput anchor did not run its scheduled composite (status {})", static_cast<int>(ran.status)));
	uiCleared = true;
	return Composed::OutputAndBackBuffer;
}

