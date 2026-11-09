#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ImDrawData;

/**
 * @brief Which renderer backend draws CS's ImGui frames: imgui_impl_dx11, or, with CS_ORG_IMGUI=1, the render graph's
 * (RenderGraph/ImGuiBasicRHI.h) at the Overlay frame anchor.
 *
 * The D3D11 backend draws the first frames on either path: the render graph starts after the menu (State::Setup). Once it runs
 * with the Overlay anchor's epoch, the next frame swaps the backends; ImGui creates its textures again for the new one. On the
 * render graph's path ImGui draws onto the render target bound when the overlay renders (the framebuffer, or HDR's UI texture),
 * imported into the graph. Textures that are D3D11 views (icons, cursors, previews, feature debug views) reach the graph as
 * bindless descriptors: icons and cursors through their ORG copies (IconLoader), any other view through Drawcall Limit Fix's
 * import of DXVK's image (GpuTextures). If the graph fails, or a frame cannot be drawn, the D3D11 backend takes over again for good.
 *
 * Render thread, all of it.
 */
namespace ImGuiRenderer
{
	/** @brief CS_ORG_IMGUI=1: the render graph draws ImGui once it can (default 0: imgui_impl_dx11 only, as before). */
	bool OrgRequested();

	/** @brief Menu::Init: the D3D11 backend installed, and the Overlay anchor reserved when OrgRequested. */
	void Init(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	/** @brief Menu's destructor: whichever backend is installed shut down. */
	void Shutdown();

	/** @brief Before ImGui::NewFrame: the backend switch (or fallback) when due, then the D3D11 backend's NewFrame when it draws. */
	void NewFrame();

	/** @brief After ImGui::Render: the frame drawn by the installed backend. */
	void RenderDrawData(ImDrawData* a_drawData);

	/** @brief ThemeManager's font reload: the installed backend's textures destroyed (ImGui creates them again). */
	void InvalidateDeviceObjects();

	/** @brief ThemeManager's font reload: the D3D11 backend's device objects made again; nothing to make on the graph's path. */
	bool CreateDeviceObjects();
}
