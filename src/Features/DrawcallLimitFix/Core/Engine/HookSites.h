#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace DCLF::HookSites
{
	/**
	 * @brief DCLF's engine hook sites, checked before any hook is written.
	 *
	 * A hook that chains takes whatever another plugin left at the site. A hook that does not chain needs the engine's own code there.
	 * A conflict leaves DCLF off with an error naming the site and the module that holds it, rather than half installed.
	 * Half installs have stalled the retirement chain before, when a producer went in without its consumer.
	 * Sites held by Community Shaders' own code are not conflicts. Main thread, at PostPostLoad.
	 */

	/** @brief A chained CALL rel32. Any call target is taken. */
	void RequireCall(std::string_view a_name, std::uintptr_t a_site);
	/** @brief A chained JMP rel32, likewise. */
	void RequireJump(std::string_view a_name, std::uintptr_t a_site);
	/** @brief A non-chaining hook on a CALL rel32. The call must still reach a_target, or with 0 any function in the game's image. */
	void RequireCallTo(std::string_view a_name, std::uintptr_t a_site, std::uintptr_t a_target = 0);
	/** @brief A non-chaining hook on a JMP rel32, likewise. */
	void RequireJumpTo(std::string_view a_name, std::uintptr_t a_site, std::uintptr_t a_target = 0);
	/** @brief A vtable slot a non-chaining hook takes. It must still hold a_engine, or with 0 any function in the game's image. */
	void RequireSlot(std::string_view a_name, std::uintptr_t a_vtable, std::size_t a_index, std::uintptr_t a_engine = 0);
	/** @brief A function a non-chaining detour takes. Its entry must not already jump elsewhere. */
	void RequireEntry(std::string_view a_name, std::uintptr_t a_function);
	/** @brief Engine bytes an inline patch writes over. They must be as reverse engineered. */
	void RequireBytes(std::string_view a_name, std::uintptr_t a_site, std::span<const std::uint8_t> a_bytes);
	/** @brief A required site whose address this runtime lacks. */
	void RequireKnown(std::string_view a_name, bool a_known);

	/** @brief After install, a hook that failed to go in although its site passed. */
	void NoteFailed(std::string_view a_name, std::string_view a_why);
	/** @brief A detour's install result. Its func is 0 when the attach failed. */
	inline void NoteDetour(std::string_view a_name, std::uintptr_t a_func)
	{
		if (!a_func)
			NoteFailed(a_name, "the detour did not attach");
	}

	/** @brief The conflicts recorded so far, one line each, or empty. */
	std::string Conflicts();
	/** @brief How many sites were checked. */
	std::size_t Checked();

	/** @brief Where a_address leads, through JMP rel32s and absolute-jump trampolines. */
	std::uintptr_t Resolve(std::uintptr_t a_address);
	/** @brief The module Resolve(a_address) lies in. */
	std::string ModuleOf(std::uintptr_t a_address);
	/** @brief The module the CALL or JMP rel32 at a_site leads to. */
	std::string BranchTargetModule(std::uintptr_t a_site);
}
