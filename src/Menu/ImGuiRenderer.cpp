// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Menu/ImGuiRenderer.h"

#include "Globals.h"
#include "Menu/IconLoader.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/ImGuiBasicRHI.h"
#include "RenderGraph/Switches.h"

#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "RenderGraph/Engine/UiTargets.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/KeptBindings.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <rhi_helpers.h>

#include <optional>

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
			// The client follows the interface's target (Engine::UiTargets), declared on HdrEnabled.
			RenderGraph::Frame::DeclaredClient declared;
			// What the registration the adopted revision holds draws onto. Set by the adopted call.
			bool held = false;
			bool heldHdr = false;
			FrameAnchors::Texture heldTarget;
			std::uint32_t heldFormat = 0;   // rhi::Format, as its value
			std::shared_ptr<const RasterProgram> heldProgram;
			ImDrawData* pending = nullptr;  // the frame RenderDrawData is drawing, for the commit
			// The draws' D3D11 views' bindings, kept while the same views are drawn.
			RenderGraph::KeptBindings bindings;
		};

		/** @brief What the client's registration is made for: the interface's target, a stable slot, its format and its pipeline. */
		struct TargetKey
		{
			FrameAnchors::Texture target;
			std::uint32_t format = 0;  // rhi::Format, as its value
			bool hdr = false;          // the target is HDR Display's UI texture
			std::shared_ptr<const RasterProgram> program;
			bool operator==(const TargetKey&) const = default;
		};

		State& Renderer()
		{
			static State s_state;
			return s_state;
		}

		constexpr const char* kClientId = "cs.imgui";
		/** @brief A draw's D3D11 view as a descriptor: an icon's ORG copy, else DXVK's image imported (DCLF::GpuTextures). */
		ImGuiBasicRHI::Binding Resolve(std::uint64_t a_texture)
		{
			auto* view = reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<std::uintptr_t>(a_texture));
			if (!view)
				return {};
			// An icon's ORG copy is one of the backend's exempt textures, read through its own views.
			if (auto icon = Util::IconLoader::OrgBinding(view); icon.texture)
				return icon;
			const auto* kept = Renderer().bindings.Resolve(view, DCLF::GpuTextures::Tag(DCLF::GpuTextures::Source::ImGui));
			if (!kept)
				return {};
			return { kept->index, kept->owner };
		}

		/** @brief Whether a frame draws anything or asks for a texture change. */
		bool HasWork(const ImDrawData& a_drawData)
		{
			if (a_drawData.TotalVtxCount > 0)
				return true;
			if (a_drawData.Textures)
				for (const ImTextureData* texture : *a_drawData.Textures)
					if (texture->Status != ImTextureStatus_OK)
						return true;
			return false;
		}

		/**
		 * @brief The key: the interface's target (Engine::UiTargets), HDR's UI texture while HDR is on, else kFRAMEBUFFER. Both are
		 * slots that follow the display size, so a resize changes no key. A target with no slot or format is fatal. The revision
		 * starts once its format's pipeline is built, so the key holds it.
		 */
		std::optional<TargetKey> CurrentTarget(const RenderGraph::Frame::Inputs& a_in)
		{
			const auto targets = RenderGraph::Engine::UiTargets::Current(a_in);
			TargetKey key{ targets.uiTarget, static_cast<std::uint32_t>(rhi::helpers::ToRHI(targets.uiFormat)), targets.hdr, ImGuiBasicRHI::ProgramFor(a_in) };
			if (!key.target || targets.uiFormat == DXGI_FORMAT_UNKNOWN)
				RenderGraph::Fatal("ImGui", fmt::format("the overlay's target ({}, DXGI format {}) has no graph slot", targets.hdr ? "HDR's UI texture" : "kFRAMEBUFFER",
													static_cast<int>(targets.uiFormat)));
			return key;
		}

		/**
		 * @brief Whether the frame can draw: the adopted registration was made for this frame's target. HDR switches it with a
		 * revision, so until the registration for the new target is adopted no overlay is drawn.
		 */
		bool HoldsFrameTarget(const State& a_state)
		{
			return a_state.held && a_state.heldHdr == RenderGraph::Engine::UiTargets::DrawsIntoHdrUi();
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
			client.commit = [](const FrameAnchors::Point&) {
				auto& state = Renderer();
				if (!state.pending)
					return;
				// The bound version's size: a resize is a new version of the same slot.
				const ImGuiBasicRHI::Target target{ state.heldTarget.Width(), state.heldTarget.Height(), state.heldFormat, state.heldProgram };
				if (!ImGuiBasicRHI::Commit(state.pending, target, Resolve))
					RenderGraph::Fatal("ImGui", "the overlay's frame could not be committed");
			};
			// An empty overlay frame with no texture requests has nothing to commit, which is most game frames.
			client.active = [] { return Renderer().pending && HasWork(*Renderer().pending); };
			return client;
		}

		/** @brief The registration for a_key. False is fatal (Declare.h). */
		bool MakeRegistration(const TargetKey& a_key, FrameAnchors::ClientDesc& a_out)
		{
			if (!a_key.program) {
				logger::error("[ORG] ImGui: the overlay's target (RHI format {}) has no pipeline", a_key.format);
				return false;
			}
			a_out = MakeClient(a_key);
			logger::info("[ORG] ImGui: the client is made for {} (RHI format {}); it draws from the revision that carries it",
				a_key.hdr ? "HDR's UI texture" : "kFRAMEBUFFER", a_key.format);
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
			if (!RenderGraph::Engine::GraphMode() || !RenderGraphRuntime::Get().IsActive())
				return;
			auto* renderer = globals::game::renderer;
			auto* framebufferRTV = renderer ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].RTV : nullptr;
			// The pipeline and the client are declared once for the session. Their keys are the interface's target, which only HDR
			// switches. A revision starts once its target's pipeline is built. A failed build is fatal.
			namespace RF = ::RenderGraph::Frame;
			if (framebufferRTV) {
				ImGuiBasicRHI::DeclareProgram();
				a_state.declared = RF::DeclareClient<TargetKey>(
					kClientId, RF::DependsOn(RF::Vars::HdrEnabled()), [](const RF::Inputs& a_in) { return CurrentTarget(a_in); },
					[](const TargetKey& a_key, FrameAnchors::ClientDesc& a_desc) { return MakeRegistration(a_key, a_desc); },
					[](const TargetKey& a_key) {
						auto& state = Renderer();
						state.held = true;
						state.heldHdr = a_key.hdr;
						state.heldTarget = a_key.target;
						state.heldFormat = a_key.format;
						state.heldProgram = a_key.program;
					});
			}
			if (!a_state.declared)
				RenderGraph::Fatal("ImGui", "the render graph's ImGui backend could not start");
			ShutdownDX11();
			a_state.dx11 = false;
			ImGuiBasicRHI::Init();
			a_state.org = true;
		}
	}

	void Init(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		auto& state = Renderer();
		state.device = a_device;
		state.context = a_context;
		ImGui_ImplDX11_Init(a_device, a_context);
		state.dx11 = true;
	}

	void Shutdown()
	{
		auto& state = Renderer();
		if (state.org) {
			state.declared = {};
			FrameAnchors::Unregister(kClientId);
			state.bindings.Clear();
			ImGuiBasicRHI::Shutdown();
			state.org = false;
		}
		if (state.dx11) {
			ImGui_ImplDX11_Shutdown();
			state.dx11 = false;
		}
	}

	void NewFrame()
	{
		auto& state = Renderer();
		if (RenderGraph::Engine::GraphMode() && !state.org)
			SwitchToGraph(state);
		if (state.dx11)
			ImGui_ImplDX11_NewFrame();
	}

	void RenderDrawData(ImDrawData* a_drawData)
	{
		auto& state = Renderer();
		if (state.dx11) {
			// The D3D11 backend draws into the targets it finds bound. In graph mode nothing the engine binds reaches D3D11.
			if (RenderGraph::Engine::GraphMode())
				RenderGraph::Fatal("ImGui", "the D3D11 backend drew in graph mode");
			ImGui_ImplDX11_RenderDrawData(a_drawData);
			return;
		}
		if (!state.org || !a_drawData || a_drawData->DisplaySize.x <= 0.0f || a_drawData->DisplaySize.y <= 0.0f)
			return;
		// HDR's switch is a new registration, switched in with a frame revision: until then the overlay is not drawn.
		if (!HoldsFrameTarget(state))
			return;
		state.pending = a_drawData;
		FrameAnchors::Run(FrameAnchors::Anchor::Overlay);
		state.pending = nullptr;
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
