// The water port's captures: the engine state BSWaterShader's setup reads each frame, and a water shape's own fields.
// Render thread. EffectFrame.cpp samples the Effect port's frame values the same way.

#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"
#include "Snapshot/EngineReads.h"

#include "Features/LinearLighting.h"
#include "Features/PhysicalSky.h"
#include "Features/UnifiedWater.h"
#include "Globals.h"
#include "State.h"

#include <bit>
#include <cstring>

namespace DCLF::Features::Water::Port
{
	namespace
	{
		template <class T>
		T Read(const Engine::Offset& a_offset)
		{
			T value{};
			if (const auto offset = a_offset.Get())
				std::memcpy(&value, reinterpret_cast<const void*>(REL::Module::get().base() + offset), sizeof(T));
			return value;
		}

		template <class T, std::size_t N>
		std::array<T, N> ReadArray(const Engine::Offset& a_offset)
		{
			return Read<std::array<T, N>>(a_offset);
		}

		template <class T>
		T Field(const void* a_base, std::size_t a_offset)
		{
			T value{};
			std::memcpy(&value, static_cast<const std::byte*>(a_base) + a_offset, sizeof(T));
			return value;
		}

		// ImageSpaceManager +0x1C0 is BSImagespaceShaderReflectionsRayTracing (SSR). SetupMaterial reads its +0x10 byte.
		constexpr std::size_t kSsrShader = 0x1C0;
		constexpr std::size_t kSsrShaderEnabled = 0x10;
	}

