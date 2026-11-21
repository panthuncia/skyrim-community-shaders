#pragma once

#include "RenderGraph/Port.h"

#include <d3d11.h>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

/**
* @brief Exponential Height Fog's volumetric fog on the render graph, in graph mode.
*
* Runs the D3D11 path's compute passes at the Prepass anchor, per volume, then copies into the histories.
* Dispatches go through a run gate, so a run without the fog's frame does no work. Copies and the clear have no indirect
* form and run every time, on data nobody reads then.
* The two integrated volumes are exported for the engine's shaders at t19 and t22 and published for DCLF.
* The volumes and the client are declared on their settings and the display size, so a change switches in with a frame revision.
* Graph mode has no D3D11 fog: a setup failure or an input the graph should have and does not is fatal. An input that is not
* written yet is left out of the client, and its flag is off.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGExponentialHeightFog : public RenderGraph::Port<ORGExponentialHeightFog>
{
public:
	static constexpr std::string_view kName = "Exponential Height Fog";

	/** @brief The integrated volumes' published ids. */
	static constexpr std::string_view kNearIntegratedId = "ExponentialHeightFog::IntegratedLightScattering";
	static constexpr std::string_view kFarIntegratedId = "ExponentialHeightFog::FarIntegratedLightScattering";

	/**
	* @brief CS_ORG_FOG_DEBUG, a diagnostic of the fog's graph path. 0 when off. Any thread.
	* 1: the volumes ignore the scene depth (no conservative-depth culling).
	* 2: the engine's draws and DCLF's get no volume at t19 and t22; the graph readers still read them by published id.
	* 3: the graph path stays off, so the D3D11 path runs (guest mode only).
	* 4: the engine's draws and DCLF's get each volume version before any run wrote it, as before that was fixed. To show its effect.
	* 5: the draws that still run on D3D11 get no volume (the slots Prepass binds). Every DCLF colour draw still does.
	* 6: every DCLF draw gets no volume. The draws that still run on D3D11 still do.
	* 7: the far volume is cleared to no fog every run, so every reader sees only the near volume.
	* 8: no history: the volumes take no temporal history.
	* 9: the histories are cleared every run, and the scattering's history guard is off.
	*/
	static std::uint32_t Debug();

	/**
	* @brief At SetupResources, once a session. Declares the volumes and the client on their settings (Frame/Declare.h). The client
	* registers when a revision starts with the fog on, with the inputs the last Prepass saw (before the first, the engine's depth
	* alone). Outside graph mode it does nothing; in graph mode a failure is fatal.
	*/
	void Setup();

	/** @brief At the feature's Prepass. @return False means no volumetric fog this frame (off, or no Prepass for the graph). */
	bool Prepass();

	/**
	* @brief Null when the D3D11 path has them, or until a submitted run wrote the volumes' current versions.
	* A new version (first registration, resize) holds whatever its memory held, which may be NaN or huge values.
	*/
	ID3D11ShaderResourceView* NearSRV() const;
	ID3D11ShaderResourceView* FarSRV() const;

	/** @brief The adopted volumes' slice counts, which readers use instead of the settings. Null when NearSRV is null. */
	std::optional<std::pair<std::uint32_t, std::uint32_t>> Slices() const;

	/** @brief Whether the adopted revision has the volumes on. They stay declared while off, at one texel. Render thread. */
	bool VolumesOn() const;
	~ORGExponentialHeightFog();

private:
	friend class RenderGraph::Port<ORGExponentialHeightFog>;
	ORGExponentialHeightFog();

	/** @brief Whether the readers may have the graph's volumes now. */
	bool Readable() const;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
