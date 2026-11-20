#pragma once

#include <cstdint>
#include <memory>

namespace RenderGraph::Engine
{
	struct TonemapTargets;
}

/**
* @brief Effects11's ORG FX runtime, with CS_ORG_FX=1. The preset's .fx passes run as graph passes at the Tonemap anchor in place
* of EffectManager::ExecuteEffects. See docs/development/org-fx-runtime.md.
*
* Each effect is parsed from FX11's preprocessed text, lowered to bindless HLSL and compiled per pass when first needed.
* The chain is planned with FX11's ping-pong resolved into fixed textures, declared on the display size. The client is declared on
* the display size and Effects11's shape writes (frame revision plan, 6.11): its key is the plan, made when a frame revision starts,
* so a new plan, its builds and the textures' new versions switch in together. The tonemap's targets and FX11's compiled effects
* are not variables yet: Prepare observes them and invalidates the key when they change.
* FX11 stays the value store, and the commit copies its variables' bytes into one latch region per pass.
* Whenever the runtime cannot run a frame, FX11 runs it and a log line says why. A failed graph returns to FX11 for the session.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGFxRuntime
{
public:
	static bool Requested();

	/** @brief Its state is made on the first Prepare. */
	static ORGFxRuntime& Get();

	/** @brief Effects11::PostPostLoad, before the render graph initializes. */
	static void Reserve();

	/** @brief Call before FX11 reloads the effects, whose variables the commit reads. The next revision plans again. */
	void Invalidate();

	/** @brief At the tonemap hook, before the anchor runs. Notes the frame's targets, and whether the graph runs the chain. */
	void Prepare(const RenderGraph::Engine::TonemapTargets& a_targets);

	/** @brief When true Effects11 skips FX11. */
	bool RanThisFrame() const;

	~ORGFxRuntime();

	struct Impl;

private:
	ORGFxRuntime() = default;

	std::unique_ptr<Impl> impl;
};
