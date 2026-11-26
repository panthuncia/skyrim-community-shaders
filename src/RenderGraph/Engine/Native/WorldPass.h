#pragma once

#include <array>
#include <cstdint>

/**
* @brief The render state CS's points inside native's Main::RenderWorld draw with, from CS's own frame instead of the shadow state the
* engine's accumulator functions set around them (decision 107). Each point opens a Scope; its readers ask Current(). Outside native's
* RenderWorld, or in guest, Current() is null and the readers keep the shadow state. Render thread.
*
* The values are what the engine's pre-resolve and post-resolve (SE 99938/99939, AE 106583/106584) set before each point for the main
* accumulator: they do not depend on the scene.
*/
namespace RenderGraph::Engine::Native::WorldPass
{
	enum class Point : std::uint8_t
	{
		SkyBatch,      // the sky techniques' batch inside the deferred pass
		WaterMask,     // the water mask's batch
		WaterSurface,  // geometry group 10 and the water techniques' batch
		SunQuery,      // the sun's occlusion draw after the post-resolve
		Count
	};

	struct State
	{
		std::array<std::int32_t, 3> colours{ -1, -1, -1 };  // RE::RENDER_TARGET, -1 for none
		std::int32_t depth = 0;                             // RE::RENDER_TARGETS_DEPTHSTENCIL
		std::uint32_t depthMode = 0, stencil = 0, stencilRef = 0, fill = 0, cull = 0, depthBias = 0, scissor = 0;
		std::uint32_t blend = 0, alphaToCoverage = 0, write = 0, blendExtra = 0;
		bool alphaTest = false;
	};

	/** @brief The point a native RenderWorld point runs at, while in scope. Nests; a scope outside native's RenderWorld sets nothing. */
	class Scope
	{
	public:
		explicit Scope(Point a_point);
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		Point previous;
		bool set = false;
	};

	/** @brief The state of the point in scope, or null outside every scope. */
	const State* Current();

	/** @brief The normals target the frame writes (GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP, SE 100460 / AE 107177). */
	std::int32_t NormalsTarget();

	/** @brief Whether native's RenderWorld is running on this thread. */
	bool InRenderWorld();

	/**
	* @brief The targets the main accumulator renders to when the pre-resolve starts (Deferred::StartDeferred keeps them as its forward
	* targets): kMAIN, kMOTION_VECTOR, the normals target, none.
	*/
	std::array<RE::RENDER_TARGET, 4> ForwardTargets();

	/** @brief Marks native's RenderWorld: scopes set their point only inside it. */
	class RenderWorldScope
	{
	public:
		RenderWorldScope();
		~RenderWorldScope();
		RenderWorldScope(const RenderWorldScope&) = delete;
		RenderWorldScope& operator=(const RenderWorldScope&) = delete;
	};
}
