// The CPU pipeline's view batches. Each view gets a list of fixed-size SoA batches built from the scene list. The resident
// sequences are what BuildDrawsCS would store for each drawn partition, kept in a pooled buffer version of their own.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatchesInternal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"

#include <ORGModuleServices/ElasticCapacity.h>
#include "RenderGraph/Async/Retirement.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneGraph.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowthPolicy.h>
#include <ORGModuleServices/VersionedBufferBuild.h>
#include <OpenRenderGraph/GraphHost.h>
#include <Resources/Buffers/Buffer.h>
#include <Render/Runtime/IUploadService.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>

namespace DCLF::Draws
{
	using namespace detail;

	namespace
	{
		/** @brief A sequences version's ref at byte 0, or null without a backing or a graph host. */
		RenderGraph::BufferRef SequencesRefOf(const org::services::PublishedBufferVersion& a_version)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			const auto& buffer = a_version.lease ? a_version.lease->buffer : nullptr;
			return host && buffer ? RenderGraph::MakeBufferRef(host->Device(), buffer->GetAPIResource().GetHandle(), 0, std::source_location::current()) :
			                        RenderGraph::BufferRef{};
		}

		/** @brief a_lanes rounded up to a multiple of 8, within [8, 65528], because a lane index is 16 bits. */
		std::uint32_t RoundedLanes(unsigned long a_lanes)
		{
			return std::max<std::uint32_t>(8, (static_cast<std::uint32_t>(std::min<unsigned long>(a_lanes, 65528)) + 7) & ~7u);
		}

