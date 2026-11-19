#pragma once

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"

#include <span>
#include <string_view>

namespace DCLF::Diagnostics
{
	// Scene mirror writes, for the mirror parity's observers. Kept apart from Events.h so SceneMirror.cpp need not include Coordinator.

	/** @brief A capture's records were applied to the mirror. Coordinator thread. */
	struct MirrorCaptureApplied
	{
		const SceneCapture::Records& records;
	};

	/** @brief A detached subtree's records were taken out of the mirror. Coordinator thread. */
	struct MirrorDetached
	{
		std::span<RE::BSGeometry* const> geometries;
		std::span<const void* const> nodes;
	};

	/**
	 * @brief A held subtree was named by an attach outside the world, so it left the world by no detach a hook saw. Lights and
	 * cameras aside. a_rtti is static data. Coordinator thread.
	 */
	struct MirrorEvicted
	{
		const void* key;
		const char* rtti;
		std::size_t records;
		std::string_view what;
	};

	/** @brief Every mirror record was dropped. Coordinator thread. */
	struct MirrorCleared
	{
	};

	/** @brief A reference's 3D was placed in its cell and its fade node's statics just set. Engine thread. */
	struct FadeStaticsPlaced
	{
		const RE::NiAVObject* fade;
	};
}
