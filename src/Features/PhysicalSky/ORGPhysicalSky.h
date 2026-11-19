#pragma once

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <string_view>

class Texture2D;
class Texture3D;
struct PhysicalSky;

/**
* @brief Physical Sky on the render graph (docs/development/plans/physical-sky-port.md).
*
* Three clients:
* - cs.physical-sky.luts at EarlyPrepass: the disk phase, transmittance, multiscatter, sky-view and aerial perspective LUTs.
* - cs.physical-sky at Prepass: the aerial perspective shadow (ShadowAccum).
* - cs.physical-sky.composite at DeferredComposite, after cs.deferred: the aerial perspective and cloud composite on kMAIN,
*   which the graph's deferred composite leaves out (ApplyAerialPerspectiveCS.hlsl).
* The LUTs and the AP shadow are graph textures exported to D3D11, so the feature's D3D11 binds and its remaining D3D11
* passes read the same memory, and published, so DCLF and other graph passes read them after the writes.
* Run gates hold the dispatches: a frame with the feature off dispatches nothing, and the disk phase LUT only on a change.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGPhysicalSky
{
public:
	/** @brief The published ids. */
	static constexpr std::string_view kDiskPhaseLutId = "PhysicalSky::DiskPhaseLut";
	static constexpr std::string_view kTransmittanceLutId = "PhysicalSky::TransmittanceLut";
	static constexpr std::string_view kMultiscatterLutId = "PhysicalSky::MultiscatterLut";
	static constexpr std::string_view kSkyViewLutId = "PhysicalSky::SkyViewLut";
	static constexpr std::string_view kApLutId = "PhysicalSky::ApLut";
	static constexpr std::string_view kApShadowId = "PhysicalSky::ApShadow";

	/** @brief The LUTs, in LutGen's LUTGEN order, then the disk phase LUT. */
	enum class Lut : std::uint32_t
	{
		Transmittance,
		Multiscatter,
		SkyView,
		AerialPerspective,
		DiskPhase,
		Count
	};

	static bool Requested();

	/** @brief Physical Sky's PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGPhysicalSky& Get();

	/**
	* @brief At SetupResources, before Setup. A persistent graph twin exported to D3D11 (SRV and UAV), published by its id.
	* It lives as long as the game. @return Null means make a D3D11 texture.
	*/
	static Texture2D* MakeLut2D(Lut a_lut, const D3D11_TEXTURE2D_DESC& a_desc);
	static Texture3D* MakeLut3D(Lut a_lut, const D3D11_TEXTURE3D_DESC& a_desc);

	/** @brief The AP shadow's twin, as MakeLut2D makes a LUT's. A screen-sized R8 texture. @return Null means make a D3D11 texture. */
	static Texture2D* MakeApShadow(const D3D11_TEXTURE2D_DESC& a_desc);

	/** @brief At SetupResources, after the LUTs exist. @return False keeps the LUTs on D3D11. */
	bool Setup(const PhysicalSky& a_feature);

	bool IsActive() const;

	/** @brief The samplers the LUT passes made from the feature's, in this order. */
	enum class Sampler : std::uint32_t
	{
		Transmittance,  // sampTr
		SkyView,        // sampSv
		Noise,          // sampNoise
	};
	static constexpr std::uint32_t kNoSampler = ~0u;

	/**
	* @brief Render thread. A sampler's heap index for graph draws, or kNoSampler while inactive.
	* a_owner, when given, gets what keeps the descriptor alive, for a run that latches the index.
	*/
	std::uint32_t SamplerIndex(Sampler a_sampler, std::shared_ptr<const void>* a_owner = nullptr) const;

	/**
	* @brief At the feature's EarlyPrepass with the feature on. Queues the four LUTs, and the disk phase LUT when its constants changed.
	* @return False means the D3D11 dispatches run this frame.
	*/
	bool GenerateLuts(PhysicalSky& a_feature);

	/**
	* @brief At the feature's Prepass in place of AccumShadow. Stages the AP shadow for the Prepass anchor.
	* @return False means the D3D11 dispatch runs this frame: the client is not scheduled yet, or an input is still D3D11's.
	*/
	bool AccumShadow(const PhysicalSky& a_feature);

	/**
	* @brief At the feature's Prepass with the feature on. Stages the AP composite, which runs right after the graph's deferred
	* composite on frames that one composites. The D3D11 composite applies it itself on the other frames.
	*/
	void StageComposite(const PhysicalSky& a_feature);

	~ORGPhysicalSky();

	struct Impl;

private:
	ORGPhysicalSky() = default;

	std::shared_ptr<Impl> impl;
};
