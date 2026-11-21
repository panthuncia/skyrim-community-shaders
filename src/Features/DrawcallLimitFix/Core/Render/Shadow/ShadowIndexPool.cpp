// The index pool that every plain indexed draw binds. It is a copy of the scene's index buffers in one buffer.
// Covers creation, the update at each commit, and the capacity the producer requires.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Core/Gpu/SetupPrograms.h"
#include "RenderGraph/Fatal.h"

namespace DCLF
{
	namespace Draws
	{
		namespace
		{
			const SetupPrograms::Compute kIndexPoolProgram{ "DCLF index pool", [] { return ComputeProgram::Desc{ .source = kIndexPoolShader, .constantWords = kIndexPoolConstantWords }; } };
		}

		std::shared_ptr<IndexPool> EnsureIndexPool(SceneBuffers& a_scene, rhi::Device a_device, bool a_small)
		{
			if (a_scene.pool)
				return a_scene.pool;
			auto pool = std::make_shared<IndexPool>();
			pool->program = kIndexPoolProgram.Get();
			pool->dispatchSignature = CreateDispatchSignature(a_device, pool->program->layout->GetHandle());
			if (!pool->dispatchSignature) {
				logger::warn("[DCLF] The index pool's copy program could not be created");
				return nullptr;
			}
			// Units are 16-bit indices. The pool is DCLF's largest table, so it grows by a quarter rather than to a power of two.
			pool->indices = MakeVersioned(CreateWords((a_small ? 4096u : kInitialPoolIndices) / 2, true, "cs.dclf.index-pool"),
				{ .extent = { 0, sizeof(std::uint16_t), 4 }, .policy = org::services::GrowthPolicy::Headroom(25, 2) });
			pool->firsts = MakeVersioned(CreateWords(a_small ? 64u : kInitialGeometries, false, "cs.dclf.pool-firsts"), { .extent = { 0, 4, 4 } });
			auto copies = org::Buffer::CreateUnmaterializedStructuredBuffer(a_small ? 16u : 1024u, 4 * sizeof(std::uint32_t), false);
			copies->SetName("cs.dclf.pool-copies");
			copies->Materialize();
			pool->copies = MakeVersioned(std::move(copies), { .extent = { 0, 4 * sizeof(std::uint32_t), 4 * sizeof(std::uint32_t) } });
			a_scene.pool = pool;
			return pool;
		}

		/** @brief A quarter of slack over the held indices, to absorb fragmentation before a relayout. */
		constexpr std::uint64_t PoolSlack(std::uint64_t a_indices)
		{
			return a_indices + a_indices / 4;
		}

		/** @brief A geometry slot's index count in the pool, rounded up to whole words. 0 means none. */
		std::uint32_t PoolIndicesOf(const SceneTables& a_tables, std::uint32_t a_slot)
		{
			const auto& geometry = a_tables.geometries[a_slot];
			if (!geometry.indexAddress || !geometry.indexRef || !geometry.indexBytes)
				return 0;
			return static_cast<std::uint32_t>(std::min<std::uint64_t>((geometry.indexBytes + 3) / 4 * 2, UINT32_MAX & ~1u));
		}

		namespace
		{
			/** @brief State for one index pool update. */
			struct PoolUpdate
			{
				IndexPool& p;
				const SceneTables& tables;
				const org::services::NamedEntry& indices;
				std::uint64_t capacity, firstsCapacity, copiesCapacity;
				std::vector<std::array<std::uint32_t, 4>> copies{};
				std::vector<std::uint32_t> changed{};
				bool allFirsts = false;

				void Release(std::uint32_t a_slot)
				{
					auto& range = p.slots[a_slot];
					p.slotFirst[a_slot] = IndexPool::kNoRange;
					if (range.first == IndexPool::kNoRange) {
						range = {};
						return;
					}
					// Safe to free at once. The graph orders this frame's copies after every earlier read of the pool.
					auto [at, inserted] = p.free.emplace(range.first, range.count);
					if (auto next = std::next(at); next != p.free.end() && at->first + at->second == next->first) {
						at->second += next->second;
						p.free.erase(next);
					}
					if (at != p.free.begin()) {
						if (auto previous = std::prev(at); previous->first + previous->second == at->first) {
							previous->second += at->second;
							p.free.erase(at);
						}
					}
					p.indicesHeld -= range.count;
					range = {};
				}

