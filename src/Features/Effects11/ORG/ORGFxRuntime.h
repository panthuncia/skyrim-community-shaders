#pragma once

#include "RenderGraph/Port.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace RenderGraph::Engine
{
	struct TonemapTargets;
}

/**
* @brief Effects11's ORG FX runtime, in graph mode. The preset's .fx passes run as graph passes at the Tonemap anchor in place
* of EffectManager::ExecuteEffects. See docs/development/org-fx-runtime.md.
*
* Each effect is parsed from FX11's preprocessed text and lowered to bindless HLSL on the render thread.
* The chain is planned with FX11's ping-pong resolved into fixed textures, declared on the display size. The chain's programs are
* one declared program on the display size and Effects11's shape writes (frame revision plan, 6.11): its key lists every program
* the plan asks for, and the programs lane compiles them before the revision starts. The client is declared on the same values:
* its key reads the shape and the built programs, and its make builds the chain. So a new plan, its builds and the textures' new
* versions switch in together, and the render thread never compiles. The tonemap's targets and FX11's compiled effects are not
* variables yet: Prepare observes them and invalidates both keys when they change.
* FX11 stays the value store, and the commit copies its variables' bytes into one latch region per pass.
* A plan or build that fails is fatal. FX11 never draws the chain in graph mode: a frame without a held plan (no stage holds the
* chain, or it lags behind a revision) draws the game's own tonemap, logged. A reload after the plan runs the plan's last values.
* Render thread, except the program builds. Free of ORG/Vulkan includes.
*/
class ORGFxRuntime : public RenderGraph::Port<ORGFxRuntime>
{
public:
	static constexpr std::string_view kName = "Effects11";

	/** @brief Call before FX11 reloads the effects, whose variables the commit reads. The next revision plans again. */
	void Invalidate();

	/**
	* @brief At the tonemap hook, before the anchor runs. Notes the frame's targets, and whether the graph runs the chain. Its state
	* is made on the first call, so IsActive is true from then on.
	*/
	void Prepare(const RenderGraph::Engine::TonemapTargets& a_targets);

	/** @brief When true Effects11 skips FX11. In graph mode (IsActive) FX11 never runs, whatever this says. */
	bool RanThisFrame() const;
	~ORGFxRuntime();

	struct Impl;

private:
	friend class RenderGraph::Port<ORGFxRuntime>;
	ORGFxRuntime();

	std::unique_ptr<Impl> impl;
};
