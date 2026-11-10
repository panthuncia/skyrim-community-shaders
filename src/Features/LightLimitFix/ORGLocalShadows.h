#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <span>
#include <string_view>

/**
 * @brief Light Limit Fix's local shadow cache on the render graph (CS_ORG_LLF_SHADOWS=1).
 *
 * At the EarlyPrepass anchor, right after the engine rendered its shadow maps, one compute pass copies this frame's freshly
 * rendered engine slices into the cache (LocalShadowCopyCS, built for ORG), and one copy pass moves the slots' projection data
 * into the data buffer. Both are Persistent graph resources exported to D3D11, so the engine's pixel shaders still bind them at
 * t102 (data) and t103 (cache). Both are also published (PublishedResources, kDataId and kCacheId): Drawcall Limit Fix's colour passes
 * read them through the graph.
 *
 * Every per-frame value (which slices to copy, the slots' data) goes through a LatchBlock that the anchor's commit writes, so
 * the passes' recordings stay the same from frame to frame.
 *
 * Render thread only. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGLocalShadows
{
public:
	/** @brief The published ids (PublishedResources), the twins' names: the slot data (t102) and the cache (t103). */
	static constexpr std::string_view kDataId = "LightLimitFix::LocalShadowData";
	static constexpr std::string_view kCacheId = "LightLimitFix::LocalShadowCache";

	/** @brief The most engine slices one frame copies (LightLimitFix::ENGINE_SHADOW_MAP_SLICES). */
	static constexpr std::uint32_t kMaxCopies = 8;

	/** @brief One engine shadow map slice to copy into a cache slot this frame. */
	struct Copy
	{
		std::uint32_t sourceSlice;
		std::uint32_t targetSlot;
	};

	/** @brief The cache's shape. */
	struct Desc
	{
		std::uint32_t slots = 0;
		std::uint32_t resolution = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // R32_FLOAT or R16_UNORM
		std::uint32_t scale = 1;                   // engine texels per cache texel per axis, a power of two
		std::uint32_t dataStride = 0;              // sizeof(LightLimitFix::LocalShadowData)
	};

	static ORGLocalShadows& Get();

	/** @brief Whether CS_ORG_LLF_SHADOWS=1 is set. */
	static bool Requested();

	/** @brief Reserves the EarlyPrepass anchor when requested. Call before the render graph initializes (PostPostLoad). */
	static void Reserve();

	/**
	 * @brief Creates the cache and data buffer for a_desc and registers the anchor client (again when the shape changed).
	 * @return Whether the cache lives on the graph; false, with nothing held, keeps Light Limit Fix on its D3D11 path.
	 */
	bool Setup(const Desc& a_desc);

	/** @brief Drops the client and its resources (D3D11 views first, the graph's after D3D11's last use), and withdraws them. */
	void Release();

	/** @brief Whether the cache is set up and the render graph still runs it. */
	bool IsActive() const;

	/**
	 * @brief This frame's copies and slot data, written into the latch when the anchor runs. a_data holds Desc::slots entries
	 * of Desc::dataStride bytes. Copies past kMaxCopies are dropped. A frame that does not call it copies nothing and keeps the
	 * last slot data.
	 */
	void SetFrame(std::span<const Copy> a_copies, std::span<const std::byte> a_data);

	/** @brief The exported cache (Texture2DArray, t103); null when inactive. */
	ID3D11ShaderResourceView* GetCacheSRV() const;

	/** @brief The exported slot data (StructuredBuffer, t102); null when inactive. */
	ID3D11ShaderResourceView* GetDataSRV() const;

	~ORGLocalShadows();

private:
	ORGLocalShadows() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
