#pragma once

// The water reflection's engine hooks. Render thread. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Views.h"

#include <cstdint>

namespace RE
{
	class NiCamera;
}

namespace DCLF::Features::Reflection::ReflectionFaces
{
	/*
	 * The water reflection's cube map faces (AE 1.6.1170; skyrim-engine-notes.md, "Water reflections: the cube map").
	 * TESWaterReflections::Update (0x140520570) adds the cube camera's roots (FUN_1405275e0) and renders its faces through
	 * BSCubeMapCamera vfunc 0x35. Each face clears its targets, orients the camera (FUN_1414ed500), culls into the accumulator
	 * (+0x1A0) and renders it.
	 */

	/** @brief Hooks the update, the LOD add-roots and the face render. What is drawn inside them is decided per frame. */
	void Install();
	/**
	 * @brief Checks the calls before any hook is written. The LOD add-roots are replaced, not chained, so each must be the
	 * engine's own.
	 */
	void Preflight();

	/** @brief Whether a face render is running, and its face mask. Render thread. */
	bool InFace();
	std::uint32_t FaceMask();
	/** @brief Whether the face render's mode is plain (0). PassCapture withholds only then. */
	bool Plain();

	/** @brief Called once the engine has oriented the camera to a face, before its cull and draws. Render thread. */
	using FaceOriented = void (*)(const RE::NiCamera& a_camera, std::uint32_t a_face);
	void SetFaceOriented(FaceOriented a_callback);

	/**
	 * @brief The cube camera's LOD roots DCLF draws in this update's faces. Their add-root calls are skipped, the sky's stays.
	 * Values match Api::ViewRootBit.
	 */
	enum RootBits : std::uint32_t
	{
		kLodRoots = Api::ViewRootBit(Api::ViewRoot::LodLandAndObjects),
		kTreeRoot = Api::ViewRootBit(Api::ViewRoot::LodTrees),
	};
	using RootsOwned = std::uint32_t (*)(bool a_plain);
	void SetRootsOwned(RootsOwned a_callback);
	/** @brief Updates since the last call, and those whose LOD and tree roots were not added. */
	struct RootStats
	{
		std::uint32_t updates = 0, lodSkipped = 0, treeSkipped = 0;
	};
	RootStats TakeRootStats();
}
