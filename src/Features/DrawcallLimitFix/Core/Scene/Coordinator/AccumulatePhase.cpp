// The coordinator's accumulate phase on the scene lane: membership joins bound, residency kept or dropped, pipeline constants
// made from the posted pipeline frame, and the shadow dependency index refreshed.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

#include <cassert>

namespace DCLF
{
	bool SettledMembershipPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached, AccumulatedPass& a_out);
}

namespace DCLF::Scene
{
	namespace
	{
		// Asked once each, since the pipeline lane keeps every key it built.
		ankerl::unordered_dense::set<PipelineKey, PipelineKeyHash> accumulateSettledAsked;

		// Read once, since the features install before any scene work.
		std::uint32_t TechniqueFlags(std::uint32_t a_technique)
		{
			static const auto& techniques = Extensions::RegisteredTechniques();
			return Extensions::TechniqueObjectFlags(techniques, a_technique);
		}

		// Read once, like the techniques.
		std::uint64_t LandscapeLightFlags()
		{
			static const std::uint64_t flags = Extensions::LightVolumes().landscapeFlags;
			return flags;
		}

		constexpr std::uint32_t kTextureOrder = 1u, kKeyOrder = 2u, kOcclusionOrder = 4u;

		// Returns +1 or -1 when the key appears or goes.
		template <class Index, class Order, class Key>
		std::int32_t ShadowMember(Index& a_index, Order& a_order, const Key& a_key, std::uint32_t a_slot, bool a_add, std::uint32_t a_bit, std::uint32_t& a_moved)
		{
			constexpr std::uint32_t kNoMember = ~0u;
			if (a_add) {
				auto [it, fresh] = a_index.try_emplace(a_key);
				auto& slots = it->second;
				const std::uint32_t before = slots.empty() ? kNoMember : *slots.begin();
				slots.insert(a_slot);
				if (const std::uint32_t after = *slots.begin(); after != before) {
					if (before != kNoMember)
						a_order.erase(before);
					a_order.insert_or_assign(after, a_key);
					a_moved |= a_bit;
				}
				return fresh ? 1 : 0;
			}
			const auto it = a_index.find(a_key);
			if (it == a_index.end())
				return 0;
			auto& slots = it->second;
			const std::uint32_t before = slots.empty() ? kNoMember : *slots.begin();
			slots.erase(a_slot);
			if (slots.empty()) {
				a_order.erase(before);
				a_index.erase(it);
				a_moved |= a_bit;
				return -1;
			}
			if (const std::uint32_t after = *slots.begin(); after != before) {
				a_order.erase(before);
				a_order.insert_or_assign(after, a_key);
				a_moved |= a_bit;
			}
			return 0;
		}

	}

	const AccumulatedPass* Coordinator::FindAccumulatedPass(const RE::BSGeometry* a_geometry) const
	{
		auto it = accumulatedPasses.find(a_geometry);
		return it == accumulatedPasses.end() ? nullptr : &it->second;
	}

	const AccumulatedPass* Coordinator::FindAccumulatedLayerPass(const RE::BSGeometry* a_geometry) const
	{
		auto it = accumulatedLayerPasses.find(a_geometry);
		return it == accumulatedLayerPasses.end() ? nullptr : &it->second;
	}

	void Coordinator::ApplyAccumulatePatch(const AccumulatePatch& a_patch)
	{
		const std::uint32_t objectId = a_patch.object;
		const AccumulateSnapshot before(tables, objectId);
		auto& object = tables.objects[objectId];
		object.materialIndex = a_patch.material;
		if (materialMember.size() <= a_patch.material)
			materialMember.resize(std::size_t(a_patch.material) + 1, 0);
		materialMember[a_patch.material] = 1;
		object.pipelineIndex = a_patch.pipeline;
		object.flags = a_patch.flags;
		tables.fadeDistance[objectId] = a_patch.fadeDistance;
		tables.draws[objectId].pipelineIndex = a_patch.pipeline;
		tables.ancestorValue[objectId] = a_patch.ancestorValue;
		bool extrasWritten = false;
		if (a_patch.projectedUV || a_patch.landBlend) {
			stats.projectedUV += a_patch.projectedUV ? 1 : 0;
			stats.landBlend += a_patch.landBlend ? 1 : 0;
			if (tables.extraOffset[objectId] == kNoExtraRows)
				tables.extraOffset[objectId] = tables.AllocateExtras();
			// Rows rewritten in a kept block change no offset, so they are noted below or the uploads would never send them.
			extrasWritten = WriteObjectExtras(objectId);
		} else {
			tables.FreeExtras(objectId);
		}
		tables.residentSlot[objectId] = 1;
		before.NoteWrite(tables, objectId);
		if (extrasWritten)
			tables.NoteChange(objectId, kChangeExtras);
		// The earliest it can join the set is the next frame's values.
		NameShading(objectId, true);
		ComponentsAccumulated(objectId, a_patch.decalKey);
	}

	void Coordinator::RefreshAncestorValues()
	{
		for (auto& [geometry, entry] : tracked)
			RefreshAncestorValue(entry);
	}

