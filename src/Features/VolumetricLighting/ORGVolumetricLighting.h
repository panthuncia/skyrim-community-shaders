#pragma once

#include <memory>
#include <string_view>

/**
* @brief Volumetric Lighting's GPU work on the render graph, with CS_ORG_VOLUMETRIC_LIGHTING=1.
*
* The engine's CPU side runs as before. Inside the VL render every DispatchCSShader is withheld, and the apply and blur are
* detoured whole. The blur's call runs the VolumetricLighting anchor, which holds all the passes, each gated except the depth copy.
* The density volume is declared on the feature's size settings and the Interior variable (frame revision plan 6.12), and the
* client on the inputs the VL render finds. A change switches in with its revision; until then the old one draws. The feature
* gives the engine the adopted revision's quality, so the engine's volume size is always the bound version's (a mismatch is fatal).
* The engine subsystems install it, not the feature. Native mode installs it with the feature off too, since it has no D3D11 path,
* and then draws the engine's own volumetric lighting. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGVolumetricLighting
{
public:
	static bool Requested();

	static ORGVolumetricLighting& Get();

	/** @brief Before the render graph initializes. */
	static void Reserve();

	static void Install();

	/**
	* @brief After the features' SetupResources. Declares the volume and the client. The client registers in the first revision
	* after a VL render found its inputs (the sky's noise gradient loads late).
	* @return False keeps the D3D11 path.
	*/
	bool Setup();

	/** @brief The detours then take the engine's work. */
	bool IsActive() const;

	/** @brief True when the dispatch is the VL render's on the ORG path, and the caller skips it. a_computeShader is for the log. */
	bool TakeEngineDispatch(std::string_view a_computeShader);

	~ORGVolumetricLighting();

private:
	friend struct ORGVolumetricLightingDetours;
	struct Impl;

	ORGVolumetricLighting() = default;

	std::unique_ptr<Impl> impl;
};
