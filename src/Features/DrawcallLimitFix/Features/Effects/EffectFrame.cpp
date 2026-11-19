// The Effect port's per-frame inputs. Four constant rows (camera planes, sun colour, blood globals, cloud light colour) and the soft
// effects' depth. Sampled on the render thread by each commit.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Globals.h"

#include "Features/TerrainBlending/ORGTerrainBlending.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace DCLF::Features::Effects
{
	namespace
	{
		/** @brief Far, near and far - near, as SetupTechnique (0x1414ea2c0) passes them for a Soft pass. Zero without a camera. */
		std::array<float, 4> SampleEffectCamera()
		{
			static const REL::Relocation<const std::byte* const*> camera{ REL::Offset(Address::kEffectCamera) };
			const std::byte* current = *camera.get();
			if (!current)
				return {};
			float nearPlane = 0.0f, farPlane = 0.0f;
			std::memcpy(&nearPlane, current + 0x160, sizeof(nearPlane));
			std::memcpy(&farPlane, current + 0x164, sizeof(farPlane));
			return { farPlane, nearPlane, farPlane - nearPlane, 0.0f };
		}

		/** @brief Weapon blood's AlphaTestRef.yz globals (0x1433dccf0, 0x1433dccf4), written only by BSTempEffectWeaponBlood. */
		std::array<float, 4> SampleEffectBlood()
		{
			static const REL::Relocation<const float*> globals{ REL::Offset(Address::kEffectBlood) };
			const float* blood = globals.get();
			return { 0.0f, blood[0], blood[1], 0.0f };
		}

		/**
		 * @brief DLightColor as SetupGeometry (0x1414eab10) writes it from light 0. Fade (+0x134) times HDR multiplier times NiLight +0x14c.
		 * A kCloudLOD property uses the cloud light (ShadowSceneNode +0x208) instead of the sun (+0x200).
		 */
		std::array<float, 4> SampleLightZero(bool a_cloud)
		{
			// Multiply in the engine's mulss order, HDR multiplier first. Light 0 is BSLight +0x48's NiLight.
			static const REL::Relocation<const std::byte* const*> imageSpace{ REL::Offset(Engine::Address::kImageSpaceManager) };
			auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
			const RE::BSLight* lightZero = sceneNode ? (a_cloud ? sceneNode->GetRuntimeData().cloudLight : sceneNode->GetRuntimeData().sunLight) : nullptr;
			const std::byte* light = nullptr;
			if (lightZero)
				std::memcpy(&light, reinterpret_cast<const std::byte*>(lightZero) + 0x48, sizeof(light));
			const std::byte* manager = *imageSpace.get();
			if (!light || !manager)
				return {};
			auto read = [](const std::byte* a_base, std::size_t a_offset) {
				float value = 0.0f;
				std::memcpy(&value, a_base + a_offset, sizeof(value));
				return value;
			};
			const float scale = read(manager, 0xe0) * read(light, 0x134);
			return { scale * read(light, 0x14c), scale * read(light, 0x150), scale * read(light, 0x154), 0.0f };
		}

		std::array<float, 4> SampleEffectLight() { return SampleLightZero(false); }

		std::array<float, 4> SampleEffectCloudLight() { return SampleLightZero(true); }

		template <std::array<float, 4> (*Sample)()>
		void SampleRow(float (&a_out)[4])
		{
			const auto value = Sample();
			static_assert(sizeof(value) == sizeof(a_out));
			std::memcpy(a_out, value.data(), sizeof(a_out));
		}

		// The depth copy SetupTechnique binds for a Soft pass. With Terrain Blending's ORG path, the published blended depth is read instead.
		void* SoftDepthView()
		{
			return globals::game::renderer ? static_cast<void*>(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV) :
			                                 nullptr;
		}
	}

	void RegisterFrame(Api::InstallContext& a_context)
	{
		// Latched in row order regardless of registration order.
		a_context.RegisterFrameConstantRow({ "effect camera", kEffectCameraFrameRow, &SampleRow<&SampleEffectCamera> });
		a_context.RegisterFrameConstantRow({ "effect light", kEffectLightFrameRow, &SampleRow<&SampleEffectLight> });
		a_context.RegisterFrameConstantRow({ "effect blood", kEffectBloodFrameRow, &SampleRow<&SampleEffectBlood> });
		a_context.RegisterFrameConstantRow({ "effect cloud light", kEffectCloudLightFrameRow, &SampleRow<&SampleEffectCloudLight> });
		a_context.RegisterFrameTexture({ .name = "effect soft depth", .textureRegister = kSoftDepthRegister, .view = &SoftDepthView,
			.graphResources = { ORGTerrainBlending::kBlendedDepthId } });
	}
}