	void Coordinator::RefreshAncestorValue(Tracked& a_entry)
	{
		std::int32_t value = Extensions::AncestorValue()->none;
		if (ancestorMap && a_entry.ancestorKey)
			if (const auto found = ancestorMap->find(a_entry.ancestorKey); found != ancestorMap->end())
				value = found->second;
		a_entry.ancestorValue = value;
		a_entry.ancestorMapGeneration = ancestorMapGeneration;
		for (const std::uint32_t slot : { a_entry.slot, a_entry.layerSlot }) {
			if (slot == kNoObjectSlot || slot >= tables.ancestorValue.size() || !tables.residentSlot[slot] || tables.ancestorValue[slot].value == value)
				continue;
			tables.ancestorValue[slot].value = value;
			tables.NoteChange(slot, kChangeAncestorValue);
			channels.accumulateInputs.ancestorValuesMoved.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void Coordinator::TakeAccumulateInputs()
	{
		// Latest wins. Neither post holds engine references.
		if (auto posted = channels.ancestorMapPosted.exchange(nullptr, std::memory_order_acq_rel)) {
			ancestorMap = posted->map;
			ancestorMapGeneration = posted->generation;
			channels.accumulateInputs.ancestorMapsTaken.fetch_add(1, std::memory_order_relaxed);
			if (Extensions::AncestorValueOn())
				RefreshAncestorValues();
		}
		if (auto posted = channels.pipelineFramePosted.exchange(nullptr, std::memory_order_acq_rel)) {
			scenePipelineFrame = std::move(posted);
			channels.accumulateInputs.pipelineFramesTaken.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void Coordinator::MakeNewPipelineConstants()
	{
		// It draws until the render thread's own block supersedes it. Without a sample yet the slot waits.
		if (!scenePipelineFrame) {
			for (std::size_t word = 0; word < tables.usedPipelineBits.size(); ++word)
				for (std::uint64_t remaining = tables.usedPipelineBits[word]; remaining; remaining &= remaining - 1)
					if (const std::size_t p = word * 64 + static_cast<std::size_t>(std::countr_zero(remaining)); p < tables.pipelines.size() && !tables.PipelineConstantsCurrent(p))
						channels.accumulateInputs.pipelineBlocksWaited.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		bool wrote = false;
		for (std::size_t word = 0; word < tables.usedPipelineBits.size(); ++word)
			for (std::uint64_t remaining = tables.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::size_t p = word * 64 + static_cast<std::size_t>(std::countr_zero(remaining));
				if (p >= tables.pipelines.size() || p >= tables.pipelineConstants.size() || p >= tables.pipelineBindingVersion.size() || tables.PipelineConstantsCurrent(p))
					continue;
				GeometryConstants constants{};
				if (!GeometryPort::PipelineGeometryConstants(tables.pipelines[p].passDescriptor, GeometryPort::kMainPassRenderFlags, *scenePipelineFrame, constants))
					continue;
				auto& row = tables.pipelineConstants[p];
				row.constants = constants;
				row.key = tables.pipelines[p];
				row.binding = tables.pipelineBindingVersion[p];
				row.version = tables.NextVersion();
				row.valid = true;
				wrote = true;
				channels.accumulateInputs.pipelineBlocksMade.fetch_add(1, std::memory_order_relaxed);
			}
		if (wrote)
			tables.NoteConstantsWrite();
	}

	Coordinator::AccumulateInputStats Coordinator::TakeAccumulateInputStats()
	{
		auto& c = channels.accumulateInputs;
		AccumulateInputStats s;
		s.ancestorMapsPosted = c.ancestorMapsPosted.exchange(0, std::memory_order_relaxed);
		s.ancestorMapsTaken = c.ancestorMapsTaken.exchange(0, std::memory_order_relaxed);
		s.pipelineFramesPosted = c.pipelineFramesPosted.exchange(0, std::memory_order_relaxed);
		s.pipelineFramesTaken = c.pipelineFramesTaken.exchange(0, std::memory_order_relaxed);
		s.pipelineBlocksMade = c.pipelineBlocksMade.exchange(0, std::memory_order_relaxed);
		s.pipelineBlocksWaited = c.pipelineBlocksWaited.exchange(0, std::memory_order_relaxed);
		s.ancestorValuesMoved = c.ancestorValuesMoved.exchange(0, std::memory_order_relaxed);
		s.ancestorFound = c.ancestorFound.exchange(0, std::memory_order_relaxed);
		s.ancestorNoKey = c.ancestorNoKey.exchange(0, std::memory_order_relaxed);
		s.ancestorNotMapped = c.ancestorNotMapped.exchange(0, std::memory_order_relaxed);
		return s;
	}

	template <class Result, class Evaluate>
	void Coordinator::EvaluateJoins(std::size_t a_count, bool a_pool, std::vector<Result>& a_results, Evaluate&& a_evaluate)
	{
		a_results.clear();
		a_results.resize(a_count);
		auto body = [&](std::size_t a_begin, std::size_t a_end) {
			ShardScope scope(*this, a_pool);
			for (std::size_t i = a_begin; i < a_end; ++i)
				a_evaluate(i, a_results[i]);
		};
		if (a_pool) {
			::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.Accumulate.Joins", a_count, kFanoutGrain, body);
		} else {
			for (std::size_t begin = 0; begin < a_count; begin += kFanoutGrain)
				body(begin, std::min(a_count, begin + kFanoutGrain));
		}
	}

	struct Coordinator::JoinFrame
	{
		PartTimer& timer;
		bool interior;
		std::uint32_t biasWitness;
		bool ancestorValueOn;
		std::uint32_t fadingThisFrame = 0;
	};

	struct Coordinator::JoinBinding
	{
		std::uint32_t pipelineSlot = Tables::kSlotFree, materialSlot = Tables::kSlotFree, staticFlags = 0;
		PipelineKey key{};
	};

	void Coordinator::BuildAccumulatePhase()
	{
		DCLF_ZONE("CS.DCLF.Accumulate.Tables");
		if (!sceneBuilt)
			return;  // a load screen, or the feature installed mid-frame: nothing to patch
		ShardScope passScope(*this, false);
		PartTimer timer(stats.partMs, &stats.accumulatePartMs);
		TracyCZoneN(captureZone, "CS.DCLF.Accumulate.Capture", true);
		accumulatedPasses.clear();
		accumulatedLayerPasses.clear();
		residentJoining.clear();
		residentLayerJoining.clear();
		BindByMembership();
		timer.Add(BuildPart::Walk);

		TracyCZoneEnd(captureZone);

		const bool interior = frameInterior;
		const auto& decalBiasMode = frameDecalBias;
		const std::uint32_t biasWitness = decalBiasMode[1] | (decalBiasMode[2] << 8) | (decalBiasMode[3] << 16);
		const bool ancestorValueOn = Extensions::AncestorValueOn();
		JoinFrame frame{ timer, interior, biasWitness, ancestorValueOn };

		// Only the joins. Every other record keeps its binding, or has none and is only a culling candidate.
		accumulateOrder.clear();
		TracyCZoneN(orderZone, "CS.DCLF.Accumulate.Order", true);
		accumulateOrder.reserve(accumulatedPasses.size());
		for (auto& [passGeometry, pass] : accumulatedPasses) {
			auto* mutableGeometry = const_cast<RE::BSGeometry*>(passGeometry);
			auto trackedIt = tracked.find(mutableGeometry);
			if (trackedIt != tracked.end())
				accumulateOrder.push_back({ mutableGeometry, &trackedIt->second, &pass });
		}
		for (auto& [passGeometry, pass] : accumulatedLayerPasses) {
			auto* mutableGeometry = const_cast<RE::BSGeometry*>(passGeometry);
			auto trackedIt = tracked.find(mutableGeometry);
			if (trackedIt != tracked.end())
				accumulateOrder.push_back({ mutableGeometry, &trackedIt->second, &pass, true });
		}
		timer.Add(BuildPart::PassLookup);
		TracyCZoneEnd(orderZone);
		TracyCZoneN(objectsZone, "CS.DCLF.Accumulate.Objects", true);
		RunJoins(frame);
		for (const std::uint32_t slot : rebindingMembers)
			LeaveMainForRebind(slot);
		rebindingMembers.clear();

		TracyCZoneEnd(objectsZone);
		TracyCZoneN(residentsZone, "CS.DCLF.Accumulate.Residents", true);
		SettleJoinResidency();
		TracyCZoneEnd(residentsZone);
		TracyCZoneN(statsZone, "CS.DCLF.Accumulate.StatsAndDecals", true);
		auto countBits = [](const std::vector<std::uint64_t>& a_bits) {
			std::uint32_t count = 0;
			for (const auto word : a_bits)
				count += static_cast<std::uint32_t>(std::popcount(word));
			return count;
		};
		stats.pipelines = countBits(tables.usedPipelineBits);
		stats.materials = countBits(tables.usedMaterialBits);
		ComponentsJoinsDone();
		stats.geometries = static_cast<std::uint32_t>(tables.geometrySlots.ReferencedCount());
		stats.geometriesAlive = static_cast<std::uint32_t>(tables.geometrySlots.AliveCount());
		stats.pipelinesAlive = static_cast<std::uint32_t>(tables.pipelineSlots.AliveCount());
		stats.materialsAlive = static_cast<std::uint32_t>(tables.materialSlots.AliveCount());
		stats.materialCacheEntries = static_cast<std::uint32_t>(tables.materialSlots.AliveCount());
		TracyCZoneEnd(statsZone);
	}

	void Coordinator::RunJoins(JoinFrame& a_frame)
	{
		[[maybe_unused]] const std::size_t geometries = tables.geometries.size();
		[[maybe_unused]] const std::uint32_t generation = tablesGeneration;
		const std::size_t count = accumulateOrder.size();
		auto& f = fanoutStats;
		++f.joinRounds;
		f.joinEntries += count;
		const std::uint32_t mode = FanoutMode(count);
		joinMaterialPrepared.clear();
		if (mode == 0) {
			for (const auto& entry : accumulateOrder) {
				JoinResult result;
				{
					ShardScope scope(*this, false);
					EvaluateJoin(entry, false, result);
				}
				MergeJoin(entry, result, a_frame);
			}
		} else {
			f.joinParallelRounds += mode == 2 ? 1 : 0;
			{
				DCLF_SCENE_PART(JoinsFanout, "CS.DCLF.Accumulate.JoinsFanout");
				EvaluateJoins(count, mode == 2, joinResults, [&](std::size_t a_index, JoinResult& a_out) { EvaluateJoin(accumulateOrder[a_index], false, a_out); });
			}
			{
				DCLF_SCENE_PART(JoinsFanout, "CS.DCLF.Accumulate.JoinMaterials");
				PrepareJoinMaterials(count);
			}
			DCLF_SCENE_PART(JoinsMerge, "CS.DCLF.Accumulate.JoinsMerge");
			for (std::size_t i = 0; i < count; ++i)
				MergeJoin(accumulateOrder[i], joinResults[i], a_frame);
		}
		assert(tables.geometries.size() == geometries && tablesGeneration == generation);
	}

	void Coordinator::MergeJoin(const OrderEntry& a_entry, JoinResult& a_result, JoinFrame& a_frame)
	{
		// Runs in order and does everything that reads or writes slots. EvaluateJoin ran before.
		Tracked* trackedEntry = a_entry.tracked;
		const bool layer = a_entry.layer;
		auto& timer = a_frame.timer;
		timer.Add(BuildPart::LoopTail);
		if (a_result.kind == JoinResult::Kind::Skip)
			return;
		// Only records written this frame with verdict None, never shadow-only.
		const std::uint32_t objectId = layer ? trackedEntry->layerSlot : trackedEntry->objectId;
		auto& object = tables.objects[objectId];
		// Earlier joins in this loop may have re-keyed the cached derivation's slots.
		auto& derived = layer ? trackedEntry->layerDerived : trackedEntry->derived;
		bool derivedHit = a_result.derivedWitness && derived.pipelineSlot < tables.pipelines.size() && tables.pipelineSlots.Alive(derived.pipelineSlot) &&
		                  tables.pipelines[derived.pipelineSlot] == derived.key &&
		                  derived.materialSlot < tables.materialSlotKey.size() && tables.materialSlots.Alive(derived.materialSlot) &&
		                  tables.materialSlotKey[derived.materialSlot] == std::pair{ derived.material, derived.descriptors.pass };
		if (a_result.derivedWitness && !derivedHit && !a_result.classified)
			EvaluateJoin(a_entry, true, a_result);
		auto* property = a_result.property;

		LightingDescriptors descriptors;
		const Ineligible reason = a_result.reason;
		if (derivedHit) {
			descriptors = derived.descriptors;
			++stats.derivedHits;
		} else {
			descriptors = a_result.descriptors;
		}
		timer.Add(BuildPart::ClassifyStatic);
		if (!layer && reason != trackedEntry->candidateReason) {
			--stats.ineligible[static_cast<std::size_t>(trackedEntry->candidateReason)];
			++stats.ineligible[static_cast<std::size_t>(reason)];
		}
		if (reason != Ineligible::None) {
			if (!layer) {
				trackedEntry->accumulateReason = reason;
				trackedEntry->accumulateReasonFrame = sceneFrame;
				trackedEntry->accumulateReasonPass = passSerial;
			}
			derived.valid = false;
			return;
		}
		timer.Add(BuildPart::ClassifyFrame);


		JoinBinding binding;
		if (derivedHit) {
			binding.pipelineSlot = derived.pipelineSlot;
			binding.materialSlot = derived.materialSlot;
			binding.staticFlags = derived.staticFlags;
			binding.key = derived.key;
			tables.MarkMaterialUsed(binding.materialSlot);
			joinMarkedMaterials.push_back(binding.materialSlot);
			// A pipeline's template is always a member's property, so a slot never points at a freed property.
			if (!tables.PipelineUsed(binding.pipelineSlot)) {
				tables.MarkPipelineUsed(binding.pipelineSlot);
				joinMarkedPipelines.push_back(binding.pipelineSlot);
				tables.geometryTemplate[binding.pipelineSlot] = property;
			}
			timer.Add(BuildPart::DedupHit);
		} else {
			binding.key = a_result.key;
			binding.pipelineSlot = BindJoinPipeline(a_result, descriptors, layer, timer);
			const std::size_t retries = bindRetry.size();
			if (!BindJoinMaterial(objectId, a_result, descriptors, derived, timer, binding.materialSlot)) {
				// Keeps drawing its current binding until the retry binds it.
				if (bindRetry.size() > retries && rebindingMembers.erase(objectId))
					residentJoining.erase(a_entry.geometry);
				return;
			}
			binding.staticFlags = a_result.staticFlags;
			RecordJoinDerivation(a_entry, a_result, a_frame, descriptors, binding, derived);
			if (!layer && a_entry.accumulated->hint == kFadingHint)
				RequestSettledPipeline(a_entry, a_result);
		}

		if (rebindingMembers.erase(objectId)) {
			// A drawn member switches binding in the publication that can draw it, and draws its current one until then.
			if (std::uint32_t why = 0; !MainReadyWith(objectId, binding.pipelineSlot, binding.materialSlot, why)) {
				StageBinding(objectId, binding.pipelineSlot, binding.materialSlot);
				residentJoining.erase(a_entry.geometry);
				return;
			}
			LeaveMainForRebind(objectId);
		}
		PatchJoin(a_entry, a_result, a_frame, object, objectId, descriptors, binding);
	}

	void Coordinator::RequestSettledPipeline(const OrderEntry& a_entry, const JoinResult& a_result)
	{
		// Build the pipeline for the settled fade now, so that rebind finds it ready.
		AccumulatedPass settled;
		if (!SettledMembershipPass(mirror.Leaf(a_entry.geometry), a_entry.tracked->membershipDerived, settled))
			return;
		OrderEntry entry = a_entry;
		entry.accumulated = &settled;
		JoinResult result;
		EvaluateJoin(entry, true, result);
		if (result.kind != JoinResult::Kind::Joined || result.key == a_result.key || pipelineIndex.contains(result.key))
			return;
		if (accumulateSettledAsked.insert(result.key).second)
			DrawPipelines::Get().RequestLighting(result.key, ~0u);
	}

	std::uint32_t Coordinator::BindJoinPipeline(const JoinResult& a_result, const LightingDescriptors& a_descriptors, bool a_layer, PartTimer& a_timer)
	{
		auto* property = a_result.property;
		const PipelineKey& key = a_result.key;
		auto pipelineIt = pipelineIndex.find(key);
		const bool newPipeline = pipelineIt == pipelineIndex.end();
		if (!newPipeline && !tables.PipelineUsed(pipelineIt->second)) {
			// The slot's first member's property is the template.
			tables.MarkPipelineUsed(pipelineIt->second);
			joinMarkedPipelines.push_back(pipelineIt->second);
			tables.geometryTemplate[pipelineIt->second] = property;
		}
		if (newPipeline) {
			a_timer.Add(BuildPart::Dedup);
			const std::uint32_t slot = MakeJoinPipeline(a_result, a_descriptors, a_layer);
			pipelineIt = pipelineIndex.emplace(key, slot).first;
			a_timer.Add(BuildPart::PipelineEval);
		}
		const std::uint32_t pipelineSlot = pipelineIt->second;
		tables.MarkPipelineUsed(pipelineSlot);
		joinMarkedPipelines.push_back(pipelineSlot);
		return pipelineSlot;
	}

	std::uint32_t Coordinator::MakeJoinPipeline(const JoinResult& a_result, const LightingDescriptors& a_descriptors, bool a_layer)
	{
		const auto& descriptors = a_descriptors;
		const PipelineKey& key = a_result.key;
		const auto* alpha = a_layer ? nullptr : a_result.leaf.alpha;
		const bool alphaBlending = alpha && (alpha->flags & 1u);  // GetAlphaBlending
		const std::uint32_t slot = AllocatePipelineSlot();
		tables.pipelines[slot] = key;
		tables.geometryTemplate[slot] = a_result.property;

		// A port's key without technique rows holds the plain technique's, with no shadow mask.
		const auto& rules = Extensions::PortRules(descriptors.port);
		tables.pipelineTechnique[slot] = TechniqueRowFor(rules.techniqueRows ? descriptors.pass : 0u);
		stats.shadowMaskPipelines += tables.TechniqueShadowMask(slot) ? 1 : 0;

		PipelinePermutation permutation;
		permutation.vertexShaderDescriptor = descriptors.rawVertex;
		permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
		permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
		// AdditiveLighting sets destination blend ONE. Only blended decals and forward objects (FUN_1414b39d0's flag 4) apply their alpha property.
		if ((descriptors.decalGroup == 2 || descriptors.forward) && alphaBlending &&
			static_cast<RE::NiAlphaProperty::AlphaFunction>((alpha->flags >> 5) & 0xF) == RE::NiAlphaProperty::AlphaFunction::kOne)  // GetDestBlendMode
			permutation.extraShaderDescriptor |= static_cast<std::uint32_t>(State::ExtraShaderDescriptors::AdditiveLighting);
		permutation.extraFeatureDescriptor = RasterRefraction(key.rasterFlags) ? 0u : Extensions::LightingKeyPermutation(RasterLightingSpare(key.rasterFlags));
		// A raw-permutation port's pass, as BSShader_BeginTechnique sets it.
		if (rules.rawPermutation) {
			permutation.vertexShaderDescriptor = descriptors.rawVertex;
			permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
			permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
			permutation.extraFeatureDescriptor = 0u;
		}
		tables.permutations[slot] = permutation;
		tables.pipelineBindingVersion[slot] = tables.NextVersion();
		// A slot keyed again may keep its used bit, so mark its mask for lookup.
		sharedBindings.MarkMask(slot);
		return slot;
	}

	bool Coordinator::BindJoinMaterial(std::uint32_t a_objectId, const JoinResult& a_result, const LightingDescriptors& a_descriptors, Tracked::Derived& a_derived,
		PartTimer& a_timer, std::uint32_t& a_materialSlot)
	{
		const std::uint32_t objectId = a_objectId;
		const auto& descriptors = a_descriptors;
		auto& derived = a_derived;
		auto* property = a_result.property;
		const auto* material = a_result.material;
		auto materialIt = materialIndex.find(std::pair{ material, descriptors.pass });
		if (materialIt == materialIndex.end()) {
			a_timer.Add(BuildPart::Dedup);
			// Without a capture the render thread is asked once and the join waits, native meanwhile.
			auto& ms = materialSnapshotStats;
			const auto held = materialSnapshots.find(material);
			if (held == materialSnapshots.end() || !held->second.held) {
				if (materialRequested.insert(material).second) {
					MaterialRequest request;
					request.property.reset(property);
					request.material = material;
					request.pass = descriptors.pass;
					channels.materialRequests.Push(std::move(request));
					++ms.requested;
				}
				bindRetry.push_back(objectId);
				++residentStats.materialWaits;
				derived.valid = false;
				return false;
			}
			held->second.frame = sceneFrame;
			const MaterialEvaluation* evaluation = nullptr;
			std::optional<MaterialEvaluation> here;
			if (const auto prepared = joinMaterialPrepared.find(std::pair{ material, descriptors.pass }); prepared != joinMaterialPrepared.end()) {
				evaluation = &joinMaterialResults[prepared->second];
				++materialRecordStats.joinPrepared;
			} else {
				EvaluateMaterial(held->second.held->snapshot, descriptors.pass, FrameGlobals::Current().material, kNoMaterialSlot, here.emplace());
				evaluation = &*here;
				++materialRecordStats.joinInline;
			}
			if (evaluation->outcome != MaterialEvaluation::Outcome::Made) {
				// An input the frame lacks yet is retried later. Else the port does not cover the class.
				derived.valid = false;
				if (evaluation->outcome == MaterialEvaluation::Outcome::Retry) {
					bindRetry.push_back(objectId);
					++residentStats.materialWaits;
					return false;
				}
				++ms.uncovered;
				return false;
			}
			++ms.made;
			// The character light's t11 view is the frame's, never the record's.
			const MaterialRecord& record = evaluation->record;
			const std::uint32_t slot = AllocateMaterialSlot();
			materialOwners.resize(tables.materials.size());
			// The capture holds a count, so the material is alive.
			if (materialOwners[slot])
				EngineReleases::PushOwner(std::make_shared<MaterialReference>(std::move(materialOwners[slot])));
			materialOwners[slot].reset(const_cast<RE::BSShaderMaterial*>(material));
			tables.materials[slot] = record;
			PinMaterialViews(slot, *held->second.held, FrameGlobals::Current().material);
			tables.materialVersion[slot] = ++materialVersions;
			tables.NoteMaterial(slot);
			tables.materialSlotKey[slot] = std::pair{ material, descriptors.pass };
			materialIt = materialIndex.emplace(std::pair{ material, descriptors.pass }, slot).first;
			ListMaterialDependent(material, slot);
			NoteMaterialRecord(slot, held->second.held->snapshot);
			++materialRecordStats.transformsWatched;
			a_timer.Add(BuildPart::MaterialEval);
		}
		a_materialSlot = materialIt->second;
		tables.MarkMaterialUsed(a_materialSlot);
		joinMarkedMaterials.push_back(a_materialSlot);
		a_timer.Add(BuildPart::DedupHit);
		return true;
	}

	void Coordinator::RecordJoinDerivation(const OrderEntry& a_entry, const JoinResult& a_result, const JoinFrame& a_frame,
		const LightingDescriptors& a_descriptors, const JoinBinding& a_binding, Tracked::Derived& a_derived)
	{
		const AccumulatedPass* accumulated = a_entry.accumulated;
		const auto& descriptors = a_descriptors;
		auto& derived = a_derived;
		const std::uint32_t pipelineSlot = a_binding.pipelineSlot, materialSlot = a_binding.materialSlot, staticFlags = a_binding.staticFlags;
		const PipelineKey& key = a_binding.key;
		derived.valid = true;
		derived.generation = tablesGeneration;
		derived.triShape = tables.geometrySlotKey[a_result.geometrySlot];
		derived.property = a_result.property;
		derived.material = a_result.material;
		derived.fadeState = a_result.fadeState;
		derived.interior = a_frame.interior;
		derived.alphaBelowOne = a_result.alphaBelowOne;
		derived.alphaState = a_result.alphaState;
		derived.technique = accumulated->technique;
		derived.subPass = accumulated->subPass;
		derived.hint = accumulated->hint;
		derived.biasWitness = a_frame.biasWitness;
		derived.descriptors = descriptors;
		derived.staticFlags = staticFlags;
		derived.key = key;
		derived.geometrySlot = a_result.geometrySlot;
		derived.pipelineSlot = pipelineSlot;
		derived.materialSlot = materialSlot;
	}

	void Coordinator::PatchJoin(const OrderEntry& a_entry, const JoinResult& a_result, JoinFrame& a_frame, ObjectRecord& a_object, std::uint32_t a_objectId,
		const LightingDescriptors& a_descriptors, const JoinBinding& a_binding)
	{
		RE::BSGeometry* geometry = a_entry.geometry;
		Tracked* trackedEntry = a_entry.tracked;
		const AccumulatedPass* accumulated = a_entry.accumulated;
		const bool layer = a_entry.layer;
		const std::uint32_t objectId = a_objectId;
		const auto& descriptors = a_descriptors;
		auto& timer = a_frame.timer;
		const std::uint32_t pipelineSlot = a_binding.pipelineSlot, materialSlot = a_binding.materialSlot;
		const bool landBlendRecord = Extensions::TechniqueExtras(descriptors.technique) == Api::ObjectExtras::LandBlend;
		AccumulatePatch patch;
		patch.object = objectId;
		patch.material = materialSlot;
		patch.pipeline = pipelineSlot;
		patch.projectedUV = descriptors.projectedUV;
		patch.landBlend = landBlendRecord;
		// Membership is CommitSet's, which keeps a record being bound out of the set.
		patch.flags = (a_object.flags & kSceneKeptFlags) | a_binding.staticFlags | (accumulated->sunTest ? kObjectSunTest : 0u) |
		              (accumulated->fadeDistance != 0.0f ? kObjectFadeTest : 0u) | (accumulated->heightTest ? kObjectHeightTest : 0u) |
		              (patch.projectedUV ? kObjectProjectedUV : 0u) | (patch.landBlend ? kObjectLandBlend : 0u) |
		              ((PhasesIn(tables, objectId) & kSetMain) ? kObjectMember : 0u);
		if (PhasesIn(tables, objectId) & kSetMain)
			++setStats.patchedMember;
		patch.fadeDistance = accumulated->fadeDistance;
		timer.Add(BuildPart::Record);
		if (a_frame.ancestorValueOn) {
			if (a_result.ancestorSet) {
				trackedEntry->ancestorValue = a_result.ancestorValue;
				trackedEntry->ancestorMapGeneration = ancestorMapGeneration;
			}
			patch.ancestorValue.value = trackedEntry->ancestorValue;
		}
		timer.Add(BuildPart::CapturePatch);
		if (descriptors.pass & kPassAdditionalAlphaMask) {
			if (a_frame.fadingThisFrame++ == 0)
				++stats.fadingFrames;
			++stats.fadingDrawn;
		}
		// The engine draws a group's buckets in ascending pass enum, so effect decals precede Lighting ones.
		if (descriptors.decalGroup) {
			const std::uint32_t passEnum = accumulated->technique + Extensions::PortRules(descriptors.port).passEnumBase;
			patch.decalKey = (std::uint64_t(descriptors.decalGroup) << 60) | (std::uint64_t(passEnum) << 27) | (std::uint64_t(accumulated->subPass & 7) << 24);
		}
		timer.Add(BuildPart::Record);
		ApplyAccumulatePatch(patch);
		UnstageBinding(objectId);
		timer.Add(BuildPart::ApplyPatch);
		MarkResidentSlot(objectId, { *accumulated, pipelineSlot, materialSlot });
		(layer ? residentLayerJoining : residentJoining).erase(geometry);
		++residentStats.joined;
		timer.Add(BuildPart::Record);
	}

	void Coordinator::SettleJoinResidency()
	{
		for (const auto* geometry : residentJoining) {
			++residentStats.failed;
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			const auto cause = entry == tracked.end() || entry->second.objectStamp != objectStamp ? 1u :                  // no record
			                   entry->second.accumulateReasonPass == passSerial ? 2u :                                           // a verdict of this pass
			                   3u;                                                                                          // material, or extras rows
			++residentStats.failedBy[cause];
			if (entry != tracked.end() && entry->second.slot != kNoObjectSlot && IsResidentSlot(entry->second.slot))
				DropResidentSlot(entry->second.slot, true);
		}
		residentJoining.clear();
		for (const auto* geometry : residentLayerJoining) {
			++residentStats.failed;
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			if (entry != tracked.end() && entry->second.layerSlot != kNoObjectSlot && IsResidentSlot(entry->second.layerSlot))
				DropResidentSlot(entry->second.layerSlot, true);
		}
		residentLayerJoining.clear();
		// The set is by geometry, so a base without its layer leaves.
		for (const auto& entry : accumulateOrder) {
			const auto& t = *entry.tracked;
			if (t.layerSlot == kNoObjectSlot || t.slot == kNoObjectSlot || IsResidentSlot(t.slot) == IsResidentSlot(t.layerSlot))
				continue;
			++residentStats.layerUnpaired;
			DropResidentSlot(IsResidentSlot(t.slot) ? t.slot : t.layerSlot, true);
		}
		KeepResidentsAlive();
		++residentStats.frames;
		residentStats.resident += residents.size();
		{
			// Slots are freed only when unreferenced. Parity neutralises any object that dangles.
			if (Policy::NeutraliseDanglingSlots(slotsFreedThisFrame, passSerial))
				CheckObjectSlots(frameResolveBuffers);
			slotsFreedThisFrame = false;
		}
	}

	void Coordinator::PrepareJoinMaterials(std::size_t a_count)
	{
		// Each (material, pass) no slot holds is evaluated once here, so the merges only allocate and store.
		auto& r = materialRecordStats;
		auto& items = joinMaterialItems;
		items.clear();
		joinMaterialPrepared.clear();
		for (std::size_t i = 0; i < a_count && i < joinResults.size(); ++i) {
			const auto& result = joinResults[i];
			if (result.kind != JoinResult::Kind::Joined || !result.classified || !result.material)
				continue;
			const std::pair key{ result.material, result.descriptors.pass };
			if (materialIndex.contains(key))
				continue;
			const auto held = materialSnapshots.find(result.material);
			if (held == materialSnapshots.end() || !held->second.held)
				continue;  // the merge requests it
			if (joinMaterialPrepared.try_emplace(key, static_cast<std::uint32_t>(items.size())).second)
				items.push_back({ result.material, &held->second.held->snapshot, result.descriptors.pass });
		}
		if (items.empty())
			return;
		auto& results = joinMaterialResults;
		results.resize(items.size());
		const auto& sources = FrameGlobals::Current().material;
		if (PrepareMaterials("CS.DCLF.Accumulate.JoinMaterialsShards", items.size(), [&](std::size_t a_begin, std::size_t a_end) {
				for (std::size_t i = a_begin; i < a_end; ++i)
					EvaluateMaterial(*items[i].snapshot, items[i].pass, sources, kNoMaterialSlot, results[i]);
			})) {
			r.evaluationsPooled += items.size();
			++r.poolRounds;
		}
	}

	void Coordinator::LeaveMainForRebind(std::uint32_t a_slot)
	{
		// A join never patches a member, so a rebinding record first leaves the main phases. The commit later readmits it if ready.
		if (a_slot < setPhasesNext.size() && (setPhasesNext[a_slot] & kSetMain))
			++setStats.rebinding;
		// Every view's member draws with its main bindings, so it leaves those too.
		const auto phases = static_cast<std::uint8_t>(kSetMain | Extensions::Views().phases);
		LeaveSet(a_slot, phases);
		if (PhasesIn(tables, a_slot) & phases) {
			tables.setPhases[a_slot] = 0;
			tables.objects[a_slot].flags &= ~kObjectMember;
			tables.NoteChange(a_slot, kChangeBindings);
		}
	}

	bool Coordinator::RebindKeepsDrawing(std::uint32_t a_slot) const
	{
		if (a_slot >= tables.objects.size() || a_slot >= setPhasesNext.size() || !(setPhasesNext[a_slot] & kSetMain) || !IsResidentSlot(a_slot))
			return false;
		return !tables.IsLayer(a_slot) && (a_slot >= tables.layerOf.size() || tables.layerOf[a_slot] == kNoObjectSlot);
	}

	void Coordinator::BindByMembership()
	{
		DCLF_ZONE("CS.DCLF.Accumulate.BindByMembership");
		// The frame globals a membership pass reads changed, so every resident rebinds. Each keeps drawing until its new binding is ready.
		if (const std::uint32_t witness = sceneInputs.membershipWitness; witness != membershipWitness) {
			for (const std::uint32_t slot : residents)
				if (const auto it = slot < tables.objectGeometry.size() ? tracked.find(const_cast<RE::BSGeometry*>(tables.objectGeometry[slot])) : tracked.end(); it != tracked.end())
					(tables.IsLayer(slot) ? it->second.layerDerived : it->second.derived).valid = false;
			bindQueue.insert(bindQueue.end(), residents.begin(), residents.end());
			membershipWitness = witness;
		}
		// The material sources know the Lighting shader only from the frame after it was found. Without waiting every record of the load would fail.
		if (!EngineShaders::Lighting() || !FrameGlobals::Current().material.vanilla.sampled || !FrameGlobals::Current().material.vanilla.shaderKnown)
			return;
		bindQueue.insert(bindQueue.end(), bindRetry.begin(), bindRetry.end());
		bindRetry.clear();
		const auto queue = std::exchange(bindQueue, {});
		// A slot may be queued twice. Evaluations read the state before the merges, and each merge handles what earlier merges changed.
		auto merge = [&](std::uint32_t a_slot, JoinPre& a_pre) {
			const std::uint32_t slot = a_slot;
			// Its partner failed after it was evaluated, so evaluate again.
			if (a_pre.kind == JoinPre::Kind::Kept && !IsResidentSlot(slot))
				EvaluateJoinPre(slot, a_pre);
			if (a_pre.kind == JoinPre::Kind::Skip)
				return;
			// Skins, faces and movers are rewritten every frame for transforms alone.
			if (a_pre.kind == JoinPre::Kind::Kept) {
				++residentStats.membershipKept;
				return;
			}
			auto* geometry = a_pre.geometry;
			const bool layer = a_pre.layer;
			auto& entry = tracked.find(geometry)->second;
			(layer ? entry.membershipLayerDerived : entry.membershipDerived) = a_pre.derived;
			if (a_pre.kind == JoinPre::Kind::Failed) {
				// Unbind, or the commit would keep it with a binding it no longer has.
				if (IsResidentSlot(slot))
					DropResidentSlot(slot, true);
				return;
			}
			(layer ? accumulatedLayerPasses : accumulatedPasses).insert_or_assign(geometry, a_pre.pass);
			(layer ? residentLayerJoining : residentJoining).insert(geometry);
			if (RebindKeepsDrawing(slot))
				rebindingMembers.insert(slot);
			else
				LeaveMainForRebind(slot);
			++residentStats.membershipQueued;
		};
		const std::size_t count = queue.size();
		const std::uint32_t mode = FanoutMode(count);
		auto& f = fanoutStats;
		++f.joinRounds;
		f.joinEntries += count;
		if (mode == 0) {
			for (const std::uint32_t slot : queue) {
				JoinPre pre;
				{
					ShardScope scope(*this, false);
					EvaluateJoinPre(slot, pre);
				}
				merge(slot, pre);
			}
			return;
		}
		f.joinParallelRounds += mode == 2 ? 1 : 0;
		{
			DCLF_SCENE_PART(JoinsFanout, "CS.DCLF.Accumulate.BindFanout");
			EvaluateJoins(count, mode == 2, joinPres, [&](std::size_t a_index, JoinPre& a_out) { EvaluateJoinPre(queue[a_index], a_out); });
		}
		{
			DCLF_SCENE_PART(JoinsMerge, "CS.DCLF.Accumulate.BindMerge");
			for (std::size_t i = 0; i < count; ++i)
				merge(queue[i], joinPres[i]);
		}
	}

	void Coordinator::EvaluateJoinPre(std::uint32_t a_slot, JoinPre& a_out) const
	{
		// Pure. PrimaryCull's pass reads the frame globals the ShardScope binds.
		const std::uint32_t slot = a_slot;
		auto& r = a_out;
		r = JoinPre{};
		if (slot >= tables.objects.size() || (tables.objects[slot].flags & (kObjectFree | kObjectShadowOnly)))
			return;
		auto* geometry = const_cast<RE::BSGeometry*>(tables.objectGeometry[slot]);
		const auto trackedIt = geometry ? tracked.find(geometry) : tracked.end();
		const bool layer = tables.IsLayer(slot);
		if (trackedIt == tracked.end() || (layer ? trackedIt->second.layerSlot : trackedIt->second.slot) != slot ||
			trackedIt->second.objectStamp != objectStamp || trackedIt->second.candidateReason != Ineligible::None)
			return;
		const auto& entry = trackedIt->second;
		r.geometry = geometry;
		r.layer = layer;
		if (IsResidentSlot(slot) && MemberBindingStands(slot, entry)) {
			r.kind = JoinPre::Kind::Kept;
			return;
		}
		r.leaf = mirror.Leaf(geometry);
		const auto& cached = layer ? entry.membershipLayerDerived : entry.membershipDerived;
		const auto membership = layer ? PrimaryCull::MembershipLayerPass(r.leaf, cached) : PrimaryCull::MembershipPass(r.leaf, cached);
		r.derived = membership.derived;
		r.fail = membership.fail;
		if (!membership.ok) {
			r.kind = JoinPre::Kind::Failed;
			return;
		}
		r.pass = membership.pass;
		if (const auto* fadeNode = r.leaf.fadeNode) {
			Api::RootTestOutput test;
			Extensions::RootStatesTest(*fadeNode, FrameGlobals::Current(), test);
			r.pass.fadeDistance = test.distance;
			r.pass.heightTest = (test.objectFlags & kObjectHeightTest) != 0;
		}
		r.kind = JoinPre::Kind::Pass;
	}

	void Coordinator::EvaluateJoin(const OrderEntry& a_entry, bool a_classify, JoinResult& a_out) const
	{
		// Pure. Anything touching slots or records is the merge's.
		auto* geometry = a_entry.geometry;
		const Tracked& entry = *a_entry.tracked;
		const AccumulatedPass* accumulated = a_entry.accumulated;
		const bool layer = a_entry.layer;
		auto& r = a_out;
		r = JoinResult{};
		const std::uint32_t objectId = layer ? entry.layerSlot : entry.objectId;
		r.geometrySlot = tables.objects[objectId].geometryIndex;
		r.leaf = mirror.Leaf(geometry);
		const auto* propertyRecord = layer ? r.leaf.layer : r.leaf.property;
		r.property = entry.HeldProperty(layer);
		if (!propertyRecord || !r.property || propertyRecord->key != r.property)
			return;
		r.material = static_cast<const RE::BSShaderMaterial*>(propertyRecord->material);
		r.fadeState = FadeStateOf(propertyRecord);
		r.alphaBelowOne = propertyRecord->materialAlpha < 1.0f;  // 1 without a material, or for any but a Lighting property
		// A flora's harvest raises the threshold in place.
		r.alphaState = layer ? 0u : r.leaf.AlphaState();
		const bool interior = frameInterior;
		const auto& decalBiasMode = frameDecalBias;
		const std::uint32_t biasWitness = decalBiasMode[1] | (decalBiasMode[2] << 8) | (decalBiasMode[3] << 16);
		const auto& derived = layer ? entry.layerDerived : entry.derived;
		r.derivedWitness = derived.valid && derived.generation == tablesGeneration && derived.geometrySlot == r.geometrySlot && derived.property == r.property &&
		                   derived.material == r.material && derived.fadeState == r.fadeState && derived.technique == accumulated->technique &&
		                   derived.subPass == accumulated->subPass && derived.hint == accumulated->hint && derived.interior == interior &&
		                   derived.alphaBelowOne == r.alphaBelowOne && derived.alphaState == r.alphaState && derived.biasWitness == biasWitness;
		r.classified = a_classify || !r.derivedWitness;
		Ineligible reason = Ineligible::None;
		if (r.classified)
			reason = layer ? Eligibility::Layer(r.leaf, &r.descriptors, accumulated) : Eligibility::Static(r.leaf, &r.descriptors, accumulated);
		if (r.classified && reason == Ineligible::None && !layer)
			ViewDecalForward(entry, r.descriptors);
		// Every time, since an object just hidden must lose its bindings now or DCLF keeps drawing what the engine stopped.
		if (reason == Ineligible::None)
			reason = ClassifyFrame(entry);
		r.reason = reason;
		if (reason != Ineligible::None) {
			r.kind = JoinResult::Kind::Rejected;
			return;
		}
		r.kind = JoinResult::Kind::Joined;
		if (r.classified)
			KeyJoin(entry, layer, *propertyRecord, interior, r);
		if (Extensions::AncestorValueOn() && entry.ancestorMapGeneration != ancestorMapGeneration) {
			r.ancestorSet = true;
			r.ancestorValue = Extensions::AncestorValue()->none;
			auto& counters = channels.accumulateInputs;
			bool found = false;
			if (ancestorMap && entry.ancestorKey)
				if (const auto it = ancestorMap->find(entry.ancestorKey); it != ancestorMap->end()) {
					r.ancestorValue = it->second;
					found = true;
				}
			(found ? counters.ancestorFound : entry.ancestorKey ? counters.ancestorNotMapped : counters.ancestorNoKey).fetch_add(1, std::memory_order_relaxed);
		}
	}

	void Coordinator::KeyJoin(const Tracked& a_entry, bool a_layer, const SceneCapture::PropertyRecord& a_property, bool a_interior, JoinResult& a_out) const
	{
		const Tracked& entry = a_entry;
		auto& r = a_out;
		const auto& decalBiasMode = frameDecalBias;
		const auto& descriptors = r.descriptors;
		const Api::LightingJoinView view{ a_property, r.leaf, r.leaf.geometry->extraValue, r.leaf.node->facts, a_property.contributorBits };
		using PropertyFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		const bool twoSided = (a_property.flags & static_cast<std::uint64_t>(PropertyFlag::kTwoSided)) != 0;
		// A layer's draws apply no alpha property.
		const auto* alpha = a_layer ? nullptr : r.leaf.alpha;
		const bool alphaTest = alpha && (alpha->flags & (1u << 9));  // GetAlphaTesting
		const bool alphaBlending = alpha && (alpha->flags & 1u);     // GetAlphaBlending
		// A decal's key carries the engine's fixed-function state indices.
		std::uint32_t rasterFlags = twoSided ? kRasterTwoSided : 0u;
		if (descriptors.decalGroup)
			rasterFlags |= PackDecalRasterFlags(descriptors.decalGroup, descriptors.opaqueInDecalGroup ? 0u : decalBiasMode[descriptors.decalGroup & 3], descriptors.decalBlendMode,
				               descriptors.decalWriteMode) |  // an opaque effect in decal group 1 has no bias (DeriveEffectDescriptors)
			               (descriptors.opaqueInDecalGroup ? 0u : kRasterDecalNudge);
		// As Terrain Blending's replay sets it.
		if (descriptors.terrainGroup)
			rasterFlags |= PackTerrainRasterFlags(descriptors.terrainGroup, descriptors.terrainGroup == 1 ? 1u : 0u, 1u);
		// As the engine's blended draw (FUN_1414b39d0) leaves it.
		if (descriptors.forward)
			rasterFlags |= PackForwardRasterFlags(descriptors.forwardBlendMode, kForwardWriteMode, descriptors.forwardDepthTest, descriptors.forwardDepthWrite);
		if (descriptors.forward && descriptors.forwardDecalGroup)
			rasterFlags |= ((decalBiasMode[descriptors.forwardDecalGroup & 3] & 15u) << kRasterDepthBiasShift) | kRasterDecalNudge;
		if (descriptors.refraction)
			rasterFlags |= PackRefractionRasterFlags(descriptors.refractionFalloff, descriptors.refractionClamp);
		// A refraction key never draws its Lighting pipeline, so its spare bits hold a billboard mode instead.
		if (!descriptors.port && descriptors.refraction) {
			if (entry.parentReason == Ineligible::Billboard)
				if (const std::int32_t mode = BillboardModeOf(entry); mode >= 0)
					rasterFlags |= static_cast<std::uint32_t>(mode + 1) << kRasterRefractionBillboardShift;
		} else if (!descriptors.port)
			rasterFlags |= Extensions::LightingKeyBitsOf(view) << kRasterLightingSpareShift;
		const auto& family = Extensions::FamilyRulesOfKind(descriptors.port);
		rasterFlags |= family.keyBit;
		if (family.turnedKeyBits && entry.parentReason == Ineligible::Billboard)
			if (const std::int32_t mode = BillboardModeOf(entry); mode >= 0)
				rasterFlags |= static_cast<std::uint32_t>(mode + 1) << family.spareShift;
		r.key = PipelineKey{ descriptors.vertex, descriptors.pixel, rasterFlags, descriptors.pass,
			VertexLayoutOf(tables.geometries[r.geometrySlot].vertexDesc) };
		// As ExternalEmittance::ShouldSuppress decides.
		const bool suppress = a_interior && (a_property.flags & static_cast<std::uint64_t>(PropertyFlag::kExternalEmittance)) && !r.leaf.node->emittance;
		r.staticFlags = (alphaTest ? kObjectAlphaTest : 0u) | (twoSided ? kObjectTwoSided : 0u) |
		                (suppress ? kObjectSuppressExternalEmittance : 0u) |
		                TechniqueFlags(descriptors.technique) |
		                // A Blood effect keeps its threshold too, tested or not.
		                ((alphaTest || (alpha && (descriptors.pass & family.records.alphaRefPassBits))) ?
		                        static_cast<std::uint32_t>(alpha->threshold) << kObjectAlphaThresholdShift :
		                        0u) |
		                (alphaTest && alphaBlending ? kObjectAlphaBlended : 0u) |
		                (descriptors.decalGroup ? kObjectDecal | (descriptors.decalGroup << kObjectDecalGroupShift) : 0u) |
		                (descriptors.terrainGroup << kObjectTerrainGroupShift) |
		                (descriptors.forward ? kObjectForward : 0u) |
		                (!descriptors.port && (a_property.flags & LandscapeLightFlags()) ? kObjectLandscapeLights : 0u) |
		                (!descriptors.port ? Extensions::LightingObjectFlagsOf(view) : 0u);  // Integrations/SubsurfaceScattering's kObjectBeastRace
	}

	bool Coordinator::MemberBindingStands(std::uint32_t a_slot, const Tracked& a_entry) const
	{
		const bool layer = tables.IsLayer(a_slot);
		const auto& derived = layer ? a_entry.layerDerived : a_entry.derived;
		const auto leaf = mirror.Leaf(a_entry.geometry.get());
		const auto* record = layer ? leaf.layer : leaf.property;
		const auto* property = record ? static_cast<const RE::BSShaderProperty*>(record->key) : nullptr;
		const auto* material = record ? static_cast<const RE::BSShaderMaterial*>(record->material) : nullptr;
		const bool alphaBelowOne = record && record->materialAlpha < 1.0f;
		const std::uint32_t alphaState = layer ? 0u : leaf.AlphaState();
		// A blended fade starting or ending changes the synthetic pass's hint, so the binding goes with it.
		const bool fading = !layer && BlendFading(leaf, record) && Extensions::ObjectClassOn(Api::ObjectClass::Forward);
		return derived.valid && (derived.hint == kFadingHint) == fading && derived.alphaState == alphaState && derived.generation == tablesGeneration && derived.geometrySlot == tables.objects[a_slot].geometryIndex &&
		       derived.property == property && derived.material == material && derived.fadeState == FadeStateOf(record) &&
		       derived.alphaBelowOne == alphaBelowOne && derived.interior == frameInterior && derived.pipelineSlot == tables.objects[a_slot].pipelineIndex &&
		       derived.materialSlot == tables.objects[a_slot].materialIndex;
	}

	void Coordinator::ResetAccumulatedHalf(std::uint32_t a_slot)
	{
		if (a_slot >= tables.objects.size() || (tables.objects[a_slot].flags & kObjectFree))
			return;
		const auto columnsBefore = tables.ColumnsOf(a_slot);
		auto& object = tables.objects[a_slot];
		object.flags = tables.sceneFlags[a_slot];
		object.materialIndex = 0;
		object.pipelineIndex = 0;
		tables.draws[a_slot].pipelineIndex = 0;
		tables.ancestorValue[a_slot] = ObjectAncestorValue{};
		tables.FreeExtras(a_slot);
		tables.fadeDistance[a_slot] = 0.0f;
		tables.NoteWrite(a_slot, columnsBefore);
	}

	void Coordinator::RefreshShadowSets()
	{
		DCLF_ZONE("CS.DCLF.Capture.ShadowSets");
		// The dependency index holds membership across frames. Each index keeps its keys ordered by their lowest member.
		shadowTextureNet.clear();
		const bool rebuild = shadowIndexNeedsRebuild;
		std::vector<std::uint32_t> changed = std::move(shadowDirtySlots);
		shadowDirtySlots.clear();
		auto& counters = shadowSetStats;
		++counters.refreshes;
		counters.unjournaled += !rebuild && changed.empty() ? 1u : 0u;
		// Every input write journals its slot, and invalidations with no slot set shadowIndexNeedsRebuild.
		if (!rebuild) {
			stats.shadowCasters = keptShadowCasters;
			stats.shadowRejects = keptShadowRejects;
		}
		std::uint32_t moved = rebuild ? ~0u : 0u;
		if (rebuild)
			ResetShadowIndex(changed);
		shadowIndexNeedsRebuild = false;
		shadowIndexedInputs.resize(tables.objects.size());
		shadowIndexedLive.resize(tables.objects.size(), 0);
		std::sort(changed.begin(), changed.end());
		changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
		counters.journaled += rebuild ? 0u : changed.size();
		TracyCZoneN(indexZone, "CS.DCLF.Capture.ShadowSets.Index", true);
		for (const auto slot : changed) {
			if (slot >= tables.objects.size())
				continue;
			const bool live = !(tables.objects[slot].flags & kObjectFree);
			const auto inputs = ShadowInputsOf(slot);
			if ((shadowIndexedLive[slot] != 0) == live && shadowIndexedInputs[slot] == inputs)
				continue;
			++counters.applied;
			ApplyShadowInputs(slot, shadowIndexedInputs[slot], shadowIndexedLive[slot] != 0, false, moved);
			ApplyShadowInputs(slot, inputs, live, true, moved);
			shadowIndexedInputs[slot] = inputs;
			shadowIndexedLive[slot] = live;
		}
		TracyCZoneEnd(indexZone);
		shadowSetsDirty = false;
		TracyCZoneN(emitZone, "CS.DCLF.Capture.ShadowSets.Emit", true);
		EmitShadowSets(rebuild, moved);
		TracyCZoneEnd(emitZone);
	}

	void Coordinator::ResetShadowIndex(std::vector<std::uint32_t>& a_changed)
	{
		DCLF_ZONE("CS.DCLF.Capture.ShadowSets.Rebuild");
		auto& net = shadowTextureNet;
		auto& changed = a_changed;
		++shadowSetStats.rebuilds;
		// Only the difference is noted.
		for (const auto& entry : shadowTextureMembers)
			--net[entry.first];
		shadowIndexedInputs.clear();
		shadowIndexedLive.clear();
		shadowTextureMembers.clear();
		shadowKeyMembers.clear();
		for (auto& members : occlusionKeyMembers)
			members.clear();
		shadowTextureOrder.clear();
		shadowKeyOrder.clear();
		for (auto& keyOrder : occlusionKeyOrder)
			keyOrder.clear();
		stats.shadowCasters = 0;
		stats.shadowRejects = {};
		changed.clear();
		changed.reserve(tables.objects.size());
		for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot)
			changed.push_back(slot);
	}

	void Coordinator::ApplyShadowInputs(std::uint32_t a_slot, const ShadowInputs& a_inputs, bool a_live, bool a_add, std::uint32_t& a_moved)
	{
		auto& net = shadowTextureNet;
		if (!a_live)
			return;
		auto& rejects = stats.shadowRejects[std::min<std::size_t>(a_inputs.reject, stats.shadowRejects.size() - 1)];
		a_add ? ++rejects : --rejects;
		const bool caster = !(a_inputs.flags & kObjectNoShadow);
		if (caster)
			a_add ? ++stats.shadowCasters : --stats.shadowCasters;
		const bool occluder = std::any_of(a_inputs.occlusionTechnique.begin(), a_inputs.occlusionTechnique.end(), [](std::uint32_t a_t) { return a_t != 0; });
		if (a_inputs.diffuse && (caster || occluder))
			if (const std::int32_t membership = ShadowMember(shadowTextureMembers, shadowTextureOrder, a_inputs.diffuse, a_slot, a_add, kTextureOrder, a_moved))
				net[a_inputs.diffuse] += membership;
		const auto raster = (a_inputs.flags & kObjectTwoSided) ? kRasterTwoSided : 0u;
		const auto vertex = VertexLayoutOf(a_inputs.vertexDesc);
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (a_inputs.occlusionTechnique[v])
				ShadowMember(occlusionKeyMembers[v], occlusionKeyOrder[v], ShadowPipelineKey{ a_inputs.occlusionTechnique[v], raster, vertex }, a_slot, a_add, kOcclusionOrder << v, a_moved);
		if (caster)
			ShadowMember(shadowKeyMembers, shadowKeyOrder, ShadowPipelineKey{ a_inputs.technique, raster, vertex }, a_slot, a_add, kKeyOrder, a_moved);
	}

	void Coordinator::EmitShadowSets(bool a_rebuild, std::uint32_t a_moved)
	{
		auto& net = shadowTextureNet;
		auto& counters = shadowSetStats;
		const bool rebuild = a_rebuild;
		const std::uint32_t moved = a_moved;
		// Each list is made again only when its order moved.
		if (moved & kTextureOrder) {
			shadowEmittedTextures.clear();
			shadowEmittedTextures.reserve(shadowTextureOrder.size());
			for (const auto& [slot, texture] : shadowTextureOrder)
				shadowEmittedTextures.push_back(texture);
			++counters.listsEmitted;
		}
		if (rebuild) {
			shadowEmittedSeen.clear();
			shadowEmittedSeen.insert(shadowEmittedTextures.begin(), shadowEmittedTextures.end());
		} else {
			for (const auto& [texture, change] : net) {
				if (change < 0)
					shadowEmittedSeen.erase(texture);
				else if (change > 0)
					shadowEmittedSeen.insert(texture);
			}
		}
		if (moved & kKeyOrder) {
			shadowEmittedKeys.clear();
			for (const auto& [slot, key] : shadowKeyOrder)
				shadowEmittedKeys.push_back(key);
			++counters.listsEmitted;
		}
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			if (!(moved & (kOcclusionOrder << v)))
				continue;
			shadowEmittedOcclusionKeys[v].clear();
			for (const auto& [slot, key] : occlusionKeyOrder[v])
				shadowEmittedOcclusionKeys[v].push_back(key);
			++counters.listsEmitted;
		}
		tables.shadowTextureSet = shadowEmittedTextures;
		tables.shadowTextureSeen = shadowEmittedSeen;
		tables.shadowKeysUsed = shadowEmittedKeys;
		tables.occlusionKeysUsed = shadowEmittedOcclusionKeys;
		// A texture that went and came back is no change.
		for (const auto& [texture, change] : net)
			if (change < 0)
				tables.shadowTextureChanges.emplace_back(texture, false);
		for (const auto& [texture, change] : net)
			if (change > 0)
				tables.shadowTextureChanges.emplace_back(texture, true);
	}
}
