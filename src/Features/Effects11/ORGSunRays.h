#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "Features/Effects11.h"

/**
* @brief Effects11's volumetric rays and sun rays on the render graph, with CS_ORG_E11_RAYS=1.
*
* The D3D11 path's passes at the PostComposite anchor. A run gate holds each effect's draws, so an effect that is off this
* frame does no work and the recordings are kept. Effects11's Draw* functions decide the frame, then hand it here.
* A frame whose inputs the graph cannot read runs on D3D11. A failed program or registration returns to D3D11 for good.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGSunRays
{
public:
	static bool Requested();

	/** @brief Setup makes its graph state. */
	static ORGSunRays& Get();

	/** @brief Effects11's PostPostLoad. */
	static void Reserve();

	/**
	* @brief Effects11's SetupResources. The passes are built at the first frame the graph takes.
	* @return False when the rays stay on D3D11.
	*/
	bool Setup();

	/** @brief Set up, not failed, and the render graph still active. */
	bool IsActive() const;

	/** @brief kMAIN's half-res texture size, and the half-res and full dynamic resolution areas. */
	struct Sizes
	{
		std::uint32_t halfTexWidth = 0, halfTexHeight = 0;
		std::uint32_t halfWidth = 0, halfHeight = 0;
		std::uint32_t width = 0, height = 0;
	};

	static constexpr std::uint32_t kSunRaysPasses = 5;

	/** @brief @return False means the D3D11 path draws them. */
	bool VolumetricRays(const Sizes& a_sizes);

	/** @brief @return False means the D3D11 path draws them. */
	bool SunRays(const Sizes& a_sizes, const std::array<Effects11::SunRaysData, kSunRaysPasses>& a_passes);

	/** @brief For a shader cache clear. The next frame builds again. */
	void Release();

	~ORGSunRays();

	struct Impl;

private:
	ORGSunRays() = default;

	/** @brief False means the frame runs on D3D11. */
	bool PrepareFrame(const Sizes& a_sizes);

	std::unique_ptr<Impl> impl;
	bool failed = false;
};
