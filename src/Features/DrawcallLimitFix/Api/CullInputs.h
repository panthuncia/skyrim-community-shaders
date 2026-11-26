#pragma once

// The main cull input: what the main camera's cull reads besides the frustum, from a walk a feature makes after Main::Update's own.
// Features/Portals provides it. Without it the primary cull stays off, because the scene lists depend on whether the walk reached unbound space.
// Each fade root gets a program index into the feature's programs. Core never decodes it; the GPU cull shaders do.
// Cull bindings: the buffers Core's cull reads at fixed descriptor words, each one supplied by a feature.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/CullInputs.h.
	class RootProgramsJudge;
	class CullBindingSelect;

	/**
	 * @brief The main camera's cull input. One registration only. No callback runs per object.
	 *
	 * - noProgram: the program for a root culled by the frustum alone, and for a root without a node.
	 * - structure: any thread, per attach or detach.
	 * - programsVersion: render thread, per depth commit. Changes whenever judge's answers may have.
	 * - judge: render thread. Assigns every root's program in one batch.
	 * - test: parity only. The GPU's test run on the CPU. Returns 1 in view, 0 culled, -1 no program.
	 * - programWords: parity only. The program words as the walk encoded them now.
	 */
	struct MainCullInputContribution
	{
		std::string_view name;
		std::uint32_t noProgram = ~0u;
		bool (*unboundVisible)() = nullptr;
		void (*structure)(const void* a_parent) = nullptr;
		std::uint64_t (*programsVersion)() = nullptr;
		void (*judge)(RootProgramsJudge& a_roots) = nullptr;
		int (*test)(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags,
			std::string& a_why) = nullptr;
		std::span<const std::uint32_t> (*programWords)() = nullptr;
	};

	/**
	 * @brief The buffers Core's cull reads at fixed descriptor words.
	 *
	 * - RootStates: the published states a frame's cull reads. May alternate by frame.
	 * - RootStepStates: the root state step's own rows for this frame, read by the visibility events.
	 * - RootBounds: each root node's world bound this frame, which the root test reads.
	 * - MemberClaims: per-object atomic claims during query generation.
	 * - LightContexts: the frame's portal context atlas for the camera and the strict lights.
	 * - StrictGates: each strict-light gate's verdict per atlas context.
	 * - ObjectGates: each object's strict-light gate.
	 */
	enum class CullBindingSlot : std::uint8_t
	{
		Programs,
		RootPrograms,
		RootRows,
		RootStates,
		RootStepStates,
		RootSeeds,
		RootBounds,
		RootFrame,
		MemberQueries,
		MemberResults,
		MemberClaims,
		LightContexts,
		StrictGates,
		ObjectGates,
		Count
	};

	/**
	 * @brief Supplies one cull binding slot's buffer.
	 * select runs on the render thread per pass or commit. With no answer the slot is unbound.
	 * It must read only handles made at scene setup, its pool and the frame.
	 */
	struct CullBinding
	{
		CullBindingSlot slot = CullBindingSlot::Count;
		std::string_view name;
		void (*select)(CullBindingSelect& a_select) = nullptr;
	};
}
