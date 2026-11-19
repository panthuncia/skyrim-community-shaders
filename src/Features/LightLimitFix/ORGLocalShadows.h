#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

/**
* @brief Light Limit Fix's local shadow cache at EarlyPrepass, with CS_ORG_LIGHT_LIMIT_FIX_SHADOWS=1.
*
* Right after the engine renders its shadow maps, a compute pass copies the fresh slices into the cache and a copy moves the slot
* data into its buffer. Both are Persistent, exported for the engine at t102 and t103, and published for DCLF.
* Per-frame values go through the latch and each possible copy is gated, so the recordings stay the same.
* Render thread only. Free of ORG/Vulkan includes.
*/
class ORGLocalShadows
{
public:
	/** @brief The published ids of the slot data (t102) and the cache (t103). */
	static constexpr std::string_view kDataId = "LightLimitFix::LocalShadowData";
	static constexpr std::string_view kCacheId = "LightLimitFix::LocalShadowCache";

	/** @brief LightLimitFix::ENGINE_SHADOW_MAP_SLICES. */
	static constexpr std::uint32_t kMaxCopies = 8;

	struct Copy
	{
		std::uint32_t sourceSlice;
		std::uint32_t targetSlot;
	};

	struct Desc
	{
		std::uint32_t slots = 0;
		std::uint32_t resolution = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // R32_FLOAT or R16_UNORM
		std::uint32_t scale = 1;                   // engine texels per cache texel per axis, a power of two
		std::uint32_t dataStride = 0;              // sizeof(LightLimitFix::LocalShadowData)
	};

	static ORGLocalShadows& Get();

	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	/**
	* @brief Called when Light Limit Fix sizes its cache, which follows its settings, so not at SetupResources.
	* @return False, holding nothing, keeps Light Limit Fix on D3D11.
	*/
	bool Setup(const Desc& a_desc);


	/** @brief Releases D3D11 views first, then the graph's after D3D11's last use, and withdraws them. */
	void Release();

	bool IsActive() const;

	/** @brief Written into the latch when the anchor runs. Copies past kMaxCopies are dropped. Without a call nothing is copied and the slot data stays. */
	void SetFrame(std::span<const Copy> a_copies, std::span<const std::byte> a_data);

	/** @brief t103. Null when inactive. */
	ID3D11ShaderResourceView* GetCacheSRV() const;

	/** @brief t102. Null when inactive. */
	ID3D11ShaderResourceView* GetDataSRV() const;

	~ORGLocalShadows();

private:
	ORGLocalShadows() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
