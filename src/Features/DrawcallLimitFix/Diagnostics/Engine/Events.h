#pragma once

#include <cstdint>

namespace RE
{
	class BSRenderPass;
}

namespace DCLF::Diagnostics
{
	// Registrations, on whatever thread the engine registers.

	/** @brief The hint 1 list insertion withheld a member's pass from the main views. */
	struct MemberListPassWithheld
	{
		const RE::BSRenderPass* pass;
	};

	/** @brief The geometry group 5 insertion withheld a member's pass from the main views. */
	struct MemberGroup5PassWithheld
	{
		const RE::BSRenderPass* pass;
	};
}
