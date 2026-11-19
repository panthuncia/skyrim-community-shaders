#pragma once

// Limits an engine cull to the frame's engine-only subset. This is how a view DCLF draws hands the engine what DCLF does not draw.
// While a Scope lives on a thread, every culling process on that thread runs only for the subset's roots and their ancestors,
// so the engine's own tests still run on the path.
// The hooks chain with the other Process1 hooks of those classes. Unarmed, they only read a thread-local pointer.

#include <cstdint>
#include <functional>
#include <string>

namespace DCLF::Scene
{
	struct EngineOnlySubset;
}

namespace DCLF::EngineOnlyCull
{
	/** @brief Arms a_subset's a_view roots for the culls this thread runs while it lives. Null arms nothing. Scopes nest. */
	class Scope
	{
	public:
		Scope(const Scene::EngineOnlySubset* a_subset, std::uint8_t a_view) noexcept;
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		const Scene::EngineOnlySubset* previous = nullptr;
		std::uint8_t previousView = 0;
	};

	/** @brief Installs the Process1 hooks once, at PostPostLoad on the main thread. */
	void Install();

	/**
	 * @brief Render thread. Runs a_cull under a Scope with the scene lists' cull jobs held off, so the list culls run inline where
	 * the scope is armed.
	 */
	void RunInline(const Scene::EngineOnlySubset* a_subset, std::uint8_t a_view, const std::function<void()>& a_cull);

	/** @brief Why the engine culled a DCLF view whole this frame. Each is a gap in DCLF's coverage, counted and warned about once. */
	enum class WholeCull : std::uint8_t
	{
		SunNotCovered,  // a sun cascade not DCLF's, culled with the entry exclusion
		SunNoExclusion,  // no exclusion current either, so everything
		PointNotCovered,  // or no selection this frame
		PointPastMask,  // past the light mask's 32
		SpotNotCovered,
		SpotPastMask,
		PrimaryNoSnapshot,  // no candidates yet
		OcclusionNotTaken,  // the engine's SetupMask over every scene list
		Count
	};
	/** @brief Any thread. Counts a whole cull. The first of each reason is logged. */
	void NoteWholeCull(WholeCull a_reason);
	/** @brief The whole culls since the last call as a report line, or empty. */
	std::string WholeCullLine();

	/** @brief The armed culls' counts since the last call. */
	struct Stats
	{
		std::uint64_t culled = 0, descended = 0, skipped = 0;
	};
	Stats TakeStats();
}
