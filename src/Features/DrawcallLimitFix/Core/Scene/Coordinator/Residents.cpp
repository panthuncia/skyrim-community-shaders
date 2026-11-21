// The coordinator's residents on the scene lane: resident slots marked, dropped and kept alive, and fade nodes held for the slots' listings.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

namespace DCLF::Scene
{
	void Coordinator::MarkResidentSlot(std::uint32_t a_slot, const ResidentPatch& a_patch)
	{
		NoteResidentTouched(a_slot);
		if (residentPos.size() <= a_slot)
			residentPos.resize(std::max<std::size_t>(a_slot + 1, tables.objects.size()), kNotResident);
		ComponentsResident(a_slot);
		if (residentPos[a_slot] != kNotResident) {
			residentPatches[residentPos[a_slot]] = a_patch;
			return;
		}
		residentPos[a_slot] = static_cast<std::uint32_t>(residents.size());
		residents.push_back(a_slot);
		residentPatches.push_back(a_patch);
		if (a_slot < tables.residentSlot.size())
			tables.residentSlot[a_slot] = 1;
	}

	void Coordinator::DropResidentSlot(std::uint32_t a_slot, bool a_restore)
	{
		if (!IsResidentSlot(a_slot))
			return;
		NoteResidentTouched(a_slot);
		const std::uint32_t at = residentPos[a_slot];
		const std::uint32_t last = residents.back();
		residents[at] = last;
		residentPatches[at] = residentPatches.back();
		residentPos[last] = at;
		residents.pop_back();
		residentPatches.pop_back();
		residentPos[a_slot] = kNotResident;
		UnstageBinding(a_slot);
		ComponentsLeft(a_slot);
		// Scene work drops reach PrimaryCull as set changes. The accumulate phase's reach it at once.
		if (a_slot < tables.objectGeometry.size() && tables.objectGeometry[a_slot]) {
			if (!holdPrimaryNotes)
				PrimaryCull::Get().NoteMemberLost(tables.objectGeometry[a_slot]);
			else if (holdLostMembers)
				channels.primaryNotes.Push({ PrimaryNote::Kind::MemberLost, tables.objectGeometry[a_slot] });
		}
		if (a_slot < tables.residentSlot.size()) {
			tables.residentSlot[a_slot] = 0;
			tables.NoteChange(a_slot, kChangeMembership);
		}
		if (a_restore)
			ResetAccumulatedHalf(a_slot);
		// A layer and its base are members together.
		const std::uint32_t partner = tables.IsLayer(a_slot) ? tables.layerBase[a_slot] : a_slot < tables.layerOf.size() ? tables.layerOf[a_slot] : kNoObjectSlot;
		if (partner != kNoObjectSlot)
			DropResidentSlot(partner, a_restore);
	}

	void Coordinator::EndAllResidency()
	{
		residentMaintenanceDirty = true;
		if (holdPrimaryNotes)
			channels.primaryNotes.Push({ PrimaryNote::Kind::AllMembersLost, nullptr });
		else
			PrimaryCull::Get().NoteAllMembersLost();
		ComponentsAllLeft();
		for (const std::uint32_t slot : residents) {
			ComponentsLeft(slot);
			ResetAccumulatedHalf(slot);
			residentPos[slot] = kNotResident;
			if (slot < tables.residentSlot.size()) {
				tables.residentSlot[slot] = 0;
				tables.NoteChange(slot, kChangeMembership);
			}
		}
		residents.clear();
		residentPatches.clear();
		for (const auto& [slot, staged] : stagedBindings)
			ReleaseStagedReferences(staged);
		stagedBindings.clear();
	}

	void Coordinator::ReleaseStagedReferences(const StagedBinding& a_staged)
	{
		tables.pipelineSlots.Release(a_staged.pipeline, a_staged.pipelineGeneration);
		tables.materialSlots.Release(a_staged.material, a_staged.materialGeneration);
	}

	void Coordinator::AddStagedReferences(const StagedBinding& a_staged)
	{
		tables.pipelineSlots.AddRef(a_staged.pipeline, a_staged.pipelineGeneration);
		tables.materialSlots.AddRef(a_staged.material, a_staged.materialGeneration);
	}

	void Coordinator::StageBinding(std::uint32_t a_slot, std::uint32_t a_pipeline, std::uint32_t a_material)
	{
		auto [it, fresh] = stagedBindings.try_emplace(a_slot);
		if (!fresh && it->second.pipeline == a_pipeline && it->second.material == a_material)
			return;
		if (!fresh)
			ReleaseStagedReferences(it->second);
		// Referenced like a record's binding, or the sweep would free the slots before they are ready.
		it->second = { a_pipeline, a_material, tables.pipelineSlots.Generation(a_pipeline), tables.materialSlots.Generation(a_material) };
		AddStagedReferences(it->second);
		NoteResidentTouched(a_slot);
	}

	void Coordinator::UnstageBinding(std::uint32_t a_slot)
	{
		const auto it = stagedBindings.find(a_slot);
		if (it == stagedBindings.end())
			return;
		ReleaseStagedReferences(it->second);
		stagedBindings.erase(it);
		NoteResidentTouched(a_slot);
	}

