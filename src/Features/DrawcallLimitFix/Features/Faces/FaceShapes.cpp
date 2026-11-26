// The scene coordinator component for NPC faces. It tracks face shapes per head, gives each a region of the positions buffer,
// and on each new head snapshot updates the shapes' vertex streams in place instead of rewriting them.

#include "Features/DrawcallLimitFix/Features/Faces/Faces.h"
#include "Features/DrawcallLimitFix/Features/Faces/FaceSnapshots.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneMirror.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::Features::Faces
{
	namespace
	{
		constexpr std::uint32_t kNoFaceRegion = Api::VertexStreamRow::kNoFirst;

		struct FaceShapes
		{
			// Keys are valid for this walk only. The captured recordId lasts.
			ankerl::unordered_dense::map<const RE::BSFaceGenNiNode*, std::shared_ptr<const FaceSnapshots::HeadView>> capturedFaceHeads;
			struct FaceRegion
			{
				std::uint32_t first = 0, count = 0;
			};
			using FaceRegions = ankerl::unordered_dense::map<const RE::BSGeometry*, FaceRegion>;
			using FaceRegionFree = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
			FaceRegions faceRegions;
			FaceRegionFree faceRegionFree;  // (first, count), sorted, coalesced
			std::uint32_t faceRegionTop = 0;
			ankerl::unordered_dense::map<const RE::BSFaceGenNiNode*, std::vector<RE::BSGeometry*>> faceHeads;
			std::vector<const RE::BSFaceGenNiNode*> facePublished, faceStale;
			ankerl::unordered_dense::map<const RE::BSGeometry*, const RE::BSFaceGenNiNode*> listedHeads;
			struct Counts
			{
				std::uint64_t walks = 0, published = 0, updated = 0, written = 0;
			} counts;
		};

		std::shared_ptr<void> Make() { return std::make_shared<FaceShapes>(); }

		// Walk thread only.
		std::uint32_t FaceRegionOf(const Api::CoordinatorComponentContext& a_context, FaceShapes& a_state, const RE::BSGeometry* a_geometry, std::uint32_t a_vertexCount)
		{
			auto& region = a_state.faceRegions[a_geometry];
			if (region.count != a_vertexCount) {
				// Retire rather than free, since an installed publication's streams may still name it.
				if (region.count)
					a_context.RetireRange(region.first, region.count);
				region = {};
				for (auto it = a_state.faceRegionFree.begin(); it != a_state.faceRegionFree.end(); ++it) {
					if (it->second < a_vertexCount)
						continue;
					region.first = it->first;
					region.count = a_vertexCount;
					it->first += a_vertexCount;
					it->second -= a_vertexCount;
					if (it->second == 0)
						a_state.faceRegionFree.erase(it);
					break;
				}
				if (!region.count) {
					region.first = a_state.faceRegionTop;
					region.count = a_vertexCount;
					a_state.faceRegionTop += a_vertexCount;
				}
			}
			return region.first;
		}

		void UnlistFaceShape(FaceShapes& a_state, const RE::BSGeometry* a_geometry)
		{
			const auto listed = a_state.listedHeads.find(a_geometry);
			if (listed == a_state.listedHeads.end())
				return;
			const auto* head = listed->second;
			a_state.listedHeads.erase(listed);
			if (!head)
				return;
			const auto it = a_state.faceHeads.find(head);
			if (it == a_state.faceHeads.end())
				return;
			std::erase(it->second, a_geometry);
			if (!it->second.empty()) {
				// Rewriting the siblings rebuilds the head's record without this shape.
				a_state.faceStale.push_back(head);
				return;
			}
			a_state.faceHeads.erase(it);
			if (FaceSnapshots::Enabled())
				FaceSnapshots::Get().Release(head);
		}

		// Existing streams take a new snapshot in place. Shapes that are waiting, mismatched or under a stale head are rewritten.
		void ApplyFacePublications(const Api::CoordinatorComponentContext& a_context, FaceShapes& a_state, Api::WalkScheduling& a_scheduling)
		{
			auto& tables = a_context.tables;
			for (const auto* head : a_state.faceStale) {
				const auto it = a_state.faceHeads.find(head);
				if (it == a_state.faceHeads.end())
					continue;
				for (auto* geometry : it->second)
					if (a_context.ScheduleWrite(geometry))
						++a_scheduling.written;
			}
			a_state.faceStale.clear();
			auto& snapshots = FaceSnapshots::Get();
			a_scheduling.events += a_state.facePublished.size();
			for (const auto* head : a_state.facePublished) {
				const auto it = a_state.faceHeads.find(head);
				if (it == a_state.faceHeads.end())
					continue;
				std::shared_ptr<const FaceSnapshots::HeadView> headView;
				bool headTaken = false;
				for (auto* geometry : it->second) {
					const auto entry = a_context.TrackedWriteOf(geometry);
					if (!entry.tracked)
						continue;
					const std::uint32_t slot = entry.slot;
					const std::uint32_t index = slot != kNoObjectSlot && slot < tables.streamRow.size() ? tables.streamRow[slot] : kNoStreamRow;
					if (index == kNoStreamRow) {
						if (entry.streamWaiting) {
							a_context.ScheduleWrite(geometry);
							++a_scheduling.written;
						}
						continue;
					}
					auto& stream = tables.streamRows[index];
					auto view = snapshots.View(geometry, head);
					if (!view.positions || view.vertexCount != stream.count) {
						a_context.ScheduleWrite(geometry);
						++a_scheduling.written;
						continue;
					}
					if (!headTaken) {
						headTaken = true;
						if (auto snapshot = snapshots.HeadSnapshot(*head); snapshot.recordId && snapshot.generation && snapshot.owner)
							headView = std::make_shared<const FaceSnapshots::HeadView>(std::move(snapshot));
					}
					stream.vertices = view.positions;
					stream.generation = view.generation;
					stream.group = headView && headView->generation == view.generation && headView->owner == view.owner ? headView : nullptr;
					stream.owner = std::move(view.owner);
					++a_scheduling.updated;
				}
			}
			a_state.facePublished.clear();
		}

		// Freed records' engine references go back through the retirement chain, to be released on the main thread.
		void WalkBegun(const Api::CoordinatorComponentContext& a_context)
		{
			auto& state = a_context.State<FaceShapes>();
			state.capturedFaceHeads.clear();
			if (FaceSnapshots::Enabled()) {
				FaceSnapshots::Get().BeginWalk(state.facePublished, state.faceStale);
				std::vector<RE::NiRefObject*> released;
				FaceSnapshots::Get().TakeReleased(released);
				for (auto* object : released) {
					RE::NiPointer<RE::NiRefObject> reference(object);
					object->DecRefCount();  // drop the count TakeReleased handed over, which the NiPointer now holds
					a_context.HandBack(std::move(reference));
				}
			}
		}

		// After a reset, a load or a live toggle, kept streams cannot be trusted, so the publications are dropped.
		void Scheduling(const Api::CoordinatorComponentContext& a_context, Api::WalkScheduling& a_scheduling)
		{
			auto& state = a_context.State<FaceShapes>();
			++state.counts.walks;
			if (a_scheduling.full) {
				state.facePublished.clear();
				state.faceStale.clear();
				return;
			}
			ApplyFacePublications(a_context, state, a_scheduling);
			state.counts.published += a_scheduling.events;
			state.counts.updated += a_scheduling.updated;
			state.counts.written += a_scheduling.written;
		}

		void DeltaLine(const Api::CoordinatorComponentContext& a_context, std::string& a_text)
		{
			auto& c = a_context.State<FaceShapes>().counts;
			const double n = static_cast<double>(std::max<std::uint64_t>(c.walks, 1));
			a_text += fmt::format("; face publications {:.1f}/frame: {:.1f} streams updated in place, {:.2f} shapes written", c.published / n, c.updated / n,
				c.written / n);
			c = {};
		}

		void WalkEnded(const Api::CoordinatorComponentContext& a_context)
		{
			auto& state = a_context.State<FaceShapes>();
			auto& tables = a_context.tables;
			for (const auto& [source, first] : tables.streamRowsReleased) {
				const auto* geometry = static_cast<const RE::BSGeometry*>(source);
				const auto it = state.faceRegions.find(geometry);
				if (it == state.faceRegions.end() || it->second.first != first)
					continue;
				if (const auto entry = a_context.TrackedWriteOf(geometry); entry.tracked) {
					const std::uint32_t slot = entry.slot;
					if (slot != kNoObjectSlot && slot < tables.streamRow.size() && tables.streamRow[slot] != kNoStreamRow &&
						tables.streamRows[tables.streamRow[slot]].first == first)
						continue;
				}
				a_context.RetireRange(it->second.first, it->second.count);
				state.faceRegions.erase(it);
			}
			tables.streamRowsReleased.clear();
			auto& faceRegionFree = state.faceRegionFree;
			std::sort(faceRegionFree.begin(), faceRegionFree.end());
			std::size_t out = 0;
			for (std::size_t i = 0; i < faceRegionFree.size(); ++i) {
				if (out && faceRegionFree[out - 1].first + faceRegionFree[out - 1].second == faceRegionFree[i].first)
					faceRegionFree[out - 1].second += faceRegionFree[i].second;
				else
					faceRegionFree[out++] = faceRegionFree[i];
			}
			faceRegionFree.resize(out);
			if (!faceRegionFree.empty() && faceRegionFree.back().first + faceRegionFree.back().second == state.faceRegionTop) {
				state.faceRegionTop = faceRegionFree.back().first;
				faceRegionFree.pop_back();
			}
			if (FaceSnapshots::Enabled())
				FaceSnapshots::Get().EndWalk();
		}

		void Reset(const Api::CoordinatorComponentContext& a_context)
		{
			auto& state = a_context.State<FaceShapes>();
			state.faceRegions.clear();
			state.faceRegionFree.clear();
			state.faceRegionTop = 0;
			a_context.tables.streamRowsReleased.clear();
		}

		void Forgot(const Api::CoordinatorComponentContext& a_context, const void* a_entry)
		{
			UnlistFaceShape(a_context.State<FaceShapes>(), static_cast<const RE::BSGeometry*>(a_entry));
		}

		void Recycled(const Api::CoordinatorComponentContext& a_context, std::uint32_t a_first, std::uint32_t a_count)
		{
			a_context.State<FaceShapes>().faceRegionFree.push_back({ a_first, a_count });
		}

		void Listed(const Api::CoordinatorComponentContext& a_context, const Api::EntryWrite& a_write)
		{
			const auto* head = static_cast<const RE::BSFaceGenNiNode*>(a_write.source);
			if (!head)
				return;
			auto& state = a_context.State<FaceShapes>();
			auto& listed = state.listedHeads[&a_write.geometry];
			if (listed)
				return;
			state.faceHeads[head].push_back(&a_write.geometry);
			listed = head;
		}

		bool Resolve(const Api::CoordinatorComponentContext& a_context, Api::EntryWrite& a_write)
		{
			FaceSnapshots::ShapeView face{};
			if (auto* head = static_cast<RE::BSFaceGenNiNode*>(a_write.source)) {
				const auto& mirror = a_context.Mirror();
				face = FaceSnapshots::Get().Shape(&a_write.geometry, head, [&](std::vector<FaceSnapshots::ShapeInput>& a_out) {
					const auto* record = mirror.Node(head);
					if (!record)
						return;
					for (const void* child : record->children) {
						const auto* g = child ? mirror.Geometry(child) : nullptr;
						if (!g || g->type != static_cast<std::uint8_t>(RE::BSGeometry::Type::kDynamicTriShape))
							continue;
						auto* geometry = a_context.TrackedGeometry(child);
						if (!geometry)
							continue;
						a_out.push_back({ static_cast<RE::BSDynamicTriShape*>(geometry), g->vertexCount });
					}
				});
			}
			std::uint32_t region = kNoFaceRegion;
			if (face.positions)
				region = FaceRegionOf(a_context, a_context.State<FaceShapes>(), &a_write.geometry, face.vertexCount);
			a_write.positions = face.positions;
			a_write.vertexCount = face.vertexCount;
			a_write.generation = face.generation;
			a_write.owner = std::move(face.owner);
			a_write.region = region;
			return region != kNoFaceRegion;
		}

		void Streamed(const Api::CoordinatorComponentContext& a_context, std::uint32_t a_slot, Api::EntryWrite& a_write)
		{
			auto& state = a_context.State<FaceShapes>();
			std::shared_ptr<const FaceSnapshots::HeadView> headView;
			const auto* head = static_cast<const RE::BSFaceGenNiNode*>(a_write.source);
			if (head) {
				auto [headEntry, inserted] = state.capturedFaceHeads.try_emplace(head);
				if (inserted) {
					auto snapshot = FaceSnapshots::Get().HeadSnapshot(*head);
					if (snapshot.recordId && snapshot.generation && snapshot.owner)
						headEntry->second = std::make_shared<const FaceSnapshots::HeadView>(std::move(snapshot));
				}
				headView = headEntry->second;
			}
			if (headView && (headView->generation != a_write.generation || headView->owner != a_write.owner))
				headView.reset();
			a_context.tables.SetStreamRow(a_slot, { .object = a_slot,
				.first = a_write.region,
				.count = a_write.vertexCount,
				.generation = a_write.generation,
				.vertices = a_write.positions,
				.owner = a_write.owner,
				.group = std::move(headView),
				.source = &a_write.geometry });
		}
	}

	void RegisterShapes(Api::InstallContext& a_context, bool a_taken)
	{
		const auto component = a_context.RegisterCoordinatorComponent({ .name = "faces",
			.make = &Make,
			.walk = { .begun = &WalkBegun, .scheduling = &Scheduling, .ended = &WalkEnded },
			.retirement = { .reset = &Reset, .forgot = &Forgot, .recycled = &Recycled },
			.entryClass = { .follows = Api::EntryClass::FaceShape, .takes = a_taken, .listed = &Listed, .resolve = &Resolve, .streamed = &Streamed } });
		a_context.RegisterReportLines({ .name = "face publications", .point = Api::ReportPoint::SceneCoordinatorDelta, .component = component, .componentText = &DeltaLine });
	}
}
