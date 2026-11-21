#pragma once

#include "RenderGraph/Port.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "Features/Effects11.h"

/**
* @brief Effects11's volumetric rays and sun rays on the render graph, in graph mode.
*
* The D3D11 path's passes at the PostComposite anchor. A run gate holds each effect's draws, so an effect that is off this
* frame does no work and the recordings are kept. Effects11's Draw* functions decide the frame, then hand it here.
* The five half-size textures are declared on the display size and the rays' toggles, the programs on kMAIN's format and the
* shader cache clears, and the client on its inputs (frame revision plan, 6.10). So a new size, toggle or program switches in
* with a frame revision. While the graph runs them, Effects11 reads the adopted toggles (raysToggles.Adopted()), and the
* effects drawn are those of the registration the frame's schedule holds (DeclaredClientOf::Held), so a toggle never draws a frame on D3D11.
* D3D11 still draws them in a frame whose inputs the graph cannot read and in a frame kind with no stage for them.
* In graph mode a failed setup is fatal.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGSunRays : public RenderGraph::Port<ORGSunRays>
{
public:
	static constexpr std::string_view kName = "Effects11 rays";

	/**
	* @brief Effects11's SetupResources. Declares the textures, the programs and the client. Outside graph mode it does nothing;
	* in graph mode a failure is fatal.
	*/
	void Setup();

	/** @brief kMAIN's half-res texture size, and the half-res and full dynamic resolution areas. */
	struct Sizes
	{
		std::uint32_t halfTexWidth = 0, halfTexHeight = 0;
		std::uint32_t halfWidth = 0, halfHeight = 0;
		std::uint32_t width = 0, height = 0;

		bool operator==(const Sizes&) const = default;
	};

	static constexpr std::uint32_t kSunRaysPasses = 5;

	/**
	* @brief @return False outside graph mode: the D3D11 path draws them. True means the graph takes them: it draws them when the registration
	* the frame's schedule holds has them, else nothing this frame. The graph draws at the sizes of the revision it adopted, which
	* a_sizes only matches once that revision carries this frame's size.
	*/
	bool VolumetricRays(const Sizes& a_sizes);

	/** @brief @return False outside graph mode: the D3D11 path draws them. a_passes' areas are replaced by the adopted revision's. */
	bool SunRays(const Sizes& a_sizes, const std::array<Effects11::SunRaysData, kSunRaysPasses>& a_passes);
	~ORGSunRays();

	struct Impl;

private:
	friend class RenderGraph::Port<ORGSunRays>;
	ORGSunRays();

	/** @brief Observes the frame's inputs once per frame. False only outside graph mode. A missing input is fatal. */
	bool PrepareFrame();

	std::unique_ptr<Impl> impl;
};
