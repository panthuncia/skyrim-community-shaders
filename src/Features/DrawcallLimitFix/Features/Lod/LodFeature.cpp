// Registers object and terrain LOD under TES::lodLandRoot, the LOD gates and object LOD's segment events.

#include "Features/DrawcallLimitFix/Features/Lod/Lod.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGates.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegmentEvents.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegments.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"

#include "ShaderCache.h"

#include <cstdint>
#include <vector>

namespace DCLF::Features::Lod
{
	namespace
	{
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;

		// Object LOD is a BSSubIndexTriShape and terrain LOD is a land block's BSTriShape.
		constexpr std::uint64_t kObjectLodFlags = static_cast<std::uint64_t>(Flag::kLODObjects) | static_cast<std::uint64_t>(Flag::kHDLODObjects);
		constexpr std::uint64_t kTerrainLodFlags = static_cast<std::uint64_t>(Flag::kLODLandscape);
		constexpr auto kSubIndexTriShape = static_cast<std::uint8_t>(RE::BSGeometry::Type::kSubIndexTriShape);
		constexpr auto kTriShape = static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape);

		// Holds the terrain manager's LOD blocks, one BSMultiBoundNode each, attached and detached as the camera moves.
		RE::NiNode* LodLandRoot()
		{
			const auto* tes = RE::TES::GetSingleton();
			return tes ? tes->lodLandRoot : nullptr;
		}

		// Runs inside the engine-read window.
		void CaptureDrawnRanges(const void* a_geometry, std::vector<Api::EntryRange>& a_out)
		{
			LodSegments::DrawnRanges(a_geometry, a_out);
		}

		// A view's faces can draw the LOD roots in the engine's place only while swaps are gated. Without gates, newly shown
		// LOD takes a few frames to bind, so the engine must keep drawing those roots.
		bool LodRootsOwned(const Api::ViewRootFrame&)
		{
			return Extensions::ShowVetoRegistered() && Extensions::ObjectClassOn(Api::ObjectClass::LodObjects) && Extensions::ObjectClassOn(Api::ObjectClass::LodTerrain);
		}

		void Install(Api::InstallContext& a_context)
		{
			const auto gatesSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LOD_GATES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "0: the engine's LOD swaps ungated (T6b5: by default an object or terrain LOD swap keeps the outgoing level shown, and the incoming hidden, until DCLF's claims cover the incoming)",
				.after = "CS_DCLF_PLAIN_SNOW_SPARKLE" });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::LodObjects, .categoryRoot = &LodLandRoot });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::LodTerrain, .categoryRoot = &LodLandRoot });
			a_context.RegisterViewRoot({ .root = kOwnedRoot, .owned = &LodRootsOwned });
			// Object LOD sub-index shapes are drawn as tri-shapes over their visible segment ranges.
			// DCLF draws no other sub-index shape.
			for (const auto technique : { Technique::LODObjects, Technique::LODObjectHD })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique), .propertyFlags = kObjectLodFlags, .geometryType = kSubIndexTriShape });
			a_context.RegisterGeometryType({ .geometryType = kSubIndexTriShape, .ported = true, .propertyFlags = kObjectLodFlags,
				.ranges = &CaptureDrawnRanges });
			for (const auto technique : { Technique::LODLand, Technique::LODLandNoise })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique), .propertyFlags = kTerrainLodFlags, .geometryType = kTriShape });
			// The gates register first, so their component runs before the segments' and every later feature's.
			Gates::RegisterGates(a_context, gatesSwitch);
			RegisterSegmentChannel(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { .name = "lod", .install = &Install, .earlyHooks = &InstallSegmentHooks, .preflight = &LodGates::Preflight };
	}
}