				std::uint32_t Allocate(std::uint32_t a_count)
				{
					for (auto it = p.free.begin(); it != p.free.end(); ++it) {
						if (it->second < a_count)
							continue;
						const std::uint32_t first = it->first, rest = it->second - a_count;
						p.free.erase(it);
						if (rest)
							p.free.emplace(first + a_count, rest);
						return first;
					}
					if (std::uint64_t(p.end) + a_count > capacity)
						return IndexPool::kNoRange;
					const std::uint32_t first = p.end;
					p.end += a_count;
					return first;
				}

				void CopyOf(const IndexPool::Range& a_range)
				{
					copies.push_back({ a_range.ref.viewSlot, a_range.ref.byteOffset, a_range.first,
						static_cast<std::uint32_t>((a_range.bytes + 3) / 4) });
				}

				// Packs every range into a new version, which frames in flight do not read. Call only between the releases
				// and the acquires, so every held range matches the tables.
				void Relayout()
				{
					p.free.clear();
					p.end = 0;
					copies.clear();
					for (auto& range : p.slots) {
						if (range.first == IndexPool::kNoRange)
							continue;
						range.first = p.end;
						p.end += range.count;
						CopyOf(range);
					}
					for (std::size_t g = 0; g < p.slots.size(); ++g)
						p.slotFirst[g] = p.slots[g].first;
					allFirsts = true;
				}

				// Ranges are never shared by buffer address. The device reuses a freed buffer's address during a cell load,
				// while the unloaded cell's slots may still hold it.
				void Acquire(std::uint32_t a_slot)
				{
					const auto& geometry = tables.geometries[a_slot];
					const std::uint32_t slotIndices = PoolIndicesOf(tables, a_slot);
					if (!slotIndices)
						return;
					if (a_slot >= firstsCapacity || copies.size() >= copiesCapacity) {
						p.waiting.push_back(a_slot);
						return;
					}
					const std::uint32_t first = Allocate(slotIndices);
					if (first == IndexPool::kNoRange) {
						// No room despite enough slack means fragmentation. Ask the producer for a newer version, which gets relaid out.
						if (PoolSlack(std::uint64_t(p.indicesHeld) + slotIndices) <= capacity)
							p.fragmentedAt.store(indices.serial, std::memory_order_relaxed);
						p.waiting.push_back(a_slot);
						return;
					}
					auto& range = p.slots[a_slot];
					range = { first, slotIndices, geometry.indexAddress, geometry.indexBytes, geometry.indexRef };
					p.slotFirst[a_slot] = first;
					p.indicesHeld += slotIndices;
					CopyOf(range);
				}

				/** @brief Brings the ranges up to date. Rebuilds from empty when the log cannot be followed, else updates changed slots. */
				void UpdateRanges(std::uint32_t a_count, std::uint32_t a_generation)
				{
					const auto count = a_count;
					if (!p.cursor.Continues(tables.geometryLog, a_generation) || p.slots.size() > count) {
						p.free.clear();
						p.end = 0;
						p.indicesHeld = 0;
						p.waiting.clear();
						p.slots.assign(count, {});
						p.slotFirst.assign(count, IndexPool::kNoRange);
						for (std::uint32_t g = 0; g < count; ++g)
							Acquire(g);
						p.cursor.Restart(a_generation);
						allFirsts = true;
					} else {
						const auto first = static_cast<std::uint32_t>(p.slots.size());
						p.slots.resize(count);
						p.slotFirst.resize(count, IndexPool::kNoRange);
						for (std::uint32_t g = 0; g < count; ++g)
							if (g >= first)
								changed.push_back(g);
						for (const std::uint32_t g : p.cursor.Unread(tables.geometryLog))
							if (g < first)
								changed.push_back(g);
						for (const std::uint32_t g : p.waiting)
							if (g < count)
								changed.push_back(g);
						p.waiting.clear();
						std::sort(changed.begin(), changed.end());
						changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
						// Release all before acquiring any. A version this pool has not filled yet is relaid out in between.
						for (const std::uint32_t g : changed)
							Release(g);
						if (!p.indicesFilled.Holds(indices))
							Relayout();
						for (const std::uint32_t g : changed)
							Acquire(g);
					}
				}

				// A version not filled yet gets every first index. Otherwise only the changed slots are uploaded.
				void UploadFirsts(const org::services::NamedEntry& a_firsts, std::uint32_t a_count, CommitUploads& a_uploads)
				{
					const auto& firsts = a_firsts;
					const auto count = a_count;
					if (!p.firstsFilled.Holds(firsts))
						allFirsts = true;
					p.firstsFilled.Filled(firsts);
					const auto held = static_cast<std::uint32_t>(std::min<std::uint64_t>(count, firstsCapacity));
					if (allFirsts) {
						if (held)
							a_uploads(p.firsts, p.slotFirst.data(), std::size_t(held) * sizeof(std::uint32_t), 0);
					} else {
						for (std::size_t i = 0; i < changed.size() && changed[i] < held;) {
							std::size_t j = i + 1;
							while (j < changed.size() && changed[j] == changed[j - 1] + 1 && changed[j] < held)
								++j;
							a_uploads(p.firsts, &p.slotFirst[changed[i]], (j - i) * sizeof(std::uint32_t), std::uint64_t(changed[i]) * sizeof(std::uint32_t));
							i = j;
						}
					}
				}

