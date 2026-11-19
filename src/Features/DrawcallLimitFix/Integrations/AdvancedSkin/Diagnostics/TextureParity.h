#pragma once

// Advanced Skin's material texture parity under CS_DCLF_CAPTURE_PARITY. Render thread.
// Checks each native Lighting draw's PS t71 and t74 against the object's material record.

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

	/** @brief The t71 and t74 checks since the last report. */
	struct DrawSkinTextureParity
	{
		std::uint64_t checks = 0;
		std::uint64_t mismatches = 0;

		/** @brief A mismatch's note. */
		using NoteFn = void (*)(void* a_owner, const RE::BSGeometry* a_geometry, std::string a_message);

		/**
		 * @brief Checks a native draw of a_objectIndex while Loaded(). Compares only when the record has either view,
		 * since otherwise the hook left the previous draw's binding.
		 */
		void Compare(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables,
			NoteFn a_note, void* a_owner);
		bool Ok() const { return mismatches == 0; }
		void Reset() { checks = mismatches = 0; }
	};
}
