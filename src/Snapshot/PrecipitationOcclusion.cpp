#include "Snapshot/PrecipitationOcclusion.h"

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"

#include <cstring>

namespace Snapshot::PrecipitationOcclusion
{
	namespace
	{
		// ComputeProjection's constants (AE 0x142029cb8, SE 0x141e062d8 and the scalars beside them).
		constexpr RE::NiPoint3 kUp{ 0.0f, 1.0f, 0.0f };        // NiPoint3::UNIT_Y
		constexpr RE::NiPoint3 kFallback{ 0.0f, 0.0f, 1.0f };  // negated when the direction is about zero
		constexpr float kZeroDirection = 0.001f;
		constexpr float kNormalizeEpsilon = 1e-6f;
		constexpr float kGridScale = 0.0078125f;  // 1/128 of the cube
		constexpr float kHalf = 0.5f;
		constexpr float kFarScale = 2.0f;

		/** @brief NiPoint3::Unitize's form in ComputeProjection: zero when the length is not above 1e-6. */
		RE::NiPoint3 Unit(float a_x, float a_y, float a_z)
		{
			const float length = std::sqrt(a_x * a_x + a_y * a_y + a_z * a_z);
			if (!(length > kNormalizeEpsilon))
				return {};
			const float scale = 1.0f / length;
			return { scale * a_x, scale * a_y, scale * a_z };
		}

		/** @brief NiPlane's constructor from a normal and a point (AE 0x140d32a60): the constant is the dot, in its order. */
		float PlaneConstant(const RE::NiPoint3& a_normal, const RE::NiPoint3& a_point)
		{
			return (a_point.y * a_normal.y + a_normal.x * a_point.x) + a_point.z * a_normal.z;
		}

		/** @brief The distance along a plane, snapped down to the grid as ComputeProjection truncates and steps back. */
		float Snap(const RE::NiPoint3& a_normal, float a_constant, const RE::NiPoint3& a_point, float a_inverse, float a_grid)
		{
			const float along = (((a_point.x * a_normal.x + a_point.y * a_normal.y) + a_point.z * a_normal.z) - a_constant) * a_inverse;
			float whole = static_cast<float>(static_cast<std::int32_t>(along));
			if (!(along - whole >= 0.0f))
				whole = whole - 1.0f;
			return whole * a_grid;
		}

		/** @brief NiCamera::SetViewFrustum (AE 0x140d2b0e0): the near plane is at least far / ratio and the camera's minimum. */
		void SetViewFrustum(const RE::NiFrustum& a_frustum, const MapCameraFixed& a_fixed, RE::NiFrustum& a_out)
		{
			float nearPlane = a_frustum.fNear;
			const float limit = a_frustum.fFar / a_fixed.maxFarNearRatio;
			if (nearPlane < limit)
				nearPlane = limit;
			if (nearPlane < a_fixed.minNear)
				nearPlane = a_fixed.minNear;
			a_out = a_frustum;
			a_out.fNear = nearPlane;
		}

		bool SameBits(const void* a_a, const void* a_b, std::size_t a_size) { return std::memcmp(a_a, a_b, a_size) == 0; }

		struct GameSide
		{
			RE::NiPoint3 previousForward{};
			bool seeded = false;
		};
		GameSide& Game()
		{
			static GameSide game;
			return game;
		}

		/** @brief Main::Precipitation::RenderOcclusion's test (AE FUN_1404076c0, SE FUN_1403ae920): the map is on, rain or snow falls. */
		bool EngineMask(const RE::Sky& a_sky)
		{
			static REL::Relocation<const bool*> maskOn{ REL::RelocationID(502538, 370126) };
			if (!*maskOn)
				return false;
			const auto falls = [](const RE::TESWeather* a_weather) {
				using Data = RE::BGSShaderParticleGeometryData;
				auto* data = a_weather ? a_weather->precipitationData : nullptr;
				if (!data)
					return false;
				const auto type = data->GetSettingValue(Data::DataID::kParticleType).i;
				return type == static_cast<std::uint32_t>(Data::ParticleType::kRain) || type == static_cast<std::uint32_t>(Data::ParticleType::kSnow);
			};
			return falls(a_sky.currentWeather) || falls(a_sky.lastWeather);
		}
	}

