#pragma once

// Per-object gates for portal-strict point lights (dclf-portal-query-batches.md, "Strict point-light gates").
// A strict light's native cull decides about an object from its gating ancestors: rooms its walk reached, shared nodes it crossed,
// and kAllPass multibound nodes. Everything below a fade root is the same for every light and already DCLF's.
// The CPU side serves the receivers' bits and the parity. The GPU side keeps a versioned gate per object and publishes each gate's
// verdict per context each frame, which StrictGateRejects reads.

#include "Features/DrawcallLimitFix/Features/Portals/PortalContext.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"

#include <cstdint>
#include <span>
#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Portals
{
	class ProgramWords
	{
	public:
		explicit ProgramWords(std::span<const std::uint32_t> a_words);
		std::uint32_t Count() const { return count; }
		/** @brief The walk reached the program's room (kPortalReached). */
		bool Reached(std::uint32_t a_program) const;
		/** @brief Whether the room's compound frustum has operators, so Process1 tests it. */
		bool HasOps(std::uint32_t a_program) const;
		bool SkipView(std::uint32_t a_program) const;
		bool Pass(std::uint32_t a_program, const float a_centre[3], float a_radius) const;

	private:
		const std::uint32_t* Entry(std::uint32_t a_program) const;
		std::span<const std::uint32_t> words;
		std::uint32_t count = 0, records = 0;
	};

	struct GateLink
	{
		enum Kind : std::uint8_t
		{
			kRoom,     // BSMultiBoundRoom: program (1 + its room index; kPortalNoProgram: not the walked graph's), its culling mode
			kShared,   // BSPortalSharedNode
			kAllPass,  // BSMultiBoundNode with cullingMode kAllPass
			kShape,    // BSMultiBoundNode with a shape: tested by the engine outside modes 1, 3 and 4 (not modelled)
		};
		Kind kind = kRoom;
		const void* node = nullptr;
		std::uint32_t program = kPortalNoProgram;
		std::uint32_t roomMode = 0;
		bool operator==(const GateLink&) const = default;
	};
	using GateChain = std::vector<GateLink>;  // top-down

	/**
	 * @brief a_object's gating ancestors, itself included, top-down, at most 256 levels.
	 * Render thread, or a cull's thread while the graph does not change.
	 */
	void ChainOf(const RE::NiAVObject* a_object, GateChain& a_out);

	/**
	 * @brief What a strict light's cull does under a chain: closed, or untested, or tested against a room program's compound frustum.
	 * unmodelled means a multibound shape the engine would test, so the verdict is open.
	 */
	struct GateVerdict
	{
		bool open = true, untested = false, unmodelled = false;
		std::uint32_t program = kPortalNoProgram;
	};
	GateVerdict Evaluate(std::span<const GateLink> a_chain, const Context& a_context);

	bool Admit(const Context& a_context, std::span<const GateLink> a_chain, const float a_centre[3], float a_radius);

	/**
	 * @brief The verdicts publication: an 8-word header, then each gate's verdict per context of the atlas.
	 * Gate 0 is the empty chain, open everywhere, used by objects past the column and gates past the count.
	 */
	inline constexpr std::uint32_t kGateHeaderWords = 8;
	inline constexpr std::uint32_t kGateClosed = 0xFFFFFFFEu;
	/** @brief A gate's verdict word for one context. Camera contexts are open. */
	std::uint32_t GateWord(std::span<const GateLink> a_chain, const Context& a_context);
}