				// The revision sized the copies buffer for every slot, so overflowing it is a defect, never something to drop.
				std::uint32_t UploadCopies(const org::LatchBlock& a_latch, const org::RunRef& a_latchRun, std::uint32_t a_poolOffset, CommitUploads& a_uploads)
				{
					const auto copyCount = static_cast<std::uint32_t>(copies.size());
					if (copyCount > copiesCapacity)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the index pool's {} copies past the {} its buffer holds (the requirement missed them)", copyCount,
							copiesCapacity));
					if (copyCount)
						a_uploads(p.copies, copies.data(), std::size_t(copyCount) * sizeof(copies[0]), 0);
					const std::uint32_t dispatch[4] = { std::min(copyCount, kIndexPoolGroupsX), (copyCount + kIndexPoolGroupsX - 1) / kIndexPoolGroupsX, 1, copyCount };
					LatchWrite(a_latch, "shadow index-pool dispatch", a_latchRun, a_poolOffset, std::as_bytes(std::span(dispatch)));
					return copyCount;
				}
			};
		}

		/**
		 * @brief Brings the index pool up to date with the tables and writes the copy dispatch into the latch.
		 * IndexPoolPass runs the copies before the views draw. Returns the copy count.
		 */
		std::uint32_t UpdateIndexPool(const NamedVersions& a_sealed, IndexPool& a_pool, const SceneTables& a_tables, std::uint32_t a_generation, const org::LatchBlock& a_latch,
			const org::RunRef& a_latchRun, std::uint32_t a_poolOffset, CommitUploads& a_uploads)
		{
			auto& p = a_pool;
			const auto count = static_cast<std::uint32_t>(a_tables.geometries.size());
			// A range that does not fit waits. Only unclaimed geometry can, since room for the claims was required.
			const auto& indices = a_sealed.Of(p.indices);
			const auto& firsts = a_sealed.Of(p.firsts);
			PoolUpdate update{ p, a_tables, indices, indices.units, firsts.units, a_sealed.Of(p.copies).units };
			update.UpdateRanges(count, a_generation);
			p.cursor.Advance(a_tables.geometryLog);
			p.indicesFilled.Filled(indices);

			update.UploadFirsts(firsts, count, a_uploads);
			return update.UploadCopies(a_latch, a_latchRun, a_poolOffset, a_uploads);
		}
	}

	void IndirectDraws::Impl::RequireIndexPool(GrowthBuild& a_build, SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_generation)
	{
		if (!a_scene.pool)
			return;
		auto& p = *a_scene.pool;
		// Revision code only.
		auto& b = producer.poolBound;
		const auto count = static_cast<std::uint32_t>(a_tables.geometries.size());
		auto take = [&](std::uint32_t a_slot) {
			const std::uint32_t indices = PoolIndicesOf(a_tables, a_slot);
			b.total = b.total + indices - b.indices[a_slot];
			b.indices[a_slot] = indices;
		};
		if (!b.cursor.Continues(a_tables.geometryLog, a_generation) || b.indices.size() > count) {
			b.indices.assign(count, 0u);
			b.total = 0;
			for (std::uint32_t g = 0; g < count; ++g)
				take(g);
			b.cursor.Restart(a_generation);
		} else {
			const auto first = static_cast<std::uint32_t>(b.indices.size());
			b.indices.resize(count, 0u);
			for (std::uint32_t g = first; g < count; ++g)
				take(g);
			for (const std::uint32_t g : b.cursor.Unread(a_tables.geometryLog))
				if (g < first)
					take(g);
		}
		b.cursor.Advance(a_tables.geometryLog);
		// Require the slack, and a newer version if the last one was found fragmented.
		auto& buffers = SceneBufferSet::Get();
		buffers.Require(a_build, p.indices, Draws::PoolSlack(b.total));
		buffers.RequireNewer(a_build, p.indices, p.fragmentedAt.load(std::memory_order_relaxed));
		UnitGroup{}.Add(p.firsts).Add(p.copies).Require(a_build, std::max(count, 1u));
	}
}
