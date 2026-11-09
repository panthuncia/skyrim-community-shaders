#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <span>

/**
 * @brief Grass Collision's collision update (CollisionUpdateCS) as render-graph passes, behind CS_ORG_GRASS_COLLISION=1.
 *
 * Two passes at the EarlyPrepass anchor: a copy moves the frame's colliders (bounding boxes and collision spheres) from the
 * client's latch block into two graph buffers, then the update writes the 512x512 collision texture. The per-frame values are a
 * LATCH entry of the shader, read from the same block. The texture is a graph twin (GraphResources::Texture2D) exported as a D3D11
 * SRV: the engine's grass vertex shader (t100) and Drawcall Limit Fix's grass draws read it there. Each update reads the last
 * frame's texture, so it is History.
 *
 * Why EarlyPrepass: the colliders are queued in Main::Update, before the frame renders, and the eye is the main camera's then. The
 * anchor runs after the shadow maps and before Main::RenderDepth, so before every main-view grass draw: the engine's Z-prepass and
 * main pass, and DCLF's depth commit, which writes the grass draw row from what GrassShared holds.
 *
 * Fallback: when the program or resources cannot be made, Setup returns false and Grass Collision keeps its D3D11 path. When the
 * graph fails later, the client is unregistered and a_onFailed (from Setup) brings the D3D11 path back.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h). Everything here runs on the render thread.
 */
class ORGGrassCollision
{
public:
	/** @brief The shapes the passes are made for: the D3D11 path's limits and strides. */
	struct Limits
	{
		std::uint32_t perFrameBytes = 0;  // GrassCollision::PerFrame
		std::uint32_t maxBoxes = 0;
		std::uint32_t boxStride = 0;  // GrassCollision::BoundingBoxPacked
		std::uint32_t maxInstances = 0;
		std::uint32_t instanceStride = 0;  // float4
		D3D11_TEXTURE2D_DESC texture{};    // the collision texture (SRV and UAV)
	};

	/** @brief One frame's values: copied by Stage, nothing kept by reference. */
	struct Frame
	{
		std::span<const std::byte> perFrame;
		std::span<const std::byte> boxes;
		std::span<const std::byte> instances;
	};

	static ORGGrassCollision& Get();

	/** @brief Whether CS_ORG_GRASS_COLLISION=1 asks for the render-graph path. */
	static bool Requested();

	/** @brief Before the render graph initializes (Grass Collision's PostPostLoad): reserves the EarlyPrepass anchor when Requested. */
	static void Reserve();

	/**
	 * @brief At SetupResources, with the runtime active: makes the program, latch block and graph resources and registers the client.
	 * @param a_onFailed Called once if the graph fails later; Grass Collision returns to D3D11.
	 * @return Whether the update runs on the render graph; false keeps Grass Collision on D3D11.
	 */
	bool Setup(const Limits& a_limits, std::function<void()> a_onFailed);

	/** @brief Whether the update runs on the render graph. */
	bool IsActive() const;

	/**
	 * @brief Before each run of the EarlyPrepass anchor (Grass Collision's EarlyPrepass, which Deferred calls right before it): the
	 * values the anchor's commit writes into the latch block. Sizes past the limits are cut to them. The client is always active,
	 * so its passes never run on a latch region the commit did not write.
	 */
	void Stage(const Frame& a_frame);

	/** @brief The exported collision texture's D3D11 view; null when inactive. */
	ID3D11ShaderResourceView* CollisionSRV() const;

	~ORGGrassCollision();

private:
	ORGGrassCollision() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
