#pragma once

// Advanced Skin's wetness parity. Render thread.
// The capture parity checks each native draw's PS b7 and actor ownership.

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class BSGeometry;
}

namespace org::async
{
	template <class T>
	class KeptChunks;
}

namespace DCLF
{
	struct SceneTables;
	struct BindlessShading;
}

namespace DCLF::Integrations::AdvancedSkin
{
	/** @brief CaptureParity's b7 checks since its last report. */
	struct DrawWetnessParity
	{
		std::uint64_t checks = 0;
		std::uint64_t mismatches = 0;
		std::uint64_t wetDraws = 0;  // checked draws with sweat or water wetness
		std::uint64_t ownerMismatches = 0;

		/** @brief A mismatch's note. */
		using NoteFn = void (*)(void* a_owner, const RE::BSGeometry* a_geometry, std::string a_message);

		/** @brief Checks a native draw of a_objectIndex: the hook's b7 upload against the frame's row, and actor ownership against the tables. */
		void Compare(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables,
			const org::async::KeptChunks<BindlessShading>* a_rows, NoteFn a_note, void* a_owner);
		bool Ok() const { return mismatches == 0 && ownerMismatches == 0; }
		void Reset() { checks = mismatches = wetDraws = ownerMismatches = 0; }
	};
}
