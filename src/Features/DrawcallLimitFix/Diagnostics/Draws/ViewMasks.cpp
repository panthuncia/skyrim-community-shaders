// View mask parity. Every payload input against its object's view mask, and every list input against the list's view types.
// Run by the builds ahead, one at a time.

#include "DrawsState.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	void CheckViewMasks(const DrawsBuiltAhead& a_built)
	{
		if (!ParityDue(a_built.frame))
			return;
		DCLF_ZONE("CS.DCLF.ViewMaskParity");
		const auto& a_draws = a_built.draws;
		const auto& a_tables = a_built.tables;
		auto& p = DrawsState::Get().viewMasks;
		std::uint64_t inputs = 0, differ = 0;
		// The main build's bits, and the CategoryRoot views' bits resolved at install.
		const std::uint32_t mainSideBits = MainSideBits(), rootViewBits = Extensions::Views().rootViewBits;
		// a_list is 0 or 1 for the main part, undrawable or drawable, and 2 + mode for a shadow mode.
		// Every main input is taken by a segment, and a drawable one by both, except decals.
		auto checkMain = [&](const DrawInput& a_input) {
			++inputs;
			const std::uint32_t expected = MainMaskOf(a_tables, a_input.objectIndex);
			const std::uint32_t mask = a_input.view.mask & mainSideBits;
			// A forward object is the colour segment's alone, as a decal is.
			const bool colourOnly = ObjectDecalGroup(a_input.flags) != 0 || (a_input.flags & kObjectForward) != 0;
			const bool drawable = (a_input.flags & kInputDrawable) != 0;
			if (mask == expected && (mask & (kDepthViewBits | kColourViewBits)) &&
				(!drawable || ((mask & kColourViewBits) && (colourOnly || (mask & kDepthViewBits)))))
				return;
			// A CategoryRoot view member's mask is its view bit alone.
			if (mask == expected && (mask & rootViewBits) && !(mask & ~rootViewBits))
				return;
			++differ;
			std::uint64_t none = ~0ull;
			if (p.first.compare_exchange_strong(none, (std::uint64_t(drawable ? 1 : 0) << 56) | (std::uint64_t(a_input.objectIndex & 0xFFFFFFFFu) << 24) | (expected & 0xFFFFFFu)))
				p.firstMask.store(mask, std::memory_order_relaxed);
		};
		if (const auto& payload = a_draws.payloads[kAsyncColour] ? a_draws.payloads[kAsyncColour] : a_draws.payloads[kAsyncZPrepass]) {
			// Each scene list entry holds a main part, shadow words, or both, never neither.
			for (std::size_t i = 0; i < payload->resident.Count(); ++i) {
				const DrawInput& input = *payload->resident.At(i);
				if (input.view.mask & mainSideBits)
					checkMain(input);
				else if (!input.view.mask) {
					++inputs;
					++differ;
				}
			}
			for (const auto& input : payload->body->inputList)
				checkMain(input);
		}
		// An input's shadow mask is the views of the modes holding it, never outside its object's phases.
		if (const auto& shadowPayload = a_draws.shadow)
			shadowPayload->ForEachInput([&](const DrawInput& a_input) {
				const std::uint32_t shadowMask = a_input.view.mask & ~mainSideBits;
				if (!shadowMask)
					return;  // a scene entry's main part only
				++inputs;
				const std::uint32_t phases = ViewMaskOf(a_tables, a_input.objectIndex, false);
				std::uint32_t list = 0;
				bool ok = (shadowMask & ~phases) == 0;
				for (std::uint32_t m = 0; m < kShadowModeCount && ok; ++m)
					if (shadowPayload->inputs.modeUsed[m] && (a_input.view.mask & ModeViewBits(m))) {
						ok = ShadowKeyOf(m, a_input) != ~0u;
						list = 2 + m;
					}
				if (ok)
					return;
				++differ;
				std::uint64_t none = ~0ull;
				if (p.first.compare_exchange_strong(none, (std::uint64_t(list) << 56) | (std::uint64_t(a_input.objectIndex & 0xFFFFFFFFu) << 24) | (phases & 0xFFFFFFu)))
					p.firstMask.store(a_input.view.mask, std::memory_order_relaxed);
			});
		p.checks.fetch_add(1, std::memory_order_relaxed);
		p.inputs.fetch_add(inputs, std::memory_order_relaxed);
		p.differ.fetch_add(differ, std::memory_order_relaxed);
	}
}
