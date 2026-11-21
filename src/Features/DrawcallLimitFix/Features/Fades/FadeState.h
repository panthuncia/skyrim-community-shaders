#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF
{
	/**
	 * @brief A CPU port of the engine's fade OnVisible for BSFadeNode, BSLeafAnimNode and BSTreeNode (AE 1.6.1170).
	 * It follows the engine instruction for instruction, including MINSS/MAXSS and unordered comparisons,
	 * so the port, FadeStateCS.hlsl and the engine agree to the bit.
	 *
	 * The one value not computed here is the LOD step's distance scale, which uses logf and powf.
	 * The static row carries it from the CPU's CRT as FadeRootStatic::lodScale.
	 */
	namespace FadeState
	{
		/** @brief Update for a frame where the root's bound is in view. Returns kFadeVerdict* bits. */
		std::uint32_t OnVisible(FadeNodeState& a_state, const FadeRootStatic& a_root, const float a_centre[3], const FadeFrame& a_frame);
		/** @brief The animation job's fade update, repeated a_updates times. */
		void AnimatedUpdate(FadeNodeState& a_state, const FadeRootStatic& a_root, const float a_centre[3], const FadeFrame& a_frame, std::uint32_t a_updates);

		/** @brief Render thread. a_camera may be null. */
		FadeFrame SampleFrame(const RE::NiCamera* a_camera);

		/**
		 * @brief Render thread, at frame start after the camera setup and before the culls.
		 * Measures from the main camera so the results do not depend on the engine's list processes running.
		 */
		void BeginFrame();
		/** @brief Main camera position and LOD factor (NiCamera +0x184). Zero when no camera was seen. */
		std::array<float, 4> FadeEye();
		/** @brief Tree height test base and limit. The limit is +infinity when off. */
		std::array<float, 2> TreeHeightTest();
		/** @brief Render thread, at the depth commit. */
		void LatchGlobals(float (&a_eye)[4], float (&a_treeHeight)[2]);
		/** @brief Render thread. Null when there is none. */
		const RE::NiCamera* MainCamera();

		FadeNodeState ReadNode(const RE::NiAVObject& a_node);
		/**
		 * @brief Render thread, while nothing reads the fade nodes. The fade flag bit is set atomically because other writers own the other bits.
		 * Returns whether the fade or the LOD level changed.
		 */
		bool WriteNode(RE::NiAVObject& a_node, const FadeNodeState& a_state);
		/** @brief Size of a node of this plan, for making a copy the engine's functions can run on. */
		std::size_t NodeBytes(std::uint32_t a_plan);
		inline constexpr std::size_t kMaxNodeBytes = 0x1C0;
		/** @brief The plan, chosen by which OnVisible the node's vtable points at. */
		std::uint32_t PlanOf(const RE::NiAVObject& a_node);
		/** @brief Generation and object are left 0. */
		FadeRootStatic StaticOf(const RE::NiAVObject& a_node);
		/** @brief Whether a tree's LOD switch selects past child 0. */
		bool TreeLodSelected(const RE::NiAVObject& a_node);
	}
}
