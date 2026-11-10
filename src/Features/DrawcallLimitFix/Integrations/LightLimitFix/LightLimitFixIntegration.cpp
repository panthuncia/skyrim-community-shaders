// Light Limit Fix: the "light limit fix" classify input, the room index (an ancestor value), the graph buffers at t35-t37, the local
// shadow cache at t102 and t103 (frame textures) and its StrictLightData (PS b3), a per-draw buffer. The render thread's callbacks read
// the feature live.

#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"

#include "RenderGraph/PublishedResources.h"

#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/ORGLightCulling.h"
#include "Features/LightLimitFix/ORGLocalShadows.h"

#include <cstdint>
#include <span>

namespace DCLF::Integrations::LightLimitFix
{
	namespace
	{
		// PS b3: StrictLightData (LightLimitFix.hlsli's), a per-draw buffer (Features/Manifest.cpp).
		constexpr std::uint32_t kStrictLightDataRegister = 3;

		// Whether Light Limit Fix is loaded. Any thread.
		bool Loaded()
		{
			return globals::features::lightLimitFix.loaded;
		}

		// Light Limit Fix's GetParentRoomNode: the nearest BSMultiBoundRoom or BSPortalSharedNode up the chain, by RTTI.
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

		// Light Limit Fix's map is the render thread's (it swaps it at its own point): only a copy is posted.
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

		// The views EarlyPrepass binds at t102 and t103 are the colour commit's captured ones: the integration names none. The views with
		// frame constants of their own (the reflection's faces) read null there, as before the registration.
		void* LocalShadowView()
		{
			return nullptr;
		}

		// On the ORG path (CS_ORG_LLF_SHADOWS) EarlyPrepass binds the cache's D3D11 exports: the colour passes read the published
		// resources instead, ordered after the copies at the EarlyPrepass anchor. The bound slot data is the export exactly when the
		// graph holds the cache; otherwise it is the D3D11 buffer (or none).
		template <const std::string_view* Id>
		bool LocalShadowGraphIndex(std::uint32_t& a_index)
		{
			const auto& lightFix = globals::features::lightLimitFix;
			if (!lightFix.loaded)
				return false;
			auto* const data = lightFix.GetLocalShadowDataSRV();
			return data && data == ORGLocalShadows::Get().GetDataSRV() && PublishedResources::ShaderResourceIndex(*Id, a_index);
		}

		void Install(Api::InstallContext& a_context)
		{
			// Loaded: its clusters light the effects (Features/Effects' variants) and its rooms key the room index.
			const auto loaded = a_context.RegisterClassifyInput({ .name = "light limit fix", .sample = &Loaded });
			// StrictLightData::RoomIndex: the room's index in its map, taken at a join while the map's generation is behind.
			a_context.RegisterAncestorValue({ .name = "Light Limit Fix's room index", .recordWord = kRoomRecordWord, .none = kNoRoom,
				.ancestorRtti = { &RoomRtti, &PortalRtti }, .whileOn = loaded, .generation = &RoomMapGeneration, .fill = &RoomMap });
			// t35-t37, which the colour passes declare.
			a_context.RegisterFrameGraphBuffers({ .name = "light limit fix", .firstRegister = kLightsRegister,
				.resources = { "cs.llf.lights", "cs.llf.light-index-list", "cs.llf.light-grid" }, .indices = &GraphBufferIndices });
			// t102 and t103, which the colour passes declare while the cache is the graph's.
			a_context.RegisterFrameTexture({ .name = "light limit fix local shadow data", .textureRegister = kLocalShadowDataRegister,
				.view = &LocalShadowView, .capturedWhenNull = true, .graphResource = ORGLocalShadows::kDataId,
				.graph = &LocalShadowGraphIndex<&ORGLocalShadows::kDataId> });
			a_context.RegisterFrameTexture({ .name = "light limit fix local shadow maps", .textureRegister = kLocalShadowMapsRegister,
				.view = &LocalShadowView, .capturedWhenNull = true, .graphResource = ORGLocalShadows::kCacheId,
				.graph = &LocalShadowGraphIndex<&ORGLocalShadows::kCacheId> });
			// StrictLightData, bound per draw: the main pass's frame blocks and the capture checks leave PS b3 out.
			a_context.DeclarePerDrawBuffer({ .name = "StrictLightData", .stage = Api::ShaderStage::Pixel, .constantRegister = kStrictLightDataRegister });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "light limit fix", .install = &Install, .status = [] { return FeatureStatus(globals::features::lightLimitFix); } };
	}
}
