#pragma once

#include <cstdint>
#include <string>

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::RegistrationCensus
{
	/**
	 * @brief The registration census, CS_DCLF_CENSUS. Counts every pass the hooks hand on to a batch renderer, by view and class.
	 *
	 * Note runs on the registering threads and pushes each pass, holding a geometry reference, onto a lock-free queue.
	 * Collect drains it on the render thread at Present and classifies each pass from state the render thread may read.
	 * Non-member Lighting geometry gets the coordinator's verdict, asked asynchronously unless the scene work runs inline.
	 * Until a reply arrives a marked live approximation stands in. Nothing of the engine is read or released on DCLF's threads.
	 */
	inline bool Enabled()
	{
		static const bool on = SwitchEnabled(Switch::Census);
		return on;
	}

	/** @brief Where the hook saw the pass. */
	enum class Site : std::uint8_t
	{
		RegisterPass,   // vfunc 0x02
		MainGroup,      // direct group insertions, FUN_1414b2330
		MainList,       // the main modes' hint 1 list insertion
		ShadowDirect,   // hints 11, 7 and 3, FUN_1414b2a60
		Volumetric,     // the shadow modes' volumetric-only insertion into group 15
		Occlusion,      // group 14, render mode 0x1C
	};

	enum NoteFlags : std::uint8_t
	{
		kReflectionFace = 1u << 1,  // inside a plain-mode face render, where the faces withhold
	};

	/** @brief Notes a pass the engine keeps. Any registration thread, only while Enabled. */
	void Note(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, Site a_site, std::uint8_t a_flags, std::uint8_t a_occlusionPhase = 0);

	/** @brief Subscribes Note to the hooks' kept passes. Install, while Enabled. */
	void Install();

	/** @brief Classifies and counts the passes noted since the last call. Render thread, at Present, every frame. */
	void Collect();

	/** @brief The window's report lines, empty when nothing was noted, and restarts the window. Render thread. */
	std::string TakeReport();
}
