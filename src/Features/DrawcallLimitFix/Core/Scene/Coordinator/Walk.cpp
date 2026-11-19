// The coordinator's walk on the scene lane: the scene phase of a pass, the delta walk's rounds that evaluate then merge in order,
// the object records with their geometry slots, and the per-frame traits.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPartitions.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#if defined(CS_HAS_RENDER_GRAPH)
#	include "RenderGraph/BufferRefs.h"
#endif

#include <atomic>
#include <cassert>
#include <exception>
#include <functional>

namespace DCLF::Scene
{
	static_assert(Api::kMirroredKindFaceGen == SceneCapture::kKindFaceGen, "the Api's mirrored kind bits are the capture's");

	namespace
	{
		Api::EntryClass EntryClassOfLeaf(const SceneCapture::LeafView& a_leaf)
		{
			return a_leaf.parent ? Extensions::EntryClassOf(a_leaf.Type(), a_leaf.parent->kind) : Api::EntryClass::None;
		}

		bool PerFrameEntryClass(Api::EntryClass a_class)
		{
			return a_class != Api::EntryClass::None && Extensions::EntryClassRuleOf(a_class).perFrame;
		}
	}

	void Coordinator::BuildFrame(Phase a_phase)
	{
		if (a_phase == Phase::Scene)
			BuildScenePhase();
		else
			BuildAccumulatePhase();
	}

	/**
	 * @brief The scene phase of a pass: everything independent of the accumulator, with one object record per eligible object.
	 * The records leave the accumulator's half unset. BuildAccumulatePhase patches them by object index, which is fixed from here on.
	 */
	void Coordinator::BuildScenePhase()
	{
		{
			DCLF_SCENE_PART(Prologue, "CS.DCLF.Scene.Prologue");
			// A reader that has not read past the trimmed half reads every slot again.
			tables.changeLog.Trim(1u << 17);
			tables.materialLog.Trim(1u << 16);
			tables.geometryLog.Trim(1u << 16);
			tables.extrasBlockLog.Trim(1u << 16);
			Diagnostics::Notify<Diagnostics::ScenePoint::ScenePhaseBegin>();
		}
		// Nothing may touch tracked geometry during a load screen. The load frees renderer data and buffers while `tracked` keeps only the
		// NiAVObjects alive, and reading them hands the graph dead device addresses (VK_ERROR_DEVICE_LOST a few frames later).
		sceneBuilt = false;
		const auto& globals = FrameGlobals::Current();
		if (globals.loading) {
			ResetSlotTables();
			InvalidateObjectIndices();
			accumulatedPasses.clear();
			stats.objects = 0;
				stats.geometries = 0;
			stats.pipelines = 0;
			stats.materials = 0;
			return;
		}
		// Snapshotted for the whole pass, since the render thread may switch them while it runs.
		passSnapshot.inputs = Extensions::SampleClassifyInputs();
		ShardScope passScope(*this, false);
		PartTimer timer(stats.partMs);
		{
			DCLF_SCENE_PART(Prologue, "CS.DCLF.Scene.Prologue");
			auto& gpu = GpuResources::Scene();
			gpu.BeginFrame();
			NoteRelocatedGeometry(gpu.TakeRelocated());
			const bool resolveBuffers = gpu.Enabled();
			frameResolveBuffers = resolveBuffers;
			if (resolveBuffers != graphWasActive) {
				logger::info("[DCLF] tables frame {}: the render graph is {} (slots alive {} geometries / {} pipelines / {} materials)", sceneFrame,
					resolveBuffers ? "active from this frame" : "inactive from this frame", stats.geometriesAlive, stats.pipelinesAlive, stats.materialsAlive);
				graphWasActive = resolveBuffers;
			}
			// Read once, since these are the same for every object.
			frameInterior = globals.interior;
			frameDecalBias = globals.decalBias;
			geometryIndex.reserve(tracked.size());
		}
		timer.Add(BuildPart::Walk);
		{
			DCLF_SCENE_PART(SweepSlots, "CS.DCLF.Scene.SweepSlots");
			SweepSlots();
		}

		DeltaWalk();
		{
			DCLF_SCENE_PART(Shading, "CS.DCLF.Scene.Shading");
			NameShadingEvents();
		}
		PublishPlacementPlan();
		Diagnostics::Notify<Diagnostics::ScenePoint::WalkDone>();
		stats.objects = tables.liveObjects;
		sceneBuilt = true;
	}

	void Coordinator::BeginWalk(bool a_keepIndices)
	{
		const bool keepObjects = !denseWalk;
		tables.ClearFrame(keepObjects);
		if (!a_keepIndices)
			InvalidateObjectIndices();
		++walkSerial;
		ComponentsWalkBegun();
		stats.ineligible.fill(0);
		stats.unstableCauses.fill(0);
		stats.techniqueRejects.fill(0);
		stats.shadowMaskPipelines = 0;
		stats.materialsEvaluated = 0;
		stats.classifyHits = stats.classifyChecked = stats.classifyDiffers = 0;
		stats.derivedHits = stats.derivedChecked = stats.derivedDiffers = 0;
		stats.skinned = stats.boneRows = 0;
		stats.projectedUV = stats.landBlend = 0;
		stats.shadowCasters = 0;
		stats.shadowRejects = {};

		skinnedObjects.clear();

		// Reserve every per-object container up front, or one left out reallocates its way back up every walk.
		if (!keepObjects) {
			tables.objects.reserve(tracked.size());
			tables.objectGeometry.reserve(tracked.size());
			tables.draws.reserve(tracked.size());
			tables.ancestorValue.reserve(tracked.size());
			tables.skinPartitions.reserve(tracked.size());
		}

	}

	void Coordinator::DenseWalk()
	{
		BeginWalk();
		PartTimer timer(stats.partMs);
		timer.Add(BuildPart::PassLookup);
		for (auto& [geometry, trackedEntry, unused, layer] : order) {
			timer.Add(BuildPart::LoopTail);
			Ineligible bucket = Ineligible::Count;
			WriteObject(geometry, *trackedEntry, timer, bucket);
		}

		SweepObjectSlots();

		ComponentsWalkEnded();
		tables.InvalidateChangeLog();
	}

	void Coordinator::DenseReference(Tables& a_dense, ankerl::unordered_dense::map<const RE::BSGeometry*, Ineligible>& a_reasons)
	{
		// Everything the second walk overwrites is saved and restored, so the frame goes on with the slot tables unchanged.
		// Fade-root owners are not saved, since the dense walk lists none and copies would be references the pump drops.
		Tables slots = tables;
		const auto savedStamp = objectStamp;
		const auto savedStats = stats;
		const auto savedSkinned = skinnedObjects;
		const auto savedGeometryIndex = geometryIndex;
		const auto savedRefreshed = refreshedGeometry;
		ComponentsDense(true);
		BuildFullOrder();
		referenceReasons.clear();
		denseWalk = true;
		DenseWalk();
		denseWalk = false;
		geometryIndex = savedGeometryIndex;
		refreshedGeometry = savedRefreshed;
		ComponentsDense(false);
		a_dense = std::move(tables);
		tables = std::move(slots);
		objectStamp = savedStamp;
		stats = savedStats;
		skinnedObjects = savedSkinned;
		a_reasons = std::move(referenceReasons);
		referenceReasons.clear();
	}

