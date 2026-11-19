#pragma once

// The material write hooks on shader-property controller updates and in-place material rewrites. Any thread.
// Every write recaptures the material for the coordinator's records and names the shading it moved.
// Features' own write hooks and CS's PBR materials push through the sink in Api/MaterialWrites.h.

#include <vector>

#include <ankerl/unordered_dense.h>

namespace DCLF::MaterialWrites
{
	/** @brief Installs the write hooks on Lighting's controllers and material vtables. */
	void Install();

	/** @brief Any thread, lock-free. A material's fields were written. */
	void NoteWritten(const RE::BSShaderMaterial* a_material);

	/** @brief Any thread. A property's per-object shading moved, so its dependents are resampled. */
	void NoteShading(const RE::BSShaderProperty* a_property);

	/** @brief Calls NoteWritten when the scope ends, for in-place rewrites with early returns. */
	struct NoteWrittenOnExit
	{
		const RE::BSShaderMaterial* material;
		~NoteWrittenOnExit() { NoteWritten(material); }
	};

	/** @brief Render thread, CS_DCLF_CAPTURE_PARITY only. Every material written since the last call. Always complete. */
	bool Drain(ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& a_out);
	void DrainShadingChanges(std::vector<const void*>& a_out);
}
