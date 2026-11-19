#pragma once

// TruePBR's texture parity under CS_DCLF_CAPTURE_PARITY. Render thread.
// Checks each native Lighting draw's PS t80-t91 landscape layers against the material record's feature textures.

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

namespace DCLF::Integrations::TruePBR
{
	/** @brief The t80-t91 checks since the last report. */
	struct DrawTextureParity
	{
		std::uint64_t checks = 0;
		std::uint64_t mismatches = 0;

		/** @brief A mismatch's note. */
		using NoteFn = void (*)(void* a_owner, const RE::BSGeometry* a_geometry, std::string a_message);

		/** @brief Checks a native draw of a_objectIndex while TruePBR is loaded. A record without its layers is not compared. */
		void Compare(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables,
			NoteFn a_note, void* a_owner);
		bool Ok() const { return mismatches == 0; }
		/** @brief The report's line. */
		void Report() const;
		void Reset() { checks = mismatches = 0; }
	};
}
