#pragma once

#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

struct Skylighting;
class Texture2D;

namespace org
{
	class ExternalTextureResource;
}

/**
* @brief Skylighting's per-frame probe update as a compute pass at the Prepass anchor, with CS_ORG_SKYLIGHTING=1.
*
* The five probe volumes are History textures exported to D3D11 for the engine's draws and the composite. Resets are one-shot graph passes.
* Every ZENITH_INTERVAL-th map is the top-down zenith capture; the update's run copies it into the published zenith map first.
* The sky occlusion map is a persistent published depth twin, drawn by the engine through its DSV export or by DCLF's shadow epoch.
* A run gate holds the group counts, so a frame without an update dispatches nothing.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGSkylighting
{
public:
	/** @brief The published ids. */
	static constexpr std::string_view kProbeArrayId = "Skylighting::ProbeArray";
	static constexpr std::string_view kShadowVisibilityId = "Skylighting::ShadowVisibility";
	static constexpr std::string_view kCoverageId = "Skylighting::ProbeCoverage";
	static constexpr std::string_view kZenithId = "Skylighting::ZenithOcclusion";
	/** @brief Not published. */
	static constexpr std::string_view kAccumFramesId = "Skylighting::AccumFramesArray";
	static constexpr std::string_view kShadowBitmaskId = "Skylighting::ShadowBitmask";
	static constexpr std::string_view kOcclusionId = "Skylighting::Occlusion";

	/** @brief In UpdateProbesCS's UAV order. */
	enum class Volume : std::uint32_t
	{
		ProbeArray,
		AccumFrames,
		ShadowBitmask,
		ShadowVisibility,
		Coverage,
		Count
	};

	static bool Requested();

	/** @brief Skylighting's PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGSkylighting& Get();

	/** @brief At SetupResources with the runtime active. @return False keeps Skylighting on D3D11. */
	bool Setup(const Skylighting& a_feature);

	/**
	* @brief Call before Setup. Makes a persistent exported depth twin. Its view formats must match the target's.
	* It lives as long as the game, since D3D11 keeps drawing into its export. @return Null means make a D3D11 one.
	*/
	Texture2D* MakeOcclusionMap(const D3D11_TEXTURE2D_DESC& a_desc, DXGI_FORMAT a_srvFormat, DXGI_FORMAT a_dsvFormat);

	/** @brief Null when Skylighting made a D3D11 one. */
	std::shared_ptr<org::ExternalTextureResource> OcclusionMap() const;

	/**
	* @brief Call after MakeOcclusionMap. The zenith capture's copy, a graph texture exported to D3D11 for reading.
	* @return Null without the occlusion twin: Skylighting then makes a D3D11 one.
	*/
	Texture2D* MakeZenithMap();

	/** @brief Render thread. Whether this frame's capture may be the zenith one: the graph copies it before the next probe update. */
	bool ZenithReady() const;

	/**
	* @brief Render thread, at Prepass on an updating frame. Without an input the volumes hold this frame.
	* If DCLF drew the map, the same run clears it after the update for DCLF's next draw.
	* On a zenith frame the same run copies the map into the zenith map before the update.
	* @return False means the D3D11 dispatch runs.
	*/
	bool Update(const Skylighting& a_feature);

	/** @brief Render thread. */
	void NoteOcclusionDrawn();

	/** @brief Render thread, before each draw of the map. True once per graph clear, so an engine draw also uses it up. */
	bool TakeOcclusionCleared();

	bool IsActive() const;

	/** @brief Null when inactive. */
	ID3D11ShaderResourceView* SRV(Volume a_volume) const;

	/** @brief Render thread. Resets the volumes before the next probe update. @return False means the D3D11 volumes are Skylighting's to clear. */
	bool QueueReset();

	~ORGSkylighting();

private:
	ORGSkylighting() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
