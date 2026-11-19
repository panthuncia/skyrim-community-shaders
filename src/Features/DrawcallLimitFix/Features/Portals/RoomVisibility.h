#pragma once

#include <cstdint>

namespace DCLF::RoomVisibility
{
	/**
	 * @brief Main::IsRoomVisible (0x140649f20) answered from DCLF's VisibleSet (skyrim-engine-notes.md, "Main::IsRoomVisible").
	 * Its three callers run on job threads. While DCLF runs and a set is published, DCLF answers and the engine's function is skipped.
	 */
	void Install();
	bool Installed();
	/** @brief Checks the calls before any hook is written. DCLF answers in the engine's place, so each must call the engine's own. */
	void Preflight();
	/** @brief Records at Main::Update's entry whether DCLF runs this frame. DCLF answers only then. */
	void SetLive(bool a_live);
	/** @brief DCLF's answer for a_key from the newest VisibleSet, true without one. Any thread. Reads no engine memory. */
	bool DclfAnswer(const void* a_key);
	/** @brief The calls answered since the last Take, by DCLF and by the engine. */
	struct Answers
	{
		std::uint64_t dclf = 0, engine = 0;
	};
	Answers Take();
}
