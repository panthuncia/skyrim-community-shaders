#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <span>

/**
* @brief Grass Collision's update as render-graph passes at EarlyPrepass, with CS_ORG_GRASS_COLLISION=1.
*
* A copy moves the frame's colliders from the latch into graph buffers, then the update writes the 512x512 collision texture.
* The texture is History, since each update reads the last frame's. It is exported for the engine's grass at t100 and published for DCLF.
* EarlyPrepass runs after Main::Update queued the colliders and before every main-view grass draw.
* Setup returns false on failure and Grass Collision keeps D3D11. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGGrassCollision
{
public:
	/** @brief The D3D11 path's limits and strides. */
	struct Limits
	{
		std::uint32_t perFrameBytes = 0;
		std::uint32_t maxBoxes = 0;
		std::uint32_t boxStride = 0;
		std::uint32_t maxInstances = 0;
		std::uint32_t instanceStride = 0;
		D3D11_TEXTURE2D_DESC texture{};    // SRV and UAV
	};

	/** @brief Copied by Stage. Nothing is kept by reference. */
	struct Frame
	{
		std::span<const std::byte> perFrame;
		std::span<const std::byte> boxes;
		std::span<const std::byte> instances;  // float4 collision spheres
		bool enabled = true;                   // off clears the texture, as the D3D11 path does
	};

	static ORGGrassCollision& Get();

	static bool Requested();

	/** @brief Grass Collision's PostPostLoad, before the render graph initializes. */
	static void Reserve();

	/** @brief At SetupResources with the runtime active. @return False keeps Grass Collision on D3D11. */
	bool Setup(const Limits& a_limits);

	/** @brief Set up, not failed, and the runtime still up. */
	bool IsActive() const;

	/**
	* @brief Call before each EarlyPrepass run. Sizes past the limits are cut. Only a frame that staged opens the update,
	* so a frame with no camera updates nothing. A disabled frame writes zero everywhere.
	*/
	void Stage(const Frame& a_frame);

	/** @brief Null when inactive. */
	ID3D11ShaderResourceView* CollisionSRV() const;

	~ORGGrassCollision();

private:
	ORGGrassCollision() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
