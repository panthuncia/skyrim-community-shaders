#pragma once

#include "Features/HDRDisplay.h"

#include <d3d11.h>
#include <functional>
#include <memory>

namespace org
{
	class ExternalTextureResource;
}

namespace FrameAnchors
{
	class Texture;
}

/**
* @brief HDR Display's output on the render graph, with CS_ORG_HDR_DISPLAY=1.
*
* At the UI stage's HDROutput step, which only ApplyHDR taps: the composite, the copy into the back buffer, and the UI texture's clear.
* A Bink frame's scene clear is a gated dispatch before the composite, since Bink draws into the UI texture and leaves the scene alone.
* The UI redirect, the clean composite for screenshots and frame generation, and the no-shader fallback stay on D3D11.
* The scene, output and UI textures are persistent published twins exported to D3D11, where HDR Display makes its views.
* On any failure ApplyHDR composes with D3D11. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGHDRDisplay
{
public:
	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGHDRDisplay& Get();

	/** @brief At SetupResources with the runtime active. @return False keeps HDR Display on D3D11. */
	bool Setup();

	/** @brief Set up, not failed, and the runtime still active. */
	bool IsActive() const;

	/** @brief The graph owns these when it can. */
	enum class Texture
	{
		Scene,   // ISHDR's float output
		Output,  // the composite, in the swap chain's format
		Ui,      // the interface, drawn apart
		Count
	};

	/**
	* @brief Makes a persistent, exported, published twin. Its export lives until ReleaseTextures.
	* @return A wrapper of the export's texture only, since HDR Display makes the views. Null means make a D3D11 texture.
	*/
	Texture2D* MakeTexture(Texture a_texture, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name);

	/** @brief Call after the wrappers are gone. The next SetupResources publishes new twins under the same names, costing one rebuild. */
	void ReleaseTextures();

	/** @brief Null for any other texture, which must then be imported. */
	std::shared_ptr<org::ExternalTextureResource> GraphOf(ID3D11Texture2D* a_texture) const;

	enum class Composed
	{
		None,                 // ApplyHDR composes and copies with D3D11
		Output,               // the caller copies into the back buffer when it needs one
		OutputAndBackBuffer,
	};

	/**
	* @brief Taps the HDROutput step with this frame's constants. Composed::None while not IsActive, and until the UI stage's
	* build holds a new registration.
	* @param a_backBuffer Null means no copy pass.
	* @param a_clearScene Clear the scene to black first, for a movie frame. With Composed::None the caller clears with D3D11.
	*/
	Composed Compose(const HDRDisplay::HDRDataCB& a_constants, ID3D11Texture2D* a_backBuffer, bool a_clearScene = false);

	/** @brief Commits the UI stage, so D3D11 work ApplyHDR does next follows the stage's work on the GPU. Render owner. */
	void FollowWithD3D11();

	/** @brief ClearUIBuffer then leaves the texture alone. Resets the answer. */
	bool TakeUIClear();

	/** @brief When true, a movie frame's clear waits for Compose instead of clearing with D3D11. */
	bool CanClearScene() const;

	/** @brief The program rebuilds from its file at the next Compose. */
	void ClearShaderCache();

	/**
	* @brief The import the registered passes write a_texture through, or null. Readers in other anchors must use it, because a
	* second import of the same image would be a separate graph resource with no ordering after these writes.
	*/
	FrameAnchors::Texture ImportOf(ID3D11Texture2D* a_texture) const;

	~ORGHDRDisplay();

	struct Impl;

private:
	ORGHDRDisplay() = default;

	/** @brief For good. ApplyHDR composes with D3D11 from now on. */
	void Fail(const char* a_why);

	std::unique_ptr<Impl> impl;
	bool failed = false;
	bool uiCleared = false;
};
