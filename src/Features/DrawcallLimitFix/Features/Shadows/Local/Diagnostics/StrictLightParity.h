#pragma once

#include <cstdint>

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
}

namespace DCLF::Features::Shadows::Local::StrictLightParity
{
	/**
	 * @brief CS_DCLF_STRICT_LIGHT_PARITY=1: compares a portal-strict point light's native cull with DCLF's model of it on parity frames.
	 * The engine's side is every geometry AppendNonAccum (vtable slot 0x19) receives, with its hemisphere code (1 front, 2 back, 3 both).
	 * The model walks the same root as the AE functions do (dclf-portal-query-batches.md, "Strict point-light traversal").
	 */
	bool Enabled();
	/** @brief Subscribes the comparison to LightSelectionMade when the switch is on. */
	void Configure(bool a_on);
	/** @brief Installs the AppendNonAccum trace. Render thread, at frame start, before the lights' culls. */
	void Install();
	/** @brief Starts the light's trace around a native Accumulate when due. Returns whether End must follow. Render thread. */
	bool Begin(const RE::BSShadowLight& a_light, bool a_whole, std::uint32_t a_frame);
	/** @brief Models the cull Begin traced and compares it with the trace. Render thread. */
	void End(const RE::BSShadowLight& a_light, const RE::NiAVObject* a_root, std::uint32_t a_frame);
}
