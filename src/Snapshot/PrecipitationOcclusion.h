#pragma once

#include "Snapshot/FrameSnapshot.h"

#include "RE/N/NiFrustum.h"
#include "RE/N/NiPoint3.h"
#include "RE/N/NiTransform.h"

#include <array>

/**
* @brief The precipitation occlusion maps' cameras and projections as values (phase 7: no render-time engine reads).
*
* The engine's Precipitation::ComputeProjection (SE 25643, AE 26185) places the precipitation's occlusion camera from the
* precipitation shader's direction and cube size (written by Precipitation::Update in Sky::Update, on the game thread), the world
* camera, and the occlusion camera's own previous forward. RenderMask (SE 25642, AE 26184) then writes the occlusion projection
* into the rain emitter. Both are ported here as functions of values. The capture computes the precipitation mask's camera and
* projection once per update. Skylighting computes its sky map's camera on the render side from the block, since its direction
* is a render frame's sample.
*/
namespace Snapshot::PrecipitationOcclusion
{
	/** @brief An occlusion camera: what ComputeProjection leaves on the precipitation's NiCamera. */
	struct MapCamera
	{
		bool valid = false;
		RE::NiTransform world{};
		RE::NiFrustum frustum{};
		std::array<float, 4> port{};  // NiRect<float>: left, right, top, bottom
		bool operator==(const MapCamera& a_other) const;
	};

	/** @brief The occlusion camera's fields ComputeProjection keeps: its port, scale and SetViewFrustum's near limits. */
	struct MapCameraFixed
	{
		std::array<float, 4> port{};
		float scale = 1.0f;
		float minNear = 0.0f;
		float maxFarNearRatio = 1.0f;
		bool operator==(const MapCameraFixed&) const = default;
	};

	/** @brief The world camera's values ComputeProjection and RenderMask read. */
	struct WorldEye
	{
		RE::NiPoint3 position{};
		RE::NiPoint3 forward{};  // the world rotation's first column
	};

	/** @brief Precipitation::ComputeProjection's inputs. */
	struct ProjectionInputs
	{
		RE::NiPoint3 direction{};        // the precipitation shader's direction
		float cubeSize = 0.0f;           // the precipitation shader's cube size
		RE::NiPoint3 previousForward{};  // the occlusion camera's world forward before the call
		WorldEye eye{};
		MapCameraFixed fixed{};
	};

	struct Block
	{
		static constexpr BlockId kId = BlockId::PrecipitationOcclusion;

		bool precipitation = false;  // the sky has a Precipitation
		bool object = false;         // its current or last precipitation geometry exists
		bool engineMask = false;     // Main::Precipitation::RenderOcclusion's test: the occlusion map is on, and rain or snow falls
		bool rainEmitter = false;    // the geometry's particle property has an emitter, which holds the mask's projection
		MapCameraFixed fixed{};
		MapCamera mask{};                        // the precipitation mask's camera
		std::array<float, 16> maskProjection{};  // its occlusion projection, which the rain emitter holds
		bool operator==(const Block&) const = default;
	};

	/**
	* @brief Port of Precipitation::ComputeProjection, operation for operation. a_adjust, when set, changes the frustum before the
	* camera's SetViewFrustum clamps its near plane (Skylighting's quarter hook).
	*/
	MapCamera Compute(const ProjectionInputs& a_inputs, void (*a_adjust)(RE::NiFrustum&) = nullptr);

	/** @brief Port of Precipitation::RenderMask's occlusion projection: the camera's view-projection with the eye's offset. */
	std::array<float, 16> Projection(const MapCamera& a_camera, const RE::NiPoint3& a_eye);

	/** @brief The snapshot main camera's eye. */
	WorldEye EyeOf(const CameraView& a_view);

	/** @brief Fills the block. Registered by Snapshot::InstallHooks. Game thread. */
	void Capture(Builder& a_builder);

	/** @brief The current snapshot's block, or null. */
	const Block* Current();
}