	void Coordinator::KeepResidentsAlive()
	{
		DCLF_ZONE("CS.DCLF.Accumulate.KeepResidents");
		auto& counted = residentCounted;
		if (counted.size() < tables.objects.size())
			counted.resize(tables.objects.size());
		if (materialMembers.size() < tables.materials.size())
			materialMembers.resize(tables.materials.size(), 0);
		std::vector<std::uint32_t> pipelinesTouched, materialsTouched;
		auto keysOf = [&](std::uint32_t a_slot) {
			ResidentCounted keys;
			if (!IsResidentSlot(a_slot) || a_slot >= tables.objects.size())
				return keys;
			const auto& object = tables.objects[a_slot];
			keys.pipeline = object.pipelineIndex;
			keys.material = object.materialIndex;
			return keys;
		};
		if (stagedCounted.size() < tables.objects.size())
			stagedCounted.resize(tables.objects.size());
		auto stagedKeysOf = [&](std::uint32_t a_slot) {
			ResidentCounted keys;
			if (const auto it = stagedBindings.find(a_slot); it != stagedBindings.end() && IsResidentSlot(a_slot))
				keys = { it->second.pipeline, it->second.material };
			return keys;
		};
		auto uncountIn = [&](SlotMembers& a_pipelines, ResidentCounted& a_was, std::uint32_t a_slot) {
			if (a_was.pipeline != ResidentCounted::kNone) {
				a_pipelines.Remove(a_was.pipeline, a_slot);
				pipelinesTouched.push_back(a_was.pipeline);
			}
			if (a_was.material != ResidentCounted::kNone && a_was.material < materialMembers.size()) {
				--materialMembers[a_was.material];
				materialsTouched.push_back(a_was.material);
			}
			a_was = {};
		};
		auto countIn = [&](SlotMembers& a_pipelines, ResidentCounted& a_counted, const ResidentCounted& a_keys, std::uint32_t a_slot) {
			if (a_keys.pipeline == ResidentCounted::kNone)
				return;
			a_pipelines.Add(a_keys.pipeline, a_slot);
			pipelinesTouched.push_back(a_keys.pipeline);
			if (materialMembers.size() <= a_keys.material)
				materialMembers.resize(std::size_t(a_keys.material) + 1, 0);
			++materialMembers[a_keys.material];
			materialsTouched.push_back(a_keys.material);
			a_counted = a_keys;
		};
		auto uncount = [&](std::uint32_t a_slot) {
			uncountIn(pipelineMembers, counted[a_slot], a_slot);
			uncountIn(stagedPipelineMembers, stagedCounted[a_slot], a_slot);
		};
		auto count = [&](std::uint32_t a_slot) {
			countIn(pipelineMembers, counted[a_slot], keysOf(a_slot), a_slot);
			countIn(stagedPipelineMembers, stagedCounted[a_slot], stagedKeysOf(a_slot), a_slot);
		};
		if (residentMaintenanceDirty) {
			DCLF_ZONE("CS.DCLF.Accumulate.RebuildResidentMaintenance");
			counted.assign(tables.objects.size(), ResidentCounted{});
			stagedCounted.assign(tables.objects.size(), ResidentCounted{});
			stagedPipelineMembers.Clear();
			pipelineMembers.Clear();
			materialMembers.assign(tables.materials.size(), 0);
			tables.ClearUsed();
			for (const std::uint32_t slot : residents)
				count(slot);
			ComponentsRecount(true, {});
			residentMaintenanceDirty = false;
		} else {
			// Take all old keys out first, since a fade root can be freed and listed again for another node within the frame.
			std::sort(residentsTouched.begin(), residentsTouched.end());
			residentsTouched.erase(std::unique(residentsTouched.begin(), residentsTouched.end()), residentsTouched.end());
			for (const std::uint32_t slot : residentsTouched)
				if (slot < counted.size())
					uncount(slot);
			for (const std::uint32_t slot : residentsTouched)
				if (slot < counted.size())
					count(slot);
			ComponentsRecount(false, residentsTouched);
		}
		residentsTouched.clear();
		pipelinesTouched.insert(pipelinesTouched.end(), joinMarkedPipelines.begin(), joinMarkedPipelines.end());
		materialsTouched.insert(materialsTouched.end(), joinMarkedMaterials.begin(), joinMarkedMaterials.end());
		joinMarkedPipelines.clear();
		joinMarkedMaterials.clear();
		for (const std::uint32_t p : pipelinesTouched)
			tables.SetPipelineUsed(p, pipelineMembers.Count(p) != 0 || stagedPipelineMembers.Count(p) != 0);
		for (const std::uint32_t m : materialsTouched)
			tables.SetMaterialUsed(m, m < materialMembers.size() && materialMembers[m] != 0);
		// A pipeline's lighting template is its first member's held property. A leaf update's property swap rewrites no slot, so it notes
		// the slot here to have its pipeline recounted.
		auto templateOf = [&](std::uint32_t a_pipeline) {
			const std::uint32_t slot = pipelineMembers.Count(a_pipeline) ? pipelineMembers.First(a_pipeline) : stagedPipelineMembers.First(a_pipeline);
			return slot != ~0u ? SlotProperty(slot) : nullptr;
		};
		for (const std::uint32_t p : pipelinesTouched)
			if ((pipelineMembers.Count(p) || stagedPipelineMembers.Count(p)) && p < tables.geometryTemplate.size())
				tables.geometryTemplate[p] = templateOf(p);
	}

	RE::NiPointer<RE::NiAVObject> Coordinator::HeldFadeNode(std::uint32_t a_slot, const void* a_node)
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto it = geometry ? tracked.find(const_cast<RE::BSGeometry*>(geometry)) : tracked.end();
		if (it != tracked.end() && it->second.fadeNodeRef.get() == a_node)
			return it->second.fadeNodeRef;
		if (it != tracked.end()) {
			HandBack(std::move(it->second.fadeNodeRef));
			if (AcquireFadeNode(it->second, a_node, "a listing"))
				return it->second.fadeNodeRef;
		}
		++referenceStats.fadeRootsUnowned;
		return nullptr;
	}

	void NoteFadeRootUnmirrored()
	{
		Coordinator::Get().NoteFadeRootUnmirrored();
	}
}
