#pragma once

// The tree manager's animation walk as a move writer. Included by the feature's sources.

namespace DCLF::Api
{
	class InstallContext;
}

namespace DCLF::Features::TreeWind::TreeWalk
{
	/**
	 * @brief Pushes a move event after each walked tree's UpdateSelectedDownwardPass (`call [rax + 0x168]`).
	 * That pass shrinks the tree's multibound bound by about 15% with no other event, which left placement rows with the larger
	 * sphere (dclf-open-defects.md, "SE 1.5.97: sun entry on the GPU"). SE makes the call in FUN_1403df7d0 at 0x1403df8de. AE makes it
	 * in FUN_1404381e0's near list loop at 0x1404384f5, where the bones' palette and bound change too.
	 */
	void Install(Api::InstallContext& a_context);
	bool Installed();
	/** @brief Checks the call's bytes before any hook is written, since the thunk replaces the call inline. */
	void Preflight();
}
