// The revision producer. It runs on the snapshot builder, one pass at a time. MakeRevision makes a revision's shapes and latches
// from a publication and the posted RevisionInputs, then AssembleRevision seals them. Also the main shape's plans, which the
// commits share.

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Utils/Hash.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	namespace Draws
	{
		void PlanZBuckets(const MainLayout& a_layout, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, ZBucketPlan& a_out)
		{
			const auto slots = static_cast<std::uint32_t>(a_layout.zBucketCapacity.size());
			const auto& groups = a_indirect.zGroups;
			const auto& keys = a_tables.pipelines;
			a_out.map.assign(slots, kNoBucket);
			a_out.calls.clear();
			std::vector<std::uint32_t> groupBucket(a_indirect.zPipelines.size(), kNoBucket);
			for (std::uint32_t p = 0; p < slots; ++p) {
				if (!a_layout.zBucketCapacity[p] || p >= a_lookups.pipelines.size() || p >= keys.size())
					continue;
				const auto& entry = a_lookups.pipelines[p];
				if (entry.setIndex == Lookups::kNone || !(entry.key == keys[p]) || entry.setIndex >= groups.size())
					continue;
				// Keys outside the depth segment have no Z-prepass pipeline, so no bucket.
				if (!Extensions::FamilyRules(keys[p].rasterFlags).depthSegment)
					continue;
				const std::uint32_t group = groups[entry.setIndex];
				auto& bucket = groupBucket[group];
				// Terrain group 1 gets an equal twin bucket right after its own for its near blocks. BuildDrawsCS relies on this layout.
				const bool terrain = RasterTerrainGroup(keys[p].rasterFlags) == 1;
				if (bucket == kNoBucket) {
					bucket = a_out.Buckets();
					a_out.calls.push_back({ bucket, 0u, 0u, a_indirect.zPipelines[group] });
					if (terrain)
						a_out.calls.push_back({ bucket + 1, 0u, 0u, a_indirect.zPipelines[group], true });
				}
				a_out.calls[bucket].capacity += a_layout.zBucketCapacity[p];
				if (terrain)
					a_out.calls[bucket + 1].capacity += a_layout.zBucketCapacity[p];
				a_out.map[p] = terrain ? (bucket | kBucketTerrainPair) : bucket;
			}
			const std::uint32_t buckets = a_out.Buckets();
			a_out.table.assign(std::size_t(buckets) * 4, 0u);
			std::uint32_t first = 0;
			for (std::uint32_t b = 0; b < buckets; ++b) {
				auto& call = a_out.calls[b];
				call.first = first;
				a_out.table[2 * b] = first;
				a_out.table[2 * b + 1] = call.capacity;
				// Phase 2 uses the same ranges, offset past the first draw range.
				a_out.table[2 * (buckets + b)] = first + a_layout.sequenceDraws;
				a_out.table[2 * (buckets + b) + 1] = call.capacity;
				first += call.capacity;
			}
		}

		namespace
		{
			/**
			 * @brief A slot's state class and colour range. Must route the same way as BuildDrawsCS does by object flags.
			 * False when the slot has no pipeline in the set version, or no class call draws it, as with refraction.
			 */
			bool SlotClassRange(const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, std::uint32_t a_slot, std::uint32_t& a_class,
				std::uint32_t& a_range)
			{
				if (a_slot >= a_lookups.pipelines.size() || a_slot >= a_tables.pipelines.size())
					return false;
				const auto& entry = a_lookups.pipelines[a_slot];
				const auto& key = a_tables.pipelines[a_slot];
				if (entry.setIndex == Lookups::kNone || !(entry.key == key) || entry.setIndex >= a_indirect.classOf.size() || a_indirect.classOf[entry.setIndex] == kNoSetClass)
					return false;
				a_class = a_indirect.classOf[entry.setIndex] >> kSetClassShift;
				const std::uint32_t flags = key.rasterFlags;
				if (const std::uint32_t group = RasterDecalGroup(flags))
					a_range = ClassRangeOfSlot(group - 1);
				else if (RasterForward(flags))
					a_range = kClassRangeForward;
				else if (const std::uint32_t terrain = RasterTerrainGroup(flags))
					a_range = ClassRangeOfSlot(kDecalGroups + terrain - 1);
				else
					a_range = kClassRangeMain;
				return a_class < a_indirect.classes.size() && !(a_range == kClassRangeForward && RasterRefraction(flags));
			}

			/** @brief Lay out a_plan's sub-ranges from their capacities, and fill its latch words. */
			void LayOutSetClasses(const MainLayout& a_layout, const IndirectState& a_indirect, std::span<const std::array<bool, kDecalGroups>> a_decals,
				std::span<const std::uint32_t> a_mainSets, std::span<const std::uint32_t> a_terrainSets,
				std::span<const std::uint32_t> a_terrainGroups, std::span<const std::uint32_t> a_decalGroups, SetClassPlan& a_plan)
			{
				auto& classes = a_plan.classes;
				std::uint32_t keys = 0, main = 0, decals = 0;
				std::array<std::uint32_t, kTerrainGroups> terrain{ a_layout.sequenceDraws, a_layout.sequenceDraws + a_layout.terrainCapacity[0] };
				std::uint32_t forward = a_layout.sequenceDraws + a_layout.terrainCapacity[0] + a_layout.terrainCapacity[1];
				for (std::size_t c = 0; c < classes.size(); ++c) {
					auto& ranges = classes[c].ranges;
					classes[c].keyBase = keys;
					classes[c].size = a_indirect.classes[c].pipelines;
					keys += classes[c].size;
					ranges[kClassRangeMain].first = main;
					main += ranges[kClassRangeMain].capacity;
					for (std::uint32_t g = 0; g < kTerrainGroups; ++g) {
						auto& range = ranges[ClassRangeOfSlot(kDecalGroups + g)];
						range.first = terrain[g];
						terrain[g] += range.capacity;
					}
					// Forward takes two halves per class, staging then sorted.
					auto& pair = ranges[kClassRangeForward];
					pair.first = forward;
					forward += 2 * pair.capacity;
				}
				for (std::uint32_t g = 0; g < kDecalGroups; ++g)
					for (std::size_t c = 0; c < classes.size(); ++c)
						if (a_decals[c][g])
							classes[c].ranges[ClassRangeOfSlot(g)] = { static_cast<std::uint32_t>(2ull * a_layout.sequenceDraws + std::uint64_t(decals++) * a_layout.sequenceDecals),
								a_layout.sequenceDecals };
				a_plan.decalRanges = decals;
				a_plan.table.assign(kClassHeaderWords, 0u);
				a_plan.table[0] = static_cast<std::uint32_t>(classes.size());
				a_plan.table[1] = kClassRanges;
				for (const auto& entry : classes) {
					const std::size_t at = a_plan.table.size();
					a_plan.table.resize(at + kClassWords, 0u);
					a_plan.table[at] = entry.keyBase;
					a_plan.table[at + 1] = entry.size;
					for (std::uint32_t r = 0; r < kClassRanges; ++r) {
						a_plan.table[at + 2 + 2 * r] = entry.ranges[r].first;
						a_plan.table[at + 3 + 2 * r] = entry.ranges[r].capacity;
					}
				}
				a_plan.map.assign(kClassHeaderWords, 0u);
				a_plan.map[0] = static_cast<std::uint32_t>(a_indirect.classOf.size());
				a_plan.map.insert(a_plan.map.end(), a_indirect.classOf.begin(), a_indirect.classOf.end());
				const bool deviceGenerated = DrawPipelines::Get().DeviceGenerated();
				// Plain buckets for one range. All or nothing: if any set index with draws lacks a plain colour pipeline, there are none,
				// or a frame would draw some draws twice or not at all.
				const auto plan = [&](bool a_on, std::span<const std::uint32_t> a_sets, std::span<const std::uint32_t> a_groups, std::span<std::uint32_t> a_firsts,
									  std::uint32_t a_halves, std::vector<SetClassPlan::PlainBucket>& a_buckets, std::uint32_t a_count) {
					a_buckets.clear();
					bool plain = a_on && std::ranges::any_of(a_sets, [](std::uint32_t a_capacity) { return a_capacity != 0; });
					std::uint32_t missing = kNoBucket;
					for (std::uint32_t set = 0; plain && set < a_sets.size(); ++set) {
						plain = !a_sets[set] || (set < a_indirect.plainColour.size() && a_indirect.plainColour[set].valid());
						if (!plain)
							missing = set;
					}
					if (!plain) {
						// Without device-generated commands this is a defect, because such a key should have failed its build.
						// The range's draws are dropped.
						static std::atomic<std::uint32_t> logged{ 0 };
						if (missing != kNoBucket && !deviceGenerated && logged.fetch_add(1) < 4)
							logger::error("[DCLF] plain buckets: set index {} has draws and no plain colour pipeline, and no device-generated commands draw it",
								missing);
						return;
					}
					std::vector<std::uint32_t> bucketOf(a_indirect.classOf.size(), kNoBucket);
					for (std::uint32_t g = 0; g < a_firsts.size(); ++g)
						for (std::uint32_t set = 0; set < a_sets.size(); ++set) {
							const std::uint32_t group = a_groups.empty() ? 0u : a_groups[set];
							if (!a_sets[set] || group != (a_groups.empty() ? 0u : g + 1))
								continue;
							bucketOf[set] = static_cast<std::uint32_t>(a_buckets.size());
							a_buckets.push_back({ set, a_firsts[g], a_sets[set], group });
							a_firsts[g] += a_halves * a_sets[set];
						}
					a_plan.map[a_count] = static_cast<std::uint32_t>(a_buckets.size());
					a_plan.map.insert(a_plan.map.end(), bucketOf.begin(), bucketOf.end());
					for (const auto& bucket : a_buckets) {
						a_plan.map.push_back(bucket.first);
						a_plan.map.push_back(bucket.capacity);
					}
				};
				// The forward section has no buckets: the CPU pipeline's forward site draws the forward family.
				std::array<std::uint32_t, 1> mainFirst{ 0u };
				std::array<std::uint32_t, kTerrainGroups> terrainFirsts{ a_layout.sequenceDraws, a_layout.sequenceDraws + a_layout.terrainCapacity[0] };
				plan(true, a_mainSets, {}, mainFirst, 1, a_plan.main, 2);
				plan(true, a_terrainSets, a_terrainGroups, terrainFirsts, 1, a_plan.terrain, 3);
				// Decal buckets take two decal sub-ranges per set index, after the classes' ones. All or nothing again, and groups with a
				// depth prepass also need a plain depth pipeline.
				a_plan.decals.clear();
				std::array<bool, kDecalGroups> depthPrepass{};
				for (const auto& range : Extensions::MainRanges().drawn)
					if (range.slot < kDecalGroups && range.depthPrepass)
						depthPrepass[range.slot] = true;
				bool plainDecals = std::ranges::any_of(a_decalGroups, [](std::uint32_t a_group) { return a_group != 0; });
				std::uint32_t declinedSet = kNoBucket;
				const char* declined = "";
				for (std::uint32_t set = 0; plainDecals && set < a_decalGroups.size(); ++set) {
					const std::uint32_t group = a_decalGroups[set];
					if (!group)
						continue;
					plainDecals = set < a_indirect.plainColour.size() && a_indirect.plainColour[set].valid();
					if (!plainDecals)
						declined = "no plain colour pipeline";
					if (plainDecals && depthPrepass[group - 1]) {
						plainDecals = set < a_indirect.plainDepth.size() && a_indirect.plainDepth[set].valid();
						if (!plainDecals)
							declined = "no plain depth pipeline for its group's depth prepass";
					}
					if (!plainDecals)
						declinedSet = set;
				}
				static std::atomic<std::uint64_t> lastOutcome{ ~0ull };
				const std::uint64_t outcome = plainDecals ? std::uint64_t(std::ranges::count_if(a_decalGroups, [](std::uint32_t a_group) { return a_group != 0; })) :
				                                            (1ull << 32) | declinedSet;
				if (lastOutcome.exchange(outcome, std::memory_order_relaxed) != outcome) {
					if (plainDecals)
						logger::info("[DCLF] decal buckets: {} decal set indices drawn plain", outcome);
					else if (declinedSet != kNoBucket)
						logger::info("[DCLF] decal buckets: none, decal set index {} (group {}) has {}", declinedSet, a_decalGroups[declinedSet], declined);
				}
				if (plainDecals) {
					std::vector<std::uint32_t> bucketOf(a_indirect.classOf.size(), kNoBucket);
					std::uint32_t ranges = decals;
					for (std::uint32_t g = 1; g <= kDecalGroups; ++g)
						for (std::uint32_t set = 0; set < a_decalGroups.size(); ++set) {
							if (a_decalGroups[set] != g)
								continue;
							bucketOf[set] = static_cast<std::uint32_t>(a_plan.decals.size());
							a_plan.decals.push_back({ set, static_cast<std::uint32_t>(2ull * a_layout.sequenceDraws + std::uint64_t(ranges) * a_layout.sequenceDecals),
								a_layout.sequenceDecals, g });
							ranges += 2;
						}
					a_plan.decalRanges = ranges;
					a_plan.map[4] = static_cast<std::uint32_t>(a_plan.decals.size());
					a_plan.map.insert(a_plan.map.end(), bucketOf.begin(), bucketOf.end());
					for (const auto& bucket : a_plan.decals) {
						a_plan.map.push_back(bucket.first);
						a_plan.map.push_back(bucket.capacity);
					}
				}
			}
		}

		void PlanSetClasses(const MainLayout& a_layout, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, SetClassPlan& a_out)
		{
			a_out = {};
			a_out.classes.resize(a_indirect.classes.size());
			std::vector<std::array<bool, kDecalGroups>> decals(a_indirect.classes.size());
			std::vector<std::uint32_t> mainSets(a_indirect.classOf.size(), 0u), terrainSets(a_indirect.classOf.size(), 0u),
				terrainGroups(a_indirect.classOf.size(), 0u), decalGroups(a_indirect.classOf.size(), 0u);
			const auto slots = static_cast<std::uint32_t>(std::min(a_layout.slotCapacity.size(), a_tables.pipelines.size()));
			for (std::uint32_t p = 0; p < slots; ++p) {
				std::uint32_t c = 0, r = 0;
				if (!SlotClassRange(a_lookups, a_tables, a_indirect, p, c, r))
					continue;
				if (r >= ClassRangeOfSlot(0) && r < ClassRangeOfSlot(kDecalGroups)) {
					decals[c][r - ClassRangeOfSlot(0)] = true;
					decalGroups[a_lookups.pipelines[p].setIndex] = r - ClassRangeOfSlot(0) + 1;
				} else
					a_out.classes[c].ranges[r].capacity += a_layout.slotCapacity[p];
				if (r == kClassRangeMain)
					mainSets[a_lookups.pipelines[p].setIndex] += a_layout.slotCapacity[p];
				else if (r >= ClassRangeOfSlot(kDecalGroups) && r < ClassRangeOfSlot(kDecalGroups + kTerrainGroups)) {
					const std::uint32_t set = a_lookups.pipelines[p].setIndex;
					terrainSets[set] += a_layout.slotCapacity[p];
					terrainGroups[set] = r - ClassRangeOfSlot(kDecalGroups) + 1;
				}
			}
			LayOutSetClasses(a_layout, a_indirect, decals, mainSets, terrainSets, terrainGroups, decalGroups, a_out);
			static std::atomic<std::uint64_t> lastBuckets{ ~0ull };
			const std::uint64_t buckets = std::uint64_t(a_out.main.size()) | (std::uint64_t(a_out.terrain.size()) << 16) | (std::uint64_t(a_out.decals.size()) << 32);
			if (lastBuckets.exchange(buckets, std::memory_order_relaxed) != buckets)
				logger::info("[DCLF] plain buckets: main {}, terrain {}, decals {} (of {} set indices, {} with main slots)", a_out.main.size(),
					a_out.terrain.size(), a_out.decals.size(), a_indirect.classOf.size(),
					std::ranges::count_if(mainSets, [](std::uint32_t a_capacity) { return a_capacity != 0; }));
		}

		std::vector<LatchedCopy> MainLatchedLayout(const Resources& a_resources, bool a_depthOnly, const FrameBlockSizes& a_blocks, std::uint32_t a_buckets,
			std::uint32_t a_classWords)
		{
			std::vector<LatchedCopy> out;
			const auto targets = a_resources.latchedTargets.load(std::memory_order_acquire);
			std::size_t used = 0;
			// A target the passes did not declare is staged, not latched. Must match LatchedUploads.
			auto add = [&](const void* a_key, std::size_t a_bytes, std::uint64_t a_offset) {
				if (!a_key || !a_bytes || !targets || std::find(targets->begin(), targets->end(), a_key) == targets->end())
					return;
				const std::size_t at = (used + 15) & ~std::size_t(15);
				out.push_back({ a_key, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(a_bytes), a_offset });
				used = at + a_bytes;
			};
			// Must follow CommitMainPayload's order.
			const void* constants = a_resources.frameConstants.get();
			if (!a_depthOnly)
				for (const auto& frameBuffer : a_resources.frameBuffers)
					if (frameBuffer.copy)
						add(frameBuffer.copy.get(), std::size_t(frameBuffer.elements) * frameBuffer.stride, 0);
			add(constants, sizeof(DrawBindings), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				add(constants, a_blocks.vs[slot], FrameSlotOffset(false, slot));
				add(constants, a_blocks.ps[slot], FrameSlotOffset(true, slot));
			}
			auto addConstants = [&](std::size_t a_bytes, std::uint64_t a_offset) { add(constants, a_bytes, a_offset); };
			FrameRowWrites([](void* a_add, std::size_t a_bytes, std::uint64_t a_offset) { (*static_cast<decltype(addConstants)*>(a_add))(a_bytes, a_offset); }, &addConstants);
			if (a_depthOnly)
				Extensions::LatchSceneResources(a_resources.scene->sceneResources,
					[](void* a_add, const std::shared_ptr<org::Buffer>& a_buffer, const Versioned& a_versioned, std::size_t a_bytes) {
						(*static_cast<decltype(add)*>(a_add))(a_versioned ? a_versioned->Key() : a_buffer.get(), a_bytes, 0);
					},
					&add);
			add(a_resources.count.get(), a_depthOnly ? sizeof(kZeroCounts) : sizeof(std::uint32_t), 0);
			if (a_depthOnly)
				add(a_resources.retestDispatch.get(), sizeof(kRetestDispatchReset), 0);
			if (!a_depthOnly) {
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), sizeof(std::uint32_t), kCountDecalLayerWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), kTerrainGroups * sizeof(std::uint32_t), kCountTerrainGroupWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountForwardWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountRefractionWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), 2 * sizeof(std::uint32_t), kCountClassUnplannedWord * sizeof(std::uint32_t));
				if (a_resources.classCounts)
					add(a_resources.classCounts->Key(), std::size_t(a_classWords) * sizeof(std::uint32_t), 0);
			}
			if (a_depthOnly && a_resources.pool && a_buckets)
				for (const auto& counts : a_resources.zBucketCounts)
					if (counts)
						add(counts->Key(), std::size_t(a_buckets) * sizeof(std::uint32_t), 0);
			return out;
		}

		std::size_t LatchedBytes(const std::vector<LatchedCopy>& a_copies)
		{
			return a_copies.empty() ? 0 : std::size_t(a_copies.back().latchOffset) + a_copies.back().bytes;
		}

		/** @brief A view's latched frame value copies, packed from a_at. Must follow CommitView's order. */
		std::vector<LatchedCopy> ViewFrameLatched(const void* a_key, const std::vector<FrameBuffer>* a_frameBuffers, const FrameBlockSizes& a_depth, std::uint32_t a_at,
			std::uint32_t a_records)
		{
			std::vector<LatchedCopy> out;
			std::size_t used = 0;
			auto addTo = [&](const void* a_target, std::size_t a_bytes, std::uint64_t a_offset) {
				if (!a_bytes)
					return;
				const std::size_t at = (used + 15) & ~std::size_t(15);
				out.push_back({ a_target, static_cast<std::uint32_t>(a_at + at), static_cast<std::uint32_t>(a_bytes), a_offset });
				used = at + a_bytes;
			};
			auto add = [&](std::size_t a_bytes, std::uint64_t a_offset) { addTo(a_key, a_bytes, a_offset); };
			for (const auto& frameBuffer : a_frameBuffers ? std::span<const FrameBuffer>(*a_frameBuffers) : std::span<const FrameBuffer>())
				if (frameBuffer.copy)
					addTo(frameBuffer.copy.get(), std::size_t(frameBuffer.elements) * frameBuffer.stride, 0);
			add(sizeof(DrawBindings) * a_records, std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
			add(a_depth.ps[kSharedDataRegister], FrameSlotOffset(true, kSharedDataRegister));
			add(a_depth.ps[kFeatureDataRegister], FrameSlotOffset(true, kFeatureDataRegister));
			FrameRowWrites([](void* a_add, std::size_t a_bytes, std::uint64_t a_offset) { (*static_cast<decltype(add)*>(a_add))(a_bytes, a_offset); }, &add);
			return out;
		}

		void RequireLatchedBlock(GrowthBuild& a_build, LatchHandle a_latch, const std::vector<LatchedCopy>& a_copies)
		{
			if (const std::size_t bytes = LatchedBytes(a_copies))
				SceneBufferSet::Require(a_build, a_latch, static_cast<std::uint32_t>(bytes), 0);
		}

		MainShapeInputs MainShapeInputsOf(const Resources&, const MainLayout& a_layout, const NamedVersions&, bool a_depthOnly, std::uint32_t a_cullMode)
		{
			MainShapeInputs in;
			in.depthOnly = a_depthOnly;
			in.sequenceDraws = a_layout.sequenceDraws;
			in.sequenceDecals = a_layout.sequenceDecals;
			in.terrainCapacity = a_layout.terrainCapacity;
			in.forwardCapacity = a_layout.forwardCapacity;
			in.refractionCapacity = a_layout.refractionCapacity;
			in.cullMode = a_cullMode;
			return in;
		}

		std::shared_ptr<PassFrame> MakeMainShape(const MainShapeInputs& a_in)
		{
			auto frame = std::make_shared<PassFrame>();
			frame->sequenceDraws = a_in.sequenceDraws;
			frame->sequenceDecals = a_in.sequenceDecals;
			// Size from slot capacities, not from the tables the shape was made from. A later publication under the same revision
			// may hold more draws, and the commit would find the shape short. The depth segment uses plain bucketed draws instead.
			if (!a_in.depthOnly) {
				frame->classPlan = a_in.classPlan;
				if (a_in.classPlan)
					frame->classes = a_in.classPlan->classes;
				frame->terrainCapacity = a_in.terrainCapacity;
				frame->forwardCapacity = a_in.forwardCapacity;
				frame->refractionCapacity = a_in.refractionCapacity;
			}
			frame->width = a_in.viewport.width;
			frame->height = a_in.viewport.height;
			frame->minDepth = a_in.viewport.minDepth;
			frame->maxDepth = a_in.viewport.maxDepth;
			frame->resourceHeap = a_in.resourceHeap;
			frame->samplerHeap = a_in.samplerHeap;
			frame->indirect = a_in.indirect;
			frame->families = a_in.families;
			frame->cullMode = a_in.cullMode;
			frame->latch = a_in.latch;
			frame->latchLayout = a_in.latchLayout;
			frame->zCalls = a_in.zCalls;
			frame->zPlan = a_in.zPlan;
			frame->latched = a_in.latched;
			return frame;
		}

	}

	namespace
	{
		void MixWitness(std::uint64_t& a_hash, std::uint64_t a_value)
		{
			a_hash = (a_hash ^ a_value) * Util::Fnv1a::kPrime;
			a_hash ^= a_hash >> 29;
		}

		/**
		 * @brief A hash of only the lookup fields the requirements and shapes read. It skips versions and generations, because a
		 * material lookup write moves those without changing any shape.
		 */
		std::uint64_t LookupsWitness(const Lookups& a_lookups, const SceneTables& a_tables)
		{
			std::uint64_t hash = Util::Fnv1a::kOffset;
			const std::size_t slots = a_lookups.pipelines.size(), tabled = a_tables.pipelines.size();
			MixWitness(hash, a_lookups.instance);
			MixWitness(hash, slots);
			MixWitness(hash, tabled);
			for (std::size_t p = 0; p < slots; ++p) {
				const auto& entry = a_lookups.pipelines[p];
				const bool keyed = p < tabled && entry.key == a_tables.pipelines[p];
				MixWitness(hash, (std::uint64_t(entry.setIndex) << 1) | (keyed ? 1u : 0u));
				if (p < tabled)
					MixWitness(hash, a_tables.pipelines[p].rasterFlags);
			}
			MixWitness(hash, a_lookups.shadowSlotKeys.size());
			return hash;
		}

		/** @brief A hash of the map rows of every mode and rasterizer state pair the shadow layouts draw. */
		std::uint64_t ShadowRowsWitness(const Lookups& a_lookups, const std::vector<ShadowViewLayout>& a_placements, const std::vector<ShadowViewLayout>& a_occlusion)
		{
			std::uint64_t hash = Util::Fnv1a::kOffset;
			std::array<std::uint64_t, 16> seen{};
			std::size_t pairs = 0;
			const auto rows = [&](const std::vector<ShadowViewLayout>& a_layouts) {
				for (const auto& view : a_layouts) {
					const std::uint64_t pair = (std::uint64_t(view.modeIndex) << 32) | view.rasterState;
					if (std::find(seen.begin(), seen.begin() + pairs, pair) != seen.begin() + pairs)
						continue;
					if (pairs < seen.size())
						seen[pairs++] = pair;
					const auto row = a_lookups.ShadowMapRow(view.modeIndex, view.rasterState);
					MixWitness(hash, pair);
					MixWitness(hash, row.size());
					for (const std::uint32_t pipeline : row)
						MixWitness(hash, pipeline);
				}
			};
			rows(a_placements);
			rows(a_occlusion);
			return hash;
		}

		/** @brief A hash of the frame-moved values the requirement pass reads besides the inputs and the publication. */
		std::uint64_t VolatileWitness(const RevisionInputs& a_inputs)
		{
			std::uint64_t hash = Util::Fnv1a::kOffset;
			const auto targets = [&hash](const auto& a_latched) {
				const auto keys = a_latched.load(std::memory_order_acquire);
				MixWitness(hash, keys ? keys->size() + 1 : 0);
				if (keys)
					for (const void* key : *keys)
						MixWitness(hash, reinterpret_cast<std::uintptr_t>(key));
			};
			if (const auto& s = a_inputs.scene) {
				MixWitness(hash, s->visibilityEvents && s->visibilityReadback ? s->visibilityReadback->list->Wanted() : 0u);
				MixWitness(hash, s->pool ? s->pool->fragmentedAt.load(std::memory_order_relaxed) : 0u);
			}
			if (a_inputs.main)
				targets(a_inputs.main->latchedTargets);
			if (a_inputs.shadow)
				targets(a_inputs.shadow->latchedTargets);
			for (const auto& view : a_inputs.views) {
				const auto frameBuffers = view.resources ? view.resources->frameBuffers.load(std::memory_order_acquire) : nullptr;
				MixWitness(hash, frameBuffers ? frameBuffers->size() + 1 : 0);
				if (!frameBuffers)
					continue;
				for (const auto& frameBuffer : *frameBuffers) {
					MixWitness(hash, reinterpret_cast<std::uintptr_t>(frameBuffer.copy.get()));
					MixWitness(hash, (std::uint64_t(frameBuffer.elements) << 32) | frameBuffer.stride);
				}
			}
			return hash;
		}

		/**
		 * @brief A hash of exactly what the requirement pass reads from a_tables and the draw bound.
		 * Anything the pass starts reading must be added here, or a stale revision will be kept.
		 */
		std::uint64_t TablesWitness(const SceneTables& a_tables, const Draws::DrawBoundStore& a_bound)
		{
			std::uint64_t hash = Util::Fnv1a::kOffset;
			MixWitness(hash, a_tables.objects.size());
			MixWitness(hash, a_bound.draws);
			MixWitness(hash, a_bound.shadowVersion);
			MixWitness(hash, a_bound.perPipeline.size());
			for (const std::uint64_t draws : a_bound.perPipeline)
				MixWitness(hash, draws);
			MixWitness(hash, a_tables.pipelines.size());
			for (const auto& key : a_tables.pipelines) {
				MixWitness(hash, (std::uint64_t(key.vertexDescriptor) << 32) | key.pixelDescriptor);
				MixWitness(hash, (std::uint64_t(key.rasterFlags) << 32) | key.passDescriptor);
				MixWitness(hash, key.vertexLayout);
			}
			MixWitness(hash, a_tables.usedPipelineBits.size());
			for (const std::uint64_t word : a_tables.usedPipelineBits)
				MixWitness(hash, word);
			for (const std::uint32_t count : a_tables.rangeCount)
				MixWitness(hash, count);
			MixWitness(hash, a_tables.geometries.size());
			MixWitness(hash, a_tables.geometryLog.End());
			MixWitness(hash, a_tables.families.RequirementsWitness());
			MixWitness(hash, a_tables.streamRows.size());
			for (const auto& row : a_tables.streamRows)
				MixWitness(hash, row.object != Api::VertexStreamRow::kNoObject ? (std::uint64_t(row.first) << 32) | row.count : ~std::uint64_t{ 0 });
			return hash;
		}

		/** @brief A bit per RevisionReuse::Moved part that changed. Zero means the requirements are the same. */
		std::uint32_t MovedRequirements(const ImplSnapshotBuilder::RevisionReuse::Key& a_last, const ImplSnapshotBuilder::RevisionReuse::Key& a_next)
		{
			using Reuse = ImplSnapshotBuilder::RevisionReuse;
			const auto same = [](const auto& a_left, const auto& a_right) { return !a_left.owner_before(a_right) && !a_right.owner_before(a_left); };
			std::uint32_t moved = 0;
			const auto note = [&moved](bool a_moved, Reuse::Moved a_part) { moved |= a_moved ? 1u << a_part : 0u; };
			note(a_last.tables != a_next.tables, Reuse::kMovedTables);
			note(!same(a_last.catalog, a_next.catalog), Reuse::kMovedCatalog);
			note(a_last.tablesGeneration != a_next.tablesGeneration, Reuse::kMovedGeneration);
			note(a_last.request.pipelines != a_next.request.pipelines || a_last.request.shadowSlotKeys != a_next.request.shadowSlotKeys ||
					 a_last.request.shadowKeys != a_next.request.shadowKeys,
				Reuse::kMovedRequest);
			note(a_last.inputs != a_next.inputs || a_last.toggles != a_next.toggles, Reuse::kMovedInputs);
			note(a_last.lookups != a_next.lookups, Reuse::kMovedLookups);
			note(a_last.shadowRows != a_next.shadowRows, Reuse::kMovedShadowRows);
			note(a_last.volatiles != a_next.volatiles, Reuse::kMovedVolatiles);
			return moved;
		}

		/** @brief Build the reuse key into a_key and return what moved since the last seal. ~0u when there is nothing to keep. */
		std::uint32_t RequirementsMoved(ImplProducer& a_producer, const ImplSnapshotBuilder::RevisionReuse& a_reuse, const ScenePublication& a_publication,
			const Lookups& a_lookups, ImplSnapshotBuilder::RevisionReuse::Key& a_key)
		{
			DCLF_ZONE("CS.DCLF.MakeRevisionShapes.Key");
			const auto& tables = *a_publication.tables;
			const auto& producer = a_producer.producer;
			const auto& inputs = producer.inputs;
			a_producer.UpdateDrawBound(tables, a_publication.tablesGeneration);
			a_key = { TablesWitness(tables, producer.drawBound), a_publication.catalog, a_publication.tablesGeneration, a_publication.revision, inputs.generation,
				a_publication.toggles, LookupsWitness(a_lookups, tables), ShadowRowsWitness(a_lookups, inputs.shadowPlacements, producer.occlusionLayouts),
				VolatileWitness(inputs) };
			return a_reuse.valid ? MovedRequirements(a_reuse.key, a_key) : ~0u;
		}

	}

	void IndirectDraws::Impl::EnsureRevisionLatches()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		auto& latches = producer.latches;
		auto& handles = producer.latchHandles;
		const auto& inputs = producer.inputs;
		// Register the handles once. The seals make the actual blocks.
		if (!handles) {
			auto& buffers = SceneBufferSet::Get();
			// One region per run, except the zeros, which every run shares.
			const auto perRun = SceneBufferSet::LatchRegions::kPerRun;
			const auto shared = SceneBufferSet::LatchRegions::kShared;
			// Headroom, because a commit's frame blocks change size.
			const auto latched = org::services::GrowthPolicy::Headroom(100, 4096);
			handles.main = buffers.RegisterLatch("cs.dclf.latch", perRun);
			for (auto& block : handles.mainLatched)
				block = buffers.RegisterLatch("cs.dclf.latched-copies", perRun, latched);
			handles.shadow = buffers.RegisterLatch("cs.dclf.shadow.latch", perRun);
			handles.shadowZeros = buffers.RegisterLatch("cs.dclf.shadow.zeros", shared);
			handles.shadowLatched = buffers.RegisterLatch("cs.dclf.latched-copies", perRun, latched);
			for (const auto& slot : kViewSlots) {
				if (!Extensions::ViewRegistered(slot.view))
					continue;
				const std::size_t v = ViewIndex(slot.view);
				handles.views[v] = buffers.RegisterLatch(fmt::format("{}.latch", TraitsOf(slot.view).prefix), perRun);
				handles.viewZeros[v] = buffers.RegisterLatch(fmt::format("{}.zeros", TraitsOf(slot.view).prefix), shared);
			}
		}
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		// The main latch outlives any one set of main resources, so it starts at their first sizes.
		if (!latches.mainLaidOut && inputs.main) {
			latches.main = {};
			latches.main.cascades = smallTables ? 1u : kInitialSunCascades;
			latches.main.shadowVolumes = smallTables ? 1u : kInitialShadowVolumes;
			latches.mainLaidOut = true;
		}
		if (!latches.shadowLaidOut && inputs.shadow) {
			latches.shadow = InitialShadowLatchLayout();
			latches.shadowLaidOut = true;
		}
		for (const auto& slot : kViewSlots)
			if (const std::size_t v = ViewIndex(slot.view); !latches.viewsLaidOut[v] && inputs.views[v].resources) {
				latches.views[v] = ViewLatchLayout::Initial(TraitsOf(slot.view));
				latches.viewsLaidOut[v] = true;
			}
	}

	void IndirectDraws::Impl::MakeRevision(const ScenePublication& a_publication, std::uint32_t a_frame)
	{
		const auto& request = a_publication.revision;
		DCLF_ZONE("CS.DCLF.MakeRevisionShapes");
		auto& reuse = revisionReuse;
		reuse.reusedNow = false;
		revisionInputsSlot.TakeInto(producer.inputs);
		// Use the publication's toggles, because the frames that install it draw under those.
		producer.toggles = a_publication.toggles;
		producer.frame = a_frame;
		auto& buffers = SceneBufferSet::Get();
		buffers.SetUploads(producer.inputs.uploads);
		const auto& inputs = producer.inputs;
		auto* host = RenderGraphRuntime::Get().Host();
		// Wait until every build of the newer state is staged, or recordings would be of the old graphs. SettleBuildState posts inputs
		// again then, naming the staged builds to record on (plan step 10a).
		if (inputs.build && inputs.build->serial > BuiltBuildSerial())
			return;
		if (!inputs.claims || !inputs.main || !inputs.main->scene || !host || !a_publication.tables) {
			reuse.valid = false;
			return;
		}
		const auto& tables = *a_publication.tables;
		// Use only the publication's own lookups and catalog, never the frame's installed ones. Mixing them would pair a late
		// revision's tables with older lookups.
		static const Lookups kNoLookups;
		const Lookups& lookups = a_publication.lookups ? *a_publication.lookups : kNoLookups;
		const PipelineCatalog* catalog = a_publication.catalog.get();
		EnsureRevisionLatches();
		// If nothing the requirement pass reads has moved, this make would seal the same revision, so keep the last one.
		ImplSnapshotBuilder::RevisionReuse::Key key;
		std::uint32_t moved = RequirementsMoved(*this, reuse, a_publication, lookups, key);
		// A due shrink changes what the pass seals without moving the key, so run the pass.
		if (!moved && (SceneBufferSet::Get().ShrinkPending() ||
						  producer.mainWindows.Due(PooledBackings::Shrink(), org::services::ShrinkWindow::Clock::now())))
			moved = 1u << ImplSnapshotBuilder::RevisionReuse::kMovedShrink;
		if (moved == 0) {
			++reuse.kept;
			reuse.reusedNow = true;
			return;
		}
		reuse.NoteRan(moved);
		reuse.valid = false;
		const std::uint64_t sequenceBefore = revisions.assembler.LatestSequence();
		const auto assemble = [&] {
			DCLF_ZONE("CS.DCLF.MakeRevisionShapes.Assemble");
			AssembleRevision(a_frame);
			if (revisions.assembler.LatestSequence() == sequenceBefore)
				return;
			const std::uint64_t setSerial = revisions.sealed.named->Serial();
			reuse.key = std::move(key);
			reuse.setSerial = setSerial;
			reuse.valid = true;
		};
		RevisionMake make{ *this, a_publication, tables, lookups, catalog };
		make.lookupsWitness = key.lookups;
		auto build = buffers.Begin();
		{
			DCLF_ZONE("CS.DCLF.MakeRevisionShapes.Require");
			for (auto* participant : participants)
				participant->RequireSequences(make, build);
			for (auto* participant : participants)
				participant->RequireLatched(make, build);
			RequireLatches(build);
		}
		// No seal means a growth is in flight. Its completion wakes the builder to make the revision again.
		auto seal = [&] {
			DCLF_ZONE("CS.DCLF.MakeRevisionShapes.Seal");
			return buffers.Seal(std::move(build));
		}();
		if (!seal) {
			++revisions.growthWaits;
			return;
		}
		const auto& sealed = *seal.named;
		// Must run before any shape is made from the set.
		if (const auto& s = inputs.scene) {
			Extensions::SealSceneResources(sealed, s->sceneResources);
			if (s->visibilityEvents && s->visibilityReadback)
				s->visibilityReadback->list->Grow(static_cast<std::uint32_t>(sealed.Of(s->visibilityEvents).units));
		}
		if (!revisions.layout || !(*revisions.layout == producer.mainLayout))
			revisions.layout = std::make_shared<const MainLayout>(producer.mainLayout);
		revisions.sealed = std::move(seal);
		const MainLayout& mainLayout = *revisions.layout;
		// Shapes are made again only when the shapes key moved. Otherwise the new revision keeps the last ones.
		for (auto* participant : participants)
			participant->NoteSealed(make);
		const auto shapesKey = [&] {
			DCLF_ZONE("CS.DCLF.MakeRevisionShapes.ShapesKey");
			return RevisionShapesKey(sealed, make, request.pipelines);
		}();
		if (shapesKey == revisionShapesKey) {
			++shapesKept;
			assemble();
			return;
		}
		for (std::size_t g = 0; g < shapesKey.size(); ++g)
			shapesMadeBy[g] += shapesKey[g] != revisionShapesKey[g] ? 1 : 0;
		revisionShapesKey = shapesKey;
		++shapesMade;
		{
			DCLF_ZONE("CS.DCLF.MakeRevisionShapes.Shapes");
			for (auto* participant : participants)
				participant->MakeShapes(make, sealed, mainLayout);
		}
		assemble();
	}

	void IndirectDraws::Impl::RequireRevisionSet(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& request = a_make.publication.revision;
		const auto& tables = a_make.tables;
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		const auto& latches = producer.latches;
		if (inputs.scene)
			RequireSceneTables(a_build, *inputs.scene, tables);
		// Must come before RequireMain, which lays out sub-ranges by its state classes.
		a_make.indirect = a_make.catalog ? GetIndirectState(*a_make.catalog) : IndirectState{};
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
			a_make.familyOn[d] = CatalogFamilyPipelines(a_make.catalog, static_cast<Api::DrawFamily>(d), a_make.familyPipelines[d]);
		RequireMain(a_build, r, a_make);
		if (inputs.scene)
			RequireIndexPool(a_build, *inputs.scene, tables, a_make.publication.tablesGeneration);
		// Reserve here rather than in a colour commit, because a frame without claims never runs one.
		// The bucket map covers every pipeline slot, since a commit past the map draws nothing for the missing slots.
		{
			const std::uint32_t views = inputs.shadowCandidates;
			const auto& layout = latches.main;
			ReserveMainLatch(std::max(views, layout.cascades), std::max(views, layout.shadowVolumes), std::max(layout.buckets, request.pipelines));
		}
	}

	void IndirectDraws::Impl::RequireRevisionViews(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& request = a_make.publication.revision;
		const auto& tables = a_make.tables;
		const auto& inputs = producer.inputs;
		// Plan each view from the same main layout its shape is made from later, so the shape never indexes past its buffers.
		auto& viewOn = a_make.viewOn;
		for (const auto& slot : kViewSlots) {
			const auto kind = slot.view;
			if (!Extensions::ViewRegistered(kind))
				continue;
			const std::size_t v = ViewIndex(kind);
			const auto& traits = TraitsOf(kind);
			const auto& view = inputs.views[v];
			viewOn[v] = view.targets.colour != DXGI_FORMAT_UNKNOWN && inputs.claims;
			ViewPipelinesOf(kind, a_make.catalog, tables, view.targets, viewOn[v], viewRevisionPipelines[v]);
			if (traits.forwardPart)
				ViewForwardSlotsOf(tables, viewOn[v] && producer.inputs.cpuPipeline, viewForwardSlots);
			for (std::size_t d = 0; traits.trees && d < Extensions::kDrawFamilies; ++d) {
				const auto family = static_cast<Api::DrawFamily>(d);
				viewRevisionFaces[d] = viewOn[v] && Extensions::DrawFamilyHasFaces(family) && Extensions::DrawFamilyOn(family) ?
				                           ForwardPipelineOf(a_make.catalog, ForwardPipelineKey{ 0, 0, FamilySlotOf(family).forwardFlags }, view.targets) :
				                           rhi::PipelineHandle{};
			}
			if (!view.resources || (traits.depthPass && !viewOn[v]))
				continue;
			// Grow the latch's frame region a page at a time to fit the view's frame values.
			if (OwnsFrameConstants(traits) && view.resources->frameConstants && producer.latches.viewsLaidOut[v]) {
				const auto frameBuffers = view.resources->frameBuffers.load(std::memory_order_acquire);
				const auto bytes = static_cast<std::uint32_t>(LatchedBytes(ViewFrameLatched(view.resources->frameConstants.get(), frameBuffers.get(), inputs.blockSizes[kDepthShape], 0, traits.faces)));
				if (auto& layout = producer.latches.views[v]; bytes > layout.frameBytes)
					layout.frameBytes = (bytes + 4095u) & ~4095u;
			}
			ViewPlan plan;
			PlanViewBuckets(traits, producer.mainLayout, &a_make.lookups, a_make.indirect, viewRevisionPipelines[v], traits.forwardPart ? std::span<const std::uint8_t>(viewForwardSlots) : std::span<const std::uint8_t>{}, plan);
			RequireView(a_build, *view.resources, static_cast<std::uint32_t>(std::max<std::size_t>(plan.map.size(), request.pipelines)),
				static_cast<std::uint32_t>(plan.buckets.size() + plan.forwardRanges.size()), plan.draws,
				plan.forwardRanges.empty() ? 0u : static_cast<std::uint32_t>(a_make.indirect.classOf.size()));
		}
	}

	void IndirectDraws::Impl::RequireRevisionLatched(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		auto& zPlans = a_make.zPlans;
		auto& mainLatched = a_make.mainLatched;
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			if (!inputs.mainKnown[shape] || !a_make.indirect.valid)
				continue;
			const bool depthOnly = shape == kDepthShape;
			if (depthOnly && r.pool)
				PlanZBuckets(producer.mainLayout, a_make.lookups, a_make.tables, a_make.indirect, zPlans[shape]);
			const std::uint32_t classWords = !depthOnly && a_make.classPlan ? a_make.classPlan->Words() : 0u;
			mainLatched[shape] = MainLatchedLayout(r, depthOnly, inputs.blockSizes[shape], static_cast<std::uint32_t>(zPlans[shape].calls.size()), classWords);
			RequireLatchedBlock(a_build, producer.latchHandles.mainLatched[shape], mainLatched[shape]);
		}
	}

	void IndirectDraws::Impl::MakeMainShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout)
	{
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		const auto& handles = producer.latchHandles;
		const rhi::DescriptorHeapHandle resourceHeap = inputs.resourceHeap, samplerHeap = inputs.samplerHeap;
		std::array<std::shared_ptr<const FamilyPipelines>, Extensions::kDrawFamilies> families{};
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
			if (a_make.familyOn[d])
				families[d] = std::make_shared<const FamilyPipelines>(a_make.familyPipelines[d]);
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			auto& mainShape = producer.made.main[shape];
			mainShape = nullptr;
			// No commit has captured this segment yet, so it has no viewport or frame blocks.
			if (!inputs.mainKnown[shape] || !a_make.indirect.valid)
				continue;
			const bool depthOnly = shape == kDepthShape;
			auto in = MainShapeInputsOf(r, a_layout, a_sealed, depthOnly, producer.toggles.cullMode);
			in.latch = a_sealed.Latch(handles.main);
			in.latchLayout = producer.latches.main;
			in.viewport = inputs.viewport[shape];
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = a_make.indirect;
			in.families = families;
			in.zCalls = a_make.zPlans[shape].calls;
			in.zPlan = std::make_shared<const ZBucketPlan>(std::move(a_make.zPlans[shape]));
			in.classPlan = a_make.classPlan;
			in.latched.copies = std::move(a_make.mainLatched[shape]);
			if (!in.latched.copies.empty())
				in.latched.latch = a_sealed.Latch(handles.mainLatched[shape]);
			mainShape = MakeMainShape(in);
		}
	}

	void IndirectDraws::Impl::MakeViewShapes(const RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout)
	{
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		const auto& handles = producer.latchHandles;
		const auto& latches = producer.latches;
		const auto& indirect = a_make.indirect;
		const auto& viewOn = a_make.viewOn;
		const rhi::DescriptorHeapHandle resourceHeap = inputs.resourceHeap, samplerHeap = inputs.samplerHeap;
		// A family's face lists form one unit group, so the usable count is the least any of them holds.
		std::array<std::uint32_t, Extensions::kDrawFamilies> familyUnits{};
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			const auto& faces = Extensions::DrawFamilyOf(family).contribution.faces;
			if (const auto& s = inputs.scene; s && faces.lists && FamilySetUp(*s, family)) {
				UnitGroup units;
				Api::DrawFamilyLists lists{ *s, {}, &units };
				faces.lists(lists);
				for (const auto& view : inputs.views)
					if (view.resources)
						for (const auto& list : view.resources->faceLists[d])
							units.Add(list);
				familyUnits[d] = static_cast<std::uint32_t>(std::min<std::uint64_t>(units.Units(a_sealed), UINT32_MAX));
			}
		}
		for (const auto& slot : kViewSlots) {
			const std::size_t v = ViewIndex(slot.view);
			const auto& traits = TraitsOf(slot.view);
			const auto& view = inputs.views[v];
			auto& shape = producer.made.views[v];
			shape = nullptr;
			ViewLatches viewLatches;
			if (latches.viewsLaidOut[v])
				viewLatches = { a_sealed.Latch(handles.views[v]), a_sealed.Latch(handles.viewZeros[v]), latches.views[v] };
			const bool ready = traits.depthPass ? viewOn[v] && inputs.mainKnown[kColourShape] && viewLatches.latch : inputs.claims && inputs.scene;
			if (!view.resources || !indirect.valid || !ready)
				continue;
			ViewShapeInputs in;
			PlanViewBuckets(traits, a_layout, &a_make.lookups, indirect, viewRevisionPipelines[v], traits.forwardPart ? std::span<const std::uint8_t>(viewForwardSlots) : std::span<const std::uint8_t>{}, in.plan);
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = indirect;
			in.latches = &viewLatches;
			// Views without their own frame constants use the colour segment's.
			const auto& own = view.resources->frameConstants;
			in.frameConstantsAddress = own ? view.resources->frameConstantsAddress : r.frameConstantsAddress;
			in.frameConstantsRef = own ? view.resources->frameConstantsRef : r.frameConstantsRef;
			if (own && viewLatches.latch && viewLatches.layout.frameBytes) {
				const auto frameBuffers = view.resources->frameBuffers.load(std::memory_order_acquire);
				in.frameCopies = ViewFrameLatched(own.get(), frameBuffers.get(), inputs.blockSizes[kDepthShape], viewLatches.layout.FrameOffset(), traits.faces);
				if (LatchedBytes(in.frameCopies) > viewLatches.layout.Bytes())
					in.frameCopies.clear();  // laid out for smaller blocks, so the commit stages them instead
			}
			in.width = view.width;
			in.height = view.height;
			in.depthRange = view.depthRange;
			in.sequenceDraws = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_sealed.Of(view.resources->sequences).units, UINT32_MAX));
			for (std::size_t d = 0; traits.trees && d < Extensions::kDrawFamilies; ++d)
				if (viewRevisionFaces[d].valid() && a_make.familyOn[d] && familyUnits[d])
					in.faces[d] = { viewRevisionFaces[d], a_make.familyPipelines[d].drawSignature, familyUnits[d] };
			if (!traits.depthPass || ((!in.plan.buckets.empty() || !in.plan.forwardRanges.empty()) && viewLatches.zeros))
				shape = MakeViewShape(*view.resources, in);
		}
	}

	IndirectDraws::Impl::ShapesKey IndirectDraws::Impl::RevisionShapesKey(const NamedVersions& a_sealed, const RevisionMake& a_make, std::size_t a_pipelines) const
	{
		ShapesKey key;
		key.fill(Util::Fnv1a::kOffset);
		ShapesKeyMixer mixer{ key };
		auto& group = mixer.group;
		auto mix = [&mixer](std::uint64_t a_value) { mixer.Mix(a_value); };
		auto ptr = [&mixer](const void* a_pointer) { mixer.Ptr(a_pointer); };
		auto raw = [&mixer]<class T>(const T& a_value) { mixer.Raw(a_value); };
		// Everything a shape reads must be mixed in here, or a stale shape will be kept.
		const auto& inputs = producer.inputs;
		const auto& lookups = a_make.lookups;
		// The set's serial moves whenever a version or latch block is replaced.
		group = kKeyVersions;
		mix(a_sealed.Serial());
		if (const auto& layout = revisions.layout) {
			mix(layout->zBucketsLayout);
			mix(layout->slotsLayout);
			raw(std::array<std::uint32_t, 7>{ layout->sequenceDraws, layout->sequenceDecals, layout->decalRanges, layout->terrainCapacity[0], layout->terrainCapacity[1],
				layout->forwardCapacity, layout->refractionCapacity });
		}
		group = kKeyPipelines;
		mix(inputs.resourceHeap.index);
		mix(inputs.resourceHeap.generation);
		mix(inputs.samplerHeap.index);
		mix(inputs.samplerHeap.generation);
		mix(producer.toggles.cullMode);
		group = kKeyLookups;
		mix(a_make.lookupsWitness);
		mix(ShadowRowsWitness(lookups, inputs.shadowPlacements, producer.occlusionLayouts));
		mix(a_pipelines);  // the publication's pipeline slot count, not the coordinator's
		// No shadow input carries the SharedData and FeatureData sizes, so mix them here.
		if (inputs.shadow) {
			group = kKeyShadow;
			const auto blocks = ShadowArenaBlocksOf();
			mix((std::uint64_t(blocks.sharedBytes) << 32) | blocks.featureBytes);
		}
		for (const auto* participant : participants)
			participant->MixShapesKey(a_make, mixer);
		return key;
	}

	void IndirectDraws::Impl::MixMainShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const
	{
		auto& group = a_mixer.group;
		auto mix = [&a_mixer](std::uint64_t a_value) { a_mixer.Mix(a_value); };
		auto ptr = [&a_mixer](const void* a_pointer) { a_mixer.Ptr(a_pointer); };
		auto raw = [&a_mixer]<class T>(const T& a_value) { a_mixer.Raw(a_value); };
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		// The refraction pipeline is the catalog's own, not part of the set version, so it is mixed separately.
		group = kKeyPipelines;
		ptr(a_make.indirect.version.get());
		mix(a_make.indirect.valid);
		mix(a_make.indirect.refraction.index);
		mix(a_make.indirect.refraction.generation);
		mix(static_cast<std::uint64_t>(a_make.indirect.refractionFormat));
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			mix(a_make.familyOn[d] ? 1u : 0u);
			if (!a_make.familyOn[d])
				continue;
			const auto& family = a_make.familyPipelines[d];
			for (const auto& pipeline : { family.depth, family.colour, family.colour2 }) {
				mix(pipeline.index);
				mix(pipeline.generation);
			}
			mix(family.drawSignature.index);
			mix(family.drawSignature.generation);
		}
		group = kKeyMain;
		const auto& r = *inputs.main;
		ptr(&r);
		raw(latches.main);
		ptr(r.latchedTargets.load(std::memory_order_acquire).get());
		ptr(r.pool.get());
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			mix(inputs.mainKnown[shape]);
			raw(inputs.viewport[shape]);
			raw(inputs.blockSizes[shape]);
		}
	}

	void IndirectDraws::Impl::ViewParticipant::MixShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const
	{
		FamilyPipelinesSet families{};
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
			families[d] = a_make.familyOn[d] ? &a_make.familyPipelines[d] : nullptr;
		owner.MixViewShapesKey(a_mixer, families);
	}

	void IndirectDraws::Impl::MixViewShapesKey(ShapesKeyMixer& a_mixer, const FamilyPipelinesSet& a_families) const
	{
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		for (const auto& slot : kViewSlots) {
			const auto kind = slot.view;
			const std::size_t v = ViewIndex(kind);
			a_mixer.group = slot.keyGroup;
			const auto& view = inputs.views[v];
			if (!view.resources)
				continue;
			a_mixer.Ptr(view.resources.get());
			// The view's latched copies name these buffers, which are remade when the main frame buffers change shape.
			if (const auto frameBuffers = view.resources->frameBuffers.load(std::memory_order_acquire))
				a_mixer.Ptr(frameBuffers.get());
			a_mixer.Mix(latches.viewsLaidOut[v]);
			a_mixer.Raw(latches.views[v]);
			a_mixer.Raw(view.targets);
			a_mixer.Mix(view.width);
			a_mixer.Mix(view.height);
			a_mixer.Raw(view.depthRange);
			for (const auto& pipeline : viewRevisionPipelines[v]) {
				a_mixer.Mix(pipeline.index);
				a_mixer.Mix(pipeline.generation);
			}
			if (TraitsOf(kind).forwardPart)
				for (const std::uint8_t forward : viewForwardSlots)
					a_mixer.Mix(forward);
			if (TraitsOf(kind).trees)
				for (const auto& pipeline : viewRevisionFaces) {
					a_mixer.Mix(pipeline.index);
					a_mixer.Mix(pipeline.generation);
				}
		}
		for (const auto& slot : kViewSlots) {
			if (!TraitsOf(slot.view).trees)
				continue;
			a_mixer.group = slot.keyGroup;
			if (const auto& sceneState = inputs.scene)
				for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
					if (!Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d)))
						continue;
					if (const auto* family = a_families[d]) {
						a_mixer.Mix(family->drawSignature.index);
						a_mixer.Mix(family->drawSignature.generation);
					}
					a_mixer.Mix(a_families[d] ? 1u : 0u);
					a_mixer.Mix(FamilySetUp(*sceneState, static_cast<Api::DrawFamily>(d)) ? 1u : 0u);
				}
		}
	}
}
