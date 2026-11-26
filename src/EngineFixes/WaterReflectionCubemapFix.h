#pragma once

#include "EngineFix.h"

struct WaterReflectionCubemapFix : EngineFix
{
	std::string GetName() override { return "Water Reflection Cubemap Fix"; }

	void Install() override;

	/** @brief Whether the cell or worldspace changed since the last call. False while the fix is not installed. Game side in native. */
	static bool SceneChanged();

	/** @brief Clears the reflection cube's faces. Render side. */
	static void ClearCubemap();

private:
	static inline bool installed = false;

	struct Main_RenderWaterEffects
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSWaterShader_SetupGeometry
	{
		static void thunk(RE::BSShader* shader, RE::BSRenderPass* pass, uint32_t renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