	FrameInputs CaptureFrame(const RE::BSShaderAccumulator* a_accumulator)
	{
		FrameInputs f;
		f.frame = globals::state ? globals::state->frameCount : 0;
		if (auto* shadowState = CS_ENGINE_READ(ShadowState, CapturePoint, globals::game::shadowState)) {
			auto& rss = shadowState->GetRuntimeData();
			const auto posAdjust = rss.posAdjust.getEye();
			const auto previous = rss.previousPosAdjust.getEye();
			f.posAdjust = { posAdjust.x, posAdjust.y, posAdjust.z };
			f.previousPosAdjust = { previous.x, previous.y, previous.z };
			const auto view = rss.cameraData.getEye();
			// SetupGeometry: the jittered view-projection when the flag is set, the unjittered one otherwise.
			const bool jittered = Read<bool>(Address::kViewProjJittered);
			std::memcpy(f.viewProj.data(), jittered ? static_cast<const void*>(&view.viewProjMat) : static_cast<const void*>(&view.viewProjMatrixUnjittered),
				sizeof(f.viewProj));
			std::array<float, 16> proj{};
			std::memcpy(proj.data(), &view.projMat, sizeof(proj));
			f.proj00 = proj[0];
			f.proj11 = proj[5];
		}
		if (const auto* sm = CS_ENGINE_READ(ShaderManagerState, CapturePoint, globals::game::smState)) {
			f.invFrameBufferRange = sm->invFrameBufferRange;
			// Its own global, not BSShaderManager::State's timerValues[0], which runs on a different clock.
			f.timer = Read<float>(Address::kReflectionTimer);
			f.interior = sm->interior;
			const auto* sceneNode = sm->sceneGraph < 4 ? sm->shadowSceneNode[sm->sceneGraph] : nullptr;
			if (const auto* fog = sceneNode ? sceneNode->GetRuntimeData().fogProperty.get() : nullptr) {
				f.hasFog = true;
				f.fogNearColor = { fog->nearColor.red, fog->nearColor.green, fog->nearColor.blue };
				f.fogFarColor = { fog->farColor.red, fog->farColor.green, fog->farColor.blue };
				f.fogNear = fog->nearDistance;
				f.fogFar = fog->farDistance;
				f.fogPower = fog->power;
				f.fogClamp = fog->clamp;
			}
		}
		if (!f.hasFog) {
			const auto color = ReadArray<float, 3>(Address::kDefaultFogColor);
			f.fogNearColor = color;
			f.fogFarColor = color;
		}
		static const REL::Relocation<const std::byte* const*> camera{ REL::Offset(Effects::Address::kEffectCamera) };
		if (const std::byte* current = *camera.get()) {
			f.cameraNear = Field<float>(current, 0x160);
			f.cameraFar = Field<float>(current, 0x164);
		}
		f.sunDir = ReadArray<float, 4>(Address::kSunDir);
		f.sunColor = ReadArray<float, 4>(Address::kSunColor);
		f.renderWidth = Read<std::uint32_t>(Address::kRenderWidth);
		f.renderHeight = Read<std::uint32_t>(Address::kRenderHeight);
		static const REL::Relocation<const std::byte* const*> imageSpace{ REL::Offset(Engine::Address::kImageSpaceManager) };
		if (const std::byte* manager = *imageSpace.get())
			if (const auto* ssr = Field<const std::byte*>(manager, kSsrShader))
				f.ssrEnabled = Field<std::uint8_t>(ssr, kSsrShaderEnabled) != 0;
		f.ssrIntensity = Read<float>(Address::kSsrIntensity);
		f.ssrBlur = Read<float>(Address::kSsrBlur);
		f.ssrPerturbation = Read<float>(Address::kSsrPerturbation);
		f.waterLighting = ReadArray<float, 4>(Address::kWaterLighting);
		f.defaultLightColor = ReadArray<float, 4>(Address::kDefaultLightColor);
		f.flowmapSize = Read<std::int32_t>(Address::kFlowmapSize);
		f.meshFlowCellOffset = ReadArray<float, 2>(Address::kMeshFlowCellOffset);
		f.displacementOffset = ReadArray<float, 4>(Address::kDisplacementOffset);
		f.waterLodRoot = Read<const RE::NiNode*>(Address::kWaterLOD);
		if (a_accumulator) {
			const auto& eye = a_accumulator->GetRuntimeData()->eyePosition;
			f.eyePosition = { eye.x, eye.y, eye.z };
		}
		f.cameraUnderwater = Read<bool>(Address::kCameraUnderwater);
		f.waterEnabled = Read<bool>(Address::kWaterEnabled);
		f.localMapPasses = Read<bool>(Address::kLocalMapPasses);
		f.rawWater = Read<bool>(Address::kRawWater);
		f.renderMode = Read<std::uint32_t>(Address::kRenderMode);

		f.linearLighting = globals::features::linearLighting.IsLinearLightingActive() && globals::state && globals::state->customPixelShader;
		const auto uw = globals::features::unifiedWater.GetFlowRewrite();
		f.unifiedWater = { uw.active, uw.width, uw.height, uw.offsetX, uw.offsetY };
		f.physicalSkySampler = globals::features::physicalSky.IsRendering();
		if (auto* tes = CS_ENGINE_READ(TES, CapturePoint, RE::TES::GetSingleton())) {
			auto* player = CS_ENGINE_READ(Player, CapturePoint, RE::PlayerCharacter::GetSingleton());
			auto* cell = player ? player->GetParentCell() : nullptr;
			f.cubemapFixInterior = tes->interiorCell || (cell && cell->IsInteriorCell());
		}
		f.mapMenuOpen = globals::state && globals::state->isMapMenuOpen;
		return f;
	}

	ObjectInputs CaptureObject(const RE::BSWaterShaderProperty& a_property, const RE::BSGeometry& a_geometry)
	{
		ObjectInputs o;
		o.waterFlags = a_property.waterFlags.underlying();
		o.stencilBit = a_property.unk8C;
		o.cellX = a_property.cellX;
		o.cellY = a_property.cellY;
		o.flowX = a_property.flowX;
		o.flowY = a_property.flowY;
		o.plane = { a_property.plane.normal.x, a_property.plane.normal.y, a_property.plane.normal.z, a_property.plane.constant };
		o.world = a_geometry.world;
		o.localScale = a_geometry.local.scale;
		o.vertexDescLow = static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(a_geometry.GetGeometryRuntimeData().vertexDesc));
		return o;
	}
}
