#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Api
{
	class InstallContext;
}
namespace DCLF::Draws
{
	struct SceneBuffers;
}

namespace DCLF::Features::LocalLights
{
	/**
	 * @brief Immutable light context atlas and strict gate verdicts, with the GPU leases behind them.
	 * Readers must hold this object for as long as they use its descriptors. gatesDescriptor is 0 when no context is strict.
	 */
	struct LightContextPublication
	{
		std::uint32_t frame = 0, descriptor = 0, gatesDescriptor = 0;
		std::uint64_t generation = 0;
		std::vector<std::uint32_t> words, gateWords;
		std::uint64_t unmodelledGates = 0;  // Gates with a multibound shape the engine may test, which DCLF does not model.
		std::uint64_t rootlessObjects = 0;  // Objects without a fade root, gated by their own chain.
		std::shared_ptr<const void> owner;
	};
	/** @brief Publishes the light contexts and gates at each commit and registers the cull bindings that read them. */
	void RegisterLightContexts(Api::InstallContext& a_context);
	/** @brief This frame's atlas, never an older frame's. Render thread. */
	std::shared_ptr<const LightContextPublication> CurrentLightContexts(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);
}