		/** @brief The live lane setting, seeded from CS_DCLF_CPU_BATCH. */
		std::atomic<std::uint32_t>& LiveLanes()
		{
			static std::atomic<std::uint32_t> lanes{ [] {
				if (const auto& text = SwitchValue(Switch::CpuBatch); !text.empty())
					if (const unsigned long parsed = std::strtoul(text.c_str(), nullptr, 10); parsed != 0)
						return RoundedLanes(parsed);
				return Direct::kDefaultBatchLanes;
			}() };
			return lanes;
		}

	}

	std::shared_ptr<Direct::ViewBatchSet> MakeViewBatches(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres, std::vector<DrawSequence>& a_sequences)
	{
		DCLF_ZONE("CS.DCLF.ViewBatches");
		if (!a_in.tables || !a_in.payload || (!a_in.main && !a_in.shadow))
			return nullptr;
		auto set = std::make_shared<Direct::ViewBatchSet>();
		set->lanes = BatchLanes();
		set->forwardOnly = a_in.forwardOnly;
		set->keep = a_in.keep;
		const auto& geometries = a_in.main ? a_in.main->body->geometryDraws : a_in.shadow->geometries;
		set->geometryIndexAddress.assign(geometries.SlotCount(), 0);
		for (std::size_t g = 0; g < set->geometryIndexAddress.size(); ++g)
			if (const GeometryDraw* geometry = geometries.slots.At(static_cast<std::uint32_t>(g)))
				set->geometryIndexAddress[g] = geometry->indexBufferAddress;
		static Direct::GeometryAddressSerials s_geometrySerials;
		set->geometrySerial = s_geometrySerials.Of(set->geometryIndexAddress);
		RowBases rows;
		rows.pipelineRows = RowBases::AddressOf(*a_in.payload, PayloadPart::PipelineRows);
		rows.materialRows = RowBases::AddressOf(*a_in.payload, PayloadPart::MaterialRows);
		rows.shadowRows = RowBases::AddressOf(*a_in.payload, PayloadPart::ShadowRows);
		rows.pipelineRowsRef = RowBases::RefOf(*a_in.payload, PayloadPart::PipelineRows);
		rows.materialRowsRef = RowBases::RefOf(*a_in.payload, PayloadPart::MaterialRows);
		rows.shadowRowsRef = RowBases::RefOf(*a_in.payload, PayloadPart::ShadowRows);
		rows.slotKeys = a_in.lookups ? &a_in.lookups->shadowSlotKeys : nullptr;
		set->pipelineRowsBase = rows.pipelineRows;
		set->materialRowsBase = rows.materialRows;
		set->shadowRowsBase = rows.shadowRows;
		set->pipelineRowsRef = rows.pipelineRowsRef;
		set->materialRowsRef = rows.materialRowsRef;
		set->shadowRowsRef = rows.shadowRowsRef;
		FillAnimatedRows(*set, a_in);
		auto& scratch = BuilderScratch();
		for (auto& lanes : scratch.lanes)
			lanes.clear();
		a_sequences.clear();
		a_sequences.reserve(scratch.lastSequences + scratch.lastSequences / 8);
		set->sequenceArgs.reserve(scratch.lastSequences + scratch.lastSequences / 8);
		SequenceSink sink{ a_sequences, set->sequenceArgs, geometries };
		ListBuilder lists(a_in, rows, sink, scratch);
		{
			DCLF_ZONE("CS.DCLF.ViewBatches.Lists");
			ForEachInput(a_in, [&](const DrawInput& a_input) { lists.Add(a_input); });
		}
		set->sequenceCount = static_cast<std::uint32_t>(a_sequences.size());
		scratch.lastSequences = a_sequences.size();
		// One allocation for every batch's columns.
		ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> bucketOf;
		const std::uint32_t laneCount = set->lanes;
		std::size_t batches = 0;
		{
			DCLF_ZONE("CS.DCLF.ViewBatches.Sort");
			for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
				SortList(scratch.lanes[v], set->buckets[v], bucketOf);
				batches += (scratch.lanes[v].size() + laneCount - 1) / laneCount;
			}
		}
		const std::size_t batchBytes = std::size_t(laneCount) * kLaneBytes;
		{
			DCLF_ZONE("CS.DCLF.ViewBatches.Storage");
			// Not zeroed, because every column is written below and nothing past a batch's count is read.
			set->storage = StoragePool::Get().Take(batches * batchBytes);
		}
		DCLF_ZONE_NAMED(fillZone, "CS.DCLF.ViewBatches.Fill", true);
		const std::size_t objects = a_in.tables->objects.size();
		auto& moverCount = scratch.moverCount;
		moverCount.assign(objects + 1, 0u);
		std::size_t next = 0;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			const auto& lanes = scratch.lanes[v];
			auto& list = set->lists[v];
			list.resize((lanes.size() + laneCount - 1) / laneCount);
			for (std::size_t b = 0; b < list.size(); ++b) {
				auto& batch = list[b];
				batch.index = static_cast<std::uint32_t>(b);
				batch.count = static_cast<std::uint32_t>(std::min<std::size_t>(laneCount, lanes.size() - b * laneCount));
				LayOut(batch, set->storage.get() + (next++) * batchBytes, laneCount);
				for (std::uint32_t l = 0; l < batch.count; ++l) {
					const auto& lane = lanes[b * laneCount + l];
					batch.object[l] = lane.object;
					batch.sequence[l] = lane.sequence;
					batch.sequenceCount[l] = lane.sequenceCount;
					batch.pipelineBucket[l] = lane.bucketKey;
					batch.fadeRoot[l] = lane.fadeRoot;
					batch.fadeDistance[l] = lane.fadeDistance;
					batch.partitions[l] = lane.partitions;
					batch.lodPartitions[l] = lane.lodPartitions;
					std::array<float, 4> sphere{ 0.0f, 0.0f, 0.0f, -1.0f };
					std::uint32_t flags = lane.flags;
					if (!SnapshotSphere(*a_in.tables, a_spheres, lane.object, sphere)) {
						flags |= Direct::kLaneMover;
						if (lane.object < objects)
							++moverCount[lane.object];
					}
					batch.flags[l] = flags;
					batch.cx[l] = sphere[0];
					batch.cy[l] = sphere[1];
					batch.cz[l] = sphere[2];
					batch.r[l] = sphere[3];
					// Both live columns start from the snapshot. FrameValues rewrites the movers every frame.
					for (const auto& live : batch.liveSpheres) {
						live.cx[l] = sphere[0];
						live.cy[l] = sphere[1];
						live.cz[l] = sphere[2];
						live.r[l] = sphere[3];
					}
				}
			}
		}
		// FrameValues writes the movers' live spheres every frame.
		auto& first = set->moverLaneFirst;
		first.assign(objects + 1, 0u);
		for (std::size_t o = 0; o < objects; ++o)
			first[o + 1] = first[o] + moverCount[o];
		set->moverLanes.resize(first[objects]);
		std::fill(moverCount.begin(), moverCount.end(), 0u);
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v)
			for (const auto& batch : set->lists[v])
				for (std::uint32_t l = 0; l < batch.count; ++l)
					if (const std::uint32_t o = batch.object[l]; (batch.flags[l] & Direct::kLaneMover) && o < objects)
						set->moverLanes[first[o] + moverCount[o]++] = { static_cast<std::uint16_t>(v), batch.index, static_cast<std::uint16_t>(l) };
		// Decal group g uses main range slot g - 1. The lanes are already sorted by group.
		const auto& decals = scratch.lanes[View(ViewType::Decal)];
		if (!decals.empty()) {
			auto& groupFirst = set->decalGroupFirst;
			groupFirst.assign(Extensions::kMainRangeSlots + 1, 0u);
			for (const auto& lane : decals)
				if (lane.decalGroup >= 1 && lane.decalGroup <= kDecalGroups)
					++groupFirst[lane.decalGroup];
			for (std::size_t g = 1; g < groupFirst.size(); ++g)
				groupFirst[g] += groupFirst[g - 1];
		}
		return set;
	}

	// ---- The sequences' version. Same rules as the payload: written into a backing no frame reads, GPU ready before posting.

	struct ViewBatchBuilder::Pending
	{
		std::atomic<bool> done{ false };
		std::shared_ptr<const org::services::PublishedBufferVersion> version;  // null if failed
		std::string error;
		std::uint64_t write = 0;
		bool lists = false;       // made by the incremental lists
	};

	namespace
	{
		/** @brief The sequences' backing pool. Never destroyed, because a lease may return late. */
		const std::shared_ptr<org::services::BackingPool>& SequencesPool()
		{
			static const auto* pool = new std::shared_ptr<org::services::BackingPool>(org::services::BackingPool::Create(PooledBackings::PerFrameHooks(), ::RenderGraph::Frame::Retirement::Clock(),
				{ .name = "cs.dclf.cpu.sequences",
					// A frame in flight's set, the installed one's, the one being built, and a set a site still holds per frame slot.
					.maxPerClass = RenderGraphRuntime::kGameFramesInFlight + 3,
					.framesInFlight = RenderGraphRuntime::kGameFramesInFlight,
					.spareFree = 1 }));
			return *pool;
		}

		/** @brief Post a set's sequence upload. False when refused, and then a_pending never completes. */
		bool PostSequences(SequencesUpload a_upload, const std::shared_ptr<org::runtime::IUploadService>& a_uploads, std::uint64_t a_version,
			const std::shared_ptr<ViewBatchBuilder::Pending>& a_pending);
	}

	std::uint32_t BatchLanes()
	{
		return LiveLanes().load(std::memory_order_relaxed);
	}

	void SetBatchLanes(std::uint32_t a_lanes)
	{
		LiveLanes().store(RoundedLanes(a_lanes), std::memory_order_relaxed);
	}

	ViewBatchBuilder::ViewBatchBuilder() = default;
	ViewBatchBuilder::~ViewBatchBuilder() = default;

	void ViewBatchBuilder::Reset()
	{
		payload = nullptr;
		set.reset();
		building.reset();
		pending.reset();  // a version in flight still finishes into its own Pending
		failed = false;
	}

	bool ViewBatchBuilder::Ready(const ViewBatchInputs& a_in)
	{
		// A new payload or a lane count change remakes the set.
		if (const std::uint32_t want = BatchLanes(); payload != a_in.payload || lanes != want || forwardOnly != a_in.forwardOnly) {
			Reset();
			payload = a_in.payload;
			lanes = want;
			forwardOnly = a_in.forwardOnly;
		}
		if (!a_in.payload || set || failed)
			return true;
		if (!pending) {
			// A forward-only set asks for no spheres, because its few lanes are all treated as movers.
			auto& values = FrameValues::Get();
			if (!a_in.forwardOnly) {
				if (auto taken = values.TakeSnapshotSpheres())
					spheres = std::move(taken);
				values.RequestSnapshotSpheres();
			}
			// The incremental lists make the set. A forward-only set is a full build.
			SequencesUpload upload;
			if (!lists)
				lists = std::make_unique<SceneLists>();
			if (!a_in.forwardOnly)
				building = lists->Make(a_in, spheres, versions + 1, upload);
			const bool fromLists = building != nullptr;
			if (fromLists && ++listSets % 128 == 0)
				if (const auto line = lists->TakeReport(); !line.empty())
					logger::info("{}", line);
			if (!building) {
				auto sequences = std::make_shared<std::vector<DrawSequence>>();
				building = MakeViewBatches(a_in, a_in.forwardOnly ? nullptr : spheres.get(), *sequences);
				if (building)
					building->ownSequences = sequences;
				const auto* view = sequences.get();
				upload = {};
				upload.total = view->size();
				upload.emit = [view](const std::function<void(const void*, std::size_t, std::uint64_t)>& a_write) {
					a_write(view->data(), view->size() * sizeof(DrawSequence), 0);
				};
				upload.keep = sequences;
			}
			if (!building) {
				failed = true;
				return true;
			}
			if (!upload.total) {
				set = std::move(building);
				return true;
			}
			if (!a_in.uploads || !a_in.uploads->HasDedicatedStreamingQueue()) {
				static std::atomic<bool> logged{ false };
				if (!logged.exchange(true))
					logger::info("[DCLF] CPU pipeline: the view batches' sequences wait for the dedicated upload queue; snapshots have no batches until it is made");
				building.reset();
				failed = true;
				return true;
			}
			auto posted = std::make_shared<Pending>();
			posted->write = ++versions;
			posted->lists = fromLists;
			if (!PostSequences(std::move(upload), a_in.uploads, posted->write, posted)) {
				if (lists)
					lists->Reset();
				building.reset();
				failed = true;
				return true;
			}
			pending = std::move(posted);
			return false;
		}
		if (!pending->done.load(std::memory_order_acquire))
			return false;
		const auto done = std::move(pending);
		if (done->lists && lists)
			lists->Posted(done->write, done->version);
		if (!done->version) {
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged++ < 4)
				logger::error("[DCLF] CPU pipeline: the view batches' sequences version failed: {}; the snapshot has no batches", done->error);
			building.reset();
			failed = true;
			return true;
		}
		building->sequencesVersion = done->version;
		building->sequencesAddress = done->version->lease->address;
		// Not a raw ref, because the CPU sites declare the sequences and the frame publishes their version.
		building->sequencesRef = SequencesRefOf(*done->version);
		set = std::move(building);
		return true;
	}

	namespace
	{
		bool PostSequences(SequencesUpload a_upload, const std::shared_ptr<org::runtime::IUploadService>& a_uploads, std::uint64_t a_version,
			const std::shared_ptr<ViewBatchBuilder::Pending>& a_pending)
		{
			org::services::VersionedBufferBuildInput input;
			input.uploadOwner = a_uploads;
			input.uploadService = a_uploads.get();
			input.debugName = "cs.dclf.cpu.sequences";
			input.writeSequence = a_version;
			input.elementStride = sizeof(DrawSequence);
			input.elementCount = a_upload.total;
			input.capacity = std::max<std::uint64_t>(input.elementCount, 1);
			input.growth = org::services::GrowthPolicy::Headroom(25, 64);
			input.shrink = PooledBackings::Shrink();
			input.flags.deviceAddress = true;
			input.backingPool = SequencesPool();
			// Never patch a backing in place. An incremental version copies its predecessor on the GPU, then adds its own writes.
			input.previous = std::move(a_upload.previous);
			input.keep = std::move(a_upload.keep);
			input.changedSince = a_upload.changedSince ? std::move(a_upload.changedSince) :
			                                             [](std::uint64_t) -> std::optional<std::vector<org::services::BufferByteRange>> { return std::nullopt; };
			input.emitDelta = std::move(a_upload.emit);
			auto tag = std::make_shared<Runtime::PayloadPartTag>();
			tag->built = [](const org::services::PublishedBufferVersion&) {};
			input.tag = std::move(tag);
			return SceneScheduler::Graph().PostPayloadPart(std::move(input), [a_pending](std::shared_ptr<const org::services::PublishedBufferVersion> a_built, std::string a_error) {
				a_pending->version = std::move(a_built);
				a_pending->error = std::move(a_error);
				a_pending->done.store(true, std::memory_order_release);
				WakeSnapshotBuilder();
			});
		}
	}

	void BindViewBatches(const Direct::ViewBatchSet& a_set, std::uint64_t a_point)
	{
		if (a_set.sequencesVersion && a_point)
			a_set.sequencesVersion->lease.Bind(a_point);
	}

	namespace
	{
		RenderGraph::LateRefBuffer& SequencesLate()
		{
			static RenderGraph::LateRefBuffer late("cs.dclf.cpu.sequences");
			return late;
		}
	}

	void PublishViewBatches(const Direct::ViewBatchSet* a_set)
	{
		const auto& version = a_set ? a_set->sequencesVersion : nullptr;
		if (!version || !version->lease) {
			SequencesLate().Publish(nullptr, nullptr);
			return;
		}
		SequencesLate().Publish(version->lease->buffer, version, a_set->sequencesRef);
	}

	void DeclareViewBatchReads(org::RenderPassBuilder& a_builder, RenderGraph::DeclaredRefs& a_out)
	{
		SequencesLate().Declare(a_builder, RenderGraph::RefAccess::Read, a_out);
	}
}
