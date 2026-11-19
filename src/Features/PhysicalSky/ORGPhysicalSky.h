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
* Phase 1: the disk phase, transmittance, multiscatter, sky-view and aerial perspective LUTs run at the EarlyPrepass anchor
* (client cs.physical-sky.luts). The LUTs are graph textures exported to D3D11, so the feature's D3D11 binds and its remaining
* D3D11 passes read the same memory, and published, so DCLF and other graph passes read them after the writes.
* A run gate holds the dispatches: a frame with the feature off dispatches nothing, and the disk phase LUT only on a change.
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

	/** @brief At SetupResources, after the LUTs exist. @return False keeps the LUTs on D3D11. */
	bool Setup(const PhysicalSky& a_feature);

	bool IsActive() const;

	/**
	* @brief At the feature's EarlyPrepass with the feature on. Queues the four LUTs, and the disk phase LUT when its constants changed.
	* @return False means the D3D11 dispatches run this frame.
	*/
	bool GenerateLuts(PhysicalSky& a_feature);

	~ORGPhysicalSky();

	struct Impl;

private:
	ORGPhysicalSky() = default;

	std::shared_ptr<Impl> impl;
};
