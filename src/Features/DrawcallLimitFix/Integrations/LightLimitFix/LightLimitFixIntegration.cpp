// Light Limit Fix: the classify input, the room index, the light buffers at t35-t37, the local shadow cache at t102 and t103,
// and StrictLightData at PS b3 as a per-draw buffer. Callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"

#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/ORGLightCulling.h"
#include "Features/LightLimitFix/ORGLocalShadows.h"

#include <cstdint>
#include <span>

namespace DCLF::Integrations::LightLimitFix
{
	namespace
	{
		// PS b3: StrictLightData, a per-draw buffer.
		constexpr std::uint32_t kStrictLightDataRegister = 3;

		// Any thread. Whether Light Limit Fix is loaded.
		bool Loaded()
		{
			return globals::features::lightLimitFix.loaded;
		}

		// LLF's GetParentRoomNode: the nearest BSMultiBoundRoom or BSPortalSharedNode up the chain, by RTTI.
		const void* RoomRtti()
		{
			return REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundRoom }.get();
		}

		const void* PortalRtti()
		{
			return REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSPortalSharedNode }.get();
		}

		std::uint64_t RoomMapGeneration()
		{
			return globals::features::lightLimitFix.GetRoomMapGeneration();
		}

		// LLF swaps its map on the render thread at its own point, so only a copy is posted.
		void RoomMap(Api::AncestorMapSink& a_map)
		{
			const auto& lightFix = globals::features::lightLimitFix;
			a_map.Reserve(lightFix.roomNodes.size());
			for (const auto& [node, index] : lightFix.roomNodes)
				a_map.Put(node, index);
		}

		// Its lights, light index list and light grid, while its ORG culling is active.
		bool GraphBufferIndices(std::span<std::uint32_t> a_out)
		{
			return ORGLightCulling::Get().GetShaderResourceIndices(a_out[0], a_out[1], a_out[2]);
		}

		void Install(Api::InstallContext& a_context)
		{
			// While loaded, its clusters light the effects and its rooms key the room index.
			const auto loaded = a_context.RegisterClassifyInput({ .name = "light limit fix", .sample = &Loaded });
			// The room's index in its map, looked up at a join while the map's generation is behind.
			a_context.RegisterAncestorValue({ .name = "Light Limit Fix's room index", .recordWord = kRoomRecordWord, .none = kNoRoom,
				.ancestorRtti = { &RoomRtti, &PortalRtti }, .whileOn = loaded, .generation = &RoomMapGeneration, .fill = &RoomMap });
			// t35-t37, which the colour passes declare.
			a_context.RegisterFrameGraphBuffers({ .name = "light limit fix", .firstRegister = kLightsRegister,
				.resources = { ORGLightCulling::kLightsId, ORGLightCulling::kLightIndexListId, ORGLightCulling::kLightGridId }, .indices = &GraphBufferIndices });
			// t102 and t103 have no views of their own. The colour commit reads the captured views and the faces null.
			// On the ORG path EarlyPrepass binds the cache's exports, so the commit reads the published resources.
			a_context.RegisterFrameTexture({ .name = "light limit fix local shadow data", .textureRegister = kLocalShadowDataRegister,
				.graphResources = { ORGLocalShadows::kDataId } });
			a_context.RegisterFrameTexture({ .name = "light limit fix local shadow maps", .textureRegister = kLocalShadowMapsRegister,
				.graphResources = { ORGLocalShadows::kCacheId } });
			// StrictLightData, bound per draw, so frame blocks and capture checks leave PS b3 out.
			a_context.DeclarePerDrawBuffer({ .name = "StrictLightData", .stage = Api::ShaderStage::Pixel, .constantRegister = kStrictLightDataRegister });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "light limit fix", .install = &Install, .status = [] { return FeatureStatus(globals::features::lightLimitFix); } };
	}
}
