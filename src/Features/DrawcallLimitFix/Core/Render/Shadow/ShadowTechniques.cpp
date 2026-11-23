// The closed set of Utility techniques the view pipelines compose, for the startup stage batch.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowTechniques.h"

#include <algorithm>

namespace DCLF::Draws
{
	DerivedUtilityTechniques EnumerateDerivedUtilityTechniques()
	{
		DerivedUtilityTechniques out;
		std::vector<std::uint32_t> shadow;
		const auto bases = AllShadowUtilityTechniques();
		out.casterBases = static_cast<std::uint32_t>(bases.size());
		for (std::uint32_t m = 0; m < kFirstOcclusionMode; ++m)
			for (const std::uint32_t base : bases)
				shadow.push_back(ViewPipelineTechnique(base, m));
		std::ranges::sort(shadow);
		shadow.erase(std::ranges::unique(shadow).begin(), shadow.end());
		out.shadow = static_cast<std::uint32_t>(shadow.size());

		// A map's rule is pure over the object's flags and alpha test. These are the flags the rules read for their technique.
		// The rest only reject an occluder. LOD never occludes, so its flags are left out.
		using PropertyFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		constexpr std::uint64_t kRead[] = { static_cast<std::uint64_t>(PropertyFlag::kZBufferWrite), static_cast<std::uint64_t>(PropertyFlag::kVertexColors),
			static_cast<std::uint64_t>(PropertyFlag::kTreeAnim), static_cast<std::uint64_t>(PropertyFlag::kSkinned) };
		constexpr std::uint32_t kCombinations = 1u << std::size(kRead);
		std::vector<std::uint32_t> occlusion;
		for (std::uint32_t map = 0; map < kOcclusionViews; ++map) {
			const auto& entry = Extensions::OcclusionMapOf(map);
			if (!entry.registered || !entry.technique)
				continue;
			for (std::uint32_t combination = 0; combination < kCombinations; ++combination) {
				Api::OccluderLeaf leaf;
				for (std::size_t i = 0; i < std::size(kRead); ++i)
					if (combination & (1u << i))
						leaf.flags |= kRead[i];
				for (const bool alphaTesting : { false, true }) {
					leaf.alphaTesting = alphaTesting;
					// Occlusion modes add no bits, but the composition is the shadow lookups' all the same.
					if (const std::uint32_t technique = entry.technique(leaf))
						occlusion.push_back(ViewPipelineTechnique(technique, OcclusionModeOf(map)));
				}
			}
		}
		std::ranges::sort(occlusion);
		occlusion.erase(std::ranges::unique(occlusion).begin(), occlusion.end());
		out.occlusion = static_cast<std::uint32_t>(occlusion.size());

		out.techniques = std::move(shadow);
		out.techniques.insert(out.techniques.end(), occlusion.begin(), occlusion.end());
		out.techniques.push_back(kRefractionTechnique);
		std::ranges::sort(out.techniques);
		out.techniques.erase(std::ranges::unique(out.techniques).begin(), out.techniques.end());
		return out;
	}
}
