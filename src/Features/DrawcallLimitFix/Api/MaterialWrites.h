#pragma once

// Calls a feature's engine hooks make when the engine writes a material or a property's shading in place, so Core's records follow.

namespace RE
{
	class BSShaderMaterial;
	class BSShaderProperty;
}

namespace DCLF::Api
{
	/** @brief Any thread, lock-free, after the engine's write. Recaptures the material. Null is ignored. */
	void NoteMaterialWritten(const RE::BSShaderMaterial* a_material);
	/** @brief Any thread, lock-free, after the engine's write. The next shading sample resamples the property. Never null. */
	void NotePropertyShading(const RE::BSShaderProperty* a_property);
}
