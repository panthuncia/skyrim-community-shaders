#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Menu/ImGuiRenderer.h"

#include "Globals.h"
#include "Menu/IconLoader.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/ImGuiBasicRHI.h"
#include "RenderGraph/Switches.h"

#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#if defined(CS_HAS_RENDER_GRAPH)
#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "Features/HDRDisplay.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/TextureDescription.h>
#	include <rhi_helpers.h>

#	include <optional>
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
			// The texture behind the render target bound at the last draw. Not a variable yet, so a new one invalidates the client,
			// whose key imports it when the next revision starts. The draw itself never imports.
			winrt::com_ptr<ID3D11Texture2D> observed;
			DXGI_FORMAT observedFormat = DXGI_FORMAT_UNKNOWN;  // the bound view's format at that draw
			// The client follows the observed target (Frame/Declare.h).
			RenderGraph::Frame::DeclaredClient declared;
			// The texture of the registration the adopted revision holds, and what its commit draws for. Set by the adopted call.
			const ID3D11Texture2D* heldTexture = nullptr;
			ImGuiBasicRHI::Target targetInfo;
			ImDrawData* pending = nullptr;  // the frame RenderDrawData is drawing, for the commit
#endif
		};

#if defined(CS_HAS_RENDER_GRAPH)
		/** @brief What the client's registration is made for: the observed target, imported by the key. */
		struct TargetKey
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			FrameAnchors::Texture target;
			bool ownImport = false;  // not the framebuffer: imported for this client (the anchors register it)
			ImGuiBasicRHI::Target info;
			bool operator==(const TargetKey& a_other) const
			{
				return texture == a_other.texture && target == a_other.target && ownImport == a_other.ownImport && info.width == a_other.info.width &&
				       info.height == a_other.info.height && info.format == a_other.info.format;
			}
		};
#endif

		State& Renderer()
		{
			static State s_state;
			return s_state;
		}

