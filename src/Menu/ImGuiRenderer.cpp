#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Menu/ImGuiRenderer.h"

#include "Globals.h"
#include "Menu/IconLoader.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/ImGuiBasicRHI.h"

#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#if defined(CS_HAS_RENDER_GRAPH)
#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/TextureDescription.h>
#	include <rhi_helpers.h>
#endif

#include <cstring>

namespace ImGuiRenderer
{
	namespace
	{
		struct State
		{
			ID3D11Device* device = nullptr;
			ID3D11DeviceContext* context = nullptr;
			bool dx11 = false;    // imgui_impl_dx11 is installed
			bool org = false;     // ImGuiBasicRHI is installed
			bool failed = false;  // the graph's path stopped: D3D11 for good
#if defined(CS_HAS_RENDER_GRAPH)
			// What ImGui draws onto: the texture behind the bound render target and its graph import.
			winrt::com_ptr<ID3D11Texture2D> targetTexture;
			std::shared_ptr<org::ExternalTextureResource> target;
			bool ownImport = false;  // registered by this client (not an engine target the anchors register)
			ImGuiBasicRHI::Target targetInfo;
			ImDrawData* pending = nullptr;  // the frame RenderDrawData is drawing, for the commit
#endif
		};

		State& Renderer()
		{
			static State s_state;
			return s_state;
		}

#if defined(CS_HAS_RENDER_GRAPH)
		constexpr const char* kClientId = "cs.imgui";
		constexpr const char* kTargetId = "cs.imgui.target";
		// GpuTextures' diagnostic source tag for ImGui's views (above its own ranges).
		constexpr std::uint32_t kSourceTag = 256;

		/** @brief A draw's D3D11 view as a descriptor: an icon's ORG copy, else DXVK's image imported (Drawcall Limit Fix's import). */
		ImGuiBasicRHI::Binding Resolve(std::uint64_t a_texture)
		{
			auto* view = reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<std::uintptr_t>(a_texture));
			if (!view)
				return {};
			if (auto icon = Util::IconLoader::OrgBinding(view); icon.index != ImGuiBasicRHI::kNoIndex)
				return icon;
			auto binding = DCLF::GpuTextures::Get().ResolveBinding(view, kSourceTag);
			if (binding.index == DCLF::GpuTextures::kInvalid)
				return {};
			return { binding.index, std::move(binding.owner) };
		}

		/**
		 * @brief The graph texture behind the bound render target: the engine's framebuffer through the anchors' import, any other
		 * (HDR's UI texture) imported here. False when there is none or it cannot be imported; the frame is not drawn then.
		 */
		bool UpdateTarget(State& a_state)
		{
			winrt::com_ptr<ID3D11RenderTargetView> rtv;
			a_state.context->OMGetRenderTargets(1, rtv.put(), nullptr);
			if (!rtv)
				return false;
			winrt::com_ptr<ID3D11Resource> resource;
			rtv->GetResource(resource.put());
			auto texture = resource.try_as<ID3D11Texture2D>();
			if (!texture)
				return false;
			if (texture == a_state.targetTexture)
				return a_state.target != nullptr;

			D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
			rtv->GetDesc(&viewDesc);
			D3D11_TEXTURE2D_DESC textureDesc{};
			texture->GetDesc(&textureDesc);
			const auto format = rhi::helpers::ToRHI(viewDesc.Format);
			std::shared_ptr<org::ExternalTextureResource> target;
			bool own = false;
			auto* renderer = globals::game::renderer;
			if (renderer && renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].texture == texture.get()) {
				target = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER));
			} else {
				auto& runtime = RenderGraphRuntime::Get();
				DxvkOrgInteropResourceInfo info{};
				if (runtime.DescribeResource(texture.get(), info) && info.kind == DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
					org::TextureDescription desc{};
					desc.format = format;
					desc.channels = 4;
					desc.hasRTV = true;
					desc.rtvFormat = format;
					desc.imageDimensions = { { info.image.extent.width, info.image.extent.height, 0, 0 } };
					target = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), info.image, desc, kTargetId);
					own = true;
				}
			}
			a_state.targetTexture = texture;
			a_state.target = target;
			a_state.ownImport = own;
			a_state.targetInfo = { textureDesc.Width, textureDesc.Height, static_cast<std::uint32_t>(format) };
			if (!target) {
				logger::warn("[ORG] ImGui: the overlay's render target ({}x{}, DXGI format {}) could not be imported; not drawn", textureDesc.Width,
					textureDesc.Height, static_cast<int>(viewDesc.Format));
				return false;
			}
			if (!ImGuiBasicRHI::Prepare(a_state.targetInfo.format)) {
				a_state.failed = true;
				return false;
			}
			logger::info("[ORG] ImGui: drawing onto {} ({}x{}, DXGI format {})", own ? "an imported render target" : "the framebuffer", textureDesc.Width,
				textureDesc.Height, static_cast<int>(viewDesc.Format));
			// The pass declares the new target from the next build.
			FrameAnchors::Refresh(kClientId);
			return true;
		}

		FrameAnchors::ClientDesc MakeClient()
		{
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			// Last in the anchor: over anything else an overlay client draws.
			client.passes.push_back({ .name = "cs.imgui.draw",
				.anchor = FrameAnchors::Anchor::Overlay,
				.order = 1000,
				.kind = FrameAnchors::PassKind::Render,
				.make = [] { return ImGuiBasicRHI::MakePass(Renderer().target); } });
			client.registerResources = [](org::RenderGraph& a_graph) {
				auto& state = Renderer();
				if (state.ownImport && state.target)
					a_graph.RegisterResource(org::ResourceIdentifier(kTargetId), state.target);
			};
			client.commit = [](const FrameAnchors::Point&) {
				auto& state = Renderer();
				if (state.pending && !ImGuiBasicRHI::Commit(state.pending, state.targetInfo, Resolve))
					state.failed = true;
			};
			client.active = [] { return Renderer().pending != nullptr; };
			client.onFailed = [] {
				Renderer().failed = true;
				logger::error("[ORG] ImGui: the render graph failed; the D3D11 backend draws from the next frame");
			};
			return client;
		}

		/**
		 * @brief imgui_impl_dx11 shut down while ImGui keeps running. Its shutdown destroys every platform window, the main
		 * viewport's Win32 data included, so the Win32 backend is set up again on the same window.
		 */
		void ShutdownDX11()
		{
			auto* window = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
			ImGui_ImplDX11_Shutdown();
			ImGui_ImplWin32_Shutdown();
			ImGui_ImplWin32_Init(window);
		}

		/** @brief The graph's backend installed in the D3D11 backend's place, once the graph runs with the Overlay anchor. */
		void SwitchToGraph(State& a_state)
		{
			if (!RenderGraphRuntime::Get().IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Overlay))
				return;
			auto* renderer = globals::game::renderer;
			auto* framebufferRTV = renderer ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].RTV : nullptr;
			D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
			if (framebufferRTV)
				framebufferRTV->GetDesc(&viewDesc);
			// The pipeline for the framebuffer now, so a shader failure leaves the D3D11 backend in place.
			if (!framebufferRTV || !ImGuiBasicRHI::Prepare(static_cast<std::uint32_t>(rhi::helpers::ToRHI(viewDesc.Format))) ||
				!FrameAnchors::Register(MakeClient())) {
				a_state.failed = true;
				logger::error("[ORG] ImGui: the render graph's backend could not start; imgui_impl_dx11 stays");
				return;
			}
			ShutdownDX11();
			a_state.dx11 = false;
			ImGuiBasicRHI::Init();
			a_state.org = true;
		}

		/** @brief Back to the D3D11 backend, for good. */
		void FallBack(State& a_state)
		{
			FrameAnchors::Unregister(kClientId);
			ImGuiBasicRHI::Shutdown();
			a_state.org = false;
			a_state.target.reset();
			a_state.targetTexture = nullptr;
			ImGui_ImplDX11_Init(a_state.device, a_state.context);
			a_state.dx11 = true;
			logger::warn("[ORG] ImGui: imgui_impl_dx11 draws again");
		}