	namespace
	{
		DrawSequence DrawTemplateOf(const GeometryBuffers& a_geometry, std::uint32_t a_pipeline)
		{
			DrawSequence draw{};
			draw.pipelineIndex = a_pipeline;
			draw.vertexBufferRef = a_geometry.vertexRef;
			draw.vertexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.vertexBytes, UINT32_MAX));
			draw.vertexStride = a_geometry.vertexStride;
			// The second stream repeats the first. The epochs replace it with a face shape's positions.
			draw.streamBufferRef = draw.vertexBufferRef;
			draw.streamBufferSize = draw.vertexBufferSize;
			draw.streamStride = draw.vertexStride;
			draw.indexBufferAddress = a_geometry.indexAddress;
			draw.indexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.indexBytes, UINT32_MAX));
			draw.indexFormat = kIndexFormatR16;
			draw.indexCount = a_geometry.indexCount;
			draw.instanceCount = 1;
			draw.firstIndex = a_geometry.firstIndex;
			draw.vertexOffset = 0;
			draw.firstInstance = 0;
			return draw;
		}
	}

	void Coordinator::EvaluateWrite(RE::BSGeometry* geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out, PartTimer& timer) const
	{
		// Pure, any thread under a ShardScope. Anything that touches a slot is MergeWrite's.
		auto& c = a_counters;
		auto& d = a_out.delta;
		const bool classifyProbe = Policy::RecomputeCachedVerdicts(Policy::Verdicts::Classification, passSerial);
		const auto& entry = a_tracked;
		// The mirror's records, never the live geometry. Without one yet it is written when its capture lands.
		const auto leaf = mirror.Leaf(geometry);
		a_out.leaf = leaf;
		if (!leaf) {
			++c.leafMissing;
			a_out.write = EvalResult::Write::NoLeaf;
			a_out.reason = Ineligible::NoRendererData;
			++c.ineligible[static_cast<std::size_t>(a_out.reason)];
			return;
		}

		// A cached verdict stale toward eligible is harmless, since the accumulate phase classifies again. One stale the other way would
		// leave an accumulated object without a record, so that phase clears the cache for it.
		Api::EntryClass entryClassOf = entry.entryClass;
		if (!entry.entryClassResolved) {
			d.entryClassSet = true;
			d.entryClass = EntryClassOfLeaf(leaf);
			entryClassOf = d.entryClass;
		}
		const bool entryTaken = entryClassOf != Api::EntryClass::None && Extensions::EntryClassTaken(entryClassOf);
		// A taken class whose records follow its component's per-frame state is classified every frame.
		const bool classifiedEveryFrame = entryTaken && Extensions::EntryClassRuleOf(entryClassOf).perFrame;
		if (!entry.ownerResolved) {
			d.ownerSet = true;
			d.owner = Extensions::OwnerClassOf(*leaf.node);
		}
		// A classification stands until an event writes the entry again. Per-frame entry classes and actors' frame verdicts are retaken
		// every frame. The dense rebuild classifies from scratch and leaves the caches alone.
		const bool rereads = entry.perFrame && (!entry.lightTraits || (entry.lightTraits & kTraitOwnerClass));
		bool cached = !denseWalk && !classifiedEveryFrame && entry.candidateFrame != 0;
		if (cached) {
			if (!EvaluateKeptVerdict(entry, leaf, rereads, c, a_out))
				return;
		} else if (!EvaluateStaticVerdict(geometry, entry, leaf, classifyProbe, c, a_out, timer)) {
			return;
		}

		a_out.write = EvalResult::Write::Record;
		a_out.entryTaken = entryTaken;
		EvaluateRecordInputs(geometry, leaf, a_out);
		EvaluateShadowInputs(leaf, a_out);
		a_out.skinLodWord = a_out.lodChain ? 0u : SkinPartitions::LodOf(leaf);
	}

	bool Coordinator::EvaluateKeptVerdict(const Tracked& entry, const SceneCapture::LeafView& leaf, bool rereads, EvalCounters& a_counters, EvalResult& a_out) const
	{
		auto& c = a_counters;
		auto& d = a_out.delta;
		Ineligible reason;
		bool shadowOnly = false;  // not the main pass's, but a caster the shadow epochs draw (kObjectShadowOnly)
		reason = entry.candidateReason;
		// Under a switch the verdict follows the selection, which changes without an event, as for a harvested plant. So it is retaken every frame.
		const bool frameVerdict = reason == Ineligible::None || reason == Ineligible::Switch ||
		                          (rereads && (reason == Ineligible::Hidden || reason == Ineligible::Actor));
		if ((entry.parentReason == Ineligible::Switch || rereads) && frameVerdict) {
			reason = ClassifyFrame(entry);
			d.candidateSet = true;
			d.candidateFrame = reason != entry.candidateReason ? PassStamp() : entry.candidateFrame;
			d.candidateReason = reason;
		}
		++c.ineligible[static_cast<std::size_t>(reason)];
		a_out.reason = reason;
		a_out.focusOnly = HiddenFocusCaster(entry, reason, leaf);
		shadowOnly = reason != Ineligible::None && !Eligibility::DeferredToAccumulate(reason) && (Eligibility::ShadowOnlyCaster(reason, leaf) || a_out.focusOnly);
		a_out.shadowOnly = shadowOnly;
		if (reason != Ineligible::None && !Eligibility::DeferredToAccumulate(reason) && !shadowOnly) {
			a_out.write = EvalResult::Write::Rejected;
			return false;
		}
		return true;
	}

	bool Coordinator::EvaluateStaticVerdict(RE::BSGeometry* geometry, const Tracked& entry, const SceneCapture::LeafView& leaf, bool classifyProbe,
		EvalCounters& a_counters, EvalResult& a_out, PartTimer& timer) const
	{
		auto& c = a_counters;
		auto& d = a_out.delta;
		Ineligible reason;
		bool shadowOnly = false;  // not the main pass's, but a caster the shadow epochs draw (kObjectShadowOnly)
		LightingDescriptors descriptors;
		const auto& verdict = entry.verdict;
		const auto* witnessRenderer = static_cast<const RE::BSGraphics::TriShape*>(leaf.geometry->rendererData);
		const auto* witnessProperty = static_cast<const RE::BSShaderProperty*>(leaf.geometry->property);
		const auto* witnessMaterial = leaf.property ? static_cast<const RE::BSShaderMaterial*>(leaf.property->material) : nullptr;
		const std::uint8_t fadeState = FadeStateOf(leaf.property);
		const bool hit = !denseWalk && verdict.cached && verdict.rendererData == witnessRenderer &&
		                 verdict.property == witnessProperty && verdict.material == witnessMaterial &&
		                 verdict.fadeState == fadeState;
		if (hit && !classifyProbe) {
			reason = verdict.reason;
			++c.classifyHits;
		} else {
			reason = Eligibility::Static(leaf, &descriptors);
			if (hit) {
				// A mismatch is a defect.
				++c.classifyChecked;
				if (reason != verdict.reason && c.classifyDiffers++ == 0)
					c.classifyDiffersFirst = fmt::format("[DCLF] classify cache: '{}' is cached as {} but recomputes as {}", geometry->name.c_str() ? geometry->name.c_str() : "?",
						kIneligibleNames[static_cast<std::size_t>(verdict.reason)], kIneligibleNames[static_cast<std::size_t>(reason)]);
				reason = verdict.reason;  // the cache is what the frame would have used
			} else if (!denseWalk) {
				d.verdictSet = true;
				d.verdict = verdict;
				if (Eligibility::Cacheable(reason))
					d.verdict = { true, reason, witnessRenderer, witnessProperty, witnessMaterial, fadeState };
				else
					d.verdict.cached = false;
			}
		}
		timer.Add(BuildPart::ClassifyStatic);
		if (reason == Ineligible::None)
			reason = ClassifyFrame(entry);
		timer.Add(BuildPart::ClassifyFrame);
		if (denseWalk) {
			a_out.referenceReason = true;
		} else {
			d.candidateSet = true;
			d.candidateFrame = PassStamp();
			d.candidateReason = reason;
		}
		++c.ineligible[static_cast<std::size_t>(reason)];
		a_out.reason = reason;
		if (reason == Ineligible::Technique)
			++c.techniqueRejects[descriptors.rejectedTechnique & 63];
		a_out.focusOnly = HiddenFocusCaster(entry, reason, leaf);
		shadowOnly = reason != Ineligible::None && !Eligibility::DeferredToAccumulate(reason) && (Eligibility::ShadowOnlyCaster(reason, leaf) || a_out.focusOnly);
		a_out.shadowOnly = shadowOnly;
		if (reason != Ineligible::None && !Eligibility::DeferredToAccumulate(reason) && !shadowOnly) {
			a_out.write = EvalResult::Write::Rejected;
			return false;
		}
		return true;
	}

	void Coordinator::EvaluateRecordInputs(RE::BSGeometry* geometry, const SceneCapture::LeafView& leaf, EvalResult& a_out) const
	{
		const auto& g = *leaf.geometry;
		// Every LOD level's partitions for a LOD skin. The draw narrows to its root's level.
		std::uint16_t partitionMask = SkinPartitions::Of(g);
		// A ranged geometry type's drawn ranges, such as object LOD's. Resolved again at each ranges event.
		const auto* shapeRanges = Extensions::GeometryTypeRanged(g.type) ? EntryRangesOf(geometry) : nullptr;
		const bool lodChain = shapeRanges && !shapeRanges->empty() && !Api::EntryRangesWhole(g.triangleCount, *shapeRanges);
		if (shapeRanges && shapeRanges->empty())
			partitionMask = static_cast<std::uint16_t>(kNoPartitions);
		else if (lodChain)
			partitionMask = static_cast<std::uint16_t>(kPartitionChain | std::min<std::size_t>(shapeRanges->size(), kPartitionChainCount));
		a_out.partitionMask = partitionMask;
		a_out.lodChain = lodChain;
		if (leaf.Skinned() && Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Skinned)) {
			a_out.skinCandidate = true;
			const std::uint32_t rows = SkinPalettes::RowsOf(*leaf.geometry);
			if (SkinPalettes::Fits(rows))
				a_out.boneRows = rows;
		}
		// A palette rows member's block, such as a particle system's rows, filled by FrameValues every frame.
		if (a_out.reason == Ineligible::None)
			if (const auto itemClass = PaletteItemClassOf(leaf); itemClass != Api::FrameValueItemClass::None)
				a_out.particleRows = Extensions::PaletteRowsOf(itemClass, leaf.geometry->particles.capacity);
	}

	void Coordinator::EvaluateShadowInputs(const SceneCapture::LeafView& leaf, EvalResult& a_out) const
	{
		using PropertyFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto* shadowProperty = leaf.property;
		const auto shadowReject = ShadowCasterReject(leaf);
		a_out.shadowReject = static_cast<std::uint8_t>(shadowReject);
		auto sampleDiffuse = [&]() {
			if (a_out.shadowMaterial)
				return;
			if (shadowProperty && shadowProperty->material) {
				a_out.shadowMaterial = static_cast<const RE::BSShaderMaterial*>(shadowProperty->material);
				a_out.shadowDiffuse = static_cast<ID3D11ShaderResourceView*>(const_cast<void*>(shadowProperty->diffuseView));
			}
		};
		if (shadowReject == ShadowReject::None || shadowReject == ShadowReject::VolumetricOnly) {
			a_out.shadowTechnique = ShadowUtilityTechnique(leaf);
			if (a_out.shadowTechnique & 0x80)
				sampleDiffuse();
		}
		// LOD occludes neither occlusion map, since Precipitation::SetupMask culls the scene lists, which do not hold the LOD root.
		constexpr std::uint64_t kLodFlags = static_cast<std::uint64_t>(PropertyFlag::kLODLandscape) | static_cast<std::uint64_t>(PropertyFlag::kLODObjects) |
		                                    static_cast<std::uint64_t>(PropertyFlag::kHDLODObjects);
		const bool lod = shadowProperty && (shadowProperty->flags & kLodFlags);
		const auto* occluderProperty = SceneCapture::LeafView::Lighting(leaf.property);
		const bool occluderLeaf = occluderProperty && leaf.geometry;
		const Api::OccluderLeaf occluder{ .flags = occluderLeaf ? occluderProperty->flags : 0u,
			.alphaTesting = leaf.AlphaTesting(),
			.referenced = leaf.node && leaf.node->userData,
			.scene = this,
			.records = &leaf,
			.nearestFadeBsx = [](const Api::OccluderLeaf& a_leaf) {
				return static_cast<const Coordinator*>(a_leaf.scene)->NearestFadeNodeBsx(*static_cast<const SceneCapture::LeafView*>(a_leaf.records));
			} };
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			const auto& map = Extensions::OcclusionMapOf(v);
			const std::uint32_t occlusion = Extensions::OcclusionOn(ActiveToggles(), v) && !lod && occluderLeaf ? map.technique(occluder) : 0u;
			a_out.occlusion[v] = occlusion;
			if (occlusion & 0x80)
				sampleDiffuse();
		}
	}

	bool Coordinator::MergeWrite(RE::BSGeometry* geometry, Tracked& a_tracked, EvalResult& a_result, PartTimer& timer, Ineligible& a_bucket)
	{
		auto* trackedEntry = &a_tracked;
		const auto& r = a_result;
		const auto& d = r.delta;
		a_bucket = r.reason;
		if (r.write == EvalResult::Write::NoLeaf) {
			if (!denseWalk) {
				trackedEntry->leafWaiting = true;
				trackedEntry->candidateFrame = 0;
			}
			return false;
		}
		// A rewritten member keeps its binding if it stays eligible in the same slot.
		const bool wasMember = !denseWalk && a_tracked.slot != kNoObjectSlot && IsResidentSlot(a_tracked.slot);
		if (wasMember)
			++residentStats.rewritten;
		if (d.entryClassSet) {
			trackedEntry->entryClassResolved = true;
			trackedEntry->entryClass = d.entryClass;
		}
		if (trackedEntry->entryClass != Api::EntryClass::None)
			ComponentsListed(*geometry, *trackedEntry, trackedEntry->entryClass);
		if (d.ownerSet) {
			trackedEntry->ownerResolved = true;
			trackedEntry->owner = d.owner;
		}
		if (d.verdictSet)
			trackedEntry->verdict = d.verdict;
		if (d.candidateSet) {
			trackedEntry->candidateFrame = d.candidateFrame;
			trackedEntry->candidateReason = d.candidateReason;
			NoteEngineCaster(geometry, r.write == EvalResult::Write::Record ? Ineligible::None : d.candidateReason);
		}
		if (r.referenceReason)
			referenceReasons[geometry] = r.reason;
		if (r.write != EvalResult::Write::Record)
			return false;

		const auto& leaf = r.leaf;
		const auto& g = *leaf.geometry;
		const bool shadowOnly = r.shadowOnly;
		// A face shape's positions come from its component. Without a snapshot it gets no record and waits for the component's publication.
		Api::EntryWrite face{ *geometry, trackedEntry->sourceRef.get() };
		trackedEntry->streamWaiting = r.entryTaken && !ComponentsResolve(face, trackedEntry->entryClass);
		// Withheld from the engine while it waits, as a bound object waiting for its pipeline is.
		if (trackedEntry->streamWaiting) {
			if (streamWaitingSince.try_emplace(geometry, sceneFrame).second)
				setPendingDirty = true;
			return false;
		}
		if (streamWaitingSince.erase(geometry))
			setPendingDirty = true;
		const std::uint32_t geometrySlot = ResolveWriteGeometry(geometry, g, r, timer, a_bucket);
		if (geometrySlot == Tables::kSlotFree)
			return false;

		std::uint32_t objectBoneRows = 0;
		ObjectRecord object = RecordOfWrite(geometry, leaf, r, geometrySlot, timer, objectBoneRows);
		// Past the last early return, so a slot is only taken by a record that is written.
		const std::uint32_t slotBefore = trackedEntry->slot;
		const std::uint32_t objectId = AcquireObjectSlot(*trackedEntry, geometry);
		// Otherwise per-frame entries would drop and retake their binding every frame.
		const bool keepMember = wasMember && objectId == slotBefore && a_bucket == Ineligible::None && !shadowOnly &&
		                        !(tables.objects[objectId].flags & kObjectFree);
		if (wasMember && !keepMember)
			DropResidentSlot(slotBefore, false);
		const bool keepHalf = keepMember;
		tables.objectSeen[objectId] = walkSerial;
		if (objectBoneRows)
			tables.PlaceBones(objectId, objectBoneRows);
		else
			tables.FreeBones(objectId);
		MergeShadowColumns(objectId, geometrySlot, leaf, r, object);
		MergeRecordRow(geometry, *trackedEntry, r, face, objectId, geometrySlot, keepHalf, object, a_bucket, timer);
		return true;
	}

	std::uint32_t Coordinator::ResolveWriteGeometry(RE::BSGeometry* geometry, const SceneCapture::GeometryRecord& g, const EvalResult& r, PartTimer& timer,
		Ineligible& a_bucket)
	{
		const Ineligible reason = r.reason;
		// Geometry slots are shared by every object drawing the same TriShape. A skinned shape draws each partition's buffers.
		const bool skinned = g.skin && g.skinPartition && !g.partitions.empty();
		const SceneCapture::GeometryRecord::Partition* skinPartition = skinned ? &g.partitions[0] : nullptr;
		const bool lodChain = r.lodChain;
		// entryRanges is written only by events, never during the walk.
		const auto* shapeRanges = lodChain ? EntryRangesOf(geometry) : nullptr;
		assert(!lodChain || shapeRanges);
		const std::uint32_t geometrySlot = lodChain ? ResolveEntryRangeSlots(g, *shapeRanges, timer) : ResolveGeometrySlot(g, skinPartition, timer);
		if (geometrySlot == Tables::kSlotFree) {
			--stats.ineligible[static_cast<std::size_t>(reason)];
			++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
			a_bucket = Ineligible::UnstableBuffer;
			NoteUnstable(geometry, g);
			return Tables::kSlotFree;
		}
		// All partitions whatever this frame's mask, since the main camera's may differ.
		if (skinned && g.partitions.size() > 1) {
			std::uint32_t previous = geometrySlot;
			bool unstable = false;
			for (std::size_t i = 1; i < g.partitions.size() && !unstable; ++i) {
				const std::uint32_t slot = ResolveGeometrySlot(g, &g.partitions[i], timer);
				if (slot == Tables::kSlotFree) {
					unstable = true;
					break;
				}
				if (tables.geometries[previous].nextPartition != slot) {
					tables.geometries[previous].nextPartition = slot;
					tables.NoteGeometry(previous);
				}
				IndexLink(previous, slot);
				previous = slot;
			}
			if (unstable) {
				--stats.ineligible[static_cast<std::size_t>(reason)];
				++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
				a_bucket = Ineligible::UnstableBuffer;
				NoteUnstable(geometry, g);
				return Tables::kSlotFree;
			}
			if (tables.geometries[previous].nextPartition != kNoPartition) {
				tables.geometries[previous].nextPartition = kNoPartition;
				tables.NoteGeometry(previous);
			}
		}
		return geometrySlot;
	}

	void Coordinator::NoteUnstable(RE::BSGeometry* a_geometry, const SceneCapture::GeometryRecord& a_record)
	{
		const auto forEachBuffer = [&](auto&& a_visit) {
			if (a_record.skin && a_record.skinPartition && !a_record.partitions.empty()) {
				for (const auto& p : a_record.partitions)
					for (auto* buffer : { p.vertexBuffer, p.indexBuffer })
						if (buffer)
							a_visit(buffer);
			} else {
				for (auto* buffer : { a_record.vertexBuffer, a_record.indexBuffer })
					if (buffer)
						a_visit(buffer);
			}
		};
		// The game can map a DYNAMIC or CPU-access buffer, which DXVK renames on every discard map, so it cannot be imported (cause 0).
		// A particle system has no buffers of its own (cause 2).
		const void* key = nullptr;
		forEachBuffer([&](ID3D11Buffer* a_buffer) { key = key ? key : a_buffer; });
		std::size_t cause = 2;
		if (!a_record.particles.system && key) {
			// A freed buffer's address can be reused, so this cache is dropped whole when it grows.
			if (unstableCauseOf.size() >= 4096)
				unstableCauseOf.clear();
			auto [it, inserted] = unstableCauseOf.try_emplace(key, std::uint8_t(1));
			if (inserted) {
				bool mappable = false;
				forEachBuffer([&](ID3D11Buffer* a_buffer) {
					D3D11_BUFFER_DESC desc{};
					a_buffer->GetDesc(&desc);
					mappable |= desc.Usage == D3D11_USAGE_DYNAMIC || desc.CPUAccessFlags != 0;
				});
				it->second = mappable ? 0 : 1;
			}
			cause = it->second;
		}
		++stats.unstableCauses[cause];
		constexpr std::uint32_t kMaxLogged = 16;
		if (unstableLogged >= kMaxLogged)
			return;
		++unstableLogged;
		std::string described;
		forEachBuffer([&](ID3D11Buffer* a_buffer) {
			D3D11_BUFFER_DESC desc{};
			a_buffer->GetDesc(&desc);
			described += fmt::format("{}{} B usage {} CPU access {:#x}", described.empty() ? "" : ", ", desc.ByteWidth, static_cast<int>(desc.Usage), desc.CPUAccessFlags);
		});
		constexpr std::array<const char*, 3> kCauses{ "a buffer the game can map", "a buffer DXVK refused to describe", "no buffers to resolve (no renderer data or particle palette)" };
		logger::info("[DCLF] unstable buffer: '{}' stays the engine's: {} ({}){}", a_geometry && a_geometry->name.c_str() ? a_geometry->name.c_str() : "",
			kCauses[cause], described.empty() ? "none" : described, unstableLogged == kMaxLogged ? "; no more such lines this session" : "");
	}

	ObjectRecord Coordinator::RecordOfWrite(RE::BSGeometry* geometry, const SceneCapture::LeafView& leaf, const EvalResult& r, std::uint32_t geometrySlot,
		PartTimer& timer, std::uint32_t& objectBoneRows)
	{
		const bool shadowOnly = r.shadowOnly;
		ObjectRecord object{};
		object.geometryIndex = geometrySlot;
		// BuildDrawsCS rejects kObjectNoBindings before reading the indices.
		object.materialIndex = 0;
		object.pipelineIndex = 0;
		object.flags = kObjectNoBindings | (shadowOnly ? (r.focusOnly ? kObjectFocusOnly : kObjectShadowOnly) : 0u);
		// Set here, not by the accumulate phase, since the shadow epochs key pipelines on them and read the alpha reference before it runs.
		using PropertyFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (leaf.property && (leaf.property->flags & static_cast<std::uint64_t>(PropertyFlag::kTwoSided)))
			object.flags |= kObjectTwoSided;
		if (leaf.AlphaTesting())
			object.flags |= kObjectAlphaTest | (static_cast<std::uint32_t>(leaf.alpha->threshold) << kObjectAlphaThresholdShift) |
			                (leaf.AlphaBlending() ? kObjectAlphaBlended : 0u);
		// FrameValues runs the engine's palette update itself (AE FUN_140e4ff90), which sizes the palette by the bone count.
		// So the block is placed by that count here.
		if (r.skinCandidate) {
			timer.Add(BuildPart::Record);
			skinnedObjects.push_back(geometry);
			if (r.boneRows) {
				objectBoneRows = r.boneRows;
				object.flags |= kObjectPaletteBlock;
				++stats.skinned;
				stats.boneRows += r.boneRows;
			}
			timer.Add(BuildPart::Skinning);
		}
		if (r.particleRows) {
			objectBoneRows = r.particleRows;
			object.flags |= kObjectPaletteBlock;
		}
		return object;
	}

	void Coordinator::MergeShadowColumns(std::uint32_t objectId, std::uint32_t geometrySlot, const SceneCapture::LeafView& leaf, const EvalResult& r,
		ObjectRecord& object)
	{
		using PropertyFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		// Decided here, since every shadow view renders before the accumulate phase.
		const auto* shadowProperty = leaf.property;
		const auto shadowReject = static_cast<ShadowReject>(r.shadowReject);
		static_assert(static_cast<std::size_t>(ShadowReject::Count) <= std::tuple_size_v<decltype(stats.shadowRejects)>);
		++stats.shadowRejects[static_cast<std::size_t>(shadowReject)];
		if (r.shadowDiffuse && tables.shadowTextureSeen.insert(r.shadowDiffuse).second)
			tables.shadowTextureSet.push_back(r.shadowDiffuse);
		auto useKey = [&](std::vector<ShadowPipelineKey>& a_used, std::uint32_t a_technique) {
			const ShadowPipelineKey key{ a_technique,
				(shadowProperty->flags & static_cast<std::uint64_t>(PropertyFlag::kTwoSided)) ? kRasterTwoSided : 0u,
				VertexLayoutOf(tables.geometries[geometrySlot].vertexDesc) };
			if (std::find(a_used.begin(), a_used.end(), key) == a_used.end())
				a_used.push_back(key);
		};
		const bool volumetricOnly = shadowReject == ShadowReject::VolumetricOnly;
		if (shadowReject == ShadowReject::None || volumetricOnly) {
			++stats.shadowCasters;
			if (volumetricOnly)
				object.flags |= kObjectVolumetricOnly;
			tables.shadowTechnique[objectId] = r.shadowTechnique;
			useKey(tables.shadowKeysUsed, r.shadowTechnique);
		} else {
			object.flags |= kObjectNoShadow;
			tables.shadowTechnique[objectId] = 0;
		}
		tables.shadowReject[objectId] = r.shadowReject;
		// The size test is the view's BuildDraws', so an animated bound crossing it is no classification input.
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			tables.occlusionTechnique[v][objectId] = r.occlusion[v];
			if (r.occlusion[v])
				useKey(tables.occlusionKeysUsed[v], r.occlusion[v]);
		}
	}

	void Coordinator::MergeRecordRow(RE::BSGeometry* geometry, Tracked& a_tracked, const EvalResult& r, Api::EntryWrite& face, std::uint32_t objectId,
		std::uint32_t geometrySlot, bool keepHalf, ObjectRecord& object, Ineligible a_bucket, PartTimer& timer)
	{
		auto* trackedEntry = &a_tracked;
		const auto& leaf = r.leaf;
		const bool shadowOnly = r.shadowOnly;
		const std::uint16_t partitionMask = r.partitionMask;
		ResolveSunEntry(*trackedEntry, *geometry);
		tables.listedRoot[objectId] = trackedEntry->entryRoot;
		tables.hasFadeNode[objectId] = leaf.property && leaf.property->fadeNode ? 1 : 0;
		if (face.positions)
			ComponentsStreamed(objectId, face, trackedEntry->entryClass);
		else
			tables.ClearStreamRow(objectId);
		tables.shadowDiffuse[objectId] = r.shadowDiffuse;
		tables.shadowMaterial[objectId] = r.shadowMaterial;
		tables.sceneFlags[objectId] = object.flags;
		tables.objectGeometry[objectId] = geometry;
		ComponentsSlotWritten(objectId);
		tables.objectIdentity[objectId] = trackedEntry->identity;
		tables.objectGroup[objectId] = trackedEntry->groupIdentity;
		const std::uint32_t keptPipeline = tables.draws[objectId].pipelineIndex;
		if (keepHalf) {
			// The scene bits the accumulate phase keeps come from this write. Everything else is the patch's.
			const auto& kept = tables.objects[objectId];
			object.flags = (object.flags & kSceneKeptFlags) | (kept.flags & ~kSceneKeptFlags);
			object.materialIndex = kept.materialIndex;
			object.pipelineIndex = kept.pipelineIndex;
			tables.objects[objectId] = object;
		} else {
			// The accumulate phase allocates extras rows, since it derives the descriptors that need them.
			tables.FreeExtras(objectId);
			tables.objects[objectId] = object;
			tables.ancestorValue[objectId] = ObjectAncestorValue{};
			tables.fadeDistance[objectId] = 0.0f;
		}
		tables.SetClassMembers(objectId, trackedEntry->owner.ownerClass, trackedEntry->owner.published ? trackedEntry->groupIdentity : 0,
			trackedEntry->identity);
		if (trackedEntry->owner.published)
			tables.classMembers[static_cast<std::size_t>(trackedEntry->owner.ownerClass)].objects.push_back(objectId);
		ComponentsRewritten(objectId, true, tables.skinPartitions[objectId], partitionMask);
		tables.skinPartitions[objectId] = partitionMask;
		tables.skinLodPartitions[objectId] = r.skinLodWord;
		if (!denseWalk) {
			trackedEntry->objectStamp = objectStamp;
			trackedEntry->objectId = objectId;
			if (a_bucket == Ineligible::None && !shadowOnly)
				bindQueue.push_back(objectId);
		}

		tables.draws[objectId] = DrawTemplateOf(tables.geometries[geometrySlot], keepHalf ? keptPipeline : 0);
		IndexObjectHead(objectId, geometrySlot);
		timer.Add(BuildPart::Record);
		WriteLayer(geometry, leaf, *trackedEntry, objectId, a_bucket == Ineligible::None && !shadowOnly, keepHalf, timer);
	}

	bool Coordinator::WriteObject(RE::BSGeometry* geometry, Tracked& a_tracked, PartTimer& timer, Ineligible& a_bucket)
	{
		EvalCounters counters;
		EvalResult result;
		{
			ShardScope scope(*this, false, &counters.mirror);
			EvaluateWrite(geometry, a_tracked, counters, result, timer);
		}
		MergeCounters(counters);
		return MergeWrite(geometry, a_tracked, result, timer, a_bucket);
	}

	void Coordinator::WriteLayer(RE::BSGeometry* a_geometry, const SceneCapture::LeafView& a_leaf, Tracked& a_tracked, std::uint32_t a_base, bool a_member, bool a_keepMember,
		PartTimer& a_timer)
	{
		const auto* property = a_member ? SceneCapture::LeafView::Lighting(a_leaf.layer) : nullptr;
		const std::uint32_t geometrySlot = property ? ResolveLayerGeometrySlot(*a_leaf.geometry, a_timer) : Tables::kSlotFree;
		if (geometrySlot == Tables::kSlotFree) {
			if (!denseWalk)
				ReleaseLayerSlot(a_tracked);
			return;
		}
		const std::uint32_t slotBefore = a_tracked.layerSlot;
		const std::uint32_t slot = AcquireLayerSlot(a_tracked, a_geometry, a_base);
		const bool same = !denseWalk && slot == slotBefore;
		const Tables::Columns columnsBefore = same ? tables.ColumnsOf(slot) : Tables::Columns{};
		const bool keepMember = same && a_keepMember && IsResidentSlot(slot) && !(tables.objects[slot].flags & kObjectFree);
		if (!denseWalk && !keepMember && IsResidentSlot(slot))
			DropResidentSlot(slot, false);
		ObjectRecord object{};
		object.geometryIndex = geometrySlot;
		// The layer's draws apply no alpha property and cast no shadow.
		object.flags = kObjectNoBindings | kObjectNoShadow |
		               ((property->flags & static_cast<std::uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided)) ? kObjectTwoSided : 0u);
		tables.objectSeen[slot] = walkSerial;
		tables.FreeBones(slot);
		tables.shadowTechnique[slot] = 0;
		tables.shadowReject[slot] = static_cast<std::uint8_t>(ShadowReject::Layer);
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			tables.occlusionTechnique[v][slot] = 0;
		tables.listedRoot[slot] = tables.listedRoot[a_base];
		tables.hasFadeNode[slot] = property->fadeNode ? 1 : 0;
		tables.ClearStreamRow(slot);
		tables.shadowDiffuse[slot] = nullptr;
		tables.shadowMaterial[slot] = nullptr;
		tables.sceneFlags[slot] = object.flags;
		tables.objectGeometry[slot] = a_geometry;
		ComponentsSlotWritten(slot);
		tables.objectIdentity[slot] = a_tracked.identity;
		tables.objectGroup[slot] = a_tracked.groupIdentity;
		ComponentsRewritten(slot, true, 0u, 0u);  // a layer's partitions are not rewritten here
		tables.layerBase[slot] = a_base;
		// Replayed by slot only when noted, and the base's write may note no change.
		if (std::exchange(tables.layerOf[a_base], slot) != slot)
			tables.NotePublished(a_base);
		const std::uint32_t keptPipeline = tables.draws[slot].pipelineIndex;
		if (keepMember) {
			const auto& kept = tables.objects[slot];
			object.flags = (object.flags & kSceneKeptFlags) | (kept.flags & ~kSceneKeptFlags);
			object.materialIndex = kept.materialIndex;
			object.pipelineIndex = kept.pipelineIndex;
			tables.objects[slot] = object;
		} else {
			tables.FreeExtras(slot);
			tables.objects[slot] = object;
			tables.ancestorValue[slot] = ObjectAncestorValue{};
			tables.fadeDistance[slot] = 0.0f;
		}
		tables.SetClassMembers(slot, Api::EntryClass::None, 0, a_tracked.identity);
		tables.skinPartitions[slot] = 0;
		tables.skinLodPartitions[slot] = 0;
		tables.draws[slot] = DrawTemplateOf(tables.geometries[geometrySlot], keepMember ? keptPipeline : 0);
		IndexObjectHead(slot, geometrySlot);
		if (denseWalk)
			return;
		if (same)
			tables.NoteWrite(slot, columnsBefore);
		else
			tables.NoteChange(slot, kChangeAll);
		shadowSetsDirty = true;
		shadowDirtySlots.push_back(slot);
		bindQueue.push_back(slot);
	}

	/** @brief Returns Tables::kSlotFree when the buffers cannot be made stable for the render graph. */
	std::uint32_t Coordinator::ResolveGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, const SceneCapture::GeometryRecord::Partition* a_partition,
		PartTimer& a_timer)
	{
		// A particle system draws the renderer's own quads (FUN_1414f2ad0 type 1), vertices pulled from its palette block.
		// One slot per capacity bucket, keyed by a DCLF address.
		const auto itemClass = !a_partition && a_geometry.particles.system ? Extensions::GeometryTypeContributionOf(a_geometry.type).itemClass :
		                                                                     Api::FrameValueItemClass::None;
		if (itemClass != Api::FrameValueItemClass::None) {
			Api::PaletteGeometrySource palette;
			if (!Extensions::PaletteGeometryOf(itemClass, a_geometry.particles.capacity, palette))
				return Tables::kSlotFree;
			GeometrySource source;
			source.key = static_cast<const RE::BSGraphics::TriShape*>(palette.key);
			source.vertexBuffer = palette.vertexBuffer;
			source.indexBuffer = palette.indexBuffer;
			source.vertexDesc = palette.vertexDesc;
			source.vertexCount = palette.vertexCount;
			source.indexCount = palette.indexCount;
			source.layer = true;  // its key is no TriShape (Tables::geometryLayerKey: never read as one)
			return ResolveGeometrySource(source, a_timer);
		}
		const void* key = a_partition ? a_partition->rendererData : a_geometry.rendererData;
		if (!key)
			return Tables::kSlotFree;
		GeometrySource source;
		source.key = static_cast<const RE::BSGraphics::TriShape*>(key);
		source.vertexBuffer = a_partition ? a_partition->vertexBuffer : a_geometry.vertexBuffer;
		source.indexBuffer = a_partition ? a_partition->indexBuffer : a_geometry.indexBuffer;
		source.vertexDesc = a_partition ? a_partition->vertexDesc : a_geometry.vertexDesc;
		source.vertexCount = a_partition ? a_partition->vertices : a_geometry.vertexCount;
		source.indexCount = (a_partition ? a_partition->triangles : a_geometry.triangleCount) * 3u;
		return ResolveGeometrySource(source, a_timer);
	}

	std::uint32_t Coordinator::ResolveEntryRangeSlots(const SceneCapture::GeometryRecord& a_geometry, const std::vector<Api::EntryRange>& a_ranges, PartTimer& a_timer)
	{
		// As the engine draws it (FUN_1414f2ad0 type 8), DrawIndexed per range.
		if (!a_geometry.rendererData || a_ranges.empty())
			return Tables::kSlotFree;
		std::uint32_t first = Tables::kSlotFree, previous = Tables::kSlotFree;
		for (const auto& range : a_ranges) {
			GeometrySource source;
			source.key = reinterpret_cast<const RE::BSGraphics::TriShape*>(range.key);
			source.vertexBuffer = a_geometry.vertexBuffer;
			source.indexBuffer = a_geometry.indexBuffer;
			source.vertexDesc = a_geometry.vertexDesc;
			source.vertexCount = a_geometry.vertexCount;
			source.indexCount = range.indexCount;
			source.firstIndex = range.firstIndex;
			const std::uint32_t slot = range.key ? ResolveGeometrySource(source, a_timer) : Tables::kSlotFree;
			if (slot == Tables::kSlotFree)
				return Tables::kSlotFree;
			if (previous == Tables::kSlotFree) {
				first = slot;
			} else {
				if (tables.geometries[previous].nextPartition != slot) {
					tables.geometries[previous].nextPartition = slot;
					tables.NoteGeometry(previous);
				}
				IndexLink(previous, slot);
			}
			previous = slot;
		}
		if (tables.geometries[previous].nextPartition != kNoPartition) {
			tables.geometries[previous].nextPartition = kNoPartition;
			tables.NoteGeometry(previous);
		}
		return first;
	}

	std::uint32_t Coordinator::ResolveLayerGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, PartTimer& a_timer)
	{
		// As the engine's hint-12 draw (FUN_140e465d0): the second index list (altIndexBuffer) as R16, from index 0.
		if (!a_geometry.rendererData || !a_geometry.altIndexBuffer || !a_geometry.altPrimCount)
			return Tables::kSlotFree;
		GeometrySource source;
		source.key = static_cast<const RE::BSGraphics::TriShape*>(a_geometry.altIndexList);
		source.vertexBuffer = a_geometry.vertexBuffer;
		source.indexBuffer = a_geometry.altIndexBuffer;
		source.vertexDesc = a_geometry.vertexDesc;
		source.vertexCount = a_geometry.vertexCount;
		source.indexCount = a_geometry.altPrimCount * 3;
		source.layer = true;
		return ResolveGeometrySource(source, a_timer);
	}

	void Coordinator::PrefetchGeometryBuffers(std::size_t a_first)
	{
		// Names the round's buffers to GpuResources at once, so there is one sync with DXVK's worker per round, not per new buffer.
		if (!frameResolveBuffers)
			return;
		DCLF_ZONE("CS.DCLF.Scene.PrefetchBuffers");
		std::vector<ID3D11Buffer*> buffers;
		auto add = [&](const void* a_shape, ID3D11Buffer* a_vertices, ID3D11Buffer* a_indices) {
			if (!a_shape)
				return;
			buffers.push_back(a_vertices);
			buffers.push_back(a_indices);
		};
		for (std::size_t i = a_first; i < order.size(); ++i) {
			const Tracked& entry = *order[i].tracked;
			if (entry.perFrame || (entry.slot == kNoObjectSlot && entry.candidateFrame != 0 && entry.candidateReason != Ineligible::None))
				continue;
			const auto* g = mirror.Geometry(order[i].geometry);
			if (!g)
				continue;
			if (g->skin && g->skinPartition) {
				for (const auto& p : g->partitions)
					add(p.rendererData, p.vertexBuffer, p.indexBuffer);
			} else {
				add(g->rendererData, g->vertexBuffer, g->indexBuffer);
			}
			if (g->altIndexList)
				buffers.push_back(g->altIndexBuffer);
		}
		GpuResources::Scene().Prefetch(buffers);
	}

	std::uint32_t Coordinator::ResolveGeometrySource(const GeometrySource& a_source, PartTimer& a_timer)
	{
		auto& gpu = GpuResources::Scene();
		const bool resolveBuffers = frameResolveBuffers;
		auto geometryIt = geometryIndex.find(a_source.key);
		const bool newGeometry = geometryIt == geometryIndex.end();
		// Differing buffers mean a TriShape reallocated at the same address, or one resolved while the graph was off.
		const bool staleGeometry = !newGeometry &&
		                           ((resolveBuffers && tables.geometries[geometryIt->second].vertexAddress == 0) ||
		                               tables.geometries[geometryIt->second].firstIndex != a_source.firstIndex ||
		                               tables.geometries[geometryIt->second].indexCount != a_source.indexCount ||
		                               (resolveBuffers && (!tables.geometryImports[geometryIt->second].vertexOwner || !tables.geometryImports[geometryIt->second].indexOwner)) ||
		                               tables.geometries[geometryIt->second].vertexBuffer != a_source.vertexBuffer ||
		                               tables.geometries[geometryIt->second].indexBuffer != a_source.indexBuffer ||
		                               (resolveBuffers && !denseWalk && relocatedGeometrySlots.contains(geometryIt->second)));
		if (staleGeometry)
			++stats.geometriesRefreshed;
		if (newGeometry || staleGeometry) {
			// The leases keep the buffers resolved and unmoved while the slot lives, since the graph reads them in place.
			std::optional<GpuResources::LeasedBuffer> vertexLease, indexLease;
			if (resolveBuffers) {
				vertexLease = gpu.Acquire(a_source.vertexBuffer);
				if (vertexLease)
					indexLease = gpu.Acquire(a_source.indexBuffer);
				if (!vertexLease || !indexLease) {
					// Nothing inserted, so the next object sharing this TriShape retries.
					if (staleGeometry) {
						const std::uint32_t slot = geometryIt->second;
						FreeGeometrySlot(slot);
						freedGeometry.push_back(slot);
						slotsFreedThisFrame = true;
					}
					a_timer.Add(BuildPart::Resolve);
					return Tables::kSlotFree;
				}
			}
			GeometryBuffers record;
			record.vertexBuffer = a_source.vertexBuffer;
			record.indexBuffer = a_source.indexBuffer;
			record.vertexDesc = a_source.vertexDesc;
			// The desc's low nibble, in dwords.
			record.vertexStride = static_cast<std::uint32_t>(record.vertexDesc & 0xFu) * 4u;
			record.vertexCount = a_source.vertexCount;
			record.indexCount = a_source.indexCount;
			record.firstIndex = a_source.firstIndex;
			if (vertexLease && indexLease && (!vertexLease->buffer.ref || !indexLease->buffer.ref)) {
				static std::atomic<bool> logged{ false };
				if (!logged.exchange(true))
					logger::warn("[DCLF] a geometry's buffers resolved without a buffer view ref (vertex slot {}, index slot {}): its objects are not drawn",
						vertexLease->buffer.ref.viewSlot, indexLease->buffer.ref.viewSlot);
			}
			if (vertexLease && indexLease && vertexLease->buffer.ref && indexLease->buffer.ref) {
				record.vertexAddress = vertexLease->buffer.address;
				record.vertexRef = vertexLease->buffer.ref;
				record.vertexBytes = vertexLease->buffer.size;
				record.indexAddress = indexLease->buffer.address;
				record.indexRef = indexLease->buffer.ref;
				record.indexBytes = indexLease->buffer.size;
#if defined(CS_HAS_RENDER_GRAPH)
				RenderGraph::CheckBufferRef(record.vertexRef, record.vertexAddress, record.vertexBytes, "geometry vertices");
				RenderGraph::CheckBufferRef(record.indexRef, record.indexAddress, record.indexBytes, "geometry indices");
#endif
			}
			const std::uint32_t slot = staleGeometry ? geometryIt->second : AllocateGeometrySlot();
			// Other objects' draws copied the old record, so the delta walk re-evaluates the ones it kept.
			if (staleGeometry)
				refreshedGeometry.push_back(slot);
			tables.geometries[slot] = record;
			if (!denseWalk)
				relocatedGeometrySlots.erase(slot);
			// A stale slot's old leases outlive the frames in flight.
			for (auto* owner : { &tables.geometryImports[slot].vertexOwner, &tables.geometryImports[slot].indexOwner })
				if (*owner)
					retiredImports.push_back(std::move(*owner));
			tables.geometryImports[slot] = vertexLease && indexLease ? Tables::GeometryImport{ vertexLease->generation, indexLease->generation,
				vertexLease->owner, indexLease->owner } : Tables::GeometryImport{};
			tables.NoteGeometry(slot);
			tables.geometrySlotKey[slot] = a_source.key;
			tables.geometryLayerKey[slot] = a_source.layer ? 1 : 0;
			if (!staleGeometry)
				geometryIt = geometryIndex.emplace(a_source.key, slot).first;
			a_timer.Add(BuildPart::Resolve);
		} else if (resolveBuffers && tables.geometryLastUsed[geometryIt->second] != PassStamp()) {
			a_timer.Add(BuildPart::DedupHit);
		}
		tables.geometryLastUsed[geometryIt->second] = PassStamp();
		return geometryIt->second;
	}

	void Coordinator::IndexObjectHead(std::uint32_t a_object, std::uint32_t a_head)
	{
		if (denseWalk)
			return;
		if (objectHead.size() <= a_object) {
			objectHead.resize(std::size_t(a_object) + 1, kNoPartition);
			objectHeadAt.resize(std::size_t(a_object) + 1, 0);
		}
		const std::uint32_t old = objectHead[a_object];
		if (old == a_head)
			return;
		if (old != kNoPartition)
			if (const auto it = headObjects.find(old); it != headObjects.end()) {
				auto& list = it->second;
				const std::uint32_t at = objectHeadAt[a_object];
				if (at < list.size() && list[at] == a_object) {
					list[at] = list.back();
					objectHeadAt[list[at]] = at;
					list.pop_back();
				}
				if (list.empty())
					headObjects.erase(it);
			}
		auto& list = headObjects[a_head];
		objectHead[a_object] = a_head;
		objectHeadAt[a_object] = static_cast<std::uint32_t>(list.size());
		list.push_back(a_object);
	}

	void Coordinator::IndexLink(std::uint32_t a_from, std::uint32_t a_to)
	{
		if (denseWalk || a_to == kNoPartition)
			return;
		auto& from = linkedFrom[a_to];
		if (std::find(from.begin(), from.end(), a_from) == from.end())
			from.push_back(a_from);
	}

	void Coordinator::ObjectsUnder(const std::vector<std::uint32_t>& a_slots, bool a_chains, std::vector<std::uint32_t>& a_out) const
	{
		// Walk back along the links still pointing at a slot. An entry whose link moved on is skipped.
		std::vector<std::uint32_t> heads(a_slots.begin(), a_slots.end());
		if (a_chains) {
			ankerl::unordered_dense::set<std::uint32_t> seen(a_slots.begin(), a_slots.end());
			for (std::size_t i = 0; i < heads.size(); ++i) {
				const std::uint32_t g = heads[i];
				const auto it = linkedFrom.find(g);
				if (it == linkedFrom.end())
					continue;
				for (const std::uint32_t from : it->second)
					if (from < tables.geometries.size() && tables.geometries[from].nextPartition == g && seen.insert(from).second)
						heads.push_back(from);
			}
		}
		a_out.clear();
		for (const std::uint32_t head : heads)
			if (const auto it = headObjects.find(head); it != headObjects.end())
				a_out.insert(a_out.end(), it->second.begin(), it->second.end());
		std::sort(a_out.begin(), a_out.end());
		a_out.erase(std::unique(a_out.begin(), a_out.end()), a_out.end());
	}

	void Coordinator::BuildFullOrder()
	{
		order.clear();
		order.reserve(tracked.size());
		for (auto& [geometry, entry] : tracked) {
			// In full, since SweepObjectSlots frees every slot this walk did not write and the light path writes nothing.
			entry.scheduledWalk = walkSerial;
			entry.fullWalk = walkSerial;
			order.push_back({ geometry, &entry, nullptr });
		}
	}

	void Coordinator::Schedule(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_full)
	{
		if (a_full)
			a_tracked.fullWalk = walkSerial;
		if (a_tracked.scheduledWalk == walkSerial && a_tracked.movedWalk != walkSerial)
			return;
		a_tracked.scheduledWalk = walkSerial;
		a_tracked.movedWalk = 0;
		order.push_back({ a_geometry, &a_tracked, nullptr });
	}

	Coordinator::ShadowInputs Coordinator::ShadowInputsOf(std::uint32_t a_slot) const
	{
		if (a_slot == kNoObjectSlot || a_slot >= tables.objects.size() || (tables.objects[a_slot].flags & kObjectFree))
			return {};
		const auto& object = tables.objects[a_slot];
		return { tables.shadowDiffuse[a_slot], object.geometryIndex < tables.geometries.size() ? tables.geometries[object.geometryIndex].vertexDesc : 0,
			tables.shadowTechnique[a_slot], object.flags & (kObjectNoShadow | kObjectTwoSided | kObjectFree), tables.shadowReject[a_slot],
			{ tables.occlusionTechnique[kOcclusionSky][a_slot], tables.occlusionTechnique[kOcclusionPrecipitation][a_slot] } };
	}

	bool Coordinator::KeepSkin(RE::BSGeometry* a_geometry, Tracked& a_tracked, const EvalResult& a_result)
	{
		// The slot half of the light path's skin check. A skin, dismember or rigid-body change reclassifies the entry instead.
		const std::uint32_t slot = a_tracked.slot;
		const std::uint32_t rows = a_result.skinRows;
		if (!(tables.objects[slot].flags & kObjectPaletteBlock) || rows != tables.boneRows[slot])
			return false;
		skinnedObjects.push_back(a_geometry);
		++stats.lightSkins;
		if (tables.skinPartitions[slot] != a_result.skinPartitions || tables.skinLodPartitions[slot] != a_result.skinLodPartitions)
			tables.NoteChange(slot, kChangeSkin);
		if (tables.skinPartitions[slot] != a_result.skinPartitions)
			ComponentsRewritten(slot, false, tables.skinPartitions[slot], a_result.skinPartitions);
		tables.skinPartitions[slot] = a_result.skinPartitions;
		tables.skinLodPartitions[slot] = a_result.skinLodPartitions;
		++stats.skinned;
		stats.boneRows += rows;
		return true;
	}

	bool Coordinator::RootMoves(const RE::NiAVObject* a_root, const bool* a_now)
	{
		// Only a reference's root. A multibound's bound is its shape's, and actors are never tested.
		const auto* record = a_root ? mirror.Node(a_root) : nullptr;
		if (!record || !record->userData)
			return false;
		// Kept until an event under the root or its last dependent leaving forgets it.
		const auto [motion, inserted] = rootMotion.try_emplace(a_root, false);
		if (inserted) {
			motion->second = a_now ? *a_now : RootMovesNow(a_root);
			if (motion->second && movingRoots.try_emplace(a_root, MovingRoot{ static_cast<const RE::TESObjectREFR*>(record->userData), FindCategoryNode(const_cast<RE::NiAVObject*>(a_root), nullptr) }).second)
				placementRootsTouched.push_back(a_root);
		}
		return motion->second;
	}

	bool Coordinator::RootMovesNow(const RE::NiAVObject* a_root) const
	{
		const auto* root = a_root ? mirror.Node(a_root) : nullptr;
		if (!root || !root->userData)
			return false;
		bool moves = false;
		ForEachMirror(mirror, a_root, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
			if (a_record.controllers || a_record.body)
				moves = true;
			else if (a_record.kind & SceneCapture::kKindGeometry)
				// A skin's bound follows its bones, which move with no controller or body on the chain.
				if (const auto* geometry = mirror.Geometry(a_key))
					moves = geometry->skin != nullptr;
			return !moves;
		}, kMaxEventNodes);
		return moves;
	}

	void Coordinator::ListDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		// An event on any property this evaluation read classifies the entry again.
		const auto leaf = mirror.Leaf(a_geometry);
		const void* property = leaf.geometry ? leaf.geometry->property : nullptr;
		const void* alpha = leaf.geometry ? leaf.geometry->alpha : nullptr;
		if (property != a_tracked.listedProperty) {
			if (a_tracked.listedProperty)
				Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedProperty, a_geometry);
			if (property)
				List(propertyDependents, propertyDependentsAt, property, a_geometry);
			a_tracked.listedProperty = property;
		}
		if (alpha != a_tracked.listedAlpha) {
			if (a_tracked.listedAlpha)
				Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedAlpha, a_geometry);
			if (alpha)
				List(propertyDependents, propertyDependentsAt, alpha, a_geometry);
			a_tracked.listedAlpha = alpha;
		}
		// The weather rewrites the shared external emittance colour in place, so it is followed by emittance events.
		const auto* lighting = SceneCapture::LeafView::Lighting(leaf.property);
		const auto* port = SceneCapture::LeafView::Port(leaf.property);
		const void* emittance = lighting && (lighting->flags & static_cast<std::uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance)) ? lighting->emissive :
		                        port && Extensions::PortRules(port->port).emissiveAlwaysListed                                                  ? port->emissive :
		                                                                                                                                          nullptr;
		if (emittance != a_tracked.listedEmittance) {
			if (a_tracked.listedEmittance)
				Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedEmittance, a_geometry);
			if (emittance)
				List(propertyDependents, propertyDependentsAt, emittance, a_geometry);
			a_tracked.listedEmittance = emittance;
		}
		// The fade node crossing 1 changes the synthetic pass's hint.
		const auto* fadeRecord = lighting && lighting->fadeNode ? leaf.FadeNodeOf(lighting) : nullptr;
		const void* fadeNode = fadeRecord ? fadeRecord->key : nullptr;
		if (fadeNode != a_tracked.listedFadeNode) {
			if (a_tracked.listedFadeNode)
				Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedFadeNode, a_geometry);
			if (fadeNode)
				List(propertyDependents, propertyDependentsAt, fadeNode, a_geometry);
			a_tracked.listedFadeNode = fadeNode;
		}
		const void* layer = leaf.geometry ? leaf.geometry->Layer() : nullptr;
		if (layer != a_tracked.listedLayerProperty) {
			if (a_tracked.listedLayerProperty)
				Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedLayerProperty, a_geometry);
			if (layer)
				List(propertyDependents, propertyDependentsAt, layer, a_geometry);
			a_tracked.listedLayerProperty = layer;
		}
	}

	void Coordinator::UnlistDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_root)
	{
		if (a_tracked.listedProperty)
			Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedProperty, a_geometry);
		if (a_tracked.listedAlpha)
			Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedAlpha, a_geometry);
		if (a_tracked.listedEmittance)
			Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedEmittance, a_geometry);
		if (a_tracked.listedLayerProperty)
			Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedLayerProperty, a_geometry);
		if (a_tracked.listedFadeNode)
			Unlist(propertyDependents, propertyDependentsAt, a_tracked.listedFadeNode, a_geometry);
		a_tracked.listedFadeNode = nullptr;
		a_tracked.listedProperty = nullptr;
		a_tracked.listedAlpha = nullptr;
		a_tracked.listedEmittance = nullptr;
		a_tracked.listedLayerProperty = nullptr;
		if (a_root)
			UnlistHiddenChain(a_geometry, a_tracked);
		if (a_root && a_tracked.lightRoot) {
			// Until the candidates update rewrites it, the snapshots naming it hold its node.
			MarkLightEntryDirty(a_tracked.lightRoot);
			if (!Unlist(lightDependents, lightDependentsAt, a_tracked.lightRoot, a_geometry))
				lightEntryChanges.push_back({ a_tracked.lightRoot, -1 });
			ReleaseRootOwner(a_tracked.lightRoot);
			a_tracked.lightRoot = nullptr;
		}
		if (a_root && a_tracked.listedRoot) {
			MarkListedRootDirty(a_tracked.listedRoot);
			TouchPlacedDependent(a_tracked.listedRoot, a_geometry);
			// The node is only a key here and may be gone.
			if (Unlist(rootDependents, rootDependentsAt, a_tracked.listedRoot, a_geometry)) {
				dirtyRoots.push_back(a_tracked.listedRoot);
			} else {
				rootMotion.erase(a_tracked.listedRoot);
				if (movingRoots.erase(a_tracked.listedRoot) + stillRoots.erase(a_tracked.listedRoot))
					placementRootsTouched.push_back(a_tracked.listedRoot);
				if (const auto reference = rootReference.find(a_tracked.listedRoot); reference != rootReference.end()) {
					if (const auto back = referenceRoot.find(reference->second); back != referenceRoot.end() && back->second == a_tracked.listedRoot)
						referenceRoot.erase(back);
					rootReference.erase(reference);
				}
			}
			ReleaseRootOwner(a_tracked.listedRoot);
			a_tracked.listedRoot = nullptr;
		}
	}

	bool Coordinator::OwnRoot(const RE::NiAVObject* a_root, bool a_live)
	{
		// An unheld root is never owned. The engine frees it when its count drops, and an owner would keep it alive for good.
		if (const auto unheld = alwaysRenderRoots.find(a_root); unheld != alwaysRenderRoots.end() && !unheld->second.root) {
			++referenceStats.refused;
			return false;
		}
		auto& owner = rootOwners[a_root];
		if (owner)
			return true;
		// Never a reference from a key alone.
		if (!(owner = Pinned(a_root)) && a_live) {
			++referenceStats.live;
			owner.reset(const_cast<RE::NiAVObject*>(a_root));
		}
		if (owner)
			return true;
		++referenceStats.refused;
		rootOwners.erase(a_root);
		return false;
	}

	void Coordinator::ReleaseRootOwner(const RE::NiAVObject* a_root)
	{
		if (!a_root || rootDependents.contains(a_root) || lightDependents.contains(a_root))
			return;
		if (const auto it = rootOwners.find(a_root); it != rootOwners.end()) {
			HandBack(std::move(it->second));
			rootOwners.erase(it);
		}
	}

	void Coordinator::ReleaseRootOwners()
	{
		for (auto& [root, owner] : rootOwners)
			HandBack(std::move(owner));
		rootOwners.clear();
	}

	RE::NiPointer<RE::NiAVObject> Coordinator::OwnedRoot(const RE::NiAVObject* a_root)
	{
		if (!a_root)
			return nullptr;
		if (const auto it = rootOwners.find(a_root); it != rootOwners.end())
			return it->second;
		if (unownedRoots++ == 0)
			logger::error("[DCLF] a root no dependents list names was handed out (step 6e F1: its reference would be made from a key); left out");
		return nullptr;
	}

	void Coordinator::Reclassify(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		a_tracked.candidateFrame = 0;
		Schedule(a_geometry, a_tracked);
	}

	bool Coordinator::PlacementMatters(const Tracked& a_tracked)
	{
		const auto reason = a_tracked.candidateReason;
		return a_tracked.slot != kNoObjectSlot || a_tracked.candidateFrame == 0 || reason == Ineligible::None || reason == Ineligible::Hidden ||
		       reason == Ineligible::Switch || reason == Ineligible::Actor;
	}

	void Coordinator::ScheduleRoot(const RE::NiAVObject* a_root, bool a_whole)
	{
		const auto it = rootDependents.find(a_root);
		auto& d = placementDeltaStats;
		if (!a_whole) {
			// The attach or detach may have changed the root's motion. Read it again now, and reclassify the dependents only if the verdict changed.
			++d.dirtyRoots;
			d.rootDependentsListed += it != rootDependents.end() ? it->second.size() : 0;
			const auto motion = rootMotion.find(a_root);
			if (motion == rootMotion.end() || RootMovesNow(a_root) == motion->second) {
				if (it != rootDependents.end() && Policy::WitnessRootSchedule(passSerial))
					for (auto* geometry : it->second) {
						const auto entry = tracked.find(geometry);
						if (entry == tracked.end() || !PlacementMatters(entry->second) || entry->second.scheduledWalk == walkSerial) {
							++d.rootWitnessSkipped;
							continue;
						}
						Reclassify(entry->first, entry->second);
						rootWitness.insert(geometry);
					}
				return;
			}
			++d.dirtyRootsWhole;
		}
		rootMotion.erase(a_root);
		if (movingRoots.erase(a_root))
			placementRootsTouched.push_back(a_root);
		if (it == rootDependents.end())
			return;
		for (auto* geometry : it->second)
			if (const auto entry = tracked.find(geometry); entry != tracked.end() && PlacementMatters(entry->second)) {
				Reclassify(entry->first, entry->second);
				d.rootDependentsScheduled += a_whole ? 0u : 1u;
			}
	}

	void Coordinator::MoveBucket(Tracked& a_tracked, Ineligible a_bucket)
	{
		const std::uint8_t bucket = a_bucket == Ineligible::Count ? Tracked::kNoBucket : static_cast<std::uint8_t>(a_bucket);
		if (a_tracked.bucket == bucket)
			return;
		if (a_tracked.bucket != Tracked::kNoBucket && buckets[a_tracked.bucket])
			--buckets[a_tracked.bucket];
		if (bucket != Tracked::kNoBucket)
			++buckets[bucket];
		a_tracked.bucket = bucket;
	}

	std::uint32_t Coordinator::PerFrameTraits(const Tracked& a_tracked, const RE::BSGeometry& a_geometry) const
	{
		return PerFrameTraitsOf(PerFrameEntryClass(a_tracked.entryClass), a_tracked.owner.everyFrame, a_tracked.categoryNode, a_geometry);
	}

	std::uint32_t Coordinator::PerFrameTraitsOf(bool a_entryClassTrait, bool a_ownerClassTrait, const RE::NiNode* a_categoryNode, const RE::BSGeometry& a_geometry) const
	{
		// A static reference never moves in place, so what moves is actor-owned, controller-animated or Havok-simulated. A controller on
		// the shading animates material alpha, which its events name, so it is never per frame.
		std::uint32_t traits = 0;
		traits |= a_entryClassTrait ? kTraitEntryClass : 0u;
		traits |= a_ownerClassTrait ? kTraitOwnerClass : 0u;
		const auto leaf = mirror.Leaf(&a_geometry);
		traits |= leaf.Skinned() ? kTraitSkin : 0u;
		// A geometry type whose objects change every frame unseen by the walk, such as a particle system, is a mover.
		if (leaf.geometry && Extensions::GeometryTypeMoves(ActiveToggles(), leaf.geometry->type))
			traits |= kTraitMoves;
		if (leaf.property && leaf.property->controllers)
			traits |= kTraitAnimatedShading;
		if (leaf.alpha && leaf.alpha->controllers)
			traits |= kTraitAnimatedShading;
		// A blended leaf under an ordered node is a mover, since its forward sort reads the node's live bound, which other children move.
		const bool orderedLeaf = Eligibility::OrderedLeaf(leaf);
		const void* key = &a_geometry;
		for (const auto* record = leaf.node; record; key = record->parent, record = record->parent ? mirror.Node(record->parent) : nullptr) {
			if (record->controllers || record->body || (orderedLeaf && (record->kind & SceneCapture::kKindOrdered))) {
				traits |= kTraitMoves;
				break;
			}
			if (key == a_categoryNode)
				break;
		}
		return traits;
	}

	std::pair<bool, std::uint32_t> Coordinator::PerFrameOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, Ineligible a_reason, std::uint32_t& a_traits,
		ankerl::unordered_dense::map<const RE::NiAVObject*, bool>* a_freshMotion)
	{
		// An unselected switch child gets a record by event when selected.
		const bool mayRecord = a_reason == Ineligible::None || Eligibility::DeferredToAccumulate(a_reason) || Eligibility::ShadowOnlyCaster(a_reason, mirror.Leaf(&a_geometry));
		std::uint32_t traits = PerFrameTraits(a_tracked, a_geometry);
		// An actor is placed every frame, so its root's motion is never read.
		if (mayRecord && !(traits & kTraitEntryClass) && !a_tracked.owner.ownMotion && a_tracked.entryRoot) {
			bool moves = false;
			if (a_freshMotion) {
				const auto [motion, inserted] = a_freshMotion->try_emplace(a_tracked.entryRoot, false);
				if (inserted)
					motion->second = RootMovesNow(a_tracked.entryRoot);
				moves = motion->second;
			} else {
				moves = RootMoves(a_tracked.entryRoot);
			}
			if (moves)
				traits |= kTraitRootMoves;
		}
		a_traits = traits;
		return PerFrameFrom(traits, a_reason, mayRecord);
	}

	std::pair<bool, std::uint32_t> Coordinator::PerFrameFrom(std::uint32_t a_traits, Ineligible a_reason, bool a_mayRecord)
	{
		// Per frame when a frame can change its record. An actor's equipment is shown, hidden and swapped with no event.
		// Root-bound motion and animated shading are not per frame, since the root pass and shading events cover them.
		const std::uint32_t traits = a_traits;
		const std::uint32_t moving = traits & ~kTraitAnimatedShading;
		const bool frameVerdict = a_reason == Ineligible::Hidden || a_reason == Ineligible::Switch || a_reason == Ineligible::Actor;
		const bool perFrame = (a_mayRecord && moving && moving != kTraitRootMoves && moving != kTraitEntryClass) || (traits & kTraitOwnerClass) ||
		                      (frameVerdict && (traits & kTraitMoves));
		// The light path takes only an actor's verdict recheck and a skin's partitions, when an event names the entry.
		// An entry that cannot have a record is written in full.
		const std::uint32_t light = perFrame && a_mayRecord &&
		                                    !(traits & ~(kTraitMoves | kTraitRootMoves | kTraitAnimatedShading | kTraitSkin | kTraitOwnerClass | kTraitEntryClass)) ?
		                                traits :
		                                0u;
		return { perFrame, light };
	}

	std::pair<bool, std::uint32_t> Coordinator::PerFrameMerge(const Tracked& a_tracked, const EvalResult& a_result)
	{
		std::uint32_t traits = a_result.traits;
		if (a_result.mayRecord && !(traits & kTraitEntryClass) && !a_tracked.owner.ownMotion && a_tracked.entryRoot) {
			const bool* now = a_tracked.entryRoot == a_result.rootCandidate ? &a_result.rootMovesNow : nullptr;
			if (RootMoves(a_tracked.entryRoot, now))
				traits |= kTraitRootMoves;
		}
		return PerFrameFrom(traits, a_tracked.candidateReason, a_result.mayRecord);
	}

	bool Coordinator::FrameVerdictKept(const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const
	{
		// Pure. The same verdict writes the same record except placement and palette, which the light path takes.
		const Ineligible cached = a_tracked.candidateReason;
		const bool frameVerdict = cached == Ineligible::None || cached == Ineligible::Switch || cached == Ineligible::Hidden || cached == Ineligible::Actor;
		if (!frameVerdict)
			return true;
		// Its inputs all have events or are retaken here, so without one it stands. A witness frame retakes it and counts an unannounced change.
		const bool announced = a_tracked.hiddenEventOwed || a_tracked.parentReason == Ineligible::Switch || !hiddenGating || a_tracked.hiddenChain.empty() ||
		                       ViewRootEntry(a_tracked.categoryNode);
		if (!announced && !hiddenWitness) {
			++a_counters.verdictsSkipped;
			return true;
		}
		++a_counters.verdictsChecked;
		const Ineligible now = ClassifyFrame(a_tracked);
		if (now != cached && !announced && !a_counters.verdictsMissed++)
			a_counters.firstVerdictMissed = fmt::format("'{}' {} now {}", a_tracked.geometry->name.c_str() ? a_tracked.geometry->name.c_str() : "?",
				kIneligibleNames[static_cast<std::size_t>(cached)], kIneligibleNames[static_cast<std::size_t>(now)]);
		if (now != cached && !announced)
			a_out.verdictUnannounced = true;
		return now == cached;
	}

	std::uint32_t Scene::ConfiguredFanout()
	{
		static const std::uint32_t configured = [] {
			const auto& value = SwitchValue(Switch::Fanout);
			return value == "0" ? 0u : value == "1" ? 1u : 2u;
		}();
		return configured;
	}

	std::uint32_t Coordinator::FanoutMode(std::size_t a_count, std::size_t a_least) const
	{
		// The mirror-read parity's live checks must run on the pass's own thread.
		if (Policy::SerialFanout())
			return 0;
		const std::uint32_t configured = ConfiguredFanout();
		// The pool only from the scene lane, never the render thread, since ParallelFor waits.
		if (configured == 2 && (!inSceneTask || a_count < a_least))
			return 1;
		return configured;
	}

	void Coordinator::EvaluateEntry(std::size_t a_index, bool a_light, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const
	{
		RE::BSGeometry* geometry = order[a_index].geometry;
		const Tracked& entry = *order[a_index].tracked;
		auto& r = a_out;
		a_timer.Add(BuildPart::LoopTail);
		r.kind = entry.lightTraits && entry.perFrame             ? EvaluateKind::Light :
		         !entry.perFrame                                 ? EvaluateKind::Event :
		         entry.entryClass != Api::EntryClass::None       ? EvaluateKind::EntryClass :
		         entry.owner.ownerClass != Api::EntryClass::None ? (entry.candidateReason == Ineligible::None ? EvaluateKind::Owned : EvaluateKind::OwnedNoRecord) :
		                                                           EvaluateKind::PerFrame;
		const bool profileKinds = a_light && Diagnostics::Observe().profile;
		struct KindSample
		{
			EvalCounters* counters;
			const EvaluateKind& kind;
			std::chrono::steady_clock::time_point start;
			~KindSample()
			{
				if (counters)
					counters->evaluateKindMs[static_cast<std::size_t>(kind)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			}
		} kindSample{ profileKinds ? &a_counters : nullptr, r.kind, profileKinds ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{} };
		if (a_light && entry.lightTraits && entry.perFrame && entry.fullWalk != walkSerial && entry.candidateFrame != 0) {
			const bool recorded = entry.slot != kNoObjectSlot && entry.objectStamp == objectStamp;
			bool kept = true;
			if (entry.lightTraits & kTraitOwnerClass)
				kept = recorded && FrameVerdictKept(entry, a_counters, r);
			else
				kept = recorded;
			if (kept && recorded && (entry.lightTraits & kTraitSkin)) {
				const auto* record = mirror.Geometry(geometry);
				const std::uint32_t rows = record ? SkinPalettes::RowsOf(*record) : 0u;
				kept = record && record->skin && Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Skinned) && SkinPalettes::Fits(rows);
				if (kept) {
					r.lightSkin = true;
					r.skinRows = rows;
					r.skinPartitions = SkinPartitions::Of(*record);
					r.skinLodPartitions = SkinPartitions::LodOf(mirror.Leaf(geometry));
				}
			}
			if (kept) {
				r.light = true;
				r.lightRecorded = recorded;
				return;
			}
		}
		if (r.kind == EvaluateKind::Light)
			r.kind = EvaluateKind::LightMissed;
		EvaluateWrite(geometry, entry, a_counters, r, a_timer);
		EvaluateClassified(geometry, entry, a_counters, r);
	}

	void Coordinator::EvaluateClassified(RE::BSGeometry* geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const
	{
		const auto& entry = a_tracked;
		const auto& d = a_out.delta;
		// A missing leaf resets the classification.
		const std::uint32_t candidateFrame = a_out.write == EvalResult::Write::NoLeaf ? (denseWalk ? entry.candidateFrame : 0u) :
		                                     d.candidateSet                            ? d.candidateFrame :
		                                                                                 entry.candidateFrame;
		const Ineligible reason = d.candidateSet ? d.candidateReason : entry.candidateReason;
		a_out.classified = candidateFrame == PassStamp();
		if (a_out.classified) {
			const Api::EntryClass entryClass = d.entryClassSet ? d.entryClass : entry.entryClass;
			const auto& owner = d.ownerSet ? d.owner : entry.owner;
			a_out.traits = PerFrameTraitsOf(PerFrameEntryClass(entryClass), owner.everyFrame, entry.categoryNode, *geometry);
			a_out.mayRecord = reason == Ineligible::None || Eligibility::DeferredToAccumulate(reason) || Eligibility::ShadowOnlyCaster(reason, a_out.leaf);
			if (a_out.mayRecord && !(a_out.traits & kTraitEntryClass) && !owner.ownMotion) {
				a_out.rootCandidate = entry.sunEntryResolved ? entry.entryRoot : static_cast<const RE::NiAVObject*>(MirrorSunEntry(geometry));
				if (a_out.rootCandidate) {
					const auto [motion, inserted] = a_counters.rootMotion.try_emplace(a_out.rootCandidate, false);
					if (inserted)
						motion->second = RootMovesNow(a_out.rootCandidate);
					a_out.rootMovesNow = motion->second;
				}
			}
			std::tie(a_out.moveKey, a_out.moveKeyOuter) = MoveKeysOf(*geometry, entry.categoryNode);
		}
	}

	void Coordinator::MergeEntry(std::size_t a_index, bool a_profileKinds, EvalResult& a_result, PartTimer& a_timer)
	{
		RE::BSGeometry* geometry = order[a_index].geometry;
		Tracked& entry = *order[a_index].tracked;
		auto& r = a_result;
		struct KindSample
		{
			Coordinator::Stats* stats;
			const EvaluateKind& kind;
			std::chrono::steady_clock::time_point start;
			~KindSample()
			{
				if (!stats)
					return;
				stats->evaluateKindMs[static_cast<std::size_t>(kind)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				++stats->evaluateKindCount[static_cast<std::size_t>(kind)];
			}
		} kindSample{ a_profileKinds ? &stats : nullptr, r.kind, a_profileKinds ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{} };
		// Evaluated after the pass's hidden events drained, so what they owed is paid.
		entry.hiddenEventOwed = false;
		if (r.verdictUnannounced)
			Diagnostics::Notify(Diagnostics::ActorVerdictUnannounced{ entry.hiddenChain });
		// Parity. Whatever the evaluation changes, the gate would have left as it was.
		const bool witnessed = entry.gateWitnessWalk == walkSerial && entry.fullWalk != walkSerial;
		const char* missed = witnessed && !r.light ? "the light path missed" : nullptr;
		auto witness = [&](bool a_moverChanged) {
			if (!witnessed)
				return;
			auto& g = moverGateStats;
			++g.witnessChecks;
			missed = missed ? missed : a_moverChanged ? "the mover set" : nullptr;
			if (!missed)
				return;
			++g.witnessDiffer;
			if (g.firstDiffer.empty())
				g.firstDiffer = fmt::format("{} on '{}' (traits {:X}, move keys {} {})", missed, entry.geometry && entry.geometry->name.c_str() ? entry.geometry->name.c_str() : "?",
					entry.lightTraits, entry.moveKey, entry.moveKeyOuter);
		};
		if (r.light && MergeLight(geometry, entry, r, a_timer, witnessed, missed)) {
			witness(NoteMover(geometry, entry, false));
			return;
		}
		const ShadowInputs shadowBefore = ShadowInputsOf(entry.slot);
		const bool hadSlot = entry.slot != kNoObjectSlot;
		const std::uint32_t slotBefore = entry.slot;
		const Tables::Columns columnsBefore = hadSlot ? tables.ColumnsOf(slotBefore) : Tables::Columns{};
		const Ineligible reasonBefore = entry.candidateReason;
		const bool rootWitnessed = !rootWitness.empty() && rootWitness.contains(geometry);
		const bool perFrameBefore = entry.perFrame;
		const std::uint32_t traitsBefore = entry.lightTraits;
		Ineligible bucket = Ineligible::Count;
		const bool written = MergeWrite(geometry, entry, r, a_timer, bucket);
		MoveBucket(entry, bucket);
		if (!written && entry.slot != kNoObjectSlot)
			ReleaseObjectSlot(entry);
		// FrameValues samples the row next frame.
		if (written && entry.slot != kNoObjectSlot) {
			WalkPlan().written.push_back(PlanItemOf(geometry, entry));
			if (entry.slot == slotBefore) {
				tables.NoteWrite(entry.slot, columnsBefore);
			} else {
				tables.NoteChange(entry.slot, kChangeAll);
			}
		}
		if (hadSlot != (entry.slot != kNoObjectSlot) || reasonBefore != entry.candidateReason || written) {
			MarkListedRootDirty(entry.entryRoot);
			MarkLightEntryDirty(entry.lightRoot);
		}
		if (!(ShadowInputsOf(written ? entry.slot : kNoObjectSlot) == shadowBefore)) {
			shadowSetsDirty = true;
			if (slotBefore != kNoObjectSlot)
				shadowDirtySlots.push_back(slotBefore);
			if (written && entry.slot != kNoObjectSlot)
				shadowDirtySlots.push_back(entry.slot);
		}
		ListDependents(geometry, entry);
		if (entry.candidateFrame == PassStamp()) {
			assert(r.classified);
			std::tie(entry.perFrame, entry.lightTraits) = PerFrameMerge(entry, r);
			entry.moveKey = r.moveKey;
			entry.moveKeyOuter = r.moveKeyOuter;
			// Every entry's hidden chain is followed by events, statics as well as actors.
			ListHiddenChain(geometry, entry);
		}
		if (entry.perFrame && !entry.perFrameListed) {
			entry.perFrameListed = true;
			perFrameSet.push_back({ geometry, &entry });
		}
		if (rootWitnessed)
			CheckRootWitness(entry, written, hadSlot, slotBefore, columnsBefore, perFrameBefore, traitsBefore, reasonBefore);
		TouchPlacedDependent(entry.listedRoot, geometry);
		witness(NoteMover(geometry, entry));
	}

	bool Coordinator::MergeLight(RE::BSGeometry* geometry, Tracked& entry, EvalResult& r, PartTimer& a_timer, bool witnessed, const char*& missed)
	{
		const bool slotted = entry.slot < tables.skinPartitions.size();
		const auto partitions = slotted ? tables.skinPartitions[entry.slot] : decltype(tables.skinPartitions)::value_type{};
		const auto lodPartitions = slotted ? tables.skinLodPartitions[entry.slot] : decltype(tables.skinLodPartitions)::value_type{};
		if (!r.lightSkin || KeepSkin(geometry, entry, r)) {
			if (witnessed && r.lightSkin && (tables.skinPartitions[entry.slot] != partitions || tables.skinLodPartitions[entry.slot] != lodPartitions))
				missed = "a skin's partitions";
			for (std::uint32_t bit = 0; bit < stats.lightByTrait.size(); ++bit)
				stats.lightByTrait[bit] += (entry.lightTraits >> bit) & 1u;
			const bool recorded = r.lightRecorded;
			if (recorded && entry.owner.published)
				tables.classMembers[static_cast<std::size_t>(entry.owner.ownerClass)].objects.push_back(entry.slot);
			// An actor's skeleton moves with no controller or body on its chain, through Havok's behaviour graph.
			if (recorded && ((entry.lightTraits & (kTraitMoves | kTraitRootMoves | kTraitSkin)) || ((entry.lightTraits & kTraitOwnerClass) && entry.owner.ownMotion))) {
				++stats.lightPlaced;
				++delta.moved;
			} else {
				++delta.kept;
			}
			entry.movedWalk = walkSerial;  // not written in full: a later round may still write it
			return true;
		}
		if (witnessed)
			missed = "a skin's slot (KeepSkin)";
		// The slot's flags or palette rows disagree, so evaluate and write in full here.
		++fanoutStats.lightMissed;
		r.light = false;
		if (r.kind == EvaluateKind::Light)
			r.kind = EvaluateKind::LightMissed;
		EvalCounters counters;
		{
			ShardScope scope(*this, false, &counters.mirror);
			EvaluateWrite(geometry, entry, counters, r, a_timer);
			EvaluateClassified(geometry, entry, counters, r);
		}
		MergeCounters(counters);
		return false;
	}

	void Coordinator::CheckRootWitness(const Tracked& entry, bool written, bool hadSlot, std::uint32_t slotBefore, const Tables::Columns& columnsBefore,
		bool perFrameBefore, std::uint32_t traitsBefore, Ineligible reasonBefore)
	{
		auto& d = placementDeltaStats;
		++d.rootWitnessChecks;
		const std::uint32_t causes = written && entry.slot == slotBefore && hadSlot ? tables.CausesBetween(columnsBefore, tables.ColumnsOf(entry.slot)) : 0u;
		if (entry.slot != slotBefore || entry.perFrame != perFrameBefore || entry.lightTraits != traitsBefore || entry.candidateReason != reasonBefore || causes) {
			++d.rootWitnessDiffer;
			if (d.rootWitnessFirst.empty())
				d.rootWitnessFirst = fmt::format("'{}' (root {}): slot {} -> {}, per frame {} -> {}, traits {:X} -> {:X}, verdict {} -> {}, causes {:X}",
					entry.geometry && entry.geometry->name.c_str() ? entry.geometry->name.c_str() : "?", static_cast<const void*>(entry.listedRoot), slotBefore, entry.slot,
					perFrameBefore, entry.perFrame, traitsBefore, entry.lightTraits, kIneligibleNames[static_cast<std::size_t>(reasonBefore)],
					kIneligibleNames[static_cast<std::size_t>(entry.candidateReason)], causes);
		}
	}

	void Coordinator::MergeCounters(EvalCounters& a_counters)
	{
		// In chunk order, so the first-named strings are deterministic.
		auto& c = a_counters;
		for (std::size_t i = 0; i < c.ineligible.size(); ++i)
			stats.ineligible[i] += c.ineligible[i];
		for (std::size_t i = 0; i < c.techniqueRejects.size(); ++i)
			stats.techniqueRejects[i] += c.techniqueRejects[i];
		stats.classifyHits += c.classifyHits;
		stats.classifyChecked += c.classifyChecked;
		if (c.classifyDiffers && !stats.classifyDiffers)
			logger::warn("{}", c.classifyDiffersFirst);
		stats.classifyDiffers += c.classifyDiffers;
		stats.verdictsChecked += c.verdictsChecked;
		stats.verdictsSkipped += c.verdictsSkipped;
		if (c.verdictsMissed && !stats.verdictsMissed)
			stats.firstVerdictMissed = std::move(c.firstVerdictMissed);
		stats.verdictsMissed += c.verdictsMissed;
		leafStats.missing += c.leafMissing;
		for (std::size_t p = 0; p < c.partMs.size(); ++p)
			stats.partMs[p] += c.partMs[p];
		for (std::size_t k = 0; k < c.evaluateKindMs.size(); ++k)
			stats.evaluateKindMs[k] += c.evaluateKindMs[k];
		MergeMirrorReads(c.mirror);
	}

	void Coordinator::MergeMirrorReads(MirrorReadStats& a_counts)
	{
		auto& m = mirrorReads;
		const auto& c = a_counts;
		for (std::size_t i = 0; i < m.reads.size(); ++i) {
			m.reads[i] += c.reads[i];
			m.checked[i] += c.checked[i];
			m.lagged[i] += c.lagged[i];
			m.differ[i] += c.differ[i];
			m.unmirrored[i] += c.unmirrored[i];
			m.refused[i] += c.refused[i];
			if (m.first[i].empty() && !c.first[i].empty())
				m.first[i] = std::move(a_counts.first[i]);
		}
	}

	void Coordinator::EvaluateRound(PartTimer& a_timer, std::size_t a_first)
	{
		if (a_first >= order.size())
			return;
		const std::size_t count = order.size() - a_first;
		const bool light = a_first == 0;
		const bool profileKinds = light && Diagnostics::Observe().profile;
		const std::uint32_t mode = FanoutMode(count);
		auto& f = fanoutStats;
		++f.rounds;
		f.entries += count;
		ShardScope passScope(*this, false);
		if (mode == 0) {
			EvalCounters counters;
			for (std::size_t i = a_first; i < order.size(); ++i) {
				EvalResult result;
				{
					ShardScope scope(*this, false, &counters.mirror);
					EvaluateEntry(i, light, counters, result, a_timer);
				}
				MergeEntry(i, profileKinds, result, a_timer);
			}
			MergeCounters(counters);
			return;
		}
		// Evaluations are pure, merges run in index order. Counters are per chunk so they do not depend on which thread ran it.
		const bool parallel = mode == 2;
		const std::size_t chunks = (count + kFanoutGrain - 1) / kFanoutGrain;
		evalResults.clear();
		evalResults.resize(count);
		evalCounters.clear();
		evalCounters.resize(chunks);
		f.parallelRounds += parallel ? 1 : 0;
		f.parallelEntries += parallel ? count : 0;
		f.chunks += chunks;
		auto evaluate = [&](std::vector<EvalResult>& a_results, std::vector<EvalCounters>& a_counters, bool a_pool) {
			auto body = [&](std::size_t a_begin, std::size_t a_end) {
				auto& counters = a_counters[a_begin / kFanoutGrain];
				ShardScope scope(*this, a_pool, &counters.mirror);
				PartTimer timer(counters.partMs);
				for (std::size_t i = a_begin; i < a_end; ++i)
					EvaluateEntry(a_first + i, light, counters, a_results[i], timer);
			};
			if (a_pool) {
				SceneScheduler::Executor().ParallelFor("CS.DCLF.Scene.Evaluate", count, kFanoutGrain, body);
			} else {
				for (std::size_t begin = 0; begin < count; begin += kFanoutGrain)
					body(begin, std::min(count, begin + kFanoutGrain));
			}
		};
		// Witnesses that nothing writes the mirror or the tracked set while evaluations read them.
		[[maybe_unused]] const std::uint64_t mirrorWrites = mirror.Writes();
		[[maybe_unused]] const std::uint64_t layout = trackedLayout;
		if (parallel) {
			EvaluateStreamed(a_timer, a_first, count, chunks, light, profileKinds, mirrorWrites, layout);
			return;
		}
		{
			DCLF_SCENE_PART(EvaluateFanout, "CS.DCLF.Scene.EvaluateFanout");
			evaluate(evalResults, evalCounters, parallel);
		}
		assert(mirror.Writes() == mirrorWrites && trackedLayout == layout);
		{
			DCLF_SCENE_PART(EvaluateMerge, "CS.DCLF.Scene.EvaluateMerge");
			for (auto& counters : evalCounters)
				MergeCounters(counters);
			for (std::size_t i = 0; i < count; ++i)
				MergeEntry(a_first + i, profileKinds, evalResults[i], a_timer);
		}
	}

	void Coordinator::EvaluateStreamed(PartTimer& a_timer, std::size_t a_first, std::size_t count, std::size_t chunks, bool light, bool profileKinds,
		[[maybe_unused]] std::uint64_t mirrorWrites, [[maybe_unused]] std::uint64_t layout)
	{
		// Chunk k merges as soon as it is ready while this thread evaluates or waits. Safe because a merge writes only state no evaluation
		// reads. A late helper finds every chunk claimed and touches only the shared Shards.
		DCLF_ZONE("CS.DCLF.Scene.EvaluateStream");
		struct Shards
		{
			explicit Shards(std::size_t a_chunks) :
				ready(a_chunks), chunks(a_chunks) {}
			std::atomic<std::size_t> next{ 0 };
			std::vector<std::atomic<std::uint8_t>> ready;
			std::size_t chunks = 0;
			std::function<void(std::size_t)> run;
			std::atomic<bool> failed{ false };
			std::exception_ptr error;  // the first chunk's that threw (written before its ready flag)
			void RunOne(std::size_t a_chunk)
			{
				try {
					run(a_chunk);
				} catch (...) {
					bool expected = false;
					if (failed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
						error = std::current_exception();
				}
				ready[a_chunk].store(1, std::memory_order_release);
				ready[a_chunk].notify_all();
			}
			void Work()
			{
				for (std::size_t chunk = next.fetch_add(1, std::memory_order_relaxed); chunk < chunks; chunk = next.fetch_add(1, std::memory_order_relaxed))
					RunOne(chunk);
			}
		};
		auto shards = std::make_shared<Shards>(chunks);
		shards->run = [this, a_first, light, count, results = evalResults.data(), counters = evalCounters.data()](std::size_t a_chunk) {
			const std::size_t begin = a_chunk * kFanoutGrain;
			const std::size_t end = std::min(count, begin + kFanoutGrain);
			auto& chunkCounters = counters[a_chunk];
			ShardScope scope(*this, true, &chunkCounters.mirror);
			PartTimer timer(chunkCounters.partMs);
			for (std::size_t i = begin; i < end; ++i)
				EvaluateEntry(a_first + i, light, chunkCounters, results[i], timer);
		};
		const std::size_t helpers = std::min<std::size_t>(chunks - 1, SceneScheduler::PreparationWorkers());
		for (std::size_t h = 0; h < helpers; ++h)
			if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "CS.DCLF.Scene.EvaluateShards",
					[shards](const org::async::TaskContext&) { shards->Work(); }))
				break;  // this thread takes what no helper does
		const auto start = std::chrono::steady_clock::now();
		double mergeMs = 0.0;
		for (std::size_t k = 0; k < chunks; ++k) {
			while (!shards->ready[k].load(std::memory_order_acquire)) {
				if (const std::size_t chunk = shards->next.fetch_add(1, std::memory_order_relaxed); chunk < chunks)
					shards->RunOne(chunk);
				else
					shards->ready[k].wait(0, std::memory_order_acquire);
			}
			// After a chunk threw, claimed chunks still finish and nothing more is merged.
			if (shards->failed.load(std::memory_order_acquire))
				continue;
			const auto mergeStart = std::chrono::steady_clock::now();
			MergeCounters(evalCounters[k]);
			const std::size_t end = std::min(count, (k + 1) * kFanoutGrain);
			for (std::size_t i = k * kFanoutGrain; i < end; ++i)
				MergeEntry(a_first + i, profileKinds, evalResults[i], a_timer);
			mergeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mergeStart).count();
		}
		const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		for (const auto& [part, ms] : { std::pair{ ScenePart::EvaluateMerge, mergeMs }, std::pair{ ScenePart::EvaluateFanout, std::max(0.0, totalMs - mergeMs) } }) {
			stats.scenePartMs[static_cast<std::size_t>(part)] += ms;
			stats.scenePartFrameMs[static_cast<std::size_t>(part)] += ms;
		}
		assert(mirror.Writes() == mirrorWrites && trackedLayout == layout);
		if (shards->failed.load(std::memory_order_acquire))
			std::rethrow_exception(shards->error);
	}

	void Coordinator::DeltaWalk()
	{
		++delta.walks;
		std::optional<ScenePartScope> scheduleScope(std::in_place, stats.scenePartMs[static_cast<std::size_t>(ScenePart::Schedule)],
			stats.scenePartFrameMs[static_cast<std::size_t>(ScenePart::Schedule)]);
		TracyCZoneN(scheduleZone, "CS.DCLF.Scene.Schedule", true);
		const bool full = fullEvaluation;
		rootWitness.clear();
		// A second pass of the same frame would rewrite the same per-frame records, so it takes only what its events schedule.
		perFrameWalked = full || perFrameFrame != sceneFrame;
		if (perFrameWalked) {
			perFrameFrame = sceneFrame;
			++pumpStats.perFrameWalks;
		} else {
			++pumpStats.eventWalks;
		}
		// Kept through the walk, since the light path takes only the actors an event names.
		std::array<std::vector<std::uint32_t>, Extensions::kPublishedEntryClasses> keptMembers;
		for (std::size_t c = 0; c < keptMembers.size(); ++c)
			keptMembers[c] = std::move(tables.classMembers[c].objects);
		// Before a full evaluation drops the node events, which name movers too.
		DrainMoveEvents(full);
		DrainHiddenEvents();
		if (full) {
			shadowIndexNeedsRebuild = true;
			shadowDirtySlots.clear();
		}
		auto keptTextureSet = std::move(tables.shadowTextureSet);
		auto keptTextureSeen = std::move(tables.shadowTextureSeen);
		auto keptKeys = std::move(tables.shadowKeysUsed);
		auto keptOcclusionKeys = std::move(tables.occlusionKeysUsed);
		shadowSetsDirty = full || shadowSetsDirty;
		BeginWalk(!full);
		PartTimer timer(stats.partMs);
		if (full) {
			ScheduleFullEvaluation();
		} else {
			SchedulePerFrameEntries();
			ScheduleEventEntries();
		}
		TracyCZoneEnd(scheduleZone);
		scheduleScope.reset();
		{
			DCLF_SCENE_PART(SwitchEvents, "CS.DCLF.Scene.SwitchEvents");
			ApplySwitchEvents(full);
		}
		{
			DCLF_SCENE_PART(Evaluate, "CS.DCLF.Scene.Evaluate");
			PrefetchGeometryBuffers(0);
			EvaluateRound(timer, 0);
		}
		if (!shadowSetsDirty) {
			tables.shadowTextureSet = std::move(keptTextureSet);
			tables.shadowTextureSeen = std::move(keptTextureSeen);
			tables.shadowKeysUsed = std::move(keptKeys);
			tables.occlusionKeysUsed = std::move(keptOcclusionKeys);
		}
		FinishDeltaWalk(timer);
		RestoreKeptMembers(keptMembers);
		if (full) {
			DCLF_SCENE_PART(ObjectSweep, "CS.DCLF.Scene.ObjectSweep");
			SweepObjectSlots();
		}
		{
			DCLF_SCENE_PART(SunCandidates, "CS.DCLF.Scene.SunCandidates");
			UpdateSunCandidates(full);
			UpdateLightCandidates(full);
		}
		{
			DCLF_SCENE_PART(FaceWalk, "CS.DCLF.Scene.FaceWalk");
			ComponentsWalkEnded();
		}
		stats.ineligible = buckets;
		delta.evaluated += order.size();
		delta.evaluatedMax = std::max(delta.evaluatedMax, static_cast<std::uint32_t>(order.size()));
		delta.live += tables.liveObjects;
	}

	void Coordinator::ScheduleFullEvaluation()
	{
		// After a reset, a load or a live toggle nothing kept can be trusted.
		fullEvaluation = false;
		ComponentsScheduling(true);
		++delta.full;
		perFrameSet.clear();
		pendingEvaluation.clear();
		propertyChanged.clear();
		HandBack(nodeChanged);
		dirtyRoots.clear();
		propertyDependents.clear();
		propertyDependentsAt.clear();
		rootMotion.clear();
		movingRoots.clear();
		moverSet.clear();
		stillRoots.clear();
		placementDirty = true;
		mirrorMotionNodes.clear();
		mirrorSkinGeometries.clear();
		hiddenDependents.clear();
		hiddenDependentsAt.clear();
		tables.InvalidateChangeLog();
		buckets = {};
		for (auto& [geometry, entry] : tracked) {
			entry.perFrameListed = false;
			entry.hiddenChain.clear();  // hiddenDependents cleared above: listed anew at classification
			entry.bucket = Tracked::kNoBucket;
			entry.listedProperty = nullptr;
			entry.listedAlpha = nullptr;
			entry.listedFadeNode = nullptr;
		}
		BuildFullOrder();
	}

	void Coordinator::SchedulePerFrameEntries()
	{
		order.clear();
		std::size_t counted = 0;
		auto count = [&](std::uint64_t& a_into) {
			a_into += order.size() - counted;
			counted = order.size();
		};
		const bool addressesHold = perFrameLayout == trackedLayout;
		if (perFrameWalked)
			stats.perFrameRelookups += addressesHold ? 0 : 1;
		// A per-frame entry is taken when an event names it, or always while move events are not gating. A witness walk takes every one.
		auto& gate = moverGateStats;
		const bool gating = moveGating;
		if (perFrameWalked) {
			++gate.perFrameWalks;
			gate.ungatedWalks += gating ? 0u : 1u;
		}
		for (std::size_t i = 0; perFrameWalked && i < perFrameSet.size();) {
			auto& item = perFrameSet[i];
			if (!addressesHold) {
				const auto it = tracked.find(item.geometry);
				item.tracked = it != tracked.end() ? &it->second : nullptr;
			}
			Tracked* entry = item.tracked;
			if (!entry || !entry->perFrameListed || !entry->perFrame || entry->scheduledWalk == walkSerial) {
				if (entry && !entry->perFrame && entry->scheduledWalk != walkSerial)
					entry->perFrameListed = false;
				item = perFrameSet.back();
				perFrameSet.pop_back();
				continue;
			}
			std::uint32_t why = 0;
			if (gating && !MoverDue(*entry, item.geometry, why)) {
				if (!hiddenWitness) {
					++gate.skipped;
					++i;
					continue;
				}
				entry->gateWitnessWalk = walkSerial;
			} else if (gating) {
				++gate.due;
				gate.moved += why == 1 || why == 5 ? 1u : 0u;
				gate.owed += why == 2 ? 1u : 0u;
				gate.firstPerson += why == 3 ? 1u : 0u;
				gate.fullEveryFrame += why == 4 ? 1u : 0u;
			}
			Schedule(item.geometry, *entry, false);
			++i;
		}
		if (perFrameWalked)
			perFrameLayout = trackedLayout;
		count(delta.perFrame);
	}

	void Coordinator::ScheduleEventEntries()
	{
		std::size_t counted = order.size();
		auto count = [&](std::uint64_t& a_into) {
			a_into += order.size() - counted;
			counted = order.size();
		};
		auto& gate = moverGateStats;
		for (auto* geometry : pendingEvaluation)
			if (const auto it = tracked.find(geometry); it != tracked.end())
				Schedule(it->first, it->second);
		pendingEvaluation.clear();
		ComponentsScheduling(false);
		count(delta.pending);
		delta.propertyEvents += propertyChanged.size();
		delta.nodeEvents += nodeChanged.size();
		for (const void* key : propertyChanged) {
			const auto dependents = propertyDependents.find(key);
			if (dependents == propertyDependents.end())
				continue;
			for (auto* geometry : dependents->second)
				if (const auto it = tracked.find(geometry); it != tracked.end())
					Reclassify(it->first, it->second);
		}
		propertyChanged.clear();
		count(delta.property);
		gate.mirrorNodes += mirrorMotionNodes.size();
		gate.mirrorGeometries += mirrorSkinGeometries.size();
		for (const void* node : mirrorMotionNodes)
			ApplyMotionUpdate(node);
		mirrorMotionNodes.clear();
		for (const void* key : mirrorSkinGeometries)
			if (const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(key))); it != tracked.end())
				Reclassify(it->first, it->second);
		mirrorSkinGeometries.clear();
		std::sort(nodeChanged.begin(), nodeChanged.end(), [](const auto& a_left, const auto& a_right) { return a_left.get() < a_right.get(); });
		nodeChanged.erase(std::unique(nodeChanged.begin(), nodeChanged.end()), nodeChanged.end());
		for (auto& node : nodeChanged)
			ApplyNodeEvent(node.get());
		// Their references are the render thread's to drop.
		HandBack(nodeChanged);
		count(delta.node);
		std::sort(dirtyRoots.begin(), dirtyRoots.end());
		dirtyRoots.erase(std::unique(dirtyRoots.begin(), dirtyRoots.end()), dirtyRoots.end());
		for (const auto* root : dirtyRoots)
			ScheduleRoot(root, false);
		dirtyRoots.clear();
		count(delta.roots);
	}

	void Coordinator::RestoreKeptMembers(std::array<std::vector<std::uint32_t>, Extensions::kPublishedEntryClasses>& keptMembers)
	{
		for (std::size_t c = 0; c < keptMembers.size(); ++c) {
			if (keptMembers[c].empty())
				continue;
			auto& members = tables.classMembers[c].objects;
			for (const std::uint32_t o : keptMembers[c]) {
				if (o >= tables.objects.size() || !tables.objectGeometry[o] || (tables.objects[o].flags & kObjectFree))
					continue;
				const auto it = tracked.find(const_cast<RE::BSGeometry*>(tables.objectGeometry[o]));
				if (it != tracked.end() && it->second.owner.published && static_cast<std::size_t>(it->second.owner.ownerClass) == c && it->second.slot == o &&
					it->second.objectStamp == objectStamp)
					members.push_back(o);
			}
			std::sort(members.begin(), members.end());
			members.erase(std::unique(members.begin(), members.end()), members.end());
		}
	}

	void Coordinator::FinishDeltaWalk(PartTimer& a_timer)
	{
		// Writing the kept objects that draw a stale geometry slot again resolves it.
		std::optional<ScenePartScope> scanScope(std::in_place, stats.scenePartMs[static_cast<std::size_t>(ScenePart::GeometryScan)],
			stats.scenePartFrameMs[static_cast<std::size_t>(ScenePart::GeometryScan)]);
		TracyCZoneN(scanZone, "CS.DCLF.Scene.GeometryScan", true);
		const std::size_t second = order.size();
		ScheduleStaleGeometry();
		TracyCZoneEnd(scanZone);
		scanScope.reset();
		std::optional<ScenePartScope> roundsScope(std::in_place, stats.scenePartMs[static_cast<std::size_t>(ScenePart::LaterRounds)],
			stats.scenePartFrameMs[static_cast<std::size_t>(ScenePart::LaterRounds)]);
		TracyCZoneN(roundsZone, "CS.DCLF.Scene.LaterRounds", true);
		PrefetchGeometryBuffers(second);
		EvaluateRound(a_timer, second);
		// Every kept object drawing a slot re-resolved in place copied the old record.
		if (!refreshedGeometry.empty())
			ScheduleRefreshedGeometry(a_timer);
		TracyCZoneEnd(roundsZone);
		roundsScope.reset();
		DCLF_SCENE_PART(ShadowSets, "CS.DCLF.Scene.ShadowSets");
		for (auto& members : tables.classMembers)
			std::sort(members.objects.begin(), members.objects.end());
		tables.liveObjects = tables.CountLive();
		if (!shadowSetsDirty) {
			stats.shadowCasters = keptShadowCasters;
			stats.shadowRejects = keptShadowRejects;
			a_timer.Add(BuildPart::Record);
			return;
		}
		RefreshShadowSets(false);
		if (Policy::RebuildShadowSets(passSerial)) {
			Diagnostics::Notify<Diagnostics::ScenePoint::ShadowSetsKept>();
			RefreshShadowSets(true);
			Diagnostics::Notify<Diagnostics::ScenePoint::ShadowSetsRebuilt>();
		}
		keptShadowCasters = stats.shadowCasters;
		keptShadowRejects = stats.shadowRejects;
		a_timer.Add(BuildPart::Record);
	}

	void Coordinator::ScheduleStaleGeometry()
	{
		const bool resolveBuffers = frameResolveBuffers;
		staleGeometrySlots.clear();
		if (resolveBuffers && !geometryResolvedLastWalk) {
			for (std::uint32_t g = 0; g < tables.geometries.size(); ++g) {
				if (!tables.geometrySlots.Alive(g) || tables.geometryLastUsed[g] == PassStamp())
					continue;  // free, or written this frame (resolved by the write)
				if (!tables.geometries[g].vertexAddress)
					staleGeometrySlots.push_back(g);
			}
		}
		// When the graph comes back every unresolved slot is stale, so every object is checked. Else only those the index lists.
		const bool whole = !staleGeometrySlots.empty();
		geometryResolvedLastWalk = resolveBuffers;
		staleGeometrySlots.insert(staleGeometrySlots.end(), freedGeometry.begin(), freedGeometry.end());
		freedGeometry.clear();
		staleGeometrySlots.insert(staleGeometrySlots.end(), relocatedGeometry.begin(), relocatedGeometry.end());
		relocatedGeometry.clear();
		if (!staleGeometrySlots.empty()) {
			std::sort(staleGeometrySlots.begin(), staleGeometrySlots.end());
			staleGeometrySlots.erase(std::unique(staleGeometrySlots.begin(), staleGeometrySlots.end()), staleGeometrySlots.end());
			auto& d = placementDeltaStats;
			++d.scans;
			d.scansWhole += whole ? 1u : 0u;
			d.staleSlots += staleGeometrySlots.size();
			d.scanObjects += tables.objects.size();
			const auto staleObject = [&](std::uint32_t s) {
				if (s >= tables.objects.size())
					return false;
				const auto& object = tables.objects[s];
				if ((object.flags & kObjectFree) || tables.objectSeen[s] == walkSerial)
					return false;
				bool stale = false;
				std::uint32_t g = object.geometryIndex;
				const std::uint32_t links = (tables.skinPartitions[s] & kPartitionChain) ? (tables.skinPartitions[s] & kPartitionChainCount) : kMaxSkinPartitions;
				for (std::uint32_t link = 0; !stale && link < links && g != kNoPartition; ++link) {
					stale = g >= tables.geometries.size() || std::binary_search(staleGeometrySlots.begin(), staleGeometrySlots.end(), g);
					if (!stale && !tables.skinPartitions[s] && !(object.flags & kObjectPaletteBlock))
						break;
					g = stale ? g : tables.geometries[g].nextPartition;
				}
				return stale;
			};
			std::vector<std::uint32_t> candidates, hits;
			if (whole) {
				candidates.resize(tables.objects.size());
				for (std::uint32_t s = 0; s < candidates.size(); ++s)
					candidates[s] = s;
			} else {
				ObjectsUnder(staleGeometrySlots, true, candidates);
			}
			d.scanChecked += candidates.size();
			for (const std::uint32_t s : candidates)
				if (staleObject(s))
					hits.push_back(s);
			if (!whole && Policy::CheckGeometryScan()) {
				++d.scanParityChecks;
				std::size_t found = 0;
				bool missed = false;
				for (std::uint32_t s = 0; s < tables.objects.size(); ++s)
					if (staleObject(s)) {
						missed = missed || !std::binary_search(hits.begin(), hits.end(), s);
						++found;
					}
				d.scanParityDiffer += missed || found != hits.size() ? 1u : 0u;
			}
			for (const std::uint32_t s : hits) {
				++delta.geometryDirty;
				++d.scanScheduled;
				if (const auto it = tracked.find(tables.objectGeometry[s]); it != tracked.end()) {
					Schedule(it->first, it->second);
				} else {
					ComponentsSlotReleased(s);
					tables.ResetObject(s);
					tables.RetireObject(s);
					shadowSetsDirty = true;
					shadowDirtySlots.push_back(s);
				}
			}
		}
	}

	void Coordinator::NoteRelocatedGeometry(const std::vector<ID3D11Buffer*>& a_buffers)
	{
		if (a_buffers.empty())
			return;
		const ankerl::unordered_dense::set<ID3D11Buffer*> moved(a_buffers.begin(), a_buffers.end());
		for (std::uint32_t g = 0; g < tables.geometries.size(); ++g) {
			if (!tables.geometrySlots.Alive(g))
				continue;
			const auto& record = tables.geometries[g];
			if ((moved.contains(record.vertexBuffer) || moved.contains(record.indexBuffer)) && relocatedGeometrySlots.insert(g).second) {
				relocatedGeometry.push_back(g);
				++stats.geometriesRelocated;
			}
		}
	}

	void Coordinator::ScheduleRefreshedGeometry(PartTimer& a_timer)
	{
		std::sort(refreshedGeometry.begin(), refreshedGeometry.end());
		refreshedGeometry.erase(std::unique(refreshedGeometry.begin(), refreshedGeometry.end()), refreshedGeometry.end());
		const std::size_t third = order.size();
		const auto refreshedObject = [&](std::uint32_t s) {
			if (s >= tables.objects.size())
				return false;
			const auto& object = tables.objects[s];
			return !(object.flags & kObjectFree) && tables.objectSeen[s] != walkSerial &&
			       std::binary_search(refreshedGeometry.begin(), refreshedGeometry.end(), object.geometryIndex);
		};
		auto& d = placementDeltaStats;
		std::vector<std::uint32_t> candidates, hits;
		ObjectsUnder(refreshedGeometry, false, candidates);
		++d.scans;
		d.staleSlots += refreshedGeometry.size();
		d.scanObjects += tables.objects.size();
		d.scanChecked += candidates.size();
		for (const std::uint32_t s : candidates)
			if (refreshedObject(s))
				hits.push_back(s);
		if (Policy::CheckGeometryScan()) {
			++d.scanParityChecks;
			std::size_t found = 0;
			bool missed = false;
			for (std::uint32_t s = 0; s < tables.objects.size(); ++s)
				if (refreshedObject(s)) {
					missed = missed || !std::binary_search(hits.begin(), hits.end(), s);
					++found;
				}
			d.scanParityDiffer += missed || found != hits.size() ? 1u : 0u;
		}
		for (const std::uint32_t s : hits)
			if (const auto it = tracked.find(tables.objectGeometry[s]); it != tracked.end())
				Schedule(it->first, it->second);
		delta.geometryDirty += order.size() - third;
		d.scanScheduled += order.size() - third;
		PrefetchGeometryBuffers(third);
		EvaluateRound(a_timer, third);
		refreshedGeometry.clear();
	}
}
