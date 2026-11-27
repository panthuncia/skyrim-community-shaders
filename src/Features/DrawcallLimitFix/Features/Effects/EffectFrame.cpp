// The Effect port's per-frame inputs. Four constant rows (camera planes, sun colour, blood globals, cloud light colour) and the soft
// effects' depth. Sampled on the render thread by each commit; the game's values among them (the lights, the blood globals) come from
// the frame's globals, which DCLF's frame start took on the game side.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"
#include "Globals.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"
#include "RenderGraph/Engine/Native/WorldPass.h"

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
			namespace WorldPass = ::RenderGraph::Engine::Native::WorldPass;
			// Native's RenderWorld: the world camera StartAccumulating made current, its far plane nearer inside a depth shift window.
			if (const auto planes = WorldPass::CurrentCameraPlanes())
				return { (*planes)[1], (*planes)[0], (*planes)[1] - (*planes)[0], 0.0f };
			// Native elsewhere: the world camera, which the engine made current at the depth pass, the masks and the post chain. A
			// reflection face's draws keep the face camera the engine's face render made current.
			if (Snapshot::FrameCamera::Owned() && !Reflection::ReflectionFaces::InFace()) {
				const auto world = WorldPass::CameraPlanes(false);
				return { world[1], world[0], world[1] - world[0], 0.0f };
			}
			static const REL::Relocation<const std::byte* const*> camera{ REL::Offset(Address::kEffectCamera) };
			const std::byte* current = *camera.get();
			if (!current)
				return {};
			float nearPlane = 0.0f, farPlane = 0.0f;
			std::memcpy(&nearPlane, current + 0x160, sizeof(nearPlane));
			std::memcpy(&farPlane, current + 0x164, sizeof(farPlane));
			return { farPlane, nearPlane, farPlane - nearPlane, 0.0f };
		}

		/** @brief Weapon blood's AlphaTestRef.yz globals (0x1433dccf0, 0x1433dccf4), written only by BSTempEffectWeaponBlood. Game side. */
		std::array<float, 4> CaptureEffectBlood()
		{
			static const REL::Relocation<const float*> globals{ REL::Offset(Address::kEffectBlood) };
			const float* blood = globals.get();
			return { 0.0f, blood[0], blood[1], 0.0f };
		}

		/**
		 * @brief DLightColor as SetupGeometry (0x1414eab10) writes it from light 0. Fade (+0x134) times HDR multiplier times NiLight +0x14c.
		 * A kCloudLOD property uses the cloud light (ShadowSceneNode +0x208) instead of the sun (+0x200).
		 */
		std::array<float, 4> CaptureLightZero(bool a_cloud)
		{
			// Multiply in the engine's mulss order, HDR multiplier first. Light 0 is BSLight +0x48's NiLight.
			static const REL::Relocation<const std::byte* const*> imageSpace{ REL::Offset(Engine::Address::kImageSpaceManager) };
			auto* sceneNode = CS_ENGINE_READ(ShaderManagerState, CapturePoint, globals::game::smState) ? CS_ENGINE_READ(ShaderManagerState, CapturePoint, globals::game::smState)->shadowSceneNode[0] : nullptr;
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

		// The game's rows in the frame's globals: the sun's, the blood's and the cloud light's, four floats each.
		constexpr std::uint32_t kLightRow = 0, kBloodRow = 4, kCloudLightRow = 8, kGameFloats = 12;
		Api::FrameGlobalsSlot gameSlot;  // install

		/** @brief DCLF's frame start, game side: the rows read of the game. */
		void CaptureGameRows(float* a_out)
		{
			const auto light = CaptureLightZero(false), blood = CaptureEffectBlood(), cloud = CaptureLightZero(true);
			std::memcpy(a_out + kLightRow, light.data(), sizeof(light));
			std::memcpy(a_out + kBloodRow, blood.data(), sizeof(blood));
			std::memcpy(a_out + kCloudLightRow, cloud.data(), sizeof(cloud));
		}

		/** @brief A game row as the frame's ticket took it. */
		std::array<float, 4> GameRow(std::uint32_t a_row)
		{
			std::array<float, 4> row{};
			if (gameSlot)
				std::memcpy(row.data(), &FrameGlobals::Current().features[gameSlot.first + a_row], sizeof(row));
			return row;
		}

		std::array<float, 4> SampleEffectLight() { return GameRow(kLightRow); }

		std::array<float, 4> SampleEffectBlood() { return GameRow(kBloodRow); }

		std::array<float, 4> SampleEffectCloudLight() { return GameRow(kCloudLightRow); }

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
			return CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer) ? static_cast<void*>(CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV) :
			                                 nullptr;
		}
	}

	void RegisterFrame(Api::InstallContext& a_context)
	{
		gameSlot = a_context.RegisterFrameGlobals({ .name = "effect rows", .floats = kGameFloats, .capture = &CaptureGameRows });
		// Latched in row order regardless of registration order.
		a_context.RegisterFrameConstantRow({ "effect camera", kEffectCameraFrameRow, &SampleRow<&SampleEffectCamera> });
		a_context.RegisterFrameConstantRow({ "effect light", kEffectLightFrameRow, &SampleRow<&SampleEffectLight> });
		a_context.RegisterFrameConstantRow({ "effect blood", kEffectBloodFrameRow, &SampleRow<&SampleEffectBlood> });
		a_context.RegisterFrameConstantRow({ "effect cloud light", kEffectCloudLightFrameRow, &SampleRow<&SampleEffectCloudLight> });
		a_context.RegisterFrameTexture({ .name = "effect soft depth", .textureRegister = kSoftDepthRegister, .view = &SoftDepthView,
			.graphResources = { ORGTerrainBlending::kBlendedDepthId } });
	}
}
