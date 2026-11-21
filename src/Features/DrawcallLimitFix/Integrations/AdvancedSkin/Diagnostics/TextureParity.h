#pragma once

// Advanced Skin's material parity under CS_DCLF_CAPTURE_PARITY. Render thread.
// Checks each native Lighting draw's PS t71, t74, t72 and b7 flags and profile against the object's material row.

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Integrations::AdvancedSkin
{
	/** @brief Advanced Skin's live loaded flag. */
	bool Loaded();

	/** @brief The texture and material checks since the last report. */
	struct DrawSkinTextureParity
	{
		std::uint64_t checks = 0;
		std::uint64_t mismatches = 0;

		/** @brief A mismatch's note. */
		using NoteFn = void (*)(void* a_owner, const RE::BSGeometry* a_geometry, std::string a_message);

		/** @brief Checks a native draw of a_objectIndex while Loaded(), for actor-owned objects only. */
		void Compare(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables,
			NoteFn a_note, void* a_owner);
		bool Ok() const { return mismatches == 0; }
		void Reset() { checks = mismatches = 0; }
	};
}
