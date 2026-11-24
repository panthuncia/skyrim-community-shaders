// The combination expander's rules. Each rule is one function over the listed keys, adding neighbours through an emitter that
// applies the caps and the unbuildable list.

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/PipelineExpander.h"

#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Utils/SwitchFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace DCLF
{
	namespace PipelineExpanderDetail
	{
		// Caps, as shares of the listed keys a rule reads. All rules together add at most kExpandTotalShare of the listed keys, so the
		// set stays under 2.5 times what sessions built. Each build costs 20 to 300 ms, and up to 2 s in a cold driver.
		constexpr double kExpandRasterSidesShare = 0.5;
		constexpr double kExpandLayoutShare = 0.5;
		constexpr double kExpandRasterGroupShare = 0.25;
		constexpr double kExpandShadowModeShare = 0.5;
		constexpr double kExpandTotalShare = 1.5;
		// Of each raster group a key was not seen in, how many of the group's most common raster values it takes.
		constexpr std::size_t kExpandGroupValuesPerKey = 2;

		// Main identity words (PipelineKeySource.h).
		constexpr std::size_t kExpandVS = 0, kExpandRaster = 2, kExpandPass = 3, kExpandLayoutLow = 4, kExpandLayoutHigh = 5;
		// Shadow identity words: technique, raster, layout low and high, then the format and the rasterizer state.
		constexpr std::size_t kExpandShadowTechnique = 0, kExpandShadowFormat = 4;

		// Raster bits the engine's state index leaves out (RasterStateBits), so a key's state words do not change with them.
		constexpr std::uint32_t kExpandMinorRasterBits = kRasterTwoSided | kRasterDecalNudge;
		// The raster groups: decal group, terrain group and forward.
		constexpr std::uint32_t kExpandRasterGroupBits = (3u << kRasterDecalGroupShift) | (3u << kRasterTerrainGroupShift) | kRasterForward;

		/** @brief A listed main line, with the fields the rules group by. */
		struct ExpandMainLine
		{
			const std::string* tag = nullptr;
			const PipelineWords* identity = nullptr;
			const PipelineWords* inputs = nullptr;
			std::uint32_t technique = 0;
			bool lighting = false;

			std::uint32_t Word(std::size_t a_index) const { return (*identity)[a_index]; }
			std::uint64_t Layout() const { return std::uint64_t{ Word(kExpandLayoutLow) } | (std::uint64_t{ Word(kExpandLayoutHigh) } << 32); }
		};

		/** @brief What every rule reads: the listed lines, split by kind, and each engine state's words by its bits. */
		struct ExpandView
		{
			std::vector<ExpandMainLine> main;
			std::size_t lightingLines = 0;
			std::vector<const PipelineWords*> shadow;
			std::map<std::uint32_t, const PipelineWords*> stateWords;
		};

		/** @brief Adds a rule's keys to the set under the rule's cap and the total cap, skipping listed and unbuildable keys. */
		class ExpandEmitter
		{
		public:
			ExpandEmitter(PipelineKeySet& a_set, const PipelineManifestLines& a_unbuildable, std::size_t a_total, std::size_t& a_skipped) :
				set(a_set), unbuildable(a_unbuildable), totalLeft(a_total), skipped(a_skipped)
			{}

			/** @brief Starts a rule allowed a_cap keys. */
			void Begin(std::size_t a_cap)
			{
				ruleLeft = std::min(a_cap, totalLeft);
				added = 0;
			}
			bool Full() const { return ruleLeft == 0; }
			std::size_t Added() const { return added; }

			void Emit(const std::string& a_tag, const PipelineWords& a_identity, const PipelineWords& a_inputs)
			{
				if (ruleLeft == 0 || set.Contains(a_tag, a_identity))
					return;
				if (const auto tagLines = unbuildable.find(a_tag); tagLines != unbuildable.end() && tagLines->second.contains(a_identity)) {
					++skipped;
					return;
				}
				if (!set.AddExpanded(a_tag, a_identity, a_inputs))
					return;
				--ruleLeft;
				--totalLeft;
				++added;
			}

		private:
			PipelineKeySet& set;
			const PipelineManifestLines& unbuildable;
			std::size_t totalLeft = 0;
			std::size_t ruleLeft = 0;
			std::size_t added = 0;
			std::size_t& skipped;
		};

		std::size_t ExpandCap(double a_share, std::size_t a_lines)
		{
			return static_cast<std::size_t>(std::ceil(a_share * static_cast<double>(a_lines)));
		}

		/**
		 * @brief Two-sided and decal nudge variants. For each Lighting technique and raster group, the minor raster values seen with
		 * any of its keys, applied to every key of it. The engine state words do not change, so each comes from its source key.
		 */
		void ExpandRasterSides(const ExpandView& a_view, ExpandEmitter& a_out, const PipelineCombinationExpander::StateBitsOf& a_stateBitsOf)
		{
			std::map<std::pair<std::uint32_t, std::uint32_t>, std::set<std::uint32_t>> seen;
			for (const auto& line : a_view.main)
				if (line.lighting)
					seen[{ line.technique, line.Word(kExpandRaster) & ~kExpandMinorRasterBits }].insert(line.Word(kExpandRaster) & kExpandMinorRasterBits);
			for (const auto& line : a_view.main) {
				if (a_out.Full())
					return;
				if (!line.lighting)
					continue;
				const std::uint32_t raster = line.Word(kExpandRaster);
				for (const std::uint32_t minor : seen[{ line.technique, raster & ~kExpandMinorRasterBits }]) {
					if (minor == (raster & kExpandMinorRasterBits))
						continue;
					PipelineWords identity = *line.identity;
					identity[kExpandRaster] = (raster & ~kExpandMinorRasterBits) | minor;
					if (a_stateBitsOf(identity) != (*line.inputs)[0])
						continue;
					a_out.Emit(*line.tag, identity, *line.inputs);
				}
			}
		}

		/**
		 * @brief Vertex layouts. For each tag, technique and vertex stage, the layouts seen with it, applied to every key with that
		 * vertex stage. The vertex stage was built with each of those layouts, so its attributes are there. Only the layout changes,
		 * so the state words come from the source key.
		 */
		void ExpandLayouts(const ExpandView& a_view, ExpandEmitter& a_out, const PipelineCombinationExpander::StateBitsOf&)
		{
			std::map<std::tuple<std::string, std::uint32_t, std::uint32_t>, std::set<std::uint64_t>> seen;
			for (const auto& line : a_view.main)
				seen[{ *line.tag, line.technique, line.Word(kExpandVS) }].insert(line.Layout());
			for (const auto& line : a_view.main) {
				if (a_out.Full())
					return;
				for (const std::uint64_t layout : seen[{ *line.tag, line.technique, line.Word(kExpandVS) }]) {
					if (layout == line.Layout())
						continue;
					PipelineWords identity = *line.identity;
					identity[kExpandLayoutLow] = static_cast<std::uint32_t>(layout);
					identity[kExpandLayoutHigh] = static_cast<std::uint32_t>(layout >> 32);
					a_out.Emit(*line.tag, identity, *line.inputs);
				}
			}
		}

		/**
		 * @brief Raster groups (decal, terrain, forward). For a Lighting key, each other group seen with its technique and vertex
		 * stage, at that group's most common raster values. The new key's state words are those a listed key with the same state
		 * bits recorded. Refraction keys are left alone, since their Lighting pipeline is never drawn.
		 */
		void ExpandRasterGroups(const ExpandView& a_view, ExpandEmitter& a_out, const PipelineCombinationExpander::StateBitsOf& a_stateBitsOf)
		{
			using GroupKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;  // technique, vertex stage, group
			std::map<GroupKey, std::map<std::uint32_t, std::size_t>> seen;
			for (const auto& line : a_view.main) {
				const std::uint32_t raster = line.Word(kExpandRaster);
				if (line.lighting && !RasterRefraction(raster))
					++seen[{ line.technique, line.Word(kExpandVS), raster & kExpandRasterGroupBits }][raster];
			}
			// Each group's raster values, most common first.
			std::map<GroupKey, std::vector<std::uint32_t>> common;
			for (const auto& [group, counts] : seen) {
				std::vector<std::pair<std::size_t, std::uint32_t>> ranked;
				for (const auto& [raster, count] : counts)
					ranked.emplace_back(count, raster);
				std::ranges::sort(ranked, [](const auto& a_left, const auto& a_right) {
					return a_left.first != a_right.first ? a_left.first > a_right.first : a_left.second < a_right.second;
				});
				auto& values = common[group];
				for (std::size_t i = 0; i < ranked.size() && i < kExpandGroupValuesPerKey; ++i)
					values.push_back(ranked[i].second);
			}
			for (const auto& line : a_view.main) {
				if (a_out.Full())
					return;
				const std::uint32_t raster = line.Word(kExpandRaster);
				if (!line.lighting || RasterRefraction(raster))
					continue;
				const std::uint32_t vertex = line.Word(kExpandVS);
				const std::uint32_t group = raster & kExpandRasterGroupBits;
				for (auto it = common.lower_bound({ line.technique, vertex, 0u }); it != common.end() && std::get<0>(it->first) == line.technique && std::get<1>(it->first) == vertex;
					 ++it) {
					if (std::get<2>(it->first) == group)
						continue;
					for (const std::uint32_t value : it->second) {
						PipelineWords identity = *line.identity;
						identity[kExpandRaster] = value;
						const auto bits = a_stateBitsOf(identity);
						if (!bits)
							continue;
						const auto words = a_view.stateWords.find(*bits);
						if (words == a_view.stateWords.end())
							continue;
						a_out.Emit(*line.tag, identity, *words->second);
					}
				}
			}
		}

		/**
		 * @brief Shadow modes. A shadow key's caster draws in every shadow mode, each with its own view technique (the caster's
		 * technique plus the mode's bits, canonical: ViewPipelineTechnique). Each listed key is crossed with every mode's view
		 * technique and every rasterizer state listed for that mode. Only keys of the three shadow modes take part.
		 */
		void ExpandShadowModes(const ExpandView& a_view, ExpandEmitter& a_out, const PipelineCombinationExpander::StateBitsOf&)
		{
			std::array<std::uint32_t, PassCapture::kShadowModes> modeBits{};
			std::uint32_t modeMask = 0;
			for (std::uint32_t m = 0; m < PassCapture::kShadowModes; ++m) {
				modeBits[m] = ShadowModeBits(PassCapture::kFirstShadowMode + m);
				modeMask |= modeBits[m];
			}
			const auto modeOf = [&](std::uint32_t a_technique) -> std::uint32_t {
				for (std::uint32_t m = 0; m < PassCapture::kShadowModes; ++m)
					if ((a_technique & modeMask) == modeBits[m])
						return m;
				return PassCapture::kShadowModes;
			};
			// Each mode's listed format and rasterizer state words.
			std::array<std::set<PipelineWords>, PassCapture::kShadowModes> states;
			for (const auto* identity : a_view.shadow)
				if (const auto m = modeOf((*identity)[kExpandShadowTechnique]); m < PassCapture::kShadowModes)
					states[m].insert(PipelineWords(identity->begin() + kExpandShadowFormat, identity->end()));
			const std::string tag(kShadowPipelineTag);
			for (const auto* identity : a_view.shadow) {
				if (a_out.Full())
					return;
				const std::uint32_t technique = (*identity)[kExpandShadowTechnique];
				if (modeOf(technique) == PassCapture::kShadowModes)
					continue;
				const std::uint32_t base = technique & ~modeMask;
				for (std::uint32_t m = 0; m < PassCapture::kShadowModes; ++m) {
					const std::uint32_t viewTechnique = CanonicalShadowTechnique(base | modeBits[m]);
					for (const auto& state : states[m]) {
						if (state[0] != (*identity)[kExpandShadowFormat])
							continue;
						PipelineWords expanded(identity->begin(), identity->begin() + kExpandShadowFormat);
						expanded[kExpandShadowTechnique] = viewTechnique;
						expanded.insert(expanded.end(), state.begin(), state.end());
						a_out.Emit(tag, expanded, {});
					}
				}
			}
		}

		using ExpandRule = void (*)(const ExpandView&, ExpandEmitter&, const PipelineCombinationExpander::StateBitsOf&);
		enum class ExpandBase
		{
			Lighting,
			Main,
			Shadow,
		};
		struct ExpandRuleEntry
		{
			const char* name;
			ExpandRule rule;
			double share;
			ExpandBase base;
		};
		// In order of precision, since the total cap may stop the later ones.
		constexpr std::array<ExpandRuleEntry, 4> kExpandRules{ {
			{ "shadow modes", &ExpandShadowModes, kExpandShadowModeShare, ExpandBase::Shadow },
			{ "raster sides", &ExpandRasterSides, kExpandRasterSidesShare, ExpandBase::Lighting },
			{ "layouts", &ExpandLayouts, kExpandLayoutShare, ExpandBase::Main },
			{ "raster groups", &ExpandRasterGroups, kExpandRasterGroupShare, ExpandBase::Lighting },
		} };
	}

	void PipelineCombinationExpander::Collect(PipelineKeySet& a_set)
	{
		using namespace PipelineExpanderDetail;
		ruleCounts.clear();
		skippedUnbuildable = 0;
		if (::Util::SwitchFile::Value("CS_DCLF_PIPELINE_EXPAND") == "0") {
			logger::info("[DCLF] pipeline expander: off (CS_DCLF_PIPELINE_EXPAND=0)");
			return;
		}
		// A copy, since the rules add to the set while reading what was listed.
		const PipelineManifestLines listed = a_set.Lines();
		const PipelineManifestLines unbuildable = PipelineManifest::Unbuildable().Lines();
		ExpandView view;
		for (const auto& [tag, tagLines] : listed) {
			if (tag == kShadowPipelineTag) {
				for (const auto& [identity, inputs] : tagLines)
					if (identity.size() == kShadowManifestIdentityWords)
						view.shadow.push_back(&identity);
				continue;
			}
			if (!IsMainPipelineTag(tag))
				continue;
			const bool lighting = tag == "lighting";
			for (const auto& [identity, inputs] : tagLines) {
				if (identity.size() != kMainManifestIdentityWords || inputs.size() != kManifestStateWords)
					continue;
				view.main.push_back({ &tag, &identity, &inputs, (identity[kExpandPass] >> 24) & 0x3f, lighting });
				view.lightingLines += lighting ? 1u : 0u;
				view.stateWords.try_emplace(inputs[0], &inputs);
			}
		}
		const std::size_t listedKeys = view.main.size() + view.shadow.size();
		ExpandEmitter emitter(a_set, unbuildable, ExpandCap(kExpandTotalShare, listedKeys), skippedUnbuildable);
		for (const auto& entry : kExpandRules) {
			const std::size_t base = entry.base == ExpandBase::Lighting ? view.lightingLines : entry.base == ExpandBase::Main ? view.main.size() : view.shadow.size();
			emitter.Begin(ExpandCap(entry.share, base));
			entry.rule(view, emitter, stateBitsOf);
			ruleCounts.emplace_back(entry.name, emitter.Added());
			logger::info("[DCLF] pipeline expander: rule \"{}\" added {} keys (cap {})", entry.name, emitter.Added(), ExpandCap(entry.share, base));
		}
		if (skippedUnbuildable)
			logger::info("[DCLF] pipeline expander: {} keys left out, listed as unbuildable", skippedUnbuildable);
	}
}
