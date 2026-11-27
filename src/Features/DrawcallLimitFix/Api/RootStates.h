#pragma once

// The single producer of the listed fade roots' OnVisible state, which decides whether and how faded the objects under them draw.
// The producer lists roots in a table family, steps their states on the GPU and binds the buffers Core's cull reads.
// Core owns the shared layouts in Records.h and every reader. Without a producer the primary cull stays off.

#include "Features/DrawcallLimitFix/Api/TableFamilies.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/RootStates.h.
	class RootListView;
	class RootTestInput;

	/** @brief The root test's answer for an object under a root. */
	struct RootTestOutput
	{
		float distance = 0.0f;         // fade-out distance from the root's centre, 0 for none
		std::uint32_t objectFlags = 0;  // within RootStatesContribution::objectFlags
	};

	/**
	 * @brief The root states. One contribution only.
	 *
	 * - family: its objectRow is every object's root.
	 * - view: any thread holding the tables. Never per object.
	 * - test: per join, never per frame.
	 * - globalsKey: mixes what test reads of the globals into a 32-bit FNV-1a witness, witness = (witness ^ word) * 16777619.
	 *   A change rebinds every object.
	 * - kick: render thread, once per frame. Starts the write-back task, which is never joined per frame.
	 * - latchGlobals: without it Core writes a zero eye and a tree height of {0, +infinity}.
	 */
	struct RootStatesContribution
	{
		std::string_view name;
		TableFamilyId family;
		void (*view)(const void* a_columns, RootListView& a_out) = nullptr;
		std::uint32_t objectFlags = 0;
		void (*test)(const RootTestInput& a_input, RootTestOutput& a_out) = nullptr;
		void (*globalsKey)(const RootTestInput& a_input, std::uint32_t& a_witness) = nullptr;
		void (*kick)() = nullptr;
		void (*latchGlobals)(float (&a_eye)[4], float (&a_treeHeight)[2]) = nullptr;
	};
}
