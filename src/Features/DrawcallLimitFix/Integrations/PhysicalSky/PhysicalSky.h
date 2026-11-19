#pragma once

// Physical Sky's pixel-stage resources in the graph modes.
// - DCLF's draws read them as frame textures. So the reflection's faces, drawn before the main pass captures, read them too.
// - The engine draw families (sky, water) latch their heap indices per draw (OrgHandles, Common/PhysicalSkyOrgHandles.hlsli).
// The LUTs and the AP shadow are graph textures (ORGPhysicalSky). The rest are still D3D11's. In guest mode a draw reads what the feature binds.
// In native mode no D3D11 work runs, so a draw reads a zero view of the declared kind. Every reader takes zero as no contribution.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ID3D11ShaderResourceView;

namespace DCLF::Integrations::PhysicalSky
{
	inline constexpr std::uint32_t kTransmittanceRegister = 61;
	inline constexpr std::uint32_t kSkyViewRegister = 62;
	inline constexpr std::uint32_t kAerialPerspectiveRegister = 63;
	inline constexpr std::uint32_t kApShadowRegister = 64;
	inline constexpr std::uint32_t kSkyTailRegister = 66;
	inline constexpr std::uint32_t kVolCubeDepthRegister = 67;
	inline constexpr std::uint32_t kShadowMeanRegister = 69;
	inline constexpr std::uint32_t kDiskPhaseRegister = 106;
	inline constexpr std::uint32_t kLightningReflectionRegister = 109;
	inline constexpr std::uint32_t kVolTrRegister = 110;
	inline constexpr std::uint32_t kVolLumRegister = 111;
	inline constexpr std::uint32_t kShadowVolumeRegister = 112;
	inline constexpr std::uint32_t kLightningChannelsRegister = 113;
	inline constexpr std::uint32_t kVolCubeTrRegister = 114;
	inline constexpr std::uint32_t kVolCubeLumRegister = 115;
	// The view medium's cloud depth (Common/ViewMedium.hlsli). D3D11 binds it at t116, which is DCLF's pixel object rows.
	inline constexpr std::uint32_t kMediumCloudDepthRegister = 105;

	/** @brief The registers the engine draw families latch, in Common/PhysicalSkyOrgHandles.hlsli's order. */
	inline constexpr std::array<std::uint32_t, 15> kOrgTextureRegisters{ kTransmittanceRegister, kSkyViewRegister, kAerialPerspectiveRegister,
		kApShadowRegister, kSkyTailRegister, kVolCubeDepthRegister, kShadowMeanRegister, kDiskPhaseRegister, kLightningReflectionRegister, kVolTrRegister,
		kVolLumRegister, kShadowVolumeRegister, kLightningChannelsRegister, kVolCubeTrRegister, kVolCubeLumRegister };

	/** @brief Each register's kind, as Physical Sky's header declares it. */
	inline constexpr std::array<GpuTextures::ViewKind, kOrgTextureRegisters.size()> kOrgTextureKinds{ GpuTextures::ViewKind::Texture2D,
		GpuTextures::ViewKind::Texture2D, GpuTextures::ViewKind::Texture3D, GpuTextures::ViewKind::Texture2D, GpuTextures::ViewKind::Texture2D,
		GpuTextures::ViewKind::Cube, GpuTextures::ViewKind::Texture2D, GpuTextures::ViewKind::Texture2D, GpuTextures::ViewKind::Cube,
		GpuTextures::ViewKind::Texture2D, GpuTextures::ViewKind::Texture2D, GpuTextures::ViewKind::Texture3D, GpuTextures::ViewKind::Texture2D,
		GpuTextures::ViewKind::Cube, GpuTextures::ViewKind::Cube };

	/** @brief PhysSkyOrgHandles (Common/PhysicalSkyOrgHandles.hlsli): the textures' heap indices, then SampTr and SampSv. */
	struct OrgHandles
	{
		std::array<std::uint32_t, 16> textures{};
		std::array<std::uint32_t, 4> samplers{};
		bool operator==(const OrgHandles&) const = default;
	};
	static_assert(sizeof(OrgHandles) == 80, "PhysSkyOrgHandles' layout");

	/**
	 * @brief Render thread. The view a graph draw reads at kOrgTextureRegisters[a_slot]: a LUT's export, or a zero view of its kind.
	 * Null means the view the feature binds on D3D11 (guest mode, while the feature makes it).
	 */
	ID3D11ShaderResourceView* OrgView(std::size_t a_slot);

	/** @brief How an engine draw family reads a published id: false when its client cannot, with a_why set. */
	using ReadPublished = std::function<bool(std::string_view a_id, std::uint32_t& a_index, std::string& a_why)>;

	/**
	 * @brief Render thread, per batch. An engine draw family's handles: OrgView's views, each published one by a_read and any other
	 * imported, and ORGPhysicalSky's samplers. All zero while Physical Sky is not loaded, since no shader then declares them.
	 * False with a_why when one cannot be read. The batch then stays the engine's.
	 */
	bool ResolveOrgHandles(OrgHandles& a_out, std::vector<std::shared_ptr<const void>>& a_owners, const ReadPublished& a_read, std::string& a_why);

	Api::Feature Integration();
}
