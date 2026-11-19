// Advanced Skin's wetness field: its membership hook and its capture at the frame's start. Render thread. Its material textures are in SkinTextures.cpp.

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/OwnerValues.h"

#include "Features/Skin.h"

#include <cstdint>

namespace DCLF::Integrations::AdvancedSkin
{
	namespace
	{
		// The capture's actor index. Membership comes from the coordinator's walks through the log. Values and fan-out are the render thread's.
		OwnerValues& Owners()
		{
			static OwnerValues owners;
			return owners;
		}

		// The last capture's counts, for the diagnostic capture.
		OwnerValues::Stats lastStats;

		void Membership(const Api::ShadingFieldMembership& a_membership)
		{
			auto& owners = Owners();
			for (const auto& change : a_membership.changes) {
				if (change.clear)
					owners.Clear();
				else
					owners.Set(change.slot, change.group, change.identity, change.resetValue);
			}
		}

		void Capture(Api::ShadingFieldCapture& a_capture)
		{
			// Skin::GetWetness advances each actor's fade once a frame on its first call, and later calls return the same value.
			// Calling it here for every actor advances every fade once a frame, whether or not the engine draws it.
			// Its cache is not thread-safe, so this must run on the render thread at the frame's start.
			DCLF_ZONE("CS.DCLF.Capture.Wetness");
			auto& skin = globals::features::skin;
			const bool wetness = skin.loaded && skin.settings.EnableSkin;
			// Use the render thread's own membership index and the newest publication's geometries, never the coordinator's tables.
			const auto geometry = a_capture.objectGeometry;
			const auto wetnessStats = Owners().Update([&](std::uint32_t o) {
				auto* mesh = o < geometry.size() ? geometry[o] : nullptr;
				const float4 value = wetness && mesh ? skin.GetWetness(mesh) : float4{};
				return OwnerValues::Value{ value.x, value.y, value.z, value.w };
			}, [&](std::uint32_t o, const auto& row) { a_capture.Put(o, row); });
			lastStats = wetnessStats;
			TracyPlot("CS.DCLF.Wetness.Actors", static_cast<std::int64_t>(wetnessStats.actors));
			TracyPlot("CS.DCLF.Wetness.ChangedActors", static_cast<std::int64_t>(wetnessStats.changed));
			TracyPlot("CS.DCLF.Wetness.VisitedMeshes", static_cast<std::int64_t>(wetnessStats.visited));
		}

		void Install(Api::InstallContext& a_context)
		{
			// Features/Actors' class, registered published whatever its toggle.
			a_context.RegisterShadingField({ .name = "wetness", .field = Api::ShadingRowField::SkinPerGeometry, .members = Api::EntryClass::ActorOwned,
				.membership = &Membership, .capture = &Capture });
			InstallMaterialTextures(a_context);
		}
	}

	Api::Feature Integration()
	{
		return { .name = "advanced skin", .install = &Install, .status = [] { return FeatureStatus(globals::features::skin) + fmt::format(", {} skin texture sets", globals::features::skin.skinExtraTextures.size()); } };
	}

	std::vector<std::string> GrowthLines()
	{
		const auto& owners = Owners();
		return { fmt::format("advanced skin wetness groups {}", owners.GroupCount()), fmt::format("advanced skin wetness slots {}", owners.SlotCount()),
			fmt::format("advanced skin wetness last capture actors {}, changed {}, visited {}, members {}", lastStats.actors, lastStats.changed, lastStats.visited,
				lastStats.members) };
	}
}
