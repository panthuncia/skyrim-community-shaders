#pragma once

#include "Features/HDRDisplay.h"
#include "RenderGraph/Port.h"

#include <d3d11.h>
#include <functional>
#include <memory>

namespace FrameAnchors
{
	class Texture;
}

/**
* @brief HDR Display's output on the render graph, in graph mode.
*
* At the UI stage's HDROutput step, which only ApplyHDR taps: the composite, the copy into kFRAMEBUFFER's slot (the back buffer the
* present copies into the swap chain), and the UI texture's clear.
* A Bink frame's scene clear is a gated dispatch before the composite, since Bink draws into the UI texture and leaves the scene alone.
* The UI redirect, the clean composite for screenshots and frame generation, and the no-shader fallback stay on D3D11.
* The scene, output and UI textures are persistent published twins exported to D3D11, where HDR Display makes its views. They are
* declared on DisplaySize, so a new display size is a new version of each, adopted with its frame revision.
* The client is declared on DisplaySize, whatever the HDR state, and registered at setup's build. The back buffer is kFRAMEBUFFER's
* slot, which the engine target set holds from Engine::Setup on. The interface draws into the UI texture by its slot
* (Engine::UiTargets). In graph mode D3D11 never composes, and any failure is fatal. Render thread throughout. Free of ORG/Vulkan
* includes.
*/
class ORGHDRDisplay : public RenderGraph::Port<ORGHDRDisplay>
{
public:
	static constexpr std::string_view kName = "HDR Display";

	/** @brief At SetupResources. @return Whether the graph path runs: false outside graph mode. In graph mode a failure is fatal. */
	bool Setup();

	/** @brief The graph owns these when it can. */
	enum class Texture
	{
		Scene,   // ISHDR's float output
		Output,  // the composite, in the swap chain's format
		Ui,      // the interface, drawn apart
		Count
	};

	/**
	* @brief Declares a persistent, exported, published twin on DisplaySize, once a session. A later call (a rebuild) wraps the same
	* twin; another format is fatal, since its id is published once.
	* @return A wrapper of the export's texture only, since HDR Display makes the views. Null outside graph mode: make a D3D11 texture.
	* In graph mode a failure is fatal.
	*/
	Texture2D* MakeTexture(Texture a_texture, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name);

	/**
	* @brief Render thread, at the frame's start. The adopted version's D3D11 texture when it is not a_current, else null.
	* The caller wraps it and makes its views again. Its reference is the caller's to take (AddRef'd).
	*/
	ID3D11Texture2D* NewExport(Texture a_texture, ID3D11Texture2D* a_current);

	/** @brief Call after the wrappers are gone. The twins stay for the session; the next SetupResources wraps them again. */
	void ReleaseTextures();

	/**
	* @brief The twin's slot, as passes declare it, for its current or its last wrapped D3D11 texture. Empty for any other
	* texture, which must then be imported.
	*/
	FrameAnchors::Texture GraphOf(ID3D11Texture2D* a_texture) const;

	/** @brief A declared twin's slot, stable for the session. Empty outside graph mode. */
	FrameAnchors::Texture Slot(Texture a_texture) const;

	enum class Composed
	{
		None,  // outside graph mode: ApplyHDR composes and copies with D3D11
		OutputAndBackBuffer,
		Nothing,  // graph mode, while the UI stage's schedule lags behind the client: nothing is composed, and no D3D11 either
	};

	/**
	* @brief Taps the HDROutput step with this frame's constants. Composed::None only while not IsActive (outside graph mode). In
	* graph mode the client is registered at setup's build, so a frame has it from the first: Composed::OutputAndBackBuffer, the
	* copy going into kFRAMEBUFFER's slot, or Composed::Nothing while the stage lags. A composite that cannot run is fatal.
	* @param a_backBuffer Not read: the back buffer is kFRAMEBUFFER's slot (Engine::UiTargets::BackBufferTexture).
	* @param a_clearScene Clear the scene to black first, for a movie frame. With Composed::None the caller clears with D3D11.
	*/
	Composed Compose(const HDRDisplay::HDRDataCB& a_constants, ID3D11Texture2D* a_backBuffer, bool a_clearScene = false);

	/** @brief Commits the UI stage, so D3D11 work ApplyHDR does next follows the stage's work on the GPU. Render owner. */
	void FollowWithD3D11();

	/** @brief ClearUIBuffer then leaves the texture alone. Resets the answer. */
	bool TakeUIClear();

	/** @brief When true, a movie frame's clear waits for Compose instead of clearing with D3D11. */
	bool CanClearScene() const;

	/**
	* @brief The import the registered passes write a_texture through, or null. Readers in other anchors must use it, because a
	* second import of the same image would be a separate graph resource with no ordering after these writes.
	*/
	FrameAnchors::Texture ImportOf(ID3D11Texture2D* a_texture) const;
	~ORGHDRDisplay();

	struct Impl;

private:
	friend class RenderGraph::Port<ORGHDRDisplay>;
	ORGHDRDisplay();

	std::unique_ptr<Impl> impl;
	bool uiCleared = false;
};
