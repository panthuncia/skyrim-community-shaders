#pragma once

/**
* Tracy zones around the engine's render functions, for attributing render-thread time that falls between CS's own zones.
* CS_ENGINE_ZONES=1 turns them on. Each zone wraps one call site with a pass-through thunk on its own trampoline, so it chains
* onto any hook already written there. Only on AE 1.6.1170: the sites are offsets inside engine functions.
*/
namespace EngineZones
{
	/** @brief Writes the call-site zones when CS_ENGINE_ZONES=1 and Tracy is compiled in. Call after every other hook is installed. */
	void Install();
}
