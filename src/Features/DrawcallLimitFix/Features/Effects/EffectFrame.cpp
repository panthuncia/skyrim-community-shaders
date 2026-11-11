// The Effect port's frame inputs: its four frame constant rows (Api::FrameConstantRow: the camera's planes, the Lighting variant's light
// colour, Blood's globals, the Lighting variant's cloud light colour; PS b13 c17-c20) and the soft effects' depth (Api::FrameTexture, t118; Terrain Blending's published blended
// depth while the depth table names its export). Sampled on the render thread by each main commit (the rows) and each colour commit
// (the depth).

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
		/**
		 * @brief The camera planes BSEffectShader::SetupTechnique (0x1414ea2c0) takes for a Soft pass (camera at 0x14332a328: near +0x160,
		 * far +0x164): far, near, far - near (CameraDataEffect.yz), 0. PS b13 c17 (DCLFEffectCamera); zero without a camera.
		 */
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

		/**
		 * @brief Blood (kWeaponBlood): PS AlphaTestRef.yz as SetupGeometry (0x1414eab10) writes them: globals 0x1433dccf0 and 0x1433dccf4,
		 * written only by FUN_1414ec2a0 from BSTempEffectWeaponBlood's Update and start (game settings DAT_14200fbe8, DAT_14200fc00); x 0
		 * (the record's threshold), w 0. PS b13 c19 (DCLFEffectBlood).
		 */
		std::array<float, 4> SampleEffectBlood()
		{
			static const REL::Relocation<const float*> globals{ REL::Offset(Address::kEffectBlood) };
			const float* blood = globals.get();
			return { 0.0f, blood[0], blood[1], 0.0f };
		}

		/**
		 * @brief The Lighting variant: PS DLightColor as BSEffectShader::SetupGeometry (0x1414eab10) writes it from light 0: its NiLight fade
		 * (+0x134) times the HDR multiplier (ImageSpaceManager +0xe0) times each channel of NiLight +0x14c (the weather's effect lighting);
		 * w 0. Zero without the light. a_cloud: light 0 of a kCloudLOD property, which FUN_1414fcf80 (AE 0x1414fcf80) takes from
		 * ShadowSceneNode[0] +0x208 (the cloud light) in place of the sun (+0x200).
		 */
		std::array<float, 4> SampleLightZero(bool a_cloud)
		{
			// SetupGeometry (0x1414eab10), the Lighting bit: PS DLightColor = (ImageSpaceManager +0xe0) x (NiLight +0x134), then that times each
			// of the light's +0x14c, +0x150, +0x154 (the multiplier first, as the engine's mulss order). Light 0 is BSLight +0x48's NiLight.
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

		/** @brief PS b13 c18 (DCLFEffectDLightColor): the sun's (SampleLightZero). */
		std::array<float, 4> SampleEffectLight() { return SampleLightZero(false); }

		/** @brief PS b13 c20 (DCLFEffectCloudLightColor): the cloud light's, for a kCloudLOD property (SampleLightZero). */
		std::array<float, 4> SampleEffectCloudLight() { return SampleLightZero(true); }

		template <std::array<float, 4> (*Sample)()>
		void SampleRow(float (&a_out)[4])
		{
			const auto value = Sample();
			static_assert(sizeof(value) == sizeof(a_out));
			std::memcpy(a_out, value.data(), sizeof(a_out));
		}

		// Soft effects' depth: the kPOST_ZPREPASS_COPY SetupTechnique binds for a Soft pass, which CS fills from the main depth after the
		// deferred pass, before the forward epoch. On Terrain Blending's ORG path the depth table names its blended depth's export there
		// from the depth pass to the terrain passes, and a colour commit in between reads the published texture.
		void* SoftDepthView()
		{
			return globals::game::renderer ? static_cast<void*>(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV) :
			                                 nullptr;
		}
	}

	void RegisterFrame(Api::InstallContext& a_context)
	{
		// The camera planes soft effects read (DCLFEffectCamera), the sun's effect colour (DCLFEffectDLightColor), Blood's
		// AlphaTestRef.yz: the weapon blood's two globals (DCLFEffectBlood), and the cloud light's effect colour for kCloudLOD properties
		// (DCLFEffectCloudLightColor; read only while CS_DCLF_EFFECT_CLOUD_LIGHT lets DCLF take them). Latched in row order whatever the
		// registration's.
		a_context.RegisterFrameConstantRow({ "effect camera", kEffectCameraFrameRow, &SampleRow<&SampleEffectCamera> });
		a_context.RegisterFrameConstantRow({ "effect light", kEffectLightFrameRow, &SampleRow<&SampleEffectLight> });
		a_context.RegisterFrameConstantRow({ "effect blood", kEffectBloodFrameRow, &SampleRow<&SampleEffectBlood> });
		a_context.RegisterFrameConstantRow({ "effect cloud light", kEffectCloudLightFrameRow, &SampleRow<&SampleEffectCloudLight> });
		a_context.RegisterFrameTexture({ .name = "effect soft depth", .textureRegister = kSoftDepthRegister, .view = &SoftDepthView,
			.graphResources = { ORGTerrainBlending::kBlendedDepthId } });
	}
}
