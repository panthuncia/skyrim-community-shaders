#pragma once

#include <d3d11.h>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

/**
* @brief Exponential Height Fog's volumetric fog on the render graph, with CS_ORG_EXPONENTIAL_HEIGHT_FOG=1.
*
* Runs the D3D11 path's compute passes at the Prepass anchor, per volume, then copies into the histories.
* Dispatches go through a run gate, so a run without the fog's frame does no work. Copies and the clear have no indirect
* form and run every time, on data nobody reads then.
* The two integrated volumes are exported for the engine's shaders at t19 and t22 and published for DCLF.
* A frame whose inputs the graph cannot read runs on D3D11. A failed program or registration returns to D3D11 for good.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGExponentialHeightFog
{
public:
	/** @brief The integrated volumes' published ids. */
	static constexpr std::string_view kNearIntegratedId = "ExponentialHeightFog::IntegratedLightScattering";
	static constexpr std::string_view kFarIntegratedId = "ExponentialHeightFog::FarIntegratedLightScattering";

	static bool Requested();

	/**
	* @brief CS_ORG_FOG_DEBUG, a diagnostic of the fog's graph path. 0 when off. Any thread.
	* 1: the volumes ignore the scene depth (no conservative-depth culling).
	* 2: the engine's draws and DCLF's get no volume at t19 and t22; the graph readers still read them by published id.
	* 3: the graph path stays off, so the D3D11 path runs (guest mode only).
	* 4: the engine's draws and DCLF's get each volume version before any run wrote it, as before that was fixed. To show its effect.
	*/
	static std::uint32_t Debug();

	static ORGExponentialHeightFog& Get();

	/** @brief Reserves the Prepass anchor when requested. Call before the render graph initializes. */
	static void Reserve();

	/**
	* @brief At SetupResources. The client registers at the first Prepass, when grid sizes and inputs are known.
	* @return False keeps the fog on D3D11.
	*/
	bool Setup();

	bool IsActive() const;

	/** @brief At the feature's Prepass. @return False means the D3D11 path runs this frame. */
	bool Prepass();

	/**
	* @brief Null when the D3D11 path has them, or until a submitted run wrote the volumes' current versions.
	* A new version (first registration, resize) holds whatever its memory held, which may be NaN or huge values.
	*/
	ID3D11ShaderResourceView* NearSRV() const;
	ID3D11ShaderResourceView* FarSRV() const;

	/** @brief A new count reaches the volumes one frame boundary after the setting changes, so readers take it from here. Null when NearSRV is null. */
	std::optional<std::pair<std::uint32_t, std::uint32_t>> Slices() const;

	/** @brief For when the fog turns off. Programs stay and the next Prepass registers again. */
	void Release();

	~ORGExponentialHeightFog();

private:
	ORGExponentialHeightFog() = default;

	/** @brief Whether the readers may have the graph's volumes now. */
	bool Readable() const;

	struct Impl;
	std::unique_ptr<Impl> impl;
	bool failed = false;
};
