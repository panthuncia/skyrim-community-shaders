#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <span>
#include <string_view>

/**
* @brief Light Limit Fix's local shadow cache at EarlyPrepass, on the render graph (guest and native graph modes).
*
* Right after the engine renders its shadow maps, a compute pass copies the fresh slices into the cache and a copy moves the slot
* data into its buffer. Both are Persistent, exported for the engine at t102 and t103, and published for DCLF and the fog.
* Per-frame values go through the latch and each possible copy is gated, so the recordings stay the same.
*
* The cache follows the frame revisions (frame revision plan, 6.13). It is declared once at setup on EnableLocalShadows,
* LocalShadowResolution and the engine's shadow map (EngineShadowMap): a new resolution is a new version, adopted with its revision,
* and it is one texel while the cache is off. A new slot count is a new slice count, so another shape class: the client's next
* registration takes a new texture, and the revision that adopts it publishes it under the same id. The data buffer holds every
* slot the setting allows, so it never changes.
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

	/** @brief The cache the frame binds. No slots while the cache is off. */
	struct Shape
	{
		std::uint32_t slots = 0;
		std::uint32_t resolution = 0;        // the cache's, square
		std::uint32_t engineResolution = 0;  // the engine's shadow maps', a power of two times the cache's
		std::uint32_t engineSlices = 0;
		explicit operator bool() const { return slots != 0; }
		friend bool operator==(const Shape&, const Shape&) = default;
	};

	static ORGLocalShadows& Get();

	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	/**
	* @brief Render thread, once at SetupResources. Declares the cache, its client and the data buffer, which follow Light Limit
	* Fix's settings from then on. a_dataStride is sizeof(LightLimitFix::LocalShadowData), a_capacity the most slots the setting
	* allows. False, holding nothing, keeps Light Limit Fix on D3D11 for the session.
	*/
	bool Setup(std::uint32_t a_dataStride, std::uint32_t a_capacity);

	bool IsActive() const;

	/** @brief Render thread. The adopted revision's cache. Empty when inactive, off, or before its client switched in. */
	Shape Adopted() const;

	/**
	* @brief Written into the latch when the anchor runs, for the adopted cache. Copies past kMaxCopies are dropped. Without a call
	* nothing is copied and the slot data stays.
	*/
	void SetFrame(std::span<const Copy> a_copies, std::span<const std::byte> a_data);

	/** @brief t103. Null while Adopted() is empty. */
	ID3D11ShaderResourceView* GetCacheSRV() const;

	/** @brief t102. Null while Adopted() is empty. */
	ID3D11ShaderResourceView* GetDataSRV() const;

	~ORGLocalShadows();

private:
	ORGLocalShadows() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
