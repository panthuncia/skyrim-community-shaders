#pragma once

#include "RenderGraph/Port.h"
#include "Snapshot/FrameSnapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

/**
* @brief Volumetric Lighting's GPU work on the render graph, in graph mode.
*
* The engine's CPU side runs as before. Inside the VL render every DispatchCSShader is withheld, and the apply and blur are
* detoured whole. The blur's call runs the VolumetricLighting anchor, which holds all the passes, each gated except the depth copy.
* The density volume is a declared transient on the feature's size settings and the Interior variable (frame revision plan 6.12),
* and the client follows it and the inputs the VL render finds: each registration makes its own volume, aliased in its builds'
* pool. A change switches in with its revision; until then the old one draws. The feature gives the engine the adopted revision's
* quality, so the engine's volume size is always the adopted registration's (a mismatch is fatal).
* The engine subsystems install it, not the feature. Native mode installs it with the feature off too, since it has no D3D11 path,
* and then draws the engine's own volumetric lighting. Its volume follows the engine's size (Vars::VolumetricLightingSize), and
* frames whose size the adopted revision does not have yet skip the work. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGVolumetricLighting : public RenderGraph::Port<ORGVolumetricLighting>
{
public:
	static constexpr std::string_view kName = "Volumetric Lighting";

	static void Install();

	/**
	* @brief After the features' SetupResources. Declares the volume and the client. The client registers in the first revision
	* after a VL render found its inputs (the sky's noise gradient loads late). Outside graph mode it does nothing; in graph mode a
	* failure is fatal. Once set up (IsActive), the detours take the engine's work.
	*/
	void Setup();

	/** @brief True when the dispatch is the VL render's on the ORG path, and the caller skips it. a_computeShader is for the log. */
	bool TakeEngineDispatch(std::string_view a_computeShader);

	/**
	* @brief Native: the game data of the engine's VL render (SE 100306, AE 107023), captured on the game thread. Native never calls the
	* VL render: CS computes its constants from this block and CS's frame (RenderNative).
	*/
	struct Block
	{
		static constexpr Snapshot::BlockId kId = Snapshot::BlockId::VolumetricLightingRender;
		bool enabled = false;  // gVolumetricLightingEnabled: Main::Draw renders VL and RenderPostProcess applies it
		bool active = false;   // the VL render's test: the apply and the blur are on this update
		// The VL descriptor's first twelve floats (intensity, colour blend, colour, the density settings, the range factor): the
		// weather's render data, copied while the copy switch is on, else the values last copied.
		std::array<float, 12> descriptor{};
		std::array<float, 3> sunDirection{};  // the sun shadow light's direction, normalised (zero when too short)
		std::array<float, 3> sunDiffuse{};    // the sun shadow light's diffuse, which the VL colour blends from
		std::array<float, 3> noiseOffset{};   // the noise scroll, stepped once per game update
		bool cascadeFlag = false;             // the generate's row 18 x (an INI value equal to 1)
		float temporal = 0.0f;                // the weather's temporal factor (SE 100298, AE 107015)
		std::uint32_t noTemporal = 0;         // apply requests with no temporal blend so far (the apply effect's +0x9, taken here)
		bool operator==(const Block&) const = default;
	};

	/** @brief Registers the block's capture. Native only, at install. */
	static void InstallCapture();

	/** @brief Native: whether this frame's snapshot has VL on and active (RenderPostProcess's test). Render thread. */
	static bool NativeActive();

	/** @brief Native: the VL colour (SE 100300, AE 107017) from the snapshot's block. Render thread. */
	static std::array<float, 3> NativeColour();

	/** @brief Native: which VL target the apply writes this frame (0 or 1), from CS's own VL counter. Render thread. */
	static std::uint32_t NativeParity();

	/**
	* @brief Native: the VL render's CPU work, from the snapshot and CS's frame: this frame's generate constants and CS's VL counter
	* (which picks the targets). Where Main::Draw rendered VL; before RenderPostProcess's reads. Render thread.
	*/
	void RenderNative();

	/** @brief Native, in RenderPostProcess's order: the apply and the blur, which run this frame's VL on the graph. Render thread. */
	void ApplyNative();

	~ORGVolumetricLighting();

private:
	friend class RenderGraph::Port<ORGVolumetricLighting>;
	friend struct ORGVolumetricLightingDetours;
	struct Impl;

	ORGVolumetricLighting();

	std::unique_ptr<Impl> impl;
};
