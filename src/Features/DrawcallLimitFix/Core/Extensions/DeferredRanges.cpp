// The deferred ranges' table.

#include "Features/DrawcallLimitFix/Core/Extensions/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "RenderGraph/Fatal.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures, before DCLF's threads exist. Read-only after.
		DeferredRangeSet rangeTable{};
	}

	void ResolveDeferredRanges(const std::array<Api::DeferredRangeContribution, kDeferredRanges>& a_registered)
	{
		for (std::size_t r = 0; r < kDeferredRanges; ++r) {
			const auto& registered = a_registered[r];
			if (registered.slot == Api::DeferredRangeSlot::Count)
				continue;
			// An untaken family has no keys, so the range stays empty. A taken one must be built for the deferred ranges.
			const auto& family = ProgramFamilyOf(registered.family);
			if (family.taken && family.contribution.build != Api::ProgramBuild::DeferredRanges)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("deferred range {} draws program family {}, which is not built for the deferred ranges",
					registered.name, family.name));
			auto& entry = rangeTable[r];
			entry.taken = true;
			entry.name = registered.name;
			entry.colourRange = "cs.dclf.deferred." + entry.name;
			entry.contribution = registered;
			entry.contribution.name = entry.name;
		}
	}

	const DeferredRangeSet& DeferredRanges() { return rangeTable; }

	DeferredDraws DeferredDrawsOf(const PipelineKey& a_key)
	{
		DeferredDraws out;
		if (!FamilyRules(a_key.rasterFlags).deferredRanges)
			return out;
		const Api::ProgramFamily family = KeyFamily(a_key.rasterFlags);
		const Api::DeferredKey key{ a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.rasterFlags, a_key.passDescriptor, a_key.vertexLayout };
		for (std::size_t r = 0; r < kDeferredRanges; ++r) {
			const auto& entry = rangeTable[r];
			std::uint8_t reference = 0;
			if (!entry.taken || entry.contribution.family != family || !entry.contribution.draws(key, reference))
				continue;
			out.ranges |= 1u << r;
			out.stencilRef[r] = reference;
		}
		return out;
	}
}