	bool MapCamera::operator==(const MapCamera& a_other) const
	{
		return valid == a_other.valid && SameBits(&world, &a_other.world, sizeof(world)) && frustum.fLeft == a_other.frustum.fLeft &&
		       frustum.fRight == a_other.frustum.fRight && frustum.fTop == a_other.frustum.fTop && frustum.fBottom == a_other.frustum.fBottom &&
		       frustum.fNear == a_other.frustum.fNear && frustum.fFar == a_other.frustum.fFar && frustum.bOrtho == a_other.frustum.bOrtho &&
		       port == a_other.port;
	}

	MapCamera Compute(const ProjectionInputs& a_inputs, void (*a_adjust)(RE::NiFrustum&))
	{
		// Port of AE 0x140408520 (SE 0x1403af530). Both runtimes group every sum the same way.
		RE::NiPoint3 d = a_inputs.direction;
		if (!(std::fabs(d.y) + std::fabs(d.x) + std::fabs(d.z) >= kZeroDirection))
			d = { -kFallback.x, -kFallback.y, -kFallback.z };
		// Right: the direction across up. Then up again: right across the direction.
		const RE::NiPoint3 a = Unit(kUp.z * d.y - kUp.y * d.z, kUp.x * d.z - kUp.z * d.x, kUp.y * d.x - kUp.x * d.y);
		const RE::NiPoint3 b = Unit(a.y * d.z - a.z * d.y, a.z * d.x - a.x * d.z, a.x * d.y - a.y * d.x);

		MapCamera camera;
		camera.valid = true;
		auto& rotate = camera.world.rotate.entry;
		rotate[0][0] = d.x, rotate[0][1] = b.x, rotate[0][2] = a.x;
		rotate[1][0] = d.y, rotate[1][1] = b.y, rotate[1][2] = a.y;
		rotate[2][0] = d.z, rotate[2][1] = b.z, rotate[2][2] = a.z;

		const float cube = a_inputs.cubeSize;
		const float grid = cube * kGridScale;
		const RE::NiPoint3 zero{};
		const float cd = PlaneConstant(d, zero), cb = PlaneConstant(b, zero), ca = PlaneConstant(a, zero);
		// Half a cube ahead of the world camera, less two cubes along the occlusion camera's previous forward.
		const auto& eye = a_inputs.eye;
		const auto& previous = a_inputs.previousForward;
		const float back = -cube;
		const float backX = back * previous.x, backY = back * previous.y, backZ = back * previous.z;
		const RE::NiPoint3 centre{ ((cube * eye.forward.x) * kHalf + eye.position.x) + (backX + backX),
			((cube * eye.forward.y) * kHalf + eye.position.y) + (backY + backY), ((cube * eye.forward.z) * kHalf + eye.position.z) + (backZ + backZ) };
		const float inverse = 1.0f / grid;
		const float sd = Snap(d, cd, centre, inverse, grid);
		const float sb = Snap(b, cb, centre, inverse, grid);
		const float sa = Snap(a, ca, centre, inverse, grid);
		camera.world.translate = { (b.x * sb + sd * d.x) + a.x * sa, (sd * d.y + b.y * sb) + a.y * sa, (sd * d.z + b.z * sb) + a.z * sa };
		// The camera has no parent, so NiAVObject::Update gives it its local transform.
		camera.world.scale = a_inputs.fixed.scale;

		const float half = cube * kHalf;
		RE::NiFrustum frustum;
		frustum.fLeft = -half;
		frustum.fRight = half;
		frustum.fTop = half;
		frustum.fBottom = -half;
		frustum.fNear = 1.0f;
		const float farHalf = cube * kFarScale;
		frustum.fFar = farHalf + farHalf;
		frustum.bOrtho = true;
		if (a_adjust)
			a_adjust(frustum);
		SetViewFrustum(frustum, a_inputs.fixed, camera.frustum);
		camera.port = a_inputs.fixed.port;
		return camera;
	}

