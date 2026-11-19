#pragma once

// Light Limit Fix: while loaded, a Lighting draw reads its clustered lights (t35-t37) and StrictLightData (PS b3),
// whose only per-object value in the main pass is the room index. The integration registers what DCLF needs to draw both:
// a "light limit fix" classify input, the room index as an ancestor value, the light buffers as graph buffers,
// the local shadow cache (t102, t103) as frame textures for its graph path, and a StrictLightData parity.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Integrations::LightLimitFix
{
	// t35-t37: its lights, light index list and light grid, as graph buffers at consecutive registers.
	inline constexpr std::uint32_t kLightsRegister = 35;
	inline constexpr std::uint32_t kGraphBuffers = 3;
	// t102 and t103: the local shadow cache's slot data and shadow maps, as frame textures.
	inline constexpr std::uint32_t kLocalShadowDataRegister = 102;
	inline constexpr std::uint32_t kLocalShadowMapsRegister = 103;
	// The room index in the object record's first word, -1 outside rooms.
	inline constexpr std::uint32_t kRoomRecordWord = 0;
	inline constexpr std::int32_t kNoRoom = -1;

	/** @brief The StrictLightData checks since the last report. */
	struct DrawLightDataParity
	{
		std::uint64_t checks = 0;
		std::uint64_t mismatches = 0;  // StrictLightData differs
		// Local shadow lights: DCLF's draw's against the native pass's ShadowBitMask, and those differing in a portal-strict light's bit.
		std::uint64_t shadowChecks = 0, shadowMismatches = 0, gatedMismatches = 0;
		std::uint64_t shadowRoomMismatches = 0;  // shadow mismatches where the room differs too
		std::uint64_t shadowReassignedChecks = 0;  // shadow checks of a receiver whose mask channels were reassigned since the readback
		std::uint64_t shadowReassignedMismatches = 0;  // of those, mismatches where it did not move
		std::uint64_t shadowMovedMismatches = 0;  // shadow mismatches of a receiver that moved since the readback
		std::string shadowFirstStill;             // the first of one that did not move
		std::string shadowFirst;
		// The first distinct room mismatches, with the live parent chain and the node DCLF's index names.
		std::vector<std::string> roomDetails;
		std::vector<const RE::BSGeometry*> roomDetailed;

		/** @brief A mismatch's note. */
		using NoteFn = void (*)(void* a_owner, const RE::BSGeometry* a_geometry, std::string a_message);

		/**
		 * @brief Checks a native draw of a_objectIndex: StrictLightData against the tables' room, and ShadowBitMask against DCLF's draw's lights.
		 * a_drawn is null when unknown. a_moved means its bound changed since that readback was armed.
		 */
		void Compare(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables, const std::uint32_t (*a_drawn)[2], bool a_moved,
			bool a_reassigned, NoteFn a_note, void* a_owner);
		bool Ok() const { return mismatches == 0 && shadowMismatches == 0; }
		/** @brief The report's line. */
		void Report() const;
		void Reset()
		{
			checks = mismatches = shadowChecks = shadowMismatches = gatedMismatches = shadowRoomMismatches = shadowMovedMismatches = shadowReassignedMismatches = shadowReassignedChecks = 0;
			shadowFirst.clear();
			shadowFirstStill.clear();
			roomDetails.clear();
			roomDetailed.clear();
		}
	};

	Api::Feature Integration();
}
