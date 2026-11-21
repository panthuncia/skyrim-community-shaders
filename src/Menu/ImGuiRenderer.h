#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ImDrawData;

/**
 * @brief Which renderer backend draws CS's ImGui frames: imgui_impl_dx11, or, in graph mode (RenderGraph::Engine::GraphMode), the render graph's
 * (RenderGraph/ImGuiBasicRHI.h) at the Overlay frame anchor.
 *
 * The D3D11 backend draws the first frames on either path: the render graph starts after the menu (State::Setup). Once it runs
 * with the Overlay anchor's epoch, the next frame swaps the backends; ImGui creates its textures again for the new one. On the
 * render graph's path ImGui draws onto the render target bound when the overlay renders (the framebuffer, or HDR's UI texture),
 * imported into the graph. Textures that are D3D11 views (icons, cursors, previews, feature debug views) reach the graph as
 * bindless descriptors: icons and cursors through their ORG copies (IconLoader), any other view through an import of DXVK's
 * image (DCLF::GpuTextures). In graph mode a failure of the graph's path is fatal.
 *
 * Render thread, all of it.
 */
namespace ImGuiRenderer
{
	/** @brief Menu::Init: the D3D11 backend installed. */
	void Init(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	/** @brief Menu's destructor: whichever backend is installed shut down. */
	void Shutdown();

	/** @brief Before ImGui::NewFrame: the backend switch when due, then the D3D11 backend's NewFrame when it draws. */
	void NewFrame();

	/** @brief After ImGui::Render: the frame drawn by the installed backend. */
	void RenderDrawData(ImDrawData* a_drawData);

	/** @brief ThemeManager's font reload: the installed backend's textures destroyed (ImGui creates them again). */
	void InvalidateDeviceObjects();

	/** @brief ThemeManager's font reload: the D3D11 backend's device objects made again; nothing to make on the graph's path. */
	bool CreateDeviceObjects();
}
