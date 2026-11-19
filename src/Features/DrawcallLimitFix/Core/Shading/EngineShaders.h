#pragma once

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

namespace DCLF
{
	/**
	 * @brief The engine's BSLightingShader instance (0x14338ca00) DCLF's programs come from, as captured at a frame's start. Null before the engine makes it.
	 * Set once on the render thread before the frame's inputs are posted, so the scene lane only reads what was set before it.
	 */
	class EngineShaders
	{
	public:
		static RE::BSShader* Lighting() { return lighting; }
		static void SetLighting(RE::BSShader* a_shader) { lighting = a_shader; }

	private:
		static inline RE::BSShader* lighting = nullptr;
	};
}
