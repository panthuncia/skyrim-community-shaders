#pragma once

#include "Features/ViewAtmosphere.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Frame/Declare.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace org
{
	class LatchBlock;
	class RenderGraph;
}

namespace RenderGraph::AnchorPasses
{
	class Builder;
}

/**
* @brief The shared atmosphere and fog volume on the render graph (view-atmosphere-port.md 3.6): per grid, the inject pass
* (LightScattering.cs) writes the source volume from the history, then the integrate pass (Integrate.cs) writes the medium's
* luminance and transmittance. The passes belong to ORGViewAtmosphere's medium client, which adds them while the shared volume
* is on, and whose commit writes the parameters this class computes.
*
* The source and the history are a ping-pong pair of History textures. Each run reads one and writes the other. Both
* directions are registered as gated steps, and the commit opens the one the run takes, so no pointer is swapped.
* Render thread throughout. Setup failures are fatal. Free of ORG/Vulkan includes.
*/
class ORGViewAtmosphereVolume
{
public:
	/** @brief What the inject pass reads besides its own resources: part of the medium client's key. */
	struct Inputs
	{
		FrameAnchors::Texture shadowMap;      // the directional shadow map, null when the frame drew none
		FrameAnchors::Texture engineShadows;  // the engine's local shadow maps, null without them
		std::string terrain;                  // Terrain Shadows' published heights, empty for none
		std::string cloudLayer;               // Cloud Shadows' last layer, published, empty for none
		bool clouds = false;                  // Physical Sky's cloud shadow volume and mean, read by their ids
		bool localShadows = false;            // Light Limit Fix's local shadow data and cache
		bool lights = false;                  // Light Limit Fix's culled lights
		bool ibl = false;                     // IBL's SH
		bool probes = false;                  // Skylighting's probes
		bool environment = false;             // Dynamic Cubemaps' environment

		bool operator==(const Inputs&) const = default;

		/** @brief The flags.z bits whose inputs this registration binds. A bit it does not bind is cleared at the commit. */
		std::uint32_t Bound() const;
	};

	/** @brief How the medium's atlas is described, from ORGViewAtmosphere's declaration. */
	struct AtlasDeclaration
	{
		RenderGraph::Frame::Dependencies dependencies;
		RenderGraph::Frame::Texture3DDescFn desc;
		RenderGraph::Frame::Existence exists;
	};

	ORGViewAtmosphereVolume();
	~ORGViewAtmosphereVolume();

	/** @brief At ORGViewAtmosphere::Setup. Programs, samplers, the gate, the latch, the light buffer and the ping-pong pair. */
	void Setup(const AtlasDeclaration& a_atlas);

	/** @brief What the key reads besides the shared volume's own switch. */
	RenderGraph::Frame::Dependencies Dependencies() const;

	/** @brief The inputs for a revision's values: what the last Prepass found, plus what is fixed for the session. */
	Inputs KeyAt(const RenderGraph::Frame::Inputs& a_in) const;

	/** @brief The passes for a registration's inputs, written into a_builder (the volume's latch and gate). */
	void AddPasses(const Inputs& a_inputs, RenderGraph::AnchorPasses::Builder& a_builder, const GraphResources::Resource& a_luminance,
		const GraphResources::Resource& a_transmittance, const GraphResources::Resource& a_parameters);

	/** @brief The light data upload's source and target: the latch region and the buffer the inject pass reads at t98. */
	std::shared_ptr<const org::LatchBlock> Latch() const;
	const GraphResources::StructuredBuffer& LightBuffer() const;
	static constexpr std::uint32_t kLightOffset = 0;
	static constexpr std::uint32_t kLightBytes = 272;

	/** @brief The gate the commit opens. The client lists it. */
	std::shared_ptr<RenderGraph::RunGate> Gate() const;

	/** @brief Registers the light buffer and the ping-pong pair with a graph being built. */
	void RegisterResources(org::RenderGraph& a_graph) const;

	/** @brief At every Prepass: notes what the inject pass can read. A change registers the client again with the next revision. */
	void Observe();

	/**
	* @brief At the Prepass of a frame whose adopted revision runs the shared volume: the frame's parameters, computed as
	* ViewAtmosphere::Prepare and Render compute them, and the run it stages for the commit.
	*/
	ViewAtmosphere::Parameters Stage();

	/**
	* @brief In the medium client's commit. a_held is the volume of the registration the run executes, or null. Clears the flags it
	* does not bind, writes the latch and opens the staged direction. Without a held volume or a staged frame, a_parameters
	* become no medium.
	*/
	void Commit(const Inputs* a_held, ViewAtmosphere::Parameters& a_parameters);

	/** @brief After a submitted run: the source it wrote is the next frame's history. */
	void Executed();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
