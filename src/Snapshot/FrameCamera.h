#pragma once

#include "Snapshot/FrameSnapshot.h"

#include <array>

namespace globals
{
	struct FrameBufferCache;
}

/**
* @brief The frame camera (render-thread roadmap, phase 2; notes: docs/development/plans/snap2-camera.md).
*
* The capture computes each view's camera matrices from the game camera, as BSGraphics::State::SetCameraData does
* (CameraView::view, proj, viewProj, eye). The render side turns the main view into the engine's per-frame block
* (VS_PerFrame, b12) once per render frame: it adds the render-owned jitter and takes the previous camera from what this
* render side last rendered, not from the snapshot. A repeated snapshot so renders with zero camera motion, and a dropped
* one reprojects from the camera that was really drawn.
*
* Native reads Main(). Guest and plain D3D11 keep the engine's own block (globals::game::frameBufferCached): the engine's
* draws read the engine's buffer there, and CS's passes must agree with them.
*/
namespace Snapshot::FrameCamera
{
	/** @brief A previous camera for the per-frame block: its unjittered view-projection (row-major) and its eye. */
	struct Previous
	{
		std::array<float, 16> viewProjUnjittered{};
		RE::NiPoint3 eye{};
	};

	/** @brief Inverse of a row-major 4x4, computed in double. Zero when singular. */
	std::array<float, 16> Inverse(const std::array<float, 16>& a_m);

	/**
	* @brief Writes VS_PerFrame's camera rows (c0-c41) as SetCameraData builds them, from row-major view, unjittered projection and
	* view-projection, and the eye (posAdjust). a_jitter is added to the projection's third row. Without a_previous, c16 and c41
	* are zero, as for a reflection face. a_out holds 45 float4 rows.
	*/
	void WriteCamera(const std::array<float, 16>& a_view, const std::array<float, 16>& a_proj, const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye,
		std::array<float, 2> a_jitter, const Previous* a_previous, float* a_out);

	/** @brief Fills a_view from a camera: placement, frustum, port and SetCameraData's unjittered matrices. Game thread, in the capture. */
	void Capture(const RE::NiCamera& a_camera, CameraView& a_view);

	/** @brief The render frame's dynamic resolution ratios: this frame's and the previous frame's (Upscaling owns them). */
	struct Ratios
	{
		float width = 1.0f, height = 1.0f;
		float previousWidth = 1.0f, previousHeight = 1.0f;
	};

	/** @brief Writes VS_PerFrame's dynamic resolution rows (c43, c44) with fDRClampOffset. */
	void WriteRatios(const Ratios& a_ratios, float a_clampOffset, float* a_out);

	/**
	* @brief Builds the render frame's main camera with its jitter (projection offsets in clip units, State::projectionPosScale's
	* meaning) and dynamic resolution. Once per render frame, when the jitter is decided (Main::UpdateJitter). Render owner.
	*/
	void BeginRender(std::array<float, 2> a_jitter, const Ratios& a_ratios);

	/** @brief Whether the render side builds the frame camera itself (native mode). Guest and D3D11 read the engine's block. */
	bool Owned();

	/**
	* @brief The main view's per-frame block for this render frame, jittered: what the world passes draw with. Native: built from
	* the snapshot (BeginRender, or without jitter on a frame that had none). Guest and plain D3D11: the engine's cached block.
	* Render owner.
	*/
	const globals::FrameBufferCache& Main();

	/**
	* @brief The same camera unjittered, as the engine sets it for the sun query and the post chain. Once the frame is back at full
	* resolution (after the upscaler, or Upscaling::PostDisplay) its dynamic resolution ratios are 1. Guest and plain D3D11: the
	* engine's cached block. Render owner.
	*/
	const globals::FrameBufferCache& Post();

	/** @brief The upscaler ran or Upscaling::PostDisplay did: the engine is at full resolution for the rest of the frame. Render owner. */
	void OnFullResolution();

	/** @brief A per-frame block matrix (stored transposed) as the engine's row-major camera data holds it. */
	std::array<float, 16> RowMajor(const void* a_blockMatrix);

	/** @brief The render frame's jitter as BeginRender got it. Render owner. */
	std::array<float, 2> Jitter();
}