#endif
	}

	bool OrgRequested()
	{
		static const bool requested = [] {
			char value[8]{};
			return GetEnvironmentVariableA("CS_ORG_IMGUI", value, sizeof(value)) && std::strcmp(value, "1") == 0;
		}();
		return requested;
	}

	void Init(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		auto& state = Renderer();
		state.device = a_device;
		state.context = a_context;
		ImGui_ImplDX11_Init(a_device, a_context);
		state.dx11 = true;
	}

	void ReserveAnchor()
	{
		if (OrgRequested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Overlay)) {
			Renderer().failed = true;
			logger::warn("[ORG] ImGui: CS_ORG_IMGUI=1, but the Overlay anchor could not be reserved; imgui_impl_dx11 draws");
		}
	}

	void Shutdown()
	{
		auto& state = Renderer();
#if defined(CS_HAS_RENDER_GRAPH)
		if (state.org) {
			FrameAnchors::Unregister(kClientId);
			ImGuiBasicRHI::Shutdown();
			state.org = false;
		}
#endif
		if (state.dx11) {
			ImGui_ImplDX11_Shutdown();
			state.dx11 = false;
		}
	}

	void NewFrame()
	{
		auto& state = Renderer();
#if defined(CS_HAS_RENDER_GRAPH)
		if (state.org && state.failed)
			FallBack(state);
		else if (OrgRequested() && !state.org && !state.failed)
			SwitchToGraph(state);
#endif
		if (state.dx11)
			ImGui_ImplDX11_NewFrame();
	}

	void RenderDrawData(ImDrawData* a_drawData)
	{
		auto& state = Renderer();
		if (state.dx11) {
			ImGui_ImplDX11_RenderDrawData(a_drawData);
			return;
		}
#if defined(CS_HAS_RENDER_GRAPH)
		if (!state.org || state.failed || !a_drawData || a_drawData->DisplaySize.x <= 0.0f || a_drawData->DisplaySize.y <= 0.0f)
			return;
		if (!UpdateTarget(state))
			return;
		state.pending = a_drawData;
		FrameAnchors::Run(FrameAnchors::Anchor::Overlay);
		state.pending = nullptr;
#endif
	}

	void InvalidateDeviceObjects()
	{
		auto& state = Renderer();
		if (state.dx11)
			ImGui_ImplDX11_InvalidateDeviceObjects();
		else
			ImGuiBasicRHI::InvalidateTextures();
	}

	bool CreateDeviceObjects()
	{
		auto& state = Renderer();
		return state.dx11 ? ImGui_ImplDX11_CreateDeviceObjects() : true;
	}
}
