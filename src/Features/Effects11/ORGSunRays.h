#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "Features/Effects11.h"

/**
 * @brief Effects11's volumetric rays and sun rays on the render graph (CS_ORG_E11_RAYS=1).
 *
 * The D3D11 path's passes, at the PostComposite anchor, which runs where the D3D11 path draws them: at the end of the deferred
 * passes, after the composite. Volumetric rays: the raymarch (a raster pass into two half-res targets), the horizontal and the
 * vertical depth-aware blur (compute), and the apply pass, added to kMAIN. Sun rays: the mask, three radial blurs and the
 * composite, all raster passes, the last added to kMAIN.
 *
 * Resources: the half-res intermediates are transient graph textures (aliasable). The per-frame constants (the half-res size
 * for the blurs and the upsample, and each sun ray pass's SunRaysData) are one latch region. A run gate holds each effect's
 * draws and the blurs' group counts: the commit opens what the frame gave the graph, so the recordings are kept and an effect
 * off this frame does no work. The areas the passes cover are part of the build (a kept recording fixes its viewports). The shared constant buffers come
 * from SharedConstants. The scene depth and kMAIN are the engine's targets; Terrain and Cloud Shadows' textures are read by
 * name when they are on the graph, else imported from D3D11.
 *
 * Effects11's Draw* functions decide the frame as before, then hand it here. A frame whose inputs the graph cannot read runs
 * on the D3D11 path instead. A failed program or graph returns the rays to D3D11 for good.
 *
 * Render thread throughout. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGSunRays
{
public:
	/** @brief Whether CS_ORG_E11_RAYS=1 is set. */
	static bool Requested();

	/** @brief The one instance. Its graph state is made by Setup. */
	static ORGSunRays& Get();

	/** @brief Reserves the PostComposite anchor when requested (Effects11's PostPostLoad). */
	static void Reserve();

	/**
	 * @brief Effects11's SetupResources, with the render graph up: the latch block, the run gate and the sampler every build
	 * shares. The passes are built at the first frame the graph takes, from that frame's inputs.
	 * @param a_onFailed Called if the render graph fails later; the rays are then on D3D11 for the session.
	 * @return False when the rays stay on D3D11 (not requested, or no graph or anchor).
	 */
	bool Setup(std::function<void()> a_onFailed);

	/** @brief Whether the graph may take the rays: set up, not failed, and the render graph still active. */
	bool IsActive() const;

	/** @brief A frame's sizes: kMAIN's half-res texture, and the half-res and full dynamic resolution areas the passes cover. */
	struct Sizes
	{
		std::uint32_t halfTexWidth = 0, halfTexHeight = 0;
		std::uint32_t halfWidth = 0, halfHeight = 0;
		std::uint32_t width = 0, height = 0;
	};

	/** @brief Sun rays' passes in order: mask, three blurs, composite. */
	static constexpr std::uint32_t kSunRaysPasses = 5;

	/**
	 * @brief This frame's volumetric rays on the render graph, which the PostComposite anchor then runs.
	 * @return Whether the graph has them; false: the D3D11 path draws them.
	 */
	bool VolumetricRays(const Sizes& a_sizes);

	/**
	 * @brief This frame's sun rays on the render graph, with each pass's constants.
	 * @return Whether the graph has them; false: the D3D11 path draws them.
	 */
	bool SunRays(const Sizes& a_sizes, const std::array<Effects11::SunRaysData, kSunRaysPasses>& a_passes);

	/** @brief Drops the client, its resources and programs (a shader cache clear); the next frame builds them again. */
	void Release();

	~ORGSunRays();

	struct Impl;

private:
	ORGSunRays() = default;

	/** @brief The shared part of VolumetricRays and SunRays: the client for this frame's inputs. False: the frame runs on D3D11. */
	bool PrepareFrame(const Sizes& a_sizes);

	std::unique_ptr<Impl> impl;
	bool failed = false;
};
