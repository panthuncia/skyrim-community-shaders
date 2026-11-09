#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/HDRDisplay/ORGHDRDisplay.h"

#include <cstring>

namespace
{
	constexpr const char* kEnvSwitch = "CS_ORG_HDR_DISPLAY";
}

bool ORGHDRDisplay::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA(kEnvSwitch, value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

ORGHDRDisplay& ORGHDRDisplay::Get()
{
	static ORGHDRDisplay s_hdrDisplay;
	return s_hdrDisplay;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "Util.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/TextureDescription.h>
#	include <rhi_helpers.h>

#	include <string_view>
#	include <utility>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.hdr";
	constexpr const char* kShader = "HDRDisplay/HDROutputCS.hlsl";
	constexpr const char* kUiId = "cs.hdr.ui";
	constexpr const char* kSceneName = "hdr.scene";
	constexpr const char* kOutputName = "hdr.output";
	constexpr const char* kBackBufferName = "hdr.back-buffer";

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
		std::uint32_t width = 0, height = 0;  // the output's size: the copy's and the clear's extent
		rhi::Format uiFormat = rhi::Format::Unknown;

		bool operator==(const Inputs&) const = default;
	};

	/** @brief What one registration's passes share: immutable once registered. */
	struct Shared
	{
		Inputs inputs;
		std::shared_ptr<const org::LatchBlock> latch;
	};

	/** @brief The defines Util::CompileShader gives the D3D11 build, so both paths build one permutation. */
	std::vector<std::pair<std::wstring, std::wstring>> ProgramDefines()
	{
		std::vector<std::pair<std::string, std::string>> defines = Util::GetCommonShaderDefines();
		for (const char* name : { "COMPUTESHADER", "WINPC", "DX11" })
			defines.emplace_back(name, "");
		// Defines are ASCII.
		std::vector<std::pair<std::wstring, std::wstring>> wide;
		for (const auto& [name, value] : defines)
			wide.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
		return wide;
	}

	struct OutputBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken scene, ui, output;
	};

	struct OutputFrame
	{
		std::shared_ptr<const Shared> shared;
	};

	/** @brief HDROutputCS: the scene and the UI into the output texture, as DispatchHDROutput. */
	class OutputPass final : public org::TypedRenderGraphPass<OutputPass, OutputFrame, OutputBindings>
	{
	public:
		explicit OutputPass(std::shared_ptr<const Shared> a_shared) :
			shared(std::move(a_shared)) {}

		OutputBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& in = shared->inputs;
			return { SharedConstants::Declare(a_builder), a_builder.ShaderResource(in.scene).View(), a_builder.ShaderResource(in.ui).View(),
				a_builder.UnorderedAccess(in.output).View() };
		}

		// Everything is fixed for the registration; the constants come from the latch and the shared constants each run.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(shared.get())); }

		OutputFrame Prepare(const OutputBindings&, const org::PassPrepareContext&) const { return { shared }; }

		static void Record(const OutputBindings& a_bindings, const OutputFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& in = a_frame.shared->inputs;
			if (!in.program || !a_frame.shared->latch)
				return;
			OutputBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.scene = a_recording.Index(a_bindings.scene);
			block.ui = a_recording.Index(a_bindings.ui);
			block.output = a_recording.Index(a_bindings.output);
			block.perFrame = ShaderBindings::LatchOf(*a_frame.shared->latch, a_recording.FrameSlot());
			in.program->Dispatch(a_recording, ShaderBindings::Data(block), in.groupsX, in.groupsY, 1);
		}

	private:
		std::shared_ptr<const Shared> shared;
	};

	struct CopyBindings
	{
		org::ResourceBindingToken from, to;
	};

	struct CopyFrame
	{
		std::uint32_t width = 0, height = 0;
	};

	/** @brief The output into the back buffer, as ApplyHDR's CopyResource. */
	class BackBufferCopyPass final : public org::TypedRenderGraphPass<BackBufferCopyPass, CopyFrame, CopyBindings>
	{
	public:
		explicit BackBufferCopyPass(std::shared_ptr<const Shared> a_shared) :
			shared(std::move(a_shared)) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopySource(shared->inputs.output), a_builder.CopyDestination(shared->inputs.backBuffer) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(shared.get())); }

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { shared->inputs.width, shared->inputs.height }; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.width || !a_frame.height)
				return;
			const rhi::TextureCopyRegion target{ .texture = a_recording.Resolve(a_bindings.to).GetHandle(), .width = a_frame.width, .height = a_frame.height };
			const rhi::TextureCopyRegion source{ .texture = a_recording.Resolve(a_bindings.from).GetHandle(), .width = a_frame.width, .height = a_frame.height };
			a_recording.Commands().CopyTextureRegion(target, source);
		}

	private:
		std::shared_ptr<const Shared> shared;
	};

	struct ClearBindings
	{
		org::DeclaredViewToken target;
	};

	struct ClearFrame
	{
		std::uint32_t width = 0, height = 0;
		rhi::Format format = rhi::Format::Unknown;
	};

	/** @brief The UI texture cleared to transparent black after the composite read it, as ClearUIBuffer's ClearRenderTargetView. */
	class UIClearPass final : public org::TypedRenderGraphPass<UIClearPass, ClearFrame, ClearBindings>
	{
	public:
		explicit UIClearPass(std::shared_ptr<const Shared> a_shared) :
			shared(std::move(a_shared)) {}

		ClearBindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(shared->inputs.ui).View() }; }

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(shared.get())); }

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const
		{
			return { shared->inputs.width, shared->inputs.height, shared->inputs.uiFormat };
		}

		static void Record(const ClearBindings& a_bindings, const ClearFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.width || !a_frame.height)
				return;
			rhi::ColorAttachment colour{};
			colour.rtv = a_recording.View(a_bindings.target);
			colour.resource = a_recording.Resolve(a_bindings.target).GetHandle();
			colour.loadOp = rhi::LoadOp::Clear;
			colour.storeOp = rhi::StoreOp::Store;
			colour.clear.type = rhi::ClearValueType::Color;
			colour.clear.format = a_frame.format;
			for (float& component : colour.clear.rgba)
				component = 0.0f;
			rhi::PassBeginInfo begin{};
			begin.colors = { &colour, 1 };
			begin.width = a_frame.width;
			begin.height = a_frame.height;
			begin.debugName = "HDR UI clear";
			auto& commands = a_recording.Commands();
			commands.BeginPass(begin);
			commands.EndPass();
		}

	private:
		std::shared_ptr<const Shared> shared;
	};

	/** @brief The UI texture as a graph texture with its SRV and RTV (FrameAnchors::D3D11Texture imports no RTV). Null when DXVK cannot describe it. */
	std::shared_ptr<org::ExternalTextureResource> ImportUI(const Texture2D& a_ui)
	{
		auto& runtime = RenderGraphRuntime::Get();
		DxvkOrgInteropResourceInfo info{};
		if (!runtime.Host() || !runtime.DescribeResource(a_ui.resource.get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return nullptr;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		a_ui.srv->GetDesc(&srvDesc);
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		a_ui.rtv->GetDesc(&rtvDesc);
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(srvDesc.Format);
		desc.channels = 4;
		desc.hasSRV = true;
		desc.srvFormat = desc.format;
		desc.hasRTV = true;
		desc.rtvFormat = rhi::helpers::ToRHI(rtvDesc.Format);
		desc.imageDimensions = { { info.image.extent.width, info.image.extent.height, 0, 0 } };
		return RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), info.image, desc, kUiId);
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

	// The UI texture's own import, kept while HDR's texture stays the same.
	winrt::com_ptr<ID3D11Texture2D> uiTexture;
	std::shared_ptr<org::ExternalTextureResource> ui;

	// The last back buffer asked about, and whether the graph copies into it (CanCopyInto and the import).
	winrt::com_ptr<ID3D11Texture2D> backBufferTexture;
	std::shared_ptr<org::ExternalTextureResource> backBuffer;

	std::shared_ptr<const Shared> registered;

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

bool ORGHDRDisplay::TakeUIClear()
{
	return std::exchange(uiCleared, false);
}

ORGHDRDisplay::Composed ORGHDRDisplay::Compose(const HDRDisplay::HDRDataCB& a_constants, ID3D11Texture2D* a_backBuffer)
{
	uiCleared = false;
	if (!Requested() || failed)
		return Composed::None;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::HDROutput))
		return Composed::None;

	auto& hdr = globals::features::hdrDisplay;
	if (!hdr.outputTexture || !hdr.outputTexture->uav || !hdr.outputTexture->srv || !hdr.uiTexture || !hdr.uiTexture->srv || !hdr.uiTexture->rtv)
		return Composed::None;

	if (!impl) {
		impl = std::make_unique<Impl>();
		impl->latch = std::make_shared<org::LatchBlock>("cs.hdr.latch", static_cast<std::uint32_t>(sizeof(HDRDisplay::HDRDataCB)), host->FrameSlots());
	}
	auto& state = *impl;

	if (!state.program || state.programDirty) {
		auto program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<OutputBlock>(), .defines = ProgramDefines() });
		state.programDirty = false;
		if (!program) {
			// A reload that fails keeps the program it had, as the D3D11 shader cache does.
			if (!state.program) {
				Fail("HDROutputCS could not be built for the graph");
				return Composed::None;
			}
			logger::warn("[ORG] HDR Display: HDROutputCS failed to build again; the graph keeps the last program");
		} else {
			state.program = std::move(program);
		}
	}

	// The scene as ApplyHDR picks it: HDR's float texture, else the framebuffer.
	const bool hdrScene = hdr.IsHDREnabledForFrame() && hdr.hdrTexture && hdr.hdrTexture->srv;
	auto scene = hdrScene ? FrameAnchors::D3D11Texture(kSceneName, hdr.hdrTexture->resource.get(), hdr.hdrTexture->srv.get(), nullptr) :
	                        FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER));
	auto output = FrameAnchors::D3D11Texture(kOutputName, hdr.outputTexture->resource.get(), hdr.outputTexture->srv.get(), hdr.outputTexture->uav.get());
	if (hdr.uiTexture->resource.get() != state.uiTexture.get()) {
		state.uiTexture.copy_from(hdr.uiTexture->resource.get());
		state.ui = ImportUI(*hdr.uiTexture);
	}
	if (!scene || !output || !state.ui) {
		Fail(!state.ui ? "the UI texture cannot be imported" : !scene ? "the scene cannot be imported" :
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

	Inputs inputs{};
	inputs.program = state.program;
	inputs.scene = std::move(scene);
	inputs.ui = state.ui;
	inputs.output = std::move(output);
	inputs.backBuffer = state.backBuffer;
	const auto groups = Util::GetScreenDispatchCount(false);
	inputs.groupsX = groups.x;
	inputs.groupsY = groups.y;
	inputs.width = outputDesc.Width;
	inputs.height = outputDesc.Height;
	D3D11_RENDER_TARGET_VIEW_DESC uiView{};
	hdr.uiTexture->rtv->GetDesc(&uiView);
	inputs.uiFormat = rhi::helpers::ToRHI(uiView.Format);

	if (!state.registered || !(state.registered->inputs == inputs)) {
		auto shared = std::make_shared<Shared>();
		shared->inputs = std::move(inputs);
		shared->latch = state.latch;
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.passes.push_back({ .name = "cs.hdr.output",
			.anchor = FrameAnchors::Anchor::HDROutput,
			.order = 100,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [shared]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<OutputPass>(shared); } });
		if (shared->inputs.backBuffer)
			client.passes.push_back({ .name = "cs.hdr.back-buffer",
				.anchor = FrameAnchors::Anchor::HDROutput,
				.order = 101,
				.kind = FrameAnchors::PassKind::Copy,
				.make = [shared]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<BackBufferCopyPass>(shared); } });
		client.passes.push_back({ .name = "cs.hdr.ui-clear",
			.anchor = FrameAnchors::Anchor::HDROutput,
			.order = 102,
			.kind = FrameAnchors::PassKind::Render,
			.make = [shared]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<UIClearPass>(shared); } });
		client.registerResources = [shared](org::RenderGraph& a_graph) { a_graph.RegisterResource(org::ResourceIdentifier(kUiId), shared->inputs.ui); };
		client.commit = [](const FrameAnchors::Point&) {
			auto& self = Get();
			if (self.impl)
				self.impl->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, self.impl->constants);
		};
		// Run unregisters the clients before this; Compose stops the path once Run returns.
		client.onFailed = [] { Get().graphFailed = true; };
		client.sharedConstants = true;
		if (!FrameAnchors::Register(std::move(client))) {
			Fail("its client could not be registered on the HDROutput anchor");
			return Composed::None;
		}
		const bool first = !state.registered;
		state.registered = std::move(shared);
		logger::info("[ORG] HDR Display {} on the render graph ({}x{}, scene from {}, back buffer copy {})", first ? "runs" : "registered again", outputDesc.Width,
			outputDesc.Height, hdrScene ? "the HDR texture" : "the framebuffer", state.registered->inputs.backBuffer ? "on the graph" : "not on the graph");
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
	return state.registered->inputs.backBuffer ? Composed::OutputAndBackBuffer : Composed::Output;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: HDR Display always composes with D3D11.
struct ORGHDRDisplay::Impl
{};

ORGHDRDisplay::~ORGHDRDisplay() = default;
void ORGHDRDisplay::Reserve() {}
void ORGHDRDisplay::Fail(const char*) {}
void ORGHDRDisplay::ClearShaderCache() {}
bool ORGHDRDisplay::TakeUIClear() { return false; }
ORGHDRDisplay::Composed ORGHDRDisplay::Compose(const HDRDisplay::HDRDataCB&, ID3D11Texture2D*) { return Composed::None; }

#endif
