#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>

/**
* @brief Subsurface Scattering on the render graph at DeferredPasses, with CS_ORG_SUBSURFACE_SCATTERING=1.
*
* Separable mode extracts diffuse, then blurs horizontally into a temporary and vertically back into the main target.
* Burley mode clears, extracts masked pixels, and blurs them back. The engine targets are imported so D3D11 readers see the result.
* The commit latches the constants and opens the gate only on frames with skin.
* The working textures are declared on the display size, the programs and the client on SSMode, so a change switches in with a
* frame revision. A setup failure keeps D3D11; a failure after setup is fatal. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGSubsurfaceScattering
{
public:
	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGSubsurfaceScattering& Get();

	/**
	* @brief At SetupResources with the runtime active. The client joins once CS's shared constants exist, and D3D11 draws until then.
	* @return False keeps the D3D11 path.
	*/
	bool Setup(const D3D11_TEXTURE2D_DESC& a_desc);

	bool IsActive() const;

	/** @brief At DrawSSS, after the anchor ran. Notes newly imported engine targets for the next revision. @return False means D3D11 draws this frame. */
	bool AfterDeferredPasses();

	/** @brief The client's commit, on the render thread: this frame's DeferredPasses ran the passes. */
	void NoteRan();

	~ORGSubsurfaceScattering();

private:
	ORGSubsurfaceScattering() = default;

	void Stop();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