	std::array<float, 16> Projection(const MapCamera& a_camera, const RE::NiPoint3& a_eye)
	{
		// Precipitation::RenderMask (AE 0x140408380): identity rows (AE 196452-196454, constant data) with the eye's offset from the
		// occlusion camera as the translation, times the camera's view-projection.
		DCLF::ShadowCameras::Camera matrices;
		DCLF::ShadowCameras::CameraMatrices(a_camera.world, a_camera.frustum, a_camera.port, matrices);
		std::array<float, 16> rows{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		const auto offset = a_eye - a_camera.world.translate;
		rows[12] = offset.x;
		rows[13] = offset.y;
		rows[14] = offset.z;
		rows[15] = 1.0f;
		return DCLF::ShadowCameras::Multiply(rows, matrices.viewProj);
	}

	WorldEye EyeOf(const CameraView& a_view)
	{
		const auto& r = a_view.rotation.entry;
		return { a_view.position, { r[0][0], r[1][0], r[2][0] } };
	}

	void Capture(Builder& a_builder)
	{
		Block block{};
		auto* sky = CS_ENGINE_READ(Sky, CapturePoint, globals::game::sky);
		auto* precip = sky ? sky->precip : nullptr;
		const auto& main = a_builder.Built().cameras.Current(ViewId::Main);
		auto* camera = precip ? precip->occlusionData.camera.get() : nullptr;
		if (precip) {
			block.precipitation = true;
			RE::BSGeometry* object = precip->currentPrecip ? precip->currentPrecip.get() : precip->lastPrecip.get();
			block.object = object != nullptr;
			block.engineMask = object && EngineMask(*sky);
			auto* property = object ? netimmerse_cast<RE::BSParticleShaderProperty*>(object->GetGeometryRuntimeData().shaderProperty.get()) : nullptr;
			auto* rain = property ? reinterpret_cast<RE::BSParticleShaderRainEmitter*>(property->particleEmitter) : nullptr;
			block.rainEmitter = rain != nullptr;
			if (camera && main.valid) {
				auto& game = Game();
				const auto& r = camera->world.rotate.entry;
				if (!game.seeded) {
					game.previousForward = { r[0][0], r[1][0], r[2][0] };
					game.seeded = true;
				}
				const auto& data = camera->GetRuntimeData2();
				std::memcpy(block.fixed.port.data(), &data.port, sizeof(block.fixed.port));
				block.fixed.scale = camera->local.scale;
				block.fixed.minNear = data.minNearPlaneDist;
				block.fixed.maxFarNearRatio = data.maxFarNearRatio;
				static const auto& direction = *reinterpret_cast<const RE::NiPoint3*>(REL::RelocationID(515509, 401648).address());
				static const auto& cubeSize = *reinterpret_cast<const float*>(REL::RelocationID(515451, 401590).address());
				const ProjectionInputs inputs{ direction, cubeSize, game.previousForward, EyeOf(main), block.fixed };
				block.mask = Compute(inputs);
				block.maskProjection = Projection(block.mask, inputs.eye.position);
				// The next update's call starts from this camera's forward, as the engine's camera kept it.
				const auto& m = block.mask.world.rotate.entry;
				game.previousForward = { m[0][0], m[1][0], m[2][0] };
				// Native: the rain emitter's projection, which the engine's particle setup reads, set here instead of in the render half.
				if (RenderGraph::Engine::Native::Active() && rain)
					std::memcpy(&rain->occlusionProjection, block.maskProjection.data(), sizeof(rain->occlusionProjection));
			}
		}
		a_builder.Set(block);
	}

	const Block* Current()
	{
		return Snapshot::CurrentBlock<Block>();
	}
}