#if defined(CS_HAS_RENDER_GRAPH)
		constexpr const char* kClientId = "cs.imgui";
		constexpr const char* kTargetId = "cs.imgui.target";
		// GpuTextures' diagnostic source tag for ImGui's views (above its own ranges).
		constexpr std::uint32_t kSourceTag = DCLF::GpuTextures::Tag(DCLF::GpuTextures::Source::ImGui);

		/** @brief A draw's D3D11 view as a descriptor: an icon's ORG copy, else DXVK's image imported (Drawcall Limit Fix's import). */
		ImGuiBasicRHI::Binding Resolve(std::uint64_t a_texture)
		{
			auto* view = reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<std::uintptr_t>(a_texture));
			if (!view)
				return {};
			// An icon's ORG copy is one of the backend's exempt textures, read through its own views.
			if (auto icon = Util::IconLoader::OrgBinding(view); icon.texture)
				return icon;
			auto binding = DCLF::GpuTextures::Get().ResolveBinding(view, kSourceTag);
			if (binding.index == DCLF::GpuTextures::kInvalid)
				return {};
			return { binding.index, std::move(binding.owner) };
		}

		/** @brief The texture behind the bound render target, or null. a_rtv receives the bound view. */
		winrt::com_ptr<ID3D11Texture2D> BoundTarget(State& a_state, winrt::com_ptr<ID3D11RenderTargetView>* a_rtv)
		{
			winrt::com_ptr<ID3D11RenderTargetView> rtv;
			a_state.context->OMGetRenderTargets(1, rtv.put(), nullptr);
			if (!rtv)
				return nullptr;
			winrt::com_ptr<ID3D11Resource> resource;
			rtv->GetResource(resource.put());
			*a_rtv = rtv;
			return resource.try_as<ID3D11Texture2D>();
		}

		/**
		 * @brief The key: the observed target, imported now, when a revision starts. The engine's framebuffer through the anchors'
		 * import, any other (HDR's UI texture) imported here. No client before a draw observed one. A target that cannot be
		 * imported sets failed, so the next NewFrame falls back to imgui_impl_dx11.
		 */
		std::optional<TargetKey> CurrentTarget(State& a_state)
		{
			if (!a_state.observed || a_state.failed)
				return std::nullopt;
			const auto& texture = a_state.observed;
			D3D11_TEXTURE2D_DESC textureDesc{};
			texture->GetDesc(&textureDesc);
			const auto format = rhi::helpers::ToRHI(a_state.observedFormat);
			TargetKey key;
			key.texture = texture;
			auto* renderer = globals::game::renderer;
			if (renderer && renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].texture == texture.get()) {
				key.target = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER));
			} else {
				// Through the anchors' imports, so the image is one graph resource whoever else imports it: HDR's UI texture by the name and
				// views Bink and Scaleform import it with, any other as a render target in the view's format.
				const auto& hdr = globals::features::hdrDisplay;
				if (hdr.uiTexture && hdr.uiTexture->resource && hdr.uiTexture->resource.get() == texture.get()) {
					const auto& ui = *hdr.uiTexture;
					key.target = FrameAnchors::D3D11Texture("hdr.ui", ui.resource.get(), ui.srv.get(), ui.uav.get(), ui.rtv.get());
				} else {
					org::TextureDescription desc{};
					desc.format = format;
					desc.channels = 4;
					desc.hasRTV = true;
					desc.rtvFormat = format;
					key.target = FrameAnchors::ImportTexture(kTargetId, texture.get(), desc);
				}
				key.ownImport = true;
			}
			key.info = { textureDesc.Width, textureDesc.Height, static_cast<std::uint32_t>(format) };
			if (!key.target) {
				// The menu must not stay invisible: the next NewFrame falls back to imgui_impl_dx11.
				logger::warn("[ORG] ImGui: the overlay's render target ({}x{}, DXGI format {}) could not be imported; imgui_impl_dx11 draws from the next frame",
					textureDesc.Width, textureDesc.Height, static_cast<int>(a_state.observedFormat));
				a_state.failed = true;
				return std::nullopt;
			}
			return key;
		}

		/**
		 * @brief Whether the frame can draw onto the bound target: the adopted registration was made for it. A new target is
		 * observed here, and the next revision imports it and registers the client for it. Until then no overlay is drawn.
		 */
		bool UpdateTarget(State& a_state)
		{
			winrt::com_ptr<ID3D11RenderTargetView> rtv;
			auto texture = BoundTarget(a_state, &rtv);
			if (!texture)
				return false;
			if (texture != a_state.observed) {
				D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
				rtv->GetDesc(&viewDesc);
				a_state.observed = std::move(texture);
				a_state.observedFormat = viewDesc.Format;
				a_state.declared.Invalidate();
				return false;
			}
			return a_state.heldTexture == texture.get();
		}

		FrameAnchors::ClientDesc MakeClient(const TargetKey& a_key)
		{
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			// The texture copies, then the draws. Last in the anchor: over anything else an overlay client draws.
			client.passes.push_back({ .name = "cs.imgui.upload",
				.anchor = FrameAnchors::Anchor::Overlay,
				.order = 999,
				.kind = FrameAnchors::PassKind::Copy,
				.make = [] { return ImGuiBasicRHI::MakeUploadPass(); },
				// It reads the run's slot, which the commit stores: recorded then, without a ticket prepared again.
				.atExecution = true });
			client.passes.push_back({ .name = "cs.imgui.draw",
				.anchor = FrameAnchors::Anchor::Overlay,
				.order = 1000,
				.kind = FrameAnchors::PassKind::Render,
				.make = [target = a_key.target] { return ImGuiBasicRHI::MakePass(target); },
				.atExecution = true });
			client.registerResources = [own = a_key.ownImport, plain = a_key.target.Plain()](org::RenderGraph& a_graph) {
				// The anchors register their imports; only a plain resource is this client's to register.
				if (own && plain)
					a_graph.RegisterResource(org::ResourceIdentifier(kTargetId), plain);
			};
			client.commit = [](const FrameAnchors::Point&) {
				auto& state = Renderer();
				if (state.pending && !ImGuiBasicRHI::Commit(state.pending, state.targetInfo, Resolve))
					state.failed = true;
			};
			client.active = [] { return Renderer().pending != nullptr; };
			return client;
		}

		/** @brief The registration for a_key. Builds the target format's pipeline first. False is fatal (Declare.h). */
		bool MakeRegistration(const TargetKey& a_key, FrameAnchors::ClientDesc& a_out)
		{
			if (!ImGuiBasicRHI::Prepare(a_key.info.format)) {
				logger::error("[ORG] ImGui: the pipeline for the overlay's target (RHI format {}) could not be built", a_key.info.format);
				return false;
			}
			a_out = MakeClient(a_key);
			logger::info("[ORG] ImGui: the client is made for {} ({}x{}, RHI format {}); it draws from the revision that carries it",
				a_key.ownImport ? "an imported render target" : "the framebuffer", a_key.info.width, a_key.info.height, a_key.info.format);
			return true;
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
			// One declaration for the session. Its key is the target the draw observed, imported when a revision starts.
			namespace RF = ::RenderGraph::Frame;
			if (framebufferRTV && ImGuiBasicRHI::Prepare(static_cast<std::uint32_t>(rhi::helpers::ToRHI(viewDesc.Format))))
				a_state.declared = RF::DeclareClient<TargetKey>(
					kClientId, RF::DependsOn(RF::Vars::DisplaySize()), [](const RF::Inputs&) { return CurrentTarget(Renderer()); },
					[](const TargetKey& a_key, FrameAnchors::ClientDesc& a_desc) { return MakeRegistration(a_key, a_desc); },
					[](const TargetKey& a_key) {
						auto& state = Renderer();
						state.heldTexture = a_key.texture.get();
						state.targetInfo = a_key.info;
					});
			if (!a_state.declared) {
				a_state.failed = true;
				// Native mode has no D3D11 to fall back to.
				if (RenderGraph::Engine::Native::Requested())
					RenderGraph::Engine::Native::Fail("the menu's ImGui", "the render graph's ImGui backend could not start, and imgui_impl_dx11 draws through D3D11");
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
			if (RenderGraph::Engine::Native::Requested())
				RenderGraph::Engine::Native::Fail("the menu's ImGui", "the render graph's ImGui backend failed, and imgui_impl_dx11 draws through D3D11");
			a_state.declared = {};
			FrameAnchors::Unregister(kClientId);
			ImGuiBasicRHI::Shutdown();
			a_state.org = false;
			a_state.observed = nullptr;
			a_state.observedFormat = DXGI_FORMAT_UNKNOWN;
			a_state.heldTexture = nullptr;
			ImGui_ImplDX11_Init(a_state.device, a_state.context);
			a_state.dx11 = true;
			logger::warn("[ORG] ImGui: imgui_impl_dx11 draws again");
		}
#endif
	}

	bool OrgRequested()
	{
		return RenderGraph::Engine::Native::GraphMode();
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
			state.declared = {};
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
		// A new target is a new registration, switched in with a frame revision: until then the overlay is not drawn.
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
