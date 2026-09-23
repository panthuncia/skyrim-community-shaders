#include "SceneStore.h"

#include "MaterialSources.h"

#include "EngineStates.h"

#include "Switches.h"
#include "Toggles.h"

#include "GpuResources.h"
#include "PassCapture.h"
#include "SceneTracker.h"
#include "ShadowProbe.h"
#include "ShadowViews.h"
#include "VertexInput.h"

#include <bit>
#include <chrono>

#include "Features/ExtendedTranslucency.h"
#include "Features/LightLimitFix.h"
#include "State.h"
#include "Utils/ExternalEmittance.h"

namespace DCLF
{
	namespace
	{
		// Cell 3D category nodes Drawcall Limit Fix draws from (engine notes: cell 3D category nodes).
		// The Actor node holds the actors of an exterior (an interior moves them into its rooms). It is
		// walked whatever CS_DCLF_ACTORS says: the toggle is ClassifyFrame's Actor rule, so it can change live.
		constexpr std::array<std::uint32_t, 4> kDrawnCategories{ 0 /*Actor*/, 3 /*Static*/, 4 /*Dynamic*/, 5 /*MultiBound*/ };
		constexpr std::uint32_t kMaxParentDepth = 64;
		constexpr std::size_t kValidationSlice = 256;
		constexpr std::uint32_t kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT
		constexpr std::uint32_t kSpecularBit = 0x200;  // pass descriptor Specular
		constexpr std::uint32_t kTechniqueEnvmap = 1;
		constexpr std::uint32_t kTechniqueTreeAnim = 12;

		const RE::BSRenderPass* FindLightingPass(RE::BSShaderProperty* a_property)
		{
			for (auto* pass = a_property->renderPassList.head; pass; pass = pass->next) {
				if (pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting && pass->numLights > 0 && pass->sceneLights)
					return pass;
			}
			return nullptr;
		}

		ObjectShading MakeShading(const RE::BSLightingShaderProperty& a_property, const LightingDescriptors& a_descriptors, std::uint32_t a_renderFlags,
			float& a_emissiveMult)
		{
			// BSLightingShader::SetupGeometry (engine notes): which components it writes depends on the pass.
			const float unwritten = std::bit_cast<float>(kUnwrittenBits);
			const bool specular = (a_descriptors.pass & kSpecularBit) != 0;
			ObjectShading shading{};
			shading.materialData[0] = a_descriptors.technique == kTechniqueEnvmap ? a_descriptors.envmapLODFade : unwritten;
			shading.materialData[1] = specular ? a_descriptors.specularLODFade : unwritten;
			shading.materialData[2] = a_property.alpha;
			shading.materialData[3] = unwritten;
			const float mult = a_property.emissiveMult;
			// The same sample the emissive colour below folds in: the shader divides it out again, so the
			// two must never come from different reads of an animated value.
			a_emissiveMult = mult;
			const auto* emissive = a_property.emissiveColor;
			shading.emitColor[0] = emissive ? emissive->red * mult : unwritten;
			shading.emitColor[1] = emissive ? emissive->green * mult : unwritten;
			shading.emitColor[2] = emissive ? emissive->blue * mult : unwritten;
			shading.ssrSpecular = ((a_renderFlags & 2) ? 0.0f : 1.0f) * (specular ? a_descriptors.specularLODFade : 0.0f);
			return shading;
		}

		void StoreTransform(const RE::NiTransform& a_transform, float (&a_out)[12])
		{
			// Row-major 3x4: rotation scaled, translation in the last column.
			const auto& r = a_transform.rotate.entry;
			const float s = a_transform.scale;
			for (int row = 0; row < 3; ++row) {
				a_out[row * 4 + 0] = r[row][0] * s;
				a_out[row * 4 + 1] = r[row][1] * s;
				a_out[row * 4 + 2] = r[row][2] * s;
			}
			a_out[3] = a_transform.translate.x;
			a_out[7] = a_transform.translate.y;
			a_out[11] = a_transform.translate.z;
		}

		bool IsHidden(const RE::NiAVObject* a_object)
		{
			return a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden);
		}

		/**
		 * @brief The engine's per-frame palette update (AE FUN_140e4ff90): what the bone setter runs from the
		 * native draw an owned object no longer gets. Idempotent within a frame (frameID); it copies the current
		 * palette to the previous one first and writes three float4 rows a bone in absolute world space.
		 * Render thread only.
		 */
		void UpdateSkin(RE::NiSkinInstance* a_skin, const RE::NiTransform& a_world)
		{
			using UpdateSkinInstance = void (*)(RE::NiSkinInstance*, const RE::NiTransform*);
			static const REL::Relocation<UpdateSkinInstance> updateSkinInstance{ REL::Offset(0xe4ff90) };
			updateSkinInstance(a_skin, &a_world);
		}

		/** @brief CS_DCLF_ASYNC=probe: the scene walk's per-object arrays, two walks compared byte for byte. */
		bool SameSceneTables(const SceneStore::Tables& a, const SceneStore::Tables& b, std::string& a_difference)
		{
			auto differs = [&](const char* a_name, const auto& a_lhs, const auto& a_rhs) {
				using T = std::remove_cvref_t<decltype(*a_lhs.data())>;
				const std::size_t lhsBytes = a_lhs.size() * sizeof(T), rhsBytes = a_rhs.size() * sizeof(T);
				if (lhsBytes != rhsBytes) {
					a_difference = fmt::format("{}: {} vs {} entries", a_name, a_lhs.size(), a_rhs.size());
					return true;
				}
				if (lhsBytes && std::memcmp(a_lhs.data(), a_rhs.data(), lhsBytes) != 0) {
					const auto* l = reinterpret_cast<const std::uint8_t*>(a_lhs.data());
					const auto* r = reinterpret_cast<const std::uint8_t*>(a_rhs.data());
					std::size_t k = 0;
					while (l[k] == r[k])
						++k;
					a_difference = fmt::format("{}: entry {} of {} (byte {} of the entry)", a_name, k / sizeof(T), a_lhs.size(), k % sizeof(T));
					return true;
				}
				return false;
			};
			return !(differs("objects", a.objects, b.objects) || differs("object geometry", a.objectGeometry, b.objectGeometry) ||
					 differs("draws", a.draws, b.draws) || differs("skin partitions", a.skinPartitions, b.skinPartitions) || differs("bones", a.bones, b.bones) || differs("previous bones", a.previousBones, b.previousBones) ||
					 differs("bone offsets", a.boneOffset, b.boneOffset) || differs("bone rows", a.boneRows, b.boneRows) ||
					 differs("shadow techniques", a.shadowTechnique, b.shadowTechnique) || differs("shadow rejects", a.shadowReject, b.shadowReject) ||
					 differs("shadow diffuse", a.shadowDiffuse, b.shadowDiffuse) || differs("shadow materials", a.shadowMaterial, b.shadowMaterial) || differs("shadow keys", a.shadowKeysUsed, b.shadowKeysUsed) ||
					 differs("shadow textures", a.shadowTextureSet, b.shadowTextureSet) || differs("extra offsets", a.extraOffset, b.extraOffset) ||
					 differs("geometry slots used", a.geometryLastUsed, b.geometryLastUsed));
		}
	}

	void SceneStore::Tables::ClearFrame()
	{
		objects.clear();
		objectGeometry.clear();
		shading.clear();
		emissiveMult.clear();
		lights.clear();
		treeAnim.clear();
		skinPartitions.clear();
		draws.clear();
		decalOrdinal.clear();
		decalCount = {};
		bones.clear();
		previousBones.clear();
		boneOffset.clear();
		boneRows.clear();
		extraRows.clear();
		extraOffset.clear();
		shadowTechnique.clear();
		shadowReject.clear();
		shadowDiffuse.clear();
		shadowMaterial.clear();
		shadowTextureSet.clear();
		shadowTextureSeen.clear();
		shadowKeysUsed.clear();
	}

	void SceneStore::Tables::Clear()
	{
		geometryLastUsed.clear();
		geometrySlotKey.clear();
		geometryFree.clear();
		pipelineLastUsed.clear();
		pipelineFree.clear();
		materialLastUsed.clear();
		materialSlotKey.clear();
		materialFree.clear();
		objects.clear();
		objectGeometry.clear();
		geometries.clear();
		pipelines.clear();
		materials.clear();
		materialVersion.clear();
		shading.clear();
		emissiveMult.clear();
		lights.clear();
		treeAnim.clear();
		skinPartitions.clear();
		geometryConstants.clear();
		geometryConstantsValid.clear();
		geometryTemplate.clear();
		geometryTemplateNative.clear();
		techniqueConstants.clear();
		permutations.clear();
		draws.clear();
		decalOrdinal.clear();
		decalCount = {};
		bones.clear();
		previousBones.clear();
		boneOffset.clear();
		boneRows.clear();
		extraRows.clear();
		extraOffset.clear();
		shadowTechnique.clear();
		shadowReject.clear();
		shadowDiffuse.clear();
		shadowMaterial.clear();
		shadowTextureSet.clear();
		shadowTextureSeen.clear();
		shadowKeysUsed.clear();
	}

	SceneStore& SceneStore::Get()
	{
		static SceneStore store;
		return store;
	}

	void SceneStore::Clear()
	{
		AbandonSceneJob();
		tracked.clear();
		categoryNodes.clear();
		ResetSlotTables();
		InvalidateObjectIndices();
		// They hold raw pointers into game allocations now that they outlive the frame, so the teardown
		// paths have to drop them rather than leave them to the next BuildFrame.
		geometryIndex.clear();
		pipelineIndex.clear();
		materialIndex.clear();
		materialCache.clear();
		validationCursor = 0;
	}

	namespace
	{
		// What a node between a leaf and its category node makes of the leaf. An ordered node depends on draw
		// order, which does not survive being drawn out of the native loop. A billboard turns to the camera in
		// the main cull (NiBillboardNode::OnVisible), after the scene walk read its world transform. A switch
		// node draws one child at a time: Switch marks the leaf for the per-frame test (SwitchSelects).
		Ineligible ParentReason(RE::NiNode* a_node)
		{
			if (a_node->AsSwitchNode())
				return Ineligible::Switch;
			if (netimmerse_cast<RE::BSOrderedNode*>(a_node))
				return Ineligible::UnsupportedParent;
			if (netimmerse_cast<RE::NiBillboardNode*>(a_node))
				return Ineligible::Billboard;
			return Ineligible::None;
		}

		// NiSkinPartition::Unk_25 (AE 140d43a10) draws partition i when this table holds a non-zero byte at
		// (LODMode.index + LODMode.singleLevel * 4) * 3 + the partition's LOD byte (Partition+0x42). Read from
		// AE 1.6.1170 at 0x14202a030, where nothing writes it: level n draws the LOD bytes below n, and
		// single-level n draws LOD byte n alone.
		constexpr std::array<std::uint8_t, 24> kPartitionLodTable{ 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
	}

	std::uint32_t SceneStore::LodRowOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty* a_property)
	{
		if (!a_geometry.GetFlags().any(RE::NiAVObject::Flag::kMeshLOD) || !a_property || !a_property->fadeNode)
			return 3;
		return a_property->fadeNode->GetRuntimeData().unk152 & 0xF;
	}

	std::uint32_t SceneStore::LodRowOf(const RE::BSRenderPass& a_pass)
	{
		return a_pass.LODMode.index + (a_pass.LODMode.singleLevel ? 4u : 0u);
	}

	std::uint32_t SceneStore::SkinPartitionMask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow)
	{
		const auto* partition = a_skin.skinPartition.get();
		if (!partition || a_lodRow >= 8)
			return 0;
		static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
		const RE::BSDismemberSkinInstance::Data* shown = a_skin.GetRTTI() == dismemberSkinInstance.get() ?
		                                                     static_cast<const RE::BSDismemberSkinInstance&>(a_skin).GetRuntimeData().partitions :
		                                                     nullptr;
		std::uint32_t mask = 0;
		for (std::uint32_t i = 0; i < partition->numPartitions && i < kMaxSkinPartitions; ++i) {
			if (shown && !shown[i].editorVisible)
				continue;
			const std::uint32_t lodByte = partition->partitions[i].pad42 & 0xFF;
			if (lodByte <= 2 && kPartitionLodTable[a_lodRow * 3 + lodByte])
				mask |= 1u << i;
		}
		return mask;
	}

	namespace
	{
		// The stronger of two parent reasons: an unsupported parent outranks a billboard, which outranks a
		// switch (the only one decided per frame).
		Ineligible CombineParentReasons(Ineligible a_lhs, Ineligible a_rhs)
		{
			for (const auto reason : { Ineligible::UnsupportedParent, Ineligible::Billboard, Ineligible::Switch })
				if (a_lhs == reason || a_rhs == reason)
					return reason;
			return Ineligible::None;
		}
	}

	RE::NiNode* SceneStore::FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason) const
	{
		Ineligible reason = Ineligible::None;
		RE::NiNode* node = a_object ? a_object->parent : nullptr;
		for (std::uint32_t depth = 0; node && depth < kMaxParentDepth; ++depth, node = node->parent) {
			if (categoryNodes.contains(node)) {
				if (a_parentReason)
					*a_parentReason = reason;
				return node;
			}
			reason = CombineParentReasons(reason, ParentReason(node));
		}
		return nullptr;
	}

	std::uint64_t SceneStore::CategorySignature() const
	{
		// A cheap stand-in for "the set of category nodes may have changed". It has to be conservative in
		// one direction only: if it misses a change, geometry attached under a newly appeared node is
		// never tracked, because AddSubtree needs the category node to already be known. So it folds in
		// everything the refresh below reads to *find* nodes - the interior cell, the grid cells, each
		// cell's loaded data and cell3D and how many children it has, and objRoot's child count - rather
		// than just the cell pointers.
		std::uint64_t hash = 0xcbf29ce484222325ull;
		auto mix = [&hash](auto a_value) {
			hash = (hash ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_value))) * 0x100000001b3ull;
		};
		auto mixCell = [&](RE::TESObjectCELL* a_cell) {
			mix(a_cell);
			if (!a_cell || !a_cell->IsAttached())
				return;
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			mix(loaded);
			RE::NiNode* cell3D = loaded ? loaded->cell3D.get() : nullptr;
			mix(cell3D);
			if (cell3D)
				hash = (hash ^ cell3D->GetChildren().size()) * 0x100000001b3ull;
		};
		if (auto* tes = RE::TES::GetSingleton()) {
			if (auto* objRoot = tes->objRoot)
				hash = (hash ^ objRoot->GetChildren().size()) * 0x100000001b3ull;
			mix(tes->objRoot);
			mix(tes->interiorCell);
			if (tes->interiorCell) {
				mixCell(tes->interiorCell);
			} else if (auto* grid = tes->gridCells) {
				const std::uint32_t count = grid->length * grid->length;
				for (std::uint32_t i = 0; i < count; ++i)
					mixCell(grid->cells[i]);
			}
		}
		return hash;
	}

	void SceneStore::RefreshCategoryNodes(bool a_force)
	{
		// This used to rebuild and diff the whole set on every Present, including an O(tracked) scan
		// whenever any node had gone. Its *content* changes only when a cell attaches or detaches, so it
		// now runs when the signature says something moved, when a detach was seen, or on a slow backstop
		// cadence in case both miss something.
		constexpr std::uint32_t kBackstopFrames = 30;
		const std::uint64_t signature = CategorySignature();
		if (!a_force && signature == categorySignature && ++categoryIdleFrames < kBackstopFrames)
			return;
		const std::uint8_t cause = a_force ? 1 : signature != categorySignature ? 0 : 2;
		categorySignature = signature;
		categoryIdleFrames = 0;

		ankerl::unordered_dense::set<RE::NiNode*> current;

		auto addCell = [&](RE::TESObjectCELL* a_cell) {
			if (!a_cell || !a_cell->IsAttached())
				return;
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			if (!loaded || !loaded->cell3D)
				return;
			auto& children = loaded->cell3D->GetChildren();
			for (auto category : kDrawnCategories) {
				const auto index = static_cast<std::uint16_t>(category);
				if (index < children.size() && children[index]) {
					if (auto* node = children[index]->AsNode())
						current.insert(node);
				}
			}
			// References the engine moves out of the category nodes into multibounds (exteriors) and the
			// portal graph's rooms (interiors), all under ObjectLODRoot (engine notes: cell multibounds and rooms).
			if (auto* multiBound = loaded->multiBoundNode.get())
				current.insert(multiBound);
			if (auto* graph = loaded->portalGraph.get()) {
				for (auto& room : graph->rooms) {
					if (room)
						current.insert(room.get());
				}
				if (auto* shared = graph->portalSharedNode.get())
					current.insert(shared);
			}
		};

		if (auto* tes = RE::TES::GetSingleton()) {
			// Multibounds: the engine moves references out of the category nodes into BSMultiBoundNodes
			// under ObjectLODRoot (TES::objRoot), in exteriors and (holding the rooms) in interiors
			// (engine notes: multibounds and rooms).
			if (auto* objRoot = tes->objRoot) {
				for (auto& child : objRoot->GetChildren()) {
					if (auto* multiBound = child ? netimmerse_cast<RE::BSMultiBoundNode*>(child.get()) : nullptr)
						current.insert(multiBound);
				}
			}
			if (tes->interiorCell) {
				addCell(tes->interiorCell);
			} else if (auto* grid = tes->gridCells) {
				const std::uint32_t count = grid->length * grid->length;
				for (std::uint32_t i = 0; i < count; ++i)
					addCell(grid->cells[i]);
			}
		}

		// Cells that went away: drop what was tracked under them.
		bool removedAny = false;
		for (auto* node : categoryNodes) {
			if (!current.contains(node)) {
				removedAny = true;
				break;
			}
		}
		if (removedAny) {
			std::vector<RE::BSGeometry*> stale;
			for (auto& [geometry, entry] : tracked) {
				if (!current.contains(entry.categoryNode))
					stale.push_back(geometry);
			}
			for (auto* geometry : stale)
				tracked.erase(geometry);
		}

		// Cells that appeared: their content was attached before the cell was, so scan it now.
		std::vector<RE::NiNode*> added;
		for (auto* node : current) {
			if (!categoryNodes.contains(node))
				added.push_back(node);
		}
		categoryNodes = std::move(current);
		std::erase_if(categoryFound, [&](const auto& a_entry) { return !categoryNodes.contains(const_cast<RE::NiNode*>(a_entry.first)); });
		const TrackSource previousSource = addSource;
		if (addSource != TrackSource::Rescan)
			addSource = TrackSource::CategoryAppeared;
		for (auto* node : added) {
			categoryFound[node] = { frame, cause };
			for (auto& child : node->GetChildren()) {
				if (child)
					AddSubtree(child.get());
			}
		}
		addSource = previousSource;
	}

	void SceneStore::AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason)
	{
		const auto [it, inserted] = tracked.try_emplace(a_geometry);
		auto& entry = it->second;
		if (inserted) {
			entry.trackedFrame = frame;
			entry.trackedBy = addSource;
		}
		entry.geometry.reset(a_geometry);
		entry.categoryNode = a_categoryNode;
		entry.parentReason = a_parentReason;
	}

	void SceneStore::AddSubtree(RE::NiAVObject* a_root)
	{
		// The attach event is drained a frame or more after it was queued, so its node can already be gone
		// (a cell transition releases the subtree). Walking it then dereferences null.
		if (!a_root)
			return;
		Ineligible reasonAbove = Ineligible::None;
		RE::NiNode* category = FindCategoryNode(a_root, &reasonAbove);
		if (!category)
			return;

		// Walk down, carrying what the nodes between the category node and the leaf make of it (ParentReason).
		std::vector<std::pair<RE::NiAVObject*, Ineligible>> stack;
		stack.emplace_back(a_root, reasonAbove);
		while (!stack.empty()) {
			auto [object, reason] = stack.back();
			stack.pop_back();
			if (!object)
				continue;
			if (auto* geometry = object->AsGeometry()) {
				AddGeometry(geometry, category, reason);
				continue;
			}
			if (auto* node = object->AsNode()) {
				const Ineligible below = CombineParentReasons(reason, ParentReason(node));
				for (auto& child : node->GetChildren()) {
					if (child)
						stack.emplace_back(child.get(), below);
				}
			}
		}
	}

	void SceneStore::ValidateSlice()
	{
		// Safety net for missed detaches: a tracked geometry must still hang under the category node it
		// was found under. Checks a slice per frame so the cost stays flat.
		if (tracked.empty())
			return;
		std::vector<RE::BSGeometry*> stale;
		const std::size_t size = tracked.size();
		const std::size_t count = std::min(kValidationSlice, size);
		for (std::size_t i = 0; i < count; ++i) {
			validationCursor = (validationCursor + 1) % size;
			// The map's storage is a dense vector, so the cursor walks it without hashing.
			auto it = tracked.begin() + static_cast<std::ptrdiff_t>(validationCursor);
			Ineligible reason = Ineligible::None;
			if (FindCategoryNode(it->first, &reason) != it->second.categoryNode)
				stale.push_back(it->first);
			else
				it->second.parentReason = reason;
		}
		for (auto* geometry : stale) {
			tracked.erase(geometry);
			++stats.validationDrops;
		}
	}

	bool SceneStore::IsLoadingScreenUp()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
	}

	void SceneStore::ProcessEvents()
	{
		// Nothing here may walk the scene graph while a load screen is up. A load tears down and rebuilds
		// TES::objRoot and the cell 3D under it, and the attach events queued across it name subtrees that
		// are still being assembled; walking either gives a pointer that is stale or simply garbage. That
		// is what crashed in RefreshCategoryNodes' objRoot walk (a child that read back as
		// 0x0001000000020001) and, the day before, in AddSubtree.
		//
		// The queue is still drained, because it holds references to attached subtrees and the comment on
		// the caller is right that it must not grow while the world is not rendered - but the events are
		// discarded rather than applied, and the first frame after the load rebuilds the tracked set from
		// scratch. A load invalidates all of it anyway, so nothing is lost by not trying to track across it.
		auto& tracker = SceneTracker::Get();
		if (SceneStore::IsLoadingScreenUp()) {
			// Only the walking stops. The tracked set is deliberately left alone until the load is over:
			// dropping it here releases the game buffers the tables reference while the previous frame's
			// epoch is still in flight, and a draw then reads a freed device address. That is a
			// VK_ERROR_DEVICE_LOST on the teleport, which is exactly what happened when this branch cleared
			// eagerly. The entries hold NiPointers, so holding them across the load is the safe direction,
			// and the rescan below replaces them on a normal frame.
			rescanPending = true;
			// Claims do not survive a load. They name geometry from the cell being torn down, and a claim
			// withholds the pass from the native loop - so a stale one means nobody draws that object in
			// the new cell. The hole detector caught exactly this at a `coc`: six claimed objects went
			// undrawn on the first frame after the transition. Publishing an empty set hands everything
			// back to the native loop until DCLF has drawn it again and re-earned the claim.
			PassCapture::Get().PublishClaims(std::make_shared<const PassCapture::ClaimSet>());
			SceneTracker::FreeEvents(tracker.Drain());
			return;
		}
		const bool rescanned = rescanPending;
		if (rescanPending) {
			// RefreshCategoryNodes treats every category node as newly appeared and walks it, which is
			// exactly the full rescan wanted here.
			rescanPending = false;
			tracked.clear();
			categoryNodes.clear();
			validationCursor = 0;
		}

		// Drained before the category refresh, so a detach this frame can force it: a detach can take a
		// category node with it, and the signature cannot see that until the cell itself goes.
		SceneTracker::Event* events = tracker.Drain();
		bool sawDetach = false;
		for (const auto* event = events; event && !sawDetach; event = event->next)
			sawDetach = event->type == SceneTracker::EventType::Detached;
		addSource = rescanned ? TrackSource::Rescan : TrackSource::AttachEvent;
		RefreshCategoryNodes(sawDetach || rescanned);
		addSource = TrackSource::AttachEvent;

		for (auto* event = events; event; event = event->next) {
			if (event->type == SceneTracker::EventType::Attached) {
				++stats.attachedEvents;
				if (!categoryNodes.empty())
					AddSubtree(event->node.get());
			} else {
				++stats.detachedEvents;
				for (auto* geometry : event->removed)
					tracked.erase(geometry);
			}
		}
		SceneTracker::FreeEvents(events);

		ValidateSlice();
		stats.tracked = static_cast<std::uint32_t>(tracked.size());
		stats.categoryNodes = static_cast<std::uint32_t>(categoryNodes.size());
	}

	void SceneStore::FindLightingShader()
	{
		// Any lighting render pass carries the BSLightingShader instance.
		for (auto& [geometry, entry] : tracked) {
			auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property)
				continue;
			for (auto* pass = property->renderPassList.head; pass; pass = pass->next) {
				if (pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					ConstantEvaluator::Get().SetLightingShader(pass->shader);
					return;
				}
			}
		}
	}

	bool SceneStore::CacheableVerdict(Ineligible a_reason)
	{
		// Only the verdicts that follow from the geometry's own shape - its type, its skin instance, its
		// renderer data and the *type* of its shader property. Those are exactly what the pointer
		// witnesses cover.
		//
		// Everything decided later is deliberately left out, because its inputs can change without any
		// witness noticing. AlphaBlend reads `materialAlpha`, which is animated and lives behind an
		// unchanged material pointer; the reasons DeriveLightingDescriptors produces (Decal, Technique,
		// ProjectedUV, Lod, Fading) read property *flags*, which Community Shaders' own features set in
		// place behind an unchanged property pointer. Caching those would trade a real correctness
		// surface for the cheapest third of the population.
		switch (a_reason) {
		case Ineligible::NotTriShape:
		case Ineligible::Skinned:
		case Ineligible::NoRendererData:
		case Ineligible::NotLightingShader:
			return true;
		default:
			return false;
		}
	}

	Ineligible SceneStore::ClassifyStatic(RE::BSGeometry& a_geometry, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated,
		bool a_wantDerived, RE::BSLightingShaderProperty** a_castCache)
	{
		if (a_geometry.GetType().get() != RE::BSGeometry::Type::kTriShape)
			return Ineligible::NotTriShape;

		auto& data = a_geometry.GetGeometryRuntimeData();
		if (auto* skin = data.skinInstance.get()) {
			if (!SkinnedEnabled())
				return Ineligible::Skinned;
			// The skinned path (engine notes: skinning): a NiSkinInstance, or with CS_DCLF_SKIN_PARTITIONS a
			// BSDismemberSkinInstance or several partitions, and a palette the native shader could index (240
			// rows). Each partition the engine draws is one draw of that PARTITION's buffer, not the geometry's
			// rendererData, which for a skinned shape is a different TriShape; which partitions it draws is
			// SkinPartitionMask's, per frame.
			static const REL::Relocation<const RE::NiRTTI*> niSkinInstance{ RE::NiSkinInstance::Ni_RTTI };
			static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
			const bool dismember = skin->GetRTTI() == dismemberSkinInstance.get();
			if (skin->GetRTTI() != niSkinInstance.get() && !dismember)
				return Ineligible::SkinShape;
			auto* partition = skin->skinPartition.get();
			auto* skinData = skin->skinData.get();
			if (!partition || !skinData || partition->numPartitions == 0 || skinData->GetBoneCount() == 0 || skinData->GetBoneCount() * 3 > 240)
				return Ineligible::SkinShape;
			if ((partition->numPartitions > 1 || dismember) && !SkinPartitionsEnabled())
				return Ineligible::SkinShape;
			if (partition->numPartitions > kMaxSkinPartitions)
				return Ineligible::SkinShape;
			// The engine indexes the dismember flags by partition; a flag array of another length is not one it
			// could have drawn from either.
			if (dismember) {
				const auto& flags = static_cast<const RE::BSDismemberSkinInstance*>(skin)->GetRuntimeData();
				if (flags.partitions && static_cast<std::uint32_t>(flags.numPartitions) != partition->numPartitions)
					return Ineligible::SkinShape;
			}
			// Every partition is drawn with the first one's pipeline, so they share its vertex layout; a LOD
			// byte past 2 would index the next row of the engine's table.
			const auto& first = partition->partitions[0];
			for (std::uint32_t i = 0; i < partition->numPartitions; ++i) {
				const auto& p = partition->partitions[i];
				if (!p.buffData || !p.buffData->vertexBuffer || !p.buffData->indexBuffer)
					return Ineligible::NoRendererData;
				if (!first.buffData || std::bit_cast<std::uint64_t>(p.buffData->vertexDesc) != std::bit_cast<std::uint64_t>(first.buffData->vertexDesc) ||
					(p.pad42 & 0xFF) > 2)
					return Ineligible::SkinShape;
			}
		} else if (!data.rendererData || !data.rendererData->vertexBuffer || !data.rendererData->indexBuffer) {
			return Ineligible::NoRendererData;
		}

		// The caller may already know what this property is (see Tracked::castProperty); the cast is a walk
		// of dependent RTTI pointer loads, which is the part of this function that neither a memo nor the
		// reads BuildFrame makes later can remove.
		auto* property = a_castCache ? *a_castCache : netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get());
		if (!property)
			return Ineligible::NotLightingShader;

		// GetRenderPasses treats material alpha below one as transparent.
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
		if (material && material->materialAlpha < 1.0f)
			return Ineligible::AlphaBlend;

		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*property, a_geometry, a_accumulated, descriptors, a_wantDerived);
		if (a_descriptors) {
			if (reason == Ineligible::None) {
				*a_descriptors = descriptors;
			} else {
				// The rejection diagnostics have to survive the rejection. Copying the descriptors only on
				// success left rejectedTechnique at its default 0, and 0 is `none` - a SUPPORTED technique -
				// so the histogram read "none(0)=1337" and looked like a finding rather than an empty field.
				a_descriptors->rejectedTechnique = descriptors.rejectedTechnique;
			}
		}
		return reason;
	}

	bool SceneStore::ReadSwitch(const RE::NiSwitchNode& a_switch, SwitchState& a_out)
	{
		if (REL::Module::IsVR())
			return false;
		const auto* base = reinterpret_cast<const std::byte*>(&a_switch);
		a_out.flags = *reinterpret_cast<const std::uint16_t*>(base + 0x128);
		a_out.index = *reinterpret_cast<const std::int32_t*>(base + 0x12C);
		a_out.revID = *reinterpret_cast<const std::uint32_t*>(base + 0x134);
		// childRevID, an NiTPrimitiveArray at +0x138: its data pointer at +0x8, its capacity at +0x10.
		a_out.childRevID = *reinterpret_cast<const std::uint32_t* const*>(base + 0x140);
		a_out.childRevCapacity = *reinterpret_cast<const std::uint16_t*>(base + 0x148);
		return true;
	}

	bool SceneStore::SwitchSelects(const RE::NiSwitchNode& a_switch, const RE::NiAVObject* a_child)
	{
		// NiSwitchNode::OnVisible (AE 140d29700) culls children[index] and nothing else. A child that has
		// become the selected one since the last update pass is brought up to date there, in the cull
		// (childRevID[index] != revID), which is after this walk read its transforms: leave it native for
		// that frame.
		// Bounded by capacity, not size: a Gamebryo array is indexed by slot, and size counts the used ones.
		SwitchState state;
		if (!ReadSwitch(a_switch, state))
			return false;
		const auto& children = a_switch.GetChildren();
		if (state.index < 0 || static_cast<std::uint32_t>(state.index) >= children.capacity() || !state.childRevID ||
			static_cast<std::uint32_t>(state.index) >= state.childRevCapacity)
			return false;
		const auto index = static_cast<std::uint16_t>(state.index);
		return children[index].get() == a_child && state.childRevID[index] == state.revID;
	}

	Ineligible SceneStore::ClassifyFrame(const Tracked& a_tracked, const AccumulatedPass* a_accumulated) const
	{
		const bool underSwitch = a_tracked.parentReason == Ineligible::Switch;
		if (underSwitch && !SwitchNodesEnabled())
			return Ineligible::Switch;
		if (a_tracked.parentReason != Ineligible::None && !underSwitch)
			return a_tracked.parentReason;

		// App-culled or hidden anywhere between the leaf and its category node; part of an actor; under a
		// switch node that does not draw this branch.
		const bool actors = ActorsEnabled();
		const RE::NiAVObject* child = nullptr;
		for (const RE::NiAVObject* object = a_tracked.geometry.get(); object; child = object, object = object->parent) {
			if (IsHidden(object))
				return Ineligible::Hidden;
			if (object == a_tracked.categoryNode)
				break;
			if (underSwitch && child) {
				if (auto* switchNode = const_cast<RE::NiAVObject*>(object)->AsSwitchNode(); switchNode && !SwitchSelects(*switchNode, child))
					return Ineligible::Switch;
			}
			if (!actors)
				if (auto* ref = object->GetUserData(); ref && ref->IsActor())
					return Ineligible::Actor;
		}

		// Fading: as the pass was registered when there is one (the withholding decided on that same value),
		// else as the fade node stands now.
		auto* property = a_tracked.geometry->GetGeometryRuntimeData().shaderProperty.get();
		// Without the pass (the scene phase, for the shadow views) a fade is only the native loop's when fades
		// are not DCLF's at all; the shadow views skip faded casters themselves (ShadowReject::Faded).
		const bool fading = a_accumulated ? a_accumulated->fading :
		                                    !FadingEnabled() && property && property->fadeNode && property->fadeNode->GetRuntimeData().currentFade < 1.0f;
		if (fading)
			return Ineligible::Fading;

		return Ineligible::None;
	}

	const std::vector<std::uint32_t>& SceneStore::GetMaterialPatchedFloats() const
	{
		return MaterialSources::FramePSFloats();
	}

	const std::vector<std::uint32_t>& SceneStore::GetMaterialPatchedVSFloats() const
	{
		return MaterialSources::FrameVSFloats();
	}

	void SceneStore::RefreshFrameMaterials()
	{
		stats.frameMaterialSamples = 0;
		auto& evaluator = ConstantEvaluator::Get();
		if (tables.materials.empty() || !evaluator.HasLightingShader())
			return;
		// One record drawn this frame per signature is evaluated live; its frame-sourced components are the
		// same in every record of that signature (MaterialSources).
		ankerl::unordered_dense::map<std::uint32_t, MaterialRecord> live;
		for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot) {
			if (tables.materialLastUsed[slot] != frame)
				continue;
			const auto key = tables.materialSlotKey[slot];
			const std::uint32_t signature = MaterialSources::Signature(key.second);
			if (!key.first || live.contains(signature))
				continue;
			MaterialRecord record;
			if (evaluator.EvaluateMaterial(key.first, key.second, record)) {
				live.emplace(signature, record);
				++stats.frameMaterialSamples;
			}
		}
		for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot) {
			if (tables.materialLastUsed[slot] != frame)
				continue;
			const auto pass = tables.materialSlotKey[slot].second;
			const auto it = live.find(MaterialSources::Signature(pass));
			if (it == live.end())
				continue;
			// The floats are repacked by every build (GetMaterialPatchedFloats); a texture is part of the version.
			if (MaterialSources::ApplyFrameComponents(it->second, tables.materials[slot], pass))
				tables.materialVersion[slot] = ++materialVersions;
		}
	}

	void SceneStore::RefreshTextureTransforms()
	{
		for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot)
			if (tables.materialLastUsed[slot] == frame && tables.materialSlotKey[slot].first)
				MaterialSources::ApplyTextureTransform(tables.materialSlotKey[slot].first, tables.materials[slot]);
	}

	void SceneStore::ProcessMaterialWrites()
	{
		writtenMaterials.clear();
		const bool complete = MaterialSources::Drain(writtenMaterials);
		stats.materialWrites = static_cast<std::uint32_t>(writtenMaterials.size());
		stats.materialsRewritten = stats.materialsDropped = 0;
		if (complete && writtenMaterials.empty())
			return;
		if (!complete)
			logger::warn("[DCLF] material write queue overflowed: every material record is re-evaluated");
		auto& evaluator = ConstantEvaluator::Get();
		const bool canEvaluate = evaluator.HasLightingShader();
		auto written = [&](const RE::BSShaderMaterial* a_material) { return !complete || writtenMaterials.contains(a_material); };
		for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot) {
			if (tables.materialLastUsed[slot] == Tables::kSlotFree)
				continue;
			const auto key = tables.materialSlotKey[slot];
			if (!key.first || !written(key.first))
				continue;
			MaterialRecord live;
			if (tables.materialLastUsed[slot] == frame && canEvaluate && evaluator.EvaluateMaterial(key.first, key.second, live)) {
				if (!(live == tables.materials[slot])) {
					tables.materials[slot] = live;
					tables.materialVersion[slot] = ++materialVersions;
					++stats.materialsRewritten;
				}
				if (auto it = materialCache.find(key); it != materialCache.end())
					it->second.record = live;
				continue;
			}
			// Not drawn this frame (or not evaluable): dropped, so that its next use evaluates it afresh. An
			// object's cached derivation checks its slot is still allocated to the same key.
			materialIndex.erase(key);
			materialCache.erase(key);
			tables.materialSlotKey[slot] = { nullptr, 0u };
			tables.materialLastUsed[slot] = Tables::kSlotFree;
			tables.materialFree.push_back(slot);
			++stats.materialsDropped;
		}
		// Cache entries without a slot.
		std::erase_if(materialCache, [&](const auto& a_entry) {
			return written(a_entry.first.first) && !materialIndex.contains(a_entry.first);
		});
	}

	void SceneStore::RefreshFrameConstants()
	{
		RefreshFrameMaterials();
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		for (std::size_t i = 0; i < tables.pipelines.size() && i < tables.geometryTemplate.size(); ++i) {
			if (!tables.PipelineUsed(i, frame))
				continue;
			// The technique constants are the frame's (fog, settings, and the shadow mask's view), so a
			// pipeline slot that outlives the frame takes them fresh here, as it did when the pipeline
			// table was rebuilt every frame. Serving the slot's first evaluation instead was both a parity
			// regression and, at startup, a stale view pointer handed to the render graph.
			EvaluateTechnique(tables.pipelines[i].passDescriptor, tables.techniqueConstants[i]);
			auto* property = tables.geometryTemplate[i];
			const auto* templatePass = property ? FindLightingPass(property) : nullptr;
			if (!templatePass)
				continue;  // keep what BuildFrame evaluated rather than blanking it
			GeometryConstants constants;
			if (evaluator.EvaluateGeometry(*templatePass, tables.pipelines[i].passDescriptor, mainPassRenderFlags, constants)) {
				tables.geometryConstants[i] = constants;
				tables.geometryConstantsValid[i] = 1;
			}
		}

		// Per-object shading is resampled here too. Its inputs - the property's alpha, emissive colour and
		// multiplier, and the LOD fades GetRenderPasses leaves on the property - are animated: candle and
		// chandelier emissives flicker, and sampling them at EarlyPrepass instead of here put them far
		// enough from the draw that capture parity's 0.1% tolerance on EmitColor stopped covering the
		// difference. Everything else about an object is camera- and time-independent and stays in
		// BuildFrame.
		for (std::size_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
			const auto* geometry = tables.objectGeometry[o];
			auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			if (!property || (tables.objects[o].flags & kObjectNoBindings))
				continue;
			const std::uint32_t passDescriptor = tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor;
			LightingDescriptors descriptors;
			descriptors.pass = passDescriptor;
			descriptors.technique = (passDescriptor >> 24) & 0x3f;
			const auto& lighting = *static_cast<RE::BSLightingShaderProperty*>(property);
			descriptors.specularLODFade = lighting.specularLODFade;
			descriptors.envmapLODFade = lighting.envmapLODFade;
			tables.shading[o] = MakeShading(lighting, descriptors, mainPassRenderFlags, tables.emissiveMult[o]);
			if (tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend))
				RefreshObjectExtras(o, lighting, *geometry);
		}
	}

	namespace
	{
		float GlobalFloatAt(const REL::Relocation<std::uintptr_t>& a_at)
		{
			return *reinterpret_cast<const float*>(a_at.address());
		}
	}

	void SceneStore::RefreshObjectExtras(std::size_t a_object, const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry)
	{
		if (a_object >= tables.extraOffset.size() || tables.extraOffset[a_object] == kNoExtraRows)
			return;
		float* rows = &tables.extraRows[std::size_t(tables.extraOffset[a_object]) * 4];
		const auto& object = tables.objects[a_object];
		auto& state = globals::game::shadowState->GetRuntimeData();
		const auto eye = state.posAdjust.getEye();

		if (object.flags & kObjectLandBlend) {
			// BSLightingShader::SetupGeometry, techniques 8 and 19 (engine notes: per-object constants):
			// xy from the landscape material, zw a blend between two BSShaderManager::State positions by
			// a clock the same state holds, minus the geometry's world translation. Module-relative reads.
			static const REL::Relocation<std::uintptr_t> blendClock{ REL::Offset(0x2033080) };
			static const REL::Relocation<std::uintptr_t> blendClockStart{ REL::Offset(0x2033118) };
			static const REL::Relocation<std::uintptr_t> blendDuration{ REL::Offset(0x20330f8) };
			static const REL::Relocation<std::uintptr_t> blendRate{ REL::Offset(0x1ad2840) };
			static const REL::Relocation<std::uintptr_t> blendFromX{ REL::Offset(0x2033108) };
			static const REL::Relocation<std::uintptr_t> blendFromY{ REL::Offset(0x203310c) };
			static const REL::Relocation<std::uintptr_t> blendToX{ REL::Offset(0x2033110) };
			static const REL::Relocation<std::uintptr_t> blendToY{ REL::Offset(0x2033114) };
			float t = (GlobalFloatAt(blendClock) - GlobalFloatAt(blendClockStart)) * (GlobalFloatAt(blendRate) / GlobalFloatAt(blendDuration));
			if (t <= 0.0f)
				t = 0.0f;
			if (1.0f <= t)
				t = 1.0f;
			const float x = (GlobalFloatAt(blendToX) - GlobalFloatAt(blendFromX)) * t + GlobalFloatAt(blendFromX);
			const float y = (GlobalFloatAt(blendToY) - GlobalFloatAt(blendFromY)) * t + GlobalFloatAt(blendFromY);
			const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(a_property.material);
			float* land = rows + kExtraRowLandBlend * 4;
			land[0] = material ? material->landBlendParams.red : 0.0f;
			land[1] = material ? material->landBlendParams.green : 0.0f;
			land[2] = x - a_geometry.world.translate.x;
			land[3] = y - a_geometry.world.translate.y;
		}

		if (object.flags & kObjectProjectedUV) {
			// The texture matrix, as SetupGeometry builds it for a ProjectedUV pass (engine notes): the
			// projection is a fixed rotation about Z placed at posAdjust, converted with the engine's own
			// NiTransform-to-matrix routine (which subtracts posAdjust, so its translation is zero); for
			// every technique but Envmap it is multiplied onto the geometry's world matrix, converted the
			// same way and with posAdjust added back. The engine's two routines are called so that the
			// result is the native one to the bit, and this runs at Prepass so posAdjust is the main
			// camera's. TextureProj's rows are the product's columns.
			using ToMatrix = void (*)(float*, const RE::NiTransform*);
			using Multiply = void* (*)(float*, const float*, const float*);
			static const REL::Relocation<ToMatrix> toMatrix{ REL::Offset(0x14aaf10) };
			static const REL::Relocation<Multiply> multiply{ REL::Offset(0x153d3c8) };
			RE::NiTransform projection;
			projection.rotate.entry[0][0] = 0.0f;
			projection.rotate.entry[0][1] = 1.0f;
			projection.rotate.entry[0][2] = 0.0f;
			projection.rotate.entry[1][0] = -1.0f;
			projection.rotate.entry[1][1] = 0.0f;
			projection.rotate.entry[1][2] = 0.0f;
			projection.rotate.entry[2][0] = 0.0f;
			projection.rotate.entry[2][1] = 0.0f;
			projection.rotate.entry[2][2] = 1.0f;
			projection.translate = eye;
			projection.scale = 1.0f;
			float p[16], m[16];
			toMatrix(p, &projection);
			const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
			if (technique == 1) {
				std::memcpy(m, p, sizeof(m));
			} else {
				float w[16];
				toMatrix(w, &a_geometry.world);
				w[12] += eye.x;
				w[13] += eye.y;
				w[14] += eye.z;
				multiply(m, w, p);
			}
			float* proj = rows + kExtraRowTextureProj * 4;
			for (std::uint32_t r = 0; r < 3; ++r) {
				proj[r * 4 + 0] = m[0 + r];
				proj[r * 4 + 1] = m[4 + r];
				proj[r * 4 + 2] = m[8 + r];
				proj[r * 4 + 3] = m[12 + r];
			}
			// The pixel parameters (FUN_1414e00c0): the property's projectedUVParams folded by its w, its
			// projectedUVColor, and the two tiling globals with the projected-normals switch.
			static const REL::Relocation<std::uintptr_t> tilingDiffuse{ REL::Offset(0x2035560) };
			static const REL::Relocation<std::uintptr_t> tilingDetail{ REL::Offset(0x2035578) };
			static const REL::Relocation<std::uintptr_t> projectedNormals{ REL::Offset(0x2035518) };
			const auto& params = a_property.projectedUVParams;
			const auto& colour = a_property.projectedUVColor;
			float* out = rows + kExtraRowProjectedParams * 4;
			const float fade = 1.0f - params.alpha;
			out[0] = fade * params.red;
			out[1] = 0.0f;  // never written by the engine
			out[2] = params.blue;
			out[3] = fade * params.green + params.alpha;
			out[4] = colour.red;
			out[5] = colour.green;
			out[6] = colour.blue;
			out[7] = colour.alpha;
			out[8] = GlobalFloatAt(tilingDiffuse);
			out[9] = GlobalFloatAt(tilingDetail);
			out[10] = 0.0f;
			out[11] = *reinterpret_cast<const std::uint8_t*>(projectedNormals.address()) ? 1.0f : 0.0f;
		}
	}

	void SceneStore::NoteProjectedTextures()
	{
		auto& state = globals::game::shadowState->GetRuntimeData();
		ProjectedTextures seen{};
		for (std::size_t i = 0; i < ProjectedTextures::kSlots.size(); ++i) {
			seen.views[i] = reinterpret_cast<ID3D11ShaderResourceView*>(state.PSTexture[ProjectedTextures::kSlots[i]]);
			if (!seen.views[i])
				return;
		}
		seen.valid = true;
		projectedTextures = seen;
	}

	void SceneStore::LatchAccumulator()
	{
		auto* accumulator = *globals::game::currentAccumulator.get();
		if (!accumulator || accumulator == latchedAccumulator)
			return;
		if (latchedAccumulator) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				logger::warn("[DCLF] the main camera's accumulator changed ({} -> {}); the tables follow it",
					static_cast<const void*>(latchedAccumulator), static_cast<const void*>(accumulator));
			}
		}
		latchedAccumulator = accumulator;
	}

	bool SceneStore::RefreshMainBatchRenderers()
	{
		auto* accumulator = latchedAccumulator ? latchedAccumulator : *globals::game::currentAccumulator.get();
		auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
		if (!batch)
			return false;  // before the first latch, i.e. the first frame only
		mainBatchRenderers.clear();
		mainBatchRenderers.insert(batch);
		for (auto* group : batch->geometryGroups) {
			if (group && group->batchRenderer)
				mainBatchRenderers.insert(group->batchRenderer);
		}
		// Published for the registration hook, which runs before this and so uses the previous frame's
		// set. These pointers are stable across frames, and an empty set on the first frame simply means
		// nothing is withheld yet.
		PassCapture::Get().SetMainBatchRenderers(
			std::make_shared<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>>(mainBatchRenderers));
		return true;
	}

	void SceneStore::CollectAccumulatedPasses()
	{
		accumulatedPasses.clear();
		// The latched accumulator, not `currentAccumulator`. BuildFrame now runs at EarlyPrepass, before
		// the depth pass, where `currentAccumulator` is still null because nothing is being rendered yet -
		// but the accumulator has held its passes since the cull job finished, which `Main::Draw` does
		// before the shadow maps. Measured: 612 passes at EarlyPrepass, at the end of the depth pass and
		// at Prepass alike, against 0 from `currentAccumulator` at the first two.
		auto* accumulator = latchedAccumulator ? latchedAccumulator : *globals::game::currentAccumulator.get();
		auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
		if (!batch)
			return;  // before the first latch, i.e. the first frame only

		// BSBatchRenderer::renderPass holds PassGroup structs inline (the engine indexes it as
		// data + (pass + group * 6) * 8), not the PassGroup pointers CommonLib declares; each of the five
		// entries heads a list chained through passGroupNext. renderPassMap maps each group's technique
		// (what SetupTechnique receives) to its index; the engine reads it as buckets of
		// { key, value, next } at +0x48, bucket count at +0x2C (engine notes: batch renderer).
		struct MapEntry
		{
			std::uint32_t key;
			std::uint32_t value;
			const MapEntry* next;
		};
		auto addBatch = [&](const RE::BSBatchRenderer* a_batch) {
			const auto* base = reinterpret_cast<const std::uint8_t*>(a_batch);
			const auto* buckets = *reinterpret_cast<const MapEntry* const*>(base + 0x48);
			const std::uint32_t bucketCount = *reinterpret_cast<const std::uint32_t*>(base + 0x2c);
			const auto* groups = reinterpret_cast<const RE::BSBatchRenderer::PassGroup*>(a_batch->renderPass.data());
			const std::uint32_t groupCount = a_batch->renderPass.size();
			for (std::uint32_t b = 0; buckets && groups && b < bucketCount; ++b) {
				const auto& entry = buckets[b];
				if (!entry.next || entry.value >= groupCount)
					continue;  // empty bucket
				const std::uint32_t technique = entry.key;
				const auto& group = groups[entry.value];
				for (std::uint32_t subPass = 0; subPass < 5; ++subPass) {
					std::uint32_t chainIndex = 0;
					for (auto* pass = group.passes[subPass]; pass; pass = pass->passGroupNext, ++chainIndex) {
						if (pass->geometry && pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting)
							AddAccumulatedPass(pass->geometry, AccumulatedPass{ pass, DrawnPassDescriptor(PassDescriptorOf(technique), subPass), subPass,
																   pass->passEnum, pass->accumulationHint, chainIndex, PassCapture::FadingAtRegistration(pass), LodRowOf(*pass) });
					}
				}
			}
		};
		addBatch(batch);
		// Geometry groups sort their passes in batch renderers of their own.
		for (auto* group : batch->geometryGroups) {
			if (group && group->batchRenderer)
				addBatch(group->batchRenderer);
		}
	}

	void SceneStore::CompareCapturedPasses(bool a_compare)
	{
		auto& capture = PassCapture::Get();
		if (!capture.Installed())
			return;
		// Always drained, whether or not anything is compared: the capture buffer is fixed-capacity and a
		// frame that does not drain it overflows.
		const auto entries = capture.Drain();
		auto& captureStats = capture.MutableStats();
		captureStats.compared = captureStats.missing = captureStats.extra = captureStats.techniqueDiffers = captureStats.subPassDiffers = 0;

		// Only the main camera's registrations; the shadow cameras register into their own renderers.
		ankerl::unordered_dense::map<const RE::BSGeometry*, const PassCapture::Entry*> captured;
		for (const auto& entry : entries) {
			if (mainBatchRenderers.contains(entry.batch))
				captured.try_emplace(entry.geometry, &entry);
		}

		for (const auto& [geometry, accumulated] : a_compare ? accumulatedPasses : decltype(accumulatedPasses){}) {
			++captureStats.compared;
			const auto it = captured.find(geometry);
			if (it == captured.end()) {
				++captureStats.missing;
				continue;
			}
			if (DrawnPassDescriptor(PassDescriptorOf(it->second->technique), it->second->subPass) != accumulated.technique)
				++captureStats.techniqueDiffers;
			if (it->second->subPass != accumulated.subPass)
				++captureStats.subPassDiffers;
		}
		if (a_compare) {
			for (const auto& [geometry, entry] : captured) {
				if (!accumulatedPasses.contains(geometry))
					++captureStats.extra;
			}
		}

		// The tables are built from the capture rather than the accumulator walk once the two agree. The
		// walk stops working the moment a pass is withheld from the batch renderer, which is the whole
		// point of static ownership; the capture sees the registration regardless of what happens to it
		// afterwards. Falling back when the capture is empty keeps the first frame and any unexpected
		// path working.
		static const bool fromAccumulator = SwitchValue("CS_DCLF_PASS_SOURCE") == "accumulator";
		if (fromAccumulator || captured.empty())
			return;
		accumulatedPasses.clear();
		for (const auto& [geometry, entry] : captured) {
			// The hint is read off the pass now, on the render thread, while the pass is alive for the
			// frame. RegisterPass PREPENDS to its list (Ghidra: passGroupNext = head; head = pass), so the
			// engine draws a bucket in reverse registration order; the chain position is reversed here so
			// that an ascending sort on it is the draw order, as it is for the accumulator walk.
			AddAccumulatedPass(geometry,
				AccumulatedPass{ entry->pass, DrawnPassDescriptor(PassDescriptorOf(entry->technique), entry->subPass), entry->subPass, entry->passEnum,
					entry->pass ? static_cast<std::uint32_t>(entry->pass->accumulationHint) : 0u,
					0xFFFFFFu - static_cast<std::uint32_t>(std::min<std::size_t>(static_cast<std::size_t>(entry - entries.data()), 0xFFFFFFu)),
					entry->fading, entry->pass ? LodRowOf(*entry->pass) : 3u });
		}
	}

	void SceneStore::AddAccumulatedPass(const RE::BSGeometry* a_geometry, const AccumulatedPass& a_pass)
	{
		// One pass per object, the first registered - except that a hint-10 pass (a LOD cross-fade's copy of the
		// old level, the native loop's) never stands for an object that also has a pass of its own.
		const auto [it, inserted] = accumulatedPasses.try_emplace(a_geometry, a_pass);
		if (!inserted && it->second.hint == 10 && a_pass.hint != 10)
			it->second = a_pass;
	}

	const AccumulatedPass* SceneStore::FindAccumulatedPass(const RE::BSGeometry* a_geometry) const
	{
		auto it = accumulatedPasses.find(a_geometry);
		return it == accumulatedPasses.end() ? nullptr : &it->second;
	}

	/**
		 * @brief Attributes the time since the last call to one BuildPart.
		 *
		 * Inert unless CS_DCLF_PROFILE=1: with profiling off `parts` is null and Add() is a null test, so
		 * the loop makes no clock calls. That matters because the calls themselves were ~4000 a frame,
		 * about 0.1 ms, which is 6% of what BuildFrame was being measured at.
		 *
		 * Timing a loop from inside it perturbs what it measures, so the profiled and unprofiled totals
		 * are both reported and the difference is the instrument's own cost.
		 */
		struct PartTimer
		{
			std::array<double, static_cast<std::size_t>(BuildPart::Count)>* parts = nullptr;
			std::chrono::steady_clock::time_point last;

			explicit PartTimer(std::array<double, static_cast<std::size_t>(BuildPart::Count)>& a_parts)
			{
				if (SceneStore::ProfileEnabled()) {
					parts = &a_parts;
					last = std::chrono::steady_clock::now();
				}
			}

			void Add(BuildPart a_part)
			{
				if (!parts)
					return;
				const auto now = std::chrono::steady_clock::now();
				(*parts)[static_cast<std::size_t>(a_part)] += std::chrono::duration<double, std::milli>(now - last).count();
				last = now;
			}
	};

	bool SceneStore::MaterialCacheEnabled()
	{
		// Default ON. It is not merely parity-neutral, it is parity-*better* than evaluating every
		// material: because RefreshFrameMaterials resamples the frame's lighting floats at Prepass rather
		// than at EarlyPrepass, the cache fixes a pre-existing mismatch the uncached path has. Measured
		// over the same route, mismatched draws per report interval:
		//
		//                       cache off   cache on
		//   cell change (coc)      253239      20572
		//   `set gamehour` step    248670          0
		//   steady state                0          0
		static const std::string mode = SwitchValue("CS_DCLF_MATERIAL_CACHE");
		static const bool enabled = mode != "off";
		return enabled;
	}

	bool SceneStore::ProfileEnabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_PROFILE");
		return enabled;
	}

	bool SceneStore::EvaluateMaterialForSlot(const RE::BSShaderMaterial* a_material, std::uint32_t a_pass,
		bool a_cacheOn, bool a_probeAll, MaterialRecord& a_record)
	{
		// The cross-frame material cache.
		//
		// EvaluateMaterial is the single most expensive call in this loop: a heap allocation, a full
		// RendererShadowState memcpy, six ~1 KB ConstantBlock resets, 28 COM releases and the engine's real
		// SetupMaterial with every CS hook on it. A record is a function of the material's own fields, which
		// change only through a write event (MaterialSources; ProcessMaterialWrites drops or re-evaluates the
		// material then), and of frame-sourced components refreshed every frame (RefreshTextureTransforms,
		// RefreshFrameMaterials). So a cached record is served as it is. CS_DCLF_MATERIAL_CACHE=probe
		// re-evaluates every one anyway and compares: a difference outside the frame-sourced components is a
		// writer the events do not cover.
		const std::pair cacheKey{ a_material, a_pass };
		auto cached = materialCache.find(cacheKey);
		const bool canServe = a_cacheOn && cached != materialCache.end();
		if (canServe && !a_probeAll) {
			a_record = cached->second.record;
			++stats.materialsFromCache;
		} else {
			if (!ConstantEvaluator::Get().EvaluateMaterial(a_material, a_pass, a_record)) {
				// No shader instance yet (nothing drawn so far): stay native this frame.
				++stats.ineligible[static_cast<std::size_t>(Ineligible::NotLightingShader)];
				--stats.ineligible[static_cast<std::size_t>(Ineligible::None)];
				return false;
			}
			++stats.materialsEvaluated;
			if (canServe) {
				// The probe: the frame behaves as it would with the cache on, the live record is the yardstick.
				++stats.materialsValidated;
				MaterialRecord served = cached->second.record;
				MaterialSources::CopyFrameComponents(a_record, served, a_pass);
				if (!(served == a_record))
					NoteStaleMaterial(~0u, cacheKey, served, a_record);
				a_record = served;
			} else {
				auto& cacheEntry = materialCache[cacheKey];
				// The reference is what makes the key safe: BSShaderMaterial is intrusively ref-counted, so
				// holding one means a freed material cannot be mistaken for a new allocation at the same
				// address - which is the one way this cache could hand an object another material's state.
				if (!cacheEntry.material)
					cacheEntry.material.reset(const_cast<RE::BSShaderMaterial*>(a_material));
				cacheEntry.record = a_record;
			}
		}
		if (auto it = materialCache.find(cacheKey); it != materialCache.end())
			it->second.lastUsed = frame;
		return true;
	}

	void SceneStore::NoteStaleMaterial(std::uint32_t a_slot, const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key,
		const MaterialRecord& a_served, const MaterialRecord& a_live)
	{
		++stats.materialCacheStale;
		if (a_served.vs.floats != a_live.vs.floats)
			stats.materialDiffMask |= 1u << 0;
		if (a_served.ps.floats != a_live.ps.floats)
			stats.materialDiffMask |= 1u << 1;
		if (a_served.textures != a_live.textures)
			stats.materialDiffMask |= 1u << 2;
		if (a_served.addressModes != a_live.addressModes)
			stats.materialDiffMask |= 1u << 3;
		if (a_served.filterModes != a_live.filterModes)
			stats.materialDiffMask |= 1u << 4;
		if (a_served.textureWritten != a_live.textureWritten)
			stats.materialDiffMask |= 1u << 5;
		static std::uint32_t logged = 0;
		if (logged++ >= 16)
			return;
		std::string what;
		for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f) {
			if (a_served.vs.floats[f] != a_live.vs.floats[f])
				what += fmt::format(" vs[{}] {}->{}", f, a_served.vs.floats[f], a_live.vs.floats[f]);
			if (a_served.ps.floats[f] != a_live.ps.floats[f])
				what += fmt::format(" ps[{}] {}->{}", f, a_served.ps.floats[f], a_live.ps.floats[f]);
		}
		for (std::size_t t = 0; t < a_served.textures.size(); ++t)
			if (a_served.textures[t] != a_live.textures[t] || a_served.addressModes[t] != a_live.addressModes[t] || a_served.filterModes[t] != a_live.filterModes[t])
				what += fmt::format(" texture[{}] {}/{}/{} -> {}/{}/{}", t, static_cast<const void*>(a_served.textures[t]), a_served.addressModes[t],
					a_served.filterModes[t], static_cast<const void*>(a_live.textures[t]), a_live.addressModes[t], a_live.filterModes[t]);
		if (a_served.textureWritten != a_live.textureWritten)
			what += fmt::format(" written {:X}->{:X}", a_served.textureWritten, a_live.textureWritten);
		logger::warn("[DCLF] STALE material record{} (material {}, pass {:X}): a writer the material events do not cover:{}",
			a_slot == ~0u ? std::string() : fmt::format(" in slot {}", a_slot), fmt::ptr(a_key.first), a_key.second, what.substr(0, 600));
	}

	std::uint32_t SceneStore::AllocateGeometrySlot()
	{
		if (!tables.geometryFree.empty()) {
			const auto slot = tables.geometryFree.back();
			tables.geometryFree.pop_back();
			return slot;
		}
		tables.geometries.emplace_back();
		tables.geometryLastUsed.push_back(Tables::kSlotFree);
		tables.geometrySlotKey.push_back(nullptr);
		return static_cast<std::uint32_t>(tables.geometries.size() - 1);
	}

	std::uint32_t SceneStore::AllocatePipelineSlot()
	{
		if (!tables.pipelineFree.empty()) {
			const auto slot = tables.pipelineFree.back();
			tables.pipelineFree.pop_back();
			return slot;
		}
		tables.pipelines.emplace_back();
		tables.geometryConstants.emplace_back();
		tables.geometryConstantsValid.push_back(0);
		tables.geometryTemplate.push_back(nullptr);
		tables.geometryTemplateNative.push_back(0);
		tables.techniqueConstants.emplace_back();
		tables.permutations.emplace_back();
		tables.pipelineLastUsed.push_back(Tables::kSlotFree);
		return static_cast<std::uint32_t>(tables.pipelines.size() - 1);
	}

	std::uint32_t SceneStore::AllocateMaterialSlot()
	{
		if (!tables.materialFree.empty()) {
			const auto slot = tables.materialFree.back();
			tables.materialFree.pop_back();
			return slot;
		}
		tables.materials.emplace_back();
		tables.materialVersion.push_back(0);
		tables.materialLastUsed.push_back(Tables::kSlotFree);
		tables.materialSlotKey.emplace_back(nullptr, 0u);
		return static_cast<std::uint32_t>(tables.materials.size() - 1);
	}

	namespace
	{
		/**
		 * @brief Whether an ineligibility verdict is one the scene phase cannot reach on its own.
		 *
		 * The decal rule is the only one: the group a decal draws in comes from its accumulated pass's
		 * accumulation hint (LightingDescriptors, the engine's GetRenderPasses), which does not exist
		 * before the shadow maps. Rejecting on it in the scene phase would leave every decal the main
		 * pass draws without a record, so the record is built and the verdict is taken again, with the
		 * pass, by the accumulate phase.
		 */
		bool DeferredToAccumulate(Ineligible a_reason)
		{
			return a_reason == Ineligible::Decal;
		}
	}

	void SceneStore::BuildFrame(Phase a_phase)
	{
		if (a_phase == Phase::Scene)
			BuildScenePhase();
		else
			BuildAccumulatePhase();
	}

	/**
	 * @brief The scene half of the frame, from Main::Draw's early hook (else at BeforeShadowMaps).
	 *
	 * Everything that does not depend on the main camera's accumulator: the tracked walk, eligibility, the
	 * geometry slots and their buffer resolve, the transforms and bounds, the bone palettes, and one object
	 * record per eligible object. It runs before the shadow maps are drawn - the scene graph is final from
	 * Main::Draw on, and a shadow epoch reads these records. The early hook is ahead of the main camera's
	 * cull; what moves before BeforeShadowMaps is listed at DrawcallLimitFix::BeginSceneFrame.
	 *
	 * The records leave the accumulator's half unset: no pipeline, no material, kObjectNoBindings, and
	 * kObjectNativeVisible clear. BuildAccumulatePhase patches them in place by object index, which is
	 * fixed for the frame from here on.
	 */
	void SceneStore::BuildScenePhase()
	{
		// A walk still pending here was never joined (no consumer of the tables ran): it is dropped, and
		// there are never two.
		AbandonSceneJob();
		++frame;
		// Nothing is drawn while a load screen is up, and nothing here may touch the tracked geometry
		// either. The load frees the renderer data and the vertex and index buffers of the cell being
		// unloaded, while the NiPointers in `tracked` keep only the NiAVObjects alive; classifying those
		// entries reads freed BSGraphics::TriShape data and hands the render graph device addresses that no
		// longer belong to anything, which the GPU answers with VK_ERROR_DEVICE_LOST a few frames later.
		//
		// Before ProcessEvents stopped walking the scene graph across loads, this did not arise: the
		// category-node refresh pruned those entries as the cell's nodes vanished, so they never reached
		// this loop. Leaving the tables empty is both the safe and the obviously correct thing to draw
		// during a load screen.
		sceneBuilt = false;
		if (IsLoadingScreenUp()) {
			ResetSlotTables();
			InvalidateObjectIndices();
			accumulatedPasses.clear();
			stats.objects = 0;
			stats.nativeVisible = 0;
			stats.geometries = 0;
			stats.pipelines = 0;
			stats.materials = 0;
			skinnedLastFrame.clear();
			return;
		}
		PartTimer timer(stats.partMs);
		RefreshLodFadeSettings();
		auto& gpu = GpuResources::Get();
		gpu.BeginFrame(frame);
		const bool resolveBuffers = gpu.Enabled();
		frameResolveBuffers = resolveBuffers;
		if (resolveBuffers != graphWasActive) {
			logger::info("[DCLF] tables frame {}: the render graph is {} (slots alive {} geometries / {} pipelines / {} materials)", frame,
				resolveBuffers ? "active from this frame" : "inactive from this frame", stats.geometriesAlive, stats.pipelinesAlive, stats.materialsAlive);
			graphWasActive = resolveBuffers;
		}
		// Frame-globals that were being read per object. ShouldSuppress in particular is three terms, and
		// only one of them is per object - the interior test is the same answer for every object in the
		// frame. The depth-bias mode of each decal group is frame state (a console toggle and whether sun
		// shadows are off), read once here rather than per decal.
		frameInterior = Util::IsInterior();
		frameDecalBias = { 0u, DecalDepthBiasMode(1), DecalDepthBiasMode(2) };
		timer.Add(BuildPart::Walk);
		// The slot tables and their maps persist across frames; the sweep is what retires what is no
		// longer used (CS_DCLF_DERIVED_CACHE).
		SweepSlots();
		auto& evaluator = ConstantEvaluator::Get();
		ConstantEvaluator::ResetFrameAudits();
		if (!evaluator.HasLightingShader())
			FindLightingShader();

		geometryIndex.reserve(tracked.size());

		// The whole tracked set, in whatever order the map holds; the order is the object indices of the
		// frame, and BuildAccumulatePhase walks the same vector.
		order.clear();
		order.reserve(tracked.size());
		for (auto& [trackedGeometry, entry] : tracked)
			order.push_back({ trackedGeometry, &entry, nullptr });

		// CS_DCLF_ASYNC: the walk on the worker, from here to AfterShadowMaps - the engine's main cull and its
		// whole shadow-map pass. Nothing reads the per-object tables in between (ExecuteShadowView does not;
		// the shadow probe keeps the walk here), the tracked set changes only at Present, and the engine data
		// the walk reads is the data the render thread reads here (see BuildScenePhase for what the cull
		// writes). The shadow build queued behind it reads what it writes.
		if (AsyncJobEnabled("scene") && !ShadowProbe::Enabled()) {
			PrepareSceneJob();
			++sceneAsync.kicked;
			sceneJob = AsyncWorker::Get().Submit("scene", [this](std::stop_token) { sceneJobResult = SceneWalk(false); });
			return;
		}
		SceneWalk(true);
		skinnedLastFrame = skinnedObjects;
		stats.objects = static_cast<std::uint32_t>(tables.objects.size());
		sceneBuilt = true;
	}

	void SceneStore::BeginWalk()
	{
		tables.ClearFrame();
		InvalidateObjectIndices();
		stats.ineligible.fill(0);
		stats.ineligibleDrawn.fill(0);
		stats.techniqueRejects.fill(0);
		stats.propertyRejects.clear();
		stats.rejectedBlended = stats.rejectedOpaque = stats.rejectedOpaqueAlphaTest = 0;
		stats.shadowMaskPipelines = 0;
		stats.derivationChecked = stats.derivationDiffers = stats.derivationBits = stats.derivationNative = 0;
		stats.derivationRuntimeDiffers = stats.derivationRuntimeBits = 0;
		stats.derivationBitCounts.fill(0);
		stats.materialsEvaluated = stats.materialsSkipped = 0;
		stats.materialsUnchanged = stats.materialsChanged = stats.materialDiffMask = 0;
		stats.materialsFromCache = stats.materialsValidated = stats.materialCacheStale = 0;
		stats.templateUpgrades = stats.templateDefects = stats.pipelinesCulledOnly = 0;
		stats.nativeVisible = 0;
		stats.classifyHits = stats.classifyChecked = stats.classifyDiffers = stats.castResolved = 0;
		stats.derivedHits = stats.derivedChecked = stats.derivedDiffers = 0;
		stats.accumulatedWithoutRecord = 0;
		stats.decals = {};
		stats.skinned = stats.boneRows = 0;
		stats.projectedUV = stats.landBlend = 0;
		stats.shadowCasters = 0;
		stats.shadowRejects = {};
		decalOrder.clear();

		skinnedObjects.clear();

		// Every per-object container, not only three of them. The three that were left out reallocated
		// their way back up every frame, and objectIndex - cleared just above - rehashed its way up to
		// ~2900 entries in the Whiterun exterior, which the profile billed to `record`.
		tables.objects.reserve(tracked.size());
		tables.objectGeometry.reserve(tracked.size());
		tables.draws.reserve(tracked.size());
		tables.shading.reserve(tracked.size());
		tables.emissiveMult.reserve(tracked.size());
		tables.lights.reserve(tracked.size());
		tables.treeAnim.reserve(tracked.size());
		tables.skinPartitions.reserve(tracked.size());

	}

	SceneStore::WalkResult SceneStore::SceneWalk(bool a_renderThread)
	{
		WalkResult result;
		BeginWalk();
		PartTimer timer(stats.partMs);

		// CS_DCLF_CLASSIFY_CACHE=off|on|probe. `probe` uses the cached verdict and *also* recomputes it,
		// comparing the two; it is the gate, and it costs more than either path alone.
		static const std::string classifyCacheMode = SwitchValue("CS_DCLF_CLASSIFY_CACHE");
		static const bool classifyCache = classifyCacheMode != "off";
		static const bool classifyProbe = classifyCacheMode == "probe";
		// CS_DCLF_COVERAGE_PROBE=1: which shader the uncovered objects actually use.
		static const bool coverageProbe = SwitchEnabled("CS_DCLF_COVERAGE_PROBE");

		timer.Add(BuildPart::PassLookup);
		for (auto& [geometry, trackedEntry, unused] : order) {
			const auto& entry = *trackedEntry;
			// Per TRACKED object: for a rejected one this is its `continue` and the iteration itself.
			timer.Add(BuildPart::LoopTail);

			// Eligibility, and nothing else. This phase needs no lighting descriptors - the pipeline and
			// material belong to the accumulator's half - so the verdict is taken from Tracked's own
			// cache (refreshed every kCandidateRefreshFrames) rather than recomputed per object per
			// frame: that cache is what made the cull-only path cheap, and here it covers every object.
			//
			// A verdict that is stale in the "eligible" direction costs nothing: the accumulate phase
			// classifies again before it hands an object any bindings. A verdict stale the other way
			// would leave an accumulated object without a record, so that phase clears the cache for it
			// and counts it (stats.accumulatedWithoutRecord, the gate: 0 in steady state).
			Ineligible reason;
			if (trackedEntry->candidateFrame != 0 && frame - trackedEntry->candidateFrame < Tracked::kCandidateRefreshFrames) {
				reason = trackedEntry->candidateReason;
				// Under a switch node the verdict follows the switch's selection, which changes (a harvested
				// plant, a tree's variant) without anything the cache witnesses: the shadow views draw what
				// this phase admits, so it is taken again every frame. Static verdicts other than None stand.
				if (entry.parentReason == Ineligible::Switch && (reason == Ineligible::None || reason == Ineligible::Switch)) {
					reason = ClassifyFrame(entry);
					trackedEntry->candidateReason = reason;
				}
				++stats.ineligible[static_cast<std::size_t>(reason)];
				if (reason != Ineligible::None && !DeferredToAccumulate(reason))
					continue;
			} else {
				LightingDescriptors descriptors;
				auto& verdict = trackedEntry->verdict;
				auto& runtime = geometry->GetGeometryRuntimeData();
				auto* witnessProperty = runtime.shaderProperty.get();
				const auto* witnessMaterial = witnessProperty ? witnessProperty->material : nullptr;
				const std::uint8_t fadeState = FadeStateOf(witnessProperty);
				// Resolve the RTTI cast once per property pointer rather than once per frame.
				if (trackedEntry->castProperty != witnessProperty) {
					trackedEntry->castProperty = witnessProperty;
					trackedEntry->castResult = netimmerse_cast<RE::BSLightingShaderProperty*>(witnessProperty);
					trackedEntry->castRtti = witnessProperty ? witnessProperty->GetRTTI() : nullptr;
					++stats.castResolved;
				}
				RE::BSLightingShaderProperty* castCache = trackedEntry->castResult;
				const bool hit = classifyCache && verdict.cached && verdict.rendererData == runtime.rendererData &&
				                 verdict.property == witnessProperty && verdict.material == witnessMaterial &&
				                 verdict.fadeState == fadeState;
				if (hit && !classifyProbe) {
					reason = verdict.reason;
					++stats.classifyHits;
				} else {
					reason = ClassifyStatic(*geometry, &descriptors, nullptr, false, &castCache);
					if (hit) {
						// probe: the cache said one thing and the computation another, which is a defect.
						++stats.classifyChecked;
						if (reason != verdict.reason) {
							++stats.classifyDiffers;
							if (stats.classifyDiffers == 1)
								logger::warn("[DCLF] classify cache: '{}' is cached as {} but recomputes as {}",
									geometry->name.c_str() ? geometry->name.c_str() : "?",
									kIneligibleNames[static_cast<std::size_t>(verdict.reason)], kIneligibleNames[static_cast<std::size_t>(reason)]);
						}
						reason = verdict.reason;  // the cache is what the frame would have used
					} else if (classifyCache && CacheableVerdict(reason)) {
						verdict = { true, reason, runtime.rendererData, witnessProperty, witnessMaterial, fadeState };
					} else if (classifyCache) {
						verdict.cached = false;
					}
				}
				timer.Add(BuildPart::ClassifyStatic);
				if (reason == Ineligible::None)
					reason = ClassifyFrame(entry);
				timer.Add(BuildPart::ClassifyFrame);
				trackedEntry->candidateFrame = frame;
				trackedEntry->candidateReason = reason;
				++stats.ineligible[static_cast<std::size_t>(reason)];
				if (reason == Ineligible::Technique)
					++stats.techniqueRejects[descriptors.rejectedTechnique & 63];
				if (coverageProbe && reason == Ineligible::NotLightingShader) {
					++stats.propertyRejects[trackedEntry->castRtti];
					const auto* rejectedAlpha = geometry->GetGeometryRuntimeData().alphaProperty.get();
					if (rejectedAlpha && rejectedAlpha->GetAlphaBlending()) {
						++stats.rejectedBlended;
					} else {
						++stats.rejectedOpaque;
						if (rejectedAlpha && rejectedAlpha->GetAlphaTesting())
							++stats.rejectedOpaqueAlphaTest;
					}
				}
				if (reason != Ineligible::None && !DeferredToAccumulate(reason))
					continue;
			}

			auto& data = geometry->GetGeometryRuntimeData();
			// Geometry, shared between every object drawing the same TriShape. A skinned shape draws its
			// skin partitions' own buffers, one draw each (ClassifyStatic has checked every one).
			const auto* skinPartitions = data.skinInstance ? data.skinInstance->skinPartition.get() : nullptr;
			const RE::NiSkinPartition::Partition* skinPartition = skinPartitions ? &skinPartitions->partitions[0] : nullptr;
			// Which of them the engine draws: for the shadow views, from the fade node's LOD level as both of
			// its pass builders read it; the accumulate phase takes the main camera's from its pass. A skin
			// the engine draws no partition of is not drawn at all.
			std::uint32_t partitionMask = 0;
			if (skinPartitions) {
				partitionMask = SkinPartitionMask(*data.skinInstance, LodRowOf(*geometry, data.shaderProperty.get()));
				if (!partitionMask) {
					--stats.ineligible[static_cast<std::size_t>(reason)];
					++stats.ineligible[static_cast<std::size_t>(Ineligible::Hidden)];
					continue;
				}
			}
			auto* triShape = skinPartition ? skinPartition->buffData : data.rendererData;
			bool geometryMiss = false;
			const std::uint32_t geometrySlot = ResolveGeometrySlot(*geometry, triShape, skinPartition, timer, a_renderThread, geometryMiss);
			if (geometryMiss) {
				++result.geometryMisses;
				continue;
			}
			if (geometrySlot == Tables::kSlotFree) {
				--stats.ineligible[static_cast<std::size_t>(reason)];
				++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
				continue;
			}
			// The other partitions' slots, linked from the first so a draw can walk them. Every partition is
			// resolved and linked whatever this frame's mask, because the main camera's may differ.
			if (skinPartitions && skinPartitions->numPartitions > 1) {
				std::uint32_t previous = geometrySlot;
				bool unstable = false;
				for (std::uint32_t i = 1; i < skinPartitions->numPartitions && !geometryMiss && !unstable; ++i) {
					const auto& part = skinPartitions->partitions[i];
					const std::uint32_t slot = ResolveGeometrySlot(*geometry, part.buffData, &part, timer, a_renderThread, geometryMiss);
					if (geometryMiss)
						break;
					if (slot == Tables::kSlotFree) {
						unstable = true;
						break;
					}
					tables.geometries[previous].nextPartition = slot;
					previous = slot;
				}
				if (geometryMiss) {
					++result.geometryMisses;
					continue;
				}
				if (unstable) {
					--stats.ineligible[static_cast<std::size_t>(reason)];
					++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
					continue;
				}
				tables.geometries[previous].nextPartition = kNoPartition;
			}

			const auto objectId = static_cast<std::uint32_t>(tables.objects.size());
			ObjectRecord object{};
			// CS_DCLF_TRANSFORM_PROBE=skip: a COST BOUND, not a mode. It renders wrong - every object
			// collapses to the origin - and exists only to answer how much of `record` the two transform
			// stores are.
			static const bool skipTransforms = SwitchValue("CS_DCLF_TRANSFORM_PROBE") == "skip";
			if (!skipTransforms) {
				StoreTransform(geometry->world, object.world);
				StoreTransform(geometry->previousWorld, object.previousWorld);
			}
			object.boundCenter[0] = geometry->worldBound.center.x;
			object.boundCenter[1] = geometry->worldBound.center.y;
			object.boundCenter[2] = geometry->worldBound.center.z;
			object.boundRadius = geometry->worldBound.radius;
			object.geometryIndex = geometrySlot;
			// The accumulator's half is not known yet: no bindings, and not native-visible. Both are
			// patched by BuildAccumulatePhase, and nothing between the two phases reads them -
			// BuildDrawsCS rejects an object with kObjectNoBindings before it looks at the indices.
			object.materialIndex = 0;
			object.pipelineIndex = 0;
			object.flags = kObjectNoBindings;
			// Two-sidedness is the property's, not the pass's: the shadow epochs key their pipelines on it
			// before the accumulate phase has computed the static flags, and that phase derives the same bit.
			if (auto* sceneProperty = data.shaderProperty.get(); sceneProperty && sceneProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided))
				object.flags |= kObjectTwoSided;
			// Likewise the alpha test and its threshold: a shadow caster's alpha reference comes from the
			// object record (Utility.hlsl, DCLFAlphaTestRef), which is packed before the accumulate phase.
			if (const auto* sceneAlpha = data.alphaProperty.get(); sceneAlpha && sceneAlpha->GetAlphaTesting())
				object.flags |= kObjectAlphaTest | (static_cast<std::uint32_t>(sceneAlpha->alphaThreshold) << kObjectAlphaThresholdShift);

			// Skinning: the engine's own palette. Its per-frame update (AE FUN_140e4ff90) is what the bone
			// setter runs from the native draw this object no longer gets; it is idempotent within a frame
			// (frameID), copies the current palette to the previous one first, and writes three float4 rows
			// a bone in absolute world space - which is what the shader indexes, so the rows are copied as
			// they are and made eye-relative by the epoch, like World.
			std::uint32_t objectBoneOffset = 0, objectBoneRows = 0;
			if (auto* skin = data.skinInstance.get(); skin && SkinnedEnabled()) {
				timer.Add(BuildPart::Record);
				if (trackedEntry->skinUpdatedFrame != frame) {
					// The palette update is the render thread's: off it, a skin PrepareSceneJob did not update
					// ahead (a newly eligible skinned object) is a miss, and the join rebuilds inline.
					if (!a_renderThread) {
						++result.skinMisses;
						continue;
					}
					UpdateSkin(skin, geometry->world);
					trackedEntry->skinUpdatedFrame = frame;
				}
				skinnedObjects.push_back(geometry);
				const std::uint32_t rows = skin->numMatrices * 3;
				if (rows && skin->boneMatrices && skin->prevBoneMatrices && rows <= 240) {
					objectBoneOffset = static_cast<std::uint32_t>(tables.bones.size() / 4);
					objectBoneRows = rows;
					const auto* current = static_cast<const float*>(skin->boneMatrices);
					const auto* previous = static_cast<const float*>(skin->prevBoneMatrices);
					tables.bones.insert(tables.bones.end(), current, current + std::size_t(rows) * 4);
					tables.previousBones.insert(tables.previousBones.end(), previous, previous + std::size_t(rows) * 4);
					object.flags |= kObjectSkinned;
					++stats.skinned;
					stats.boneRows += rows;
				}
				timer.Add(BuildPart::Skinning);
			}
			tables.boneOffset.push_back(objectBoneOffset);
			tables.boneRows.push_back(objectBoneRows);
			// Whether the engine would draw this object into a shadow map, and with which Utility
			// technique. It belongs here and nowhere else: every shadow view is rendered between this
			// phase and the next, so a verdict taken later would arrive after the views that need it.
			const auto* shadowProperty = data.shaderProperty.get();
			const auto shadowReject = ShadowCasterReject(shadowProperty, geometry);
			++stats.shadowRejects[static_cast<std::size_t>(shadowReject) & 7];
			ID3D11ShaderResourceView* shadowDiffuse = nullptr;
			const RE::BSShaderMaterial* shadowMaterial = nullptr;
			if (shadowReject == ShadowReject::None) {
				++stats.shadowCasters;
				const std::uint32_t shadowTechnique = ShadowUtilityTechnique(shadowProperty, geometry);
				tables.shadowTechnique.push_back(shadowTechnique);
				// An alpha-tested caster samples its diffuse: what the shadow epoch's binding record names, read
				// here so the epoch's build never touches the property.
				if (shadowTechnique & 0x80) {
					if (const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(shadowProperty->material)) {
						shadowMaterial = material;
						auto* texture = material->diffuseTexture ? material->diffuseTexture->rendererTexture : nullptr;
						shadowDiffuse = texture ? texture->resourceView : nullptr;
						if (shadowDiffuse && tables.shadowTextureSeen.insert(shadowDiffuse).second)
							tables.shadowTextureSet.push_back(shadowDiffuse);
					}
				}
				const ShadowPipelineKey shadowKey{ shadowTechnique,
					shadowProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided) ? kRasterTwoSided : 0u,
					VertexLayoutOf(tables.geometries[geometrySlot].vertexDesc) };
				if (std::find(tables.shadowKeysUsed.begin(), tables.shadowKeysUsed.end(), shadowKey) == tables.shadowKeysUsed.end())
					tables.shadowKeysUsed.push_back(shadowKey);
			} else {
				object.flags |= kObjectNoShadow;
				tables.shadowTechnique.push_back(0);
			}
			tables.shadowReject.push_back(static_cast<std::uint8_t>(shadowReject));
			tables.shadowDiffuse.push_back(shadowDiffuse);
			tables.shadowMaterial.push_back(shadowMaterial);
			// The extras rows are allocated by the accumulate phase, which is where the descriptors that
			// decide whether an object needs them are derived.
			tables.extraOffset.push_back(kNoExtraRows);
			tables.objects.push_back(object);
			tables.objectGeometry.push_back(geometry);
			tables.shading.push_back(ObjectShading{});
			tables.emissiveMult.push_back(1.0f);
			tables.lights.push_back(ObjectLights{});
			tables.treeAnim.push_back(ObjectTreeAnim{});
			tables.skinPartitions.push_back(static_cast<std::uint8_t>(skinPartitions && skinPartitions->numPartitions > 1 ? partitionMask : 0));
			trackedEntry->objectStamp = objectStamp;
			trackedEntry->objectId = objectId;

			const auto& geometryRecord = tables.geometries[geometrySlot];
			DrawSequence draw{};
			draw.pipelineIndex = 0;
			draw.vertexBufferAddress = geometryRecord.vertexAddress;
			draw.vertexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(geometryRecord.vertexBytes, UINT32_MAX));
			draw.vertexStride = geometryRecord.vertexStride;
			draw.indexBufferAddress = geometryRecord.indexAddress;
			draw.indexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(geometryRecord.indexBytes, UINT32_MAX));
			draw.indexFormat = kIndexFormatR16;
			draw.indexCount = geometryRecord.indexCount;
			draw.instanceCount = 1;
			draw.firstIndex = geometryRecord.firstIndex;
			draw.vertexOffset = 0;
			draw.firstInstance = 0;
			tables.draws.push_back(draw);
			timer.Add(BuildPart::Record);
		}

		return result;
	}

	void SceneStore::PrepareSceneJob()
	{
		// The two things the walk does that only the render thread may: keep the geometry slots' buffer
		// references alive (GpuResources::Touch, which also finds an eviction) and run the engine's palette
		// update. Both are done here for what last frame used - in steady state exactly what this frame uses -
		// and the walk counts anything else as a miss.
		if (frameResolveBuffers) {
			auto& gpu = GpuResources::Get();
			geometryTouched.resize(tables.geometries.size(), 0);
			for (std::size_t slot = 0; slot < tables.geometries.size() && slot < tables.geometryLastUsed.size(); ++slot) {
				if (tables.geometryLastUsed[slot] != frame - 1)
					continue;
				const auto& record = tables.geometries[slot];
				if (gpu.Touch(record.vertexBuffer) && gpu.Touch(record.indexBuffer))
					geometryTouched[slot] = frame;
				else
					++sceneAsync.touchFailures;
			}
		}
		if (SkinnedEnabled()) {
			for (auto* geometry : skinnedLastFrame) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end())
					continue;  // detached at Present
				if (auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get()) {
					UpdateSkin(skin, geometry->world);
					it->second.skinUpdatedFrame = frame;
				}
			}
		}
	}

	void SceneStore::JoinScenePhase()
	{
		if (!sceneJob)
			return;
		auto& worker = AsyncWorker::Get();
		const auto start = std::chrono::steady_clock::now();
		// Unbounded in effect: the frame cannot go on without its tables, and a walk cannot be restarted for
		// less than it costs to finish it.
		const auto joined = worker.Wait(sceneJob, std::chrono::seconds(10));
		if (joined == AsyncWorker::WaitResult::Late)
			worker.Cancel(sceneJob);
		sceneJob = {};
		const double waitedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		sceneAsync.waitMs += waitedMs;
		sceneAsync.waitMaxMs = std::max(sceneAsync.waitMaxMs, waitedMs);
		const WalkResult walked = sceneJobResult;
		const bool usable = joined == AsyncWorker::WaitResult::Done && !walked.geometryMisses && !walked.skinMisses;
		const bool probe = usable && AsyncModeSetting() == AsyncMode::Probe;
		// A job queued behind the walk (the shadow build) reads the tables: it finishes before they are
		// rewritten, and a rebuild makes it stale (GetSceneRebuilds).
		if (!usable || probe)
			worker.WaitIdle();
		if (!usable) {
			if (joined != AsyncWorker::WaitResult::Done)
				++sceneAsync.failed;
			sceneAsync.geometryMisses += walked.geometryMisses;
			sceneAsync.skinMisses += walked.skinMisses;
			++sceneAsync.rebuilt;
			++sceneRebuilds;
			SceneWalk(true);
		} else {
			++sceneAsync.used;
			if (probe) {
				// The worker's arrays against the same walk on the render thread, now. A difference is either a
				// walk that is not a function of its inputs or engine data that changed during the shadow maps.
				const Tables worker_ = tables;
				const std::size_t workerObjects = tables.objects.size();
				SceneWalk(true);
				++sceneAsync.probeCompared;
				std::string difference;
				if (!SameSceneTables(worker_, tables, difference) || workerObjects != tables.objects.size()) {
					if (sceneAsync.probeDiffer++ == 0)
						logger::warn("[DCLF] async scene probe: the worker's walk differs from the render thread's: {}", difference.empty() ? "the object index" : difference);
				}
			}
		}
		skinnedLastFrame = skinnedObjects;
		stats.objects = static_cast<std::uint32_t>(tables.objects.size());
		sceneBuilt = true;
	}

	void SceneStore::AbandonSceneJob()
	{
		if (!sceneJob)
			return;
		auto& worker = AsyncWorker::Get();
		worker.Cancel(sceneJob);
		sceneJob = {};
		worker.WaitIdle();
		// The walk's partial output is nobody's frame.
		tables.ClearFrame();
		InvalidateObjectIndices();
		sceneBuilt = false;
	}

	std::string SceneStore::SceneAsyncReport()
	{
		auto& a = sceneAsync;
		if (!a.kicked)
			return {};
		auto text = fmt::format("[DCLF] async scene: {} walks kicked, {} used, {} rebuilt inline ({} geometry misses, {} skin misses, {} failed), {} slot touches failed ahead; join waited {:.3f}/{:.3f} ms; probe: {} compared, {} differ\n",
			a.kicked, a.used, a.rebuilt, a.geometryMisses, a.skinMisses, a.failed, a.touchFailures, a.waitMs / a.kicked, a.waitMaxMs, a.probeCompared, a.probeDiffer);
		a = {};
		return text;
	}

	/**
	 * @brief The geometry slot for a TriShape: found, refreshed in place, or newly resolved.
	 *
	 * @return the slot, or Tables::kSlotFree when the buffers cannot be made stable for the render graph.
	 */
	std::uint32_t SceneStore::ResolveGeometrySlot(RE::BSGeometry& a_geometry, const RE::BSGraphics::TriShape* a_triShape,
		const RE::NiSkinPartition::Partition* a_skinPartition, PartTimer& a_timer, bool a_renderThread, bool& a_miss)
	{
		if (!a_triShape)
			return Tables::kSlotFree;
		auto& gpu = GpuResources::Get();
		const bool resolveBuffers = frameResolveBuffers;
		auto geometryIt = geometryIndex.find(a_triShape);
		const bool newGeometry = geometryIt == geometryIndex.end();
		// First use of the slot this frame needs a Touch, unless PrepareSceneJob already touched it.
		const bool touchedAhead = !newGeometry && geometryIt->second < geometryTouched.size() && geometryTouched[geometryIt->second] == frame;
		const bool needsTouch = !newGeometry && resolveBuffers && tables.geometryLastUsed[geometryIt->second] != frame && !touchedAhead;
		if (!a_renderThread) {
			// The worker may neither resolve nor touch: anything that would need either is a miss.
			const auto& known = newGeometry ? GeometryRecord{} : tables.geometries[geometryIt->second];
			const bool changed = !newGeometry && ((resolveBuffers && known.vertexAddress == 0) ||
													 known.vertexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer) ||
													 known.indexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer));
			if (newGeometry || changed || needsTouch) {
				a_miss = true;
				return Tables::kSlotFree;
			}
		}
		// A slot found by address but describing other buffers is a TriShape reallocated at the same
		// address: it is resolved again into the same slot. A slot whose buffer references were
		// evicted (nothing touched them for kEvictFrames) is resolved again the same way.
		const bool staleGeometry = !newGeometry &&
		                           ((resolveBuffers && tables.geometries[geometryIt->second].vertexAddress == 0) ||
		                               tables.geometries[geometryIt->second].vertexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer) ||
		                               tables.geometries[geometryIt->second].indexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer) ||
		                               (needsTouch &&
		                                   (!gpu.Touch(tables.geometries[geometryIt->second].vertexBuffer) || !gpu.Touch(tables.geometries[geometryIt->second].indexBuffer))));
		if (staleGeometry)
			++stats.geometriesRefreshed;
		if (newGeometry || staleGeometry) {
			// The buffers are resolved once per TRISHAPE, not once per object.
			//
			// Skipping the call altogether is NOT safe and is why this is a move rather than a cache:
			// GpuResources holds a reference on each buffer so its address cannot be reused, and it
			// drops that reference when an entry goes kEvictFrames without a Resolve. Resolving per
			// TriShape still touches every entry DCLF depends on every frame, so nothing is evicted
			// out from under the tables.
			std::optional<GpuResources::Buffer> vertexBuffer;
			std::optional<GpuResources::Buffer> indexBuffer;
			if (resolveBuffers) {
				// The render graph reads the game's buffers in place; they must never move (GpuResources).
				vertexBuffer = gpu.Resolve(reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer));
				indexBuffer = gpu.Resolve(reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer));
				if (!vertexBuffer || !indexBuffer) {
					// Nothing was inserted, so the next object sharing this TriShape retries, exactly
					// as it did when every object resolved for itself. A stale slot is freed: its
					// record no longer describes anything.
					if (staleGeometry) {
						tables.geometryLastUsed[geometryIt->second] = Tables::kSlotFree;
						tables.geometrySlotKey[geometryIt->second] = nullptr;
						tables.geometryFree.push_back(geometryIt->second);
						geometryIndex.erase(geometryIt);
					}
					a_timer.Add(BuildPart::Resolve);
					return Tables::kSlotFree;
				}
			}
			const auto& shape = static_cast<RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData();
			GeometryRecord record;
			record.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer);
			record.indexBuffer = reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer);
			record.vertexDesc = std::bit_cast<std::uint64_t>(a_triShape->vertexDesc);
			// The stride the engine binds is the desc's low nibble in dwords (engine notes: vertex input).
			record.vertexStride = static_cast<std::uint32_t>(record.vertexDesc & 0xFu) * 4u;
			record.vertexCount = a_skinPartition ? a_skinPartition->vertices : shape.vertexCount;
			record.indexCount = static_cast<std::uint32_t>(a_skinPartition ? a_skinPartition->triangles : shape.triangleCount) * 3;
			record.firstIndex = 0;
			if (vertexBuffer && indexBuffer) {
				record.vertexAddress = vertexBuffer->address;
				record.vertexBytes = vertexBuffer->size;
				record.indexAddress = indexBuffer->address;
				record.indexBytes = indexBuffer->size;
			}
			const std::uint32_t slot = staleGeometry ? geometryIt->second : AllocateGeometrySlot();
			tables.geometries[slot] = record;
			tables.geometrySlotKey[slot] = a_triShape;
			if (!staleGeometry)
				geometryIt = geometryIndex.emplace(a_triShape, slot).first;
			a_timer.Add(BuildPart::Resolve);
		} else if (resolveBuffers && tables.geometryLastUsed[geometryIt->second] != frame) {
			// First use of the slot this frame: the Touch above already kept the references alive.
			a_timer.Add(BuildPart::DedupHit);
		}
		tables.geometryLastUsed[geometryIt->second] = frame;
		return geometryIt->second;
	}

	/**
	 * @brief The accumulator half of the frame, at EarlyPrepass.
	 *
	 * The main camera's passes are complete only once Main_RenderShadowMaps returns, so everything that
	 * depends on them is here: the capture drain, the pipeline and material slots, the per-frame lighting
	 * template, the shading and light lists, the decal order and kObjectNativeVisible. It patches the
	 * records BuildScenePhase appended, in place and by object index.
	 */
	void SceneStore::BuildAccumulatePhase()
	{
		if (!sceneBuilt) {
			// A load screen, or the feature installed mid-frame: nothing to patch.
			CompareCapturedPasses(false);
			return;
		}
		PartTimer timer(stats.partMs);
		std::uint32_t fadingThisFrame = 0;
		// The pass table is filled from the capture, which is the source that keeps working once passes
		// are withheld from the batch renderer. The accumulator walk is the cross-check.
		static const bool passParity = SwitchEnabled("CS_DCLF_PASS_PARITY");
		static const bool fromAccumulator = SwitchValue("CS_DCLF_PASS_SOURCE") == "accumulator";
		const bool haveAccumulator = RefreshMainBatchRenderers();
		if (fromAccumulator || passParity)
			CollectAccumulatedPasses();
		else
			accumulatedPasses.clear();
		CompareCapturedPasses(passParity || fromAccumulator);
		// The capture had nothing and the walk was skipped: take the walk after all, so the first frame
		// after a latch is not empty.
		if (accumulatedPasses.empty() && haveAccumulator && !fromAccumulator && !passParity)
			CollectAccumulatedPasses();
		timer.Add(BuildPart::Walk);

		auto& evaluator = ConstantEvaluator::Get();
		auto& gpu = GpuResources::Get();
		const bool resolveBuffers = frameResolveBuffers;
		const bool interior = frameInterior;
		const auto& decalBiasMode = frameDecalBias;
		const std::uint32_t biasWitness = decalBiasMode[1] | (decalBiasMode[2] << 8);
		const bool lightLimitFixLoaded = globals::features::lightLimitFix.loaded;
		// CS_DCLF_DERIVED_CACHE=off|on|probe: `probe` serves the cache and recomputes, comparing the two.
		static const std::string derivedCacheMode = SwitchValue("CS_DCLF_DERIVED_CACHE");
		static const bool derivedCache = derivedCacheMode != "off";
		static const bool derivedProbe = derivedCacheMode == "probe";
		// An object the engine did not accumulate cannot be drawn while the draws are gated on the
		// engine's visibility (CS_DCLF_CULL_INPUT=native, the default): it is a culling candidate and
		// nothing more, and its scene record already carries everything the culling reads.
		const bool drawCulledCandidates = Toggles::Get().Active().cullTracked;
		// CS_DCLF_TABLES=accumulated: bindings only for what the engine's accumulator kept, which is the
		// default in any case; it remains as the switch that also keeps culled candidates out of the
		// culling's input.
		static const bool accumulatedOnly = SwitchValue("CS_DCLF_TABLES") == "accumulated";
		const bool materialCacheOn = MaterialCacheEnabled();
		static const bool materialProbeAll = SwitchValue("CS_DCLF_MATERIAL_CACHE") == "probe";
		static const bool derivationStats = SwitchEnabled("CS_DCLF_DERIVE_PROBE");
		static const bool classifyProbe = SwitchValue("CS_DCLF_CLASSIFY_CACHE") == "probe";

		// What this phase has anything to do with. Off the =tracked culling input that is the engine's
		// accumulated passes alone - ~1,700 of the exterior's 10,000 tracked objects - so the phase does
		// not walk the tracked set a second time merely to find them; the scene phase has already given
		// every other object everything it gets this frame.
		accumulateOrder.clear();
		if (drawCulledCandidates && !accumulatedOnly) {
			accumulateOrder.reserve(order.size());
			for (const auto& entry : order)
				accumulateOrder.push_back({ entry.geometry, entry.tracked, FindAccumulatedPass(entry.geometry) });
		} else {
			accumulateOrder.reserve(accumulatedPasses.size());
			for (auto& [passGeometry, pass] : accumulatedPasses) {
				auto* mutableGeometry = const_cast<RE::BSGeometry*>(passGeometry);
				auto trackedIt = tracked.find(mutableGeometry);
				if (trackedIt != tracked.end())
					accumulateOrder.push_back({ mutableGeometry, &trackedIt->second, &pass });
			}
		}
		timer.Add(BuildPart::PassLookup);
		for (auto& [geometry, trackedEntry, accumulated] : accumulateOrder) {
			timer.Add(BuildPart::LoopTail);
			if (!accumulated && !(drawCulledCandidates && !accumulatedOnly))
				continue;
			if (trackedEntry->objectStamp != objectStamp) {
				// No record: the scene phase found it ineligible, which is where the histogram's "drawn"
				// column comes from. A verdict of None here means the record itself failed (an unstable
				// buffer) or the cached verdict was stale, so that one is cleared and reported.
				if (accumulated) {
					++stats.ineligibleDrawn[static_cast<std::size_t>(trackedEntry->candidateReason)];
					if (trackedEntry->candidateReason == Ineligible::None) {
						++stats.accumulatedWithoutRecord;
						trackedEntry->candidateFrame = 0;
					}
				}
				continue;
			}
			const std::uint32_t objectId = trackedEntry->objectId;
			auto& object = tables.objects[objectId];
			const std::uint32_t geometrySlot = object.geometryIndex;

			auto& data = geometry->GetGeometryRuntimeData();
			auto* property = data.shaderProperty.get();
			auto* witnessProperty = property;
			const auto* witnessMaterial = witnessProperty ? witnessProperty->material : nullptr;
			const std::uint8_t fadeState = FadeStateOf(witnessProperty);
			RE::BSLightingShaderProperty* castCache = trackedEntry->castProperty == witnessProperty ? trackedEntry->castResult : nullptr;
			const bool alphaBelowOne = witnessMaterial && static_cast<const RE::BSLightingShaderMaterialBase*>(witnessMaterial)->materialAlpha < 1.0f;

			// The positive derivation, cached (Tracked::Derived): for an accumulated object whose
			// witnesses all match and whose slots still carry the keys they were derived for, the
			// classification and the whole derived section are skipped.
			auto& derived = trackedEntry->derived;
			bool derivedHit = derivedCache && accumulated && derived.valid && derived.generation == tablesGeneration &&
			                  derived.geometrySlot == geometrySlot && derived.property == witnessProperty &&
			                  derived.material == witnessMaterial && derived.fadeState == fadeState && derived.technique == accumulated->technique &&
			                  derived.subPass == accumulated->subPass && derived.hint == accumulated->hint && derived.interior == interior &&
			                  derived.alphaBelowOne == alphaBelowOne && derived.biasWitness == biasWitness;
			if (derivedHit) {
				derivedHit = derived.pipelineSlot < tables.pipelines.size() && tables.pipelineLastUsed[derived.pipelineSlot] != Tables::kSlotFree &&
				             tables.pipelines[derived.pipelineSlot] == derived.key &&
				             derived.materialSlot < tables.materialSlotKey.size() && tables.materialLastUsed[derived.materialSlot] != Tables::kSlotFree &&
				             tables.materialSlotKey[derived.materialSlot] == std::pair{ derived.material, derived.descriptors.pass };
			}

			LightingDescriptors descriptors;
			Ineligible reason = Ineligible::None;
			if (derivedHit && !derivedProbe) {
				descriptors = derived.descriptors;
				++stats.derivedHits;
			} else {
				reason = ClassifyStatic(*geometry, &descriptors, accumulated, derivationStats, &castCache);
			}
			timer.Add(BuildPart::ClassifyStatic);
			// Per frame whether or not the derivation was cached: hidden, part of an actor and fading are
			// states of this frame, and the scene phase's verdict for them is up to
			// kCandidateRefreshFrames old. An object that has just been hidden must lose its bindings now,
			// or DCLF keeps drawing what the engine has stopped drawing.
			if (reason == Ineligible::None)
				reason = ClassifyFrame(*trackedEntry, accumulated);
			// The skin partitions the main camera draws, from its registered pass's LODMode rather than the fade
			// node the scene phase read for the shadow views. None is not drawn at all.
			if (reason == Ineligible::None && accumulated && data.skinInstance && data.skinInstance->skinPartition) {
				const std::uint32_t mask = SkinPartitionMask(*data.skinInstance, accumulated->lodRow);
				if (!mask)
					reason = Ineligible::Hidden;
				else if (objectId < tables.skinPartitions.size())
					tables.skinPartitions[objectId] = static_cast<std::uint8_t>(data.skinInstance->skinPartition->numPartitions > 1 ? mask : 0);
			}
			// A pass in an alpha-test list is drawn with DoAlphaTest whatever it was registered with
			// (DrawnPassDescriptor, applied where the passes are taken).
			// The histogram is the scene phase's, taken over the whole tracked set; where this phase -
			// which has the accumulated pass, and so the decal group - reaches a different verdict, the
			// object is moved between the buckets so the report reads as it did before the split.
			if (reason != trackedEntry->candidateReason) {
				--stats.ineligible[static_cast<std::size_t>(trackedEntry->candidateReason)];
				++stats.ineligible[static_cast<std::size_t>(reason)];
			}
			if (reason != Ineligible::None) {
				trackedEntry->accumulateReason = reason;
				trackedEntry->accumulateReasonFrame = frame;
				// Eligible for a record but not for bindings: it stays native this frame, which is what
				// its scene record already says (kObjectNoBindings, not native-visible).
				if (accumulated)
					++stats.ineligibleDrawn[static_cast<std::size_t>(reason)];
				derived.valid = false;
				continue;
			}
			timer.Add(BuildPart::ClassifyFrame);

			// Only computed when something will report them: this whole block exists to feed one log line.
			if (derivationStats && accumulated && descriptors.derivedPass != kNotDerived) {
				++stats.derivationChecked;
				const std::uint32_t differing = descriptors.derivedPass ^ descriptors.pass;
				if (const std::uint32_t bits = differing & ~kRuntimePassBits) {
					++stats.derivationDiffers;
					stats.derivationBits |= bits;
				}
				if (const std::uint32_t bits = differing & kRuntimePassBits) {
					++stats.derivationRuntimeDiffers;
					stats.derivationRuntimeBits |= bits;
				}
				for (std::uint32_t remaining = differing; remaining;) {
					const std::uint32_t bit = std::countr_zero(remaining);
					++stats.derivationBitCounts[bit];
					remaining &= remaining - 1;
				}
			} else if (derivationStats && descriptors.derivedPass == kNotDerived) {
				++stats.derivationNative;
			}
			timer.Add(BuildPart::Diagnostics);

			std::uint32_t pipelineSlot = Tables::kSlotFree, materialSlot = Tables::kSlotFree, staticFlags = 0;
			PipelineKey key{};
			if (derivedHit && !derivedProbe) {
				pipelineSlot = derived.pipelineSlot;
				materialSlot = derived.materialSlot;
				staticFlags = derived.staticFlags;
				key = derived.key;
				tables.materialLastUsed[materialSlot] = frame;
				// The per-frame template: a pipeline's template is always a property of an object of this
				// frame (the first to use the slot, upgraded to a native-visible one by the election), so a
				// persistent slot never points at a property the game has since freed. The constants are
				// evaluated from it at Prepass (RefreshFrameConstants).
				if (tables.pipelineLastUsed[pipelineSlot] != frame) {
					tables.pipelineLastUsed[pipelineSlot] = frame;
					tables.geometryTemplate[pipelineSlot] = property;
					tables.geometryTemplateNative[pipelineSlot] = accumulated ? 1 : 0;
				} else if (accumulated && !tables.geometryTemplateNative[pipelineSlot]) {
					tables.geometryTemplate[pipelineSlot] = property;
					tables.geometryTemplateNative[pipelineSlot] = 1;
					++stats.templateUpgrades;
				}
				timer.Add(BuildPart::DedupHit);
			} else {
				const bool twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
				const auto* alpha = data.alphaProperty.get();
				const bool alphaTest = alpha && alpha->GetAlphaTesting();

				// A decal's key carries the engine's fixed-function state indices as well (Records.h): the
				// depth-bias mode from the frame, blend and write modes from the alpha property (derived with
				// the descriptors). Zero for everything else, so an opaque key is exactly what it was.
				std::uint32_t rasterFlags = twoSided ? kRasterTwoSided : 0u;
				if (descriptors.decalGroup)
					rasterFlags |= PackDecalRasterFlags(descriptors.decalGroup, decalBiasMode[descriptors.decalGroup & 3], descriptors.decalBlendMode, descriptors.decalWriteMode);
				key = PipelineKey{ descriptors.vertex, descriptors.pixel, rasterFlags, descriptors.pass,
					VertexLayoutOf(tables.geometries[geometrySlot].vertexDesc) };
				auto pipelineIt = pipelineIndex.find(key);
				const bool newPipeline = pipelineIt == pipelineIndex.end();
				if (!newPipeline && tables.pipelineLastUsed[pipelineIt->second] != frame) {
					// The slot's first use this frame: this object's property is the template until the
					// election finds a native-visible one (see the cached path).
					tables.pipelineLastUsed[pipelineIt->second] = frame;
					tables.geometryTemplate[pipelineIt->second] = property;
					tables.geometryTemplateNative[pipelineIt->second] = accumulated ? 1 : 0;
				}
				if (newPipeline) {
					timer.Add(BuildPart::Dedup);
					const std::uint32_t slot = AllocatePipelineSlot();
					tables.pipelines[slot] = key;
					// Per-frame PerGeometry values for this pass descriptor, from any object's lighting pass
					// (it supplies the scene light list the engine reads the sun from).
					GeometryConstants constants{};
					const auto* templatePass = FindLightingPass(property);
					const bool valid = templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, mainPassRenderFlags, constants);
					tables.geometryConstants[slot] = constants;
					tables.geometryConstantsValid[slot] = valid ? 1 : 0;
					tables.geometryTemplate[slot] = property;
					tables.geometryTemplateNative[slot] = accumulated ? 1 : 0;

					TechniqueConstants technique;
					EvaluateTechnique(descriptors.pass, technique);
					stats.shadowMaskPipelines += technique.shadowMask ? 1 : 0;
					tables.techniqueConstants[slot] = technique;

					PipelinePermutation permutation;
					permutation.vertexShaderDescriptor = descriptors.rawVertex;
					permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
					permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
					// Extended Translucency disables its material model for opaque geometry.
					permutation.extraFeatureDescriptor = globals::features::extendedTranslucency.loaded ?
					                                         static_cast<std::uint32_t>(ExtendedTranslucency::MaterialModel::DescriptorDisabled)
					                                             << ExtendedTranslucency::ExtraFeatureDescriptorShift :
					                                         0u;
					tables.permutations[slot] = permutation;
					pipelineIt = pipelineIndex.emplace(key, slot).first;
					timer.Add(BuildPart::PipelineEval);
				} else if (accumulated && !tables.geometryTemplateNative[pipelineIt->second]) {
					// The election. This pipeline's per-frame lighting template belongs to an object the
					// engine culled, and here is one it kept: take the template over. An object the engine
					// kept is by definition in the lighting situation being drawn, so it is the correct
					// template, and this is the ordering guarantee stated as a rule about the objects rather
					// than as a rule about the order they are visited in.
					const auto slot = pipelineIt->second;
					GeometryConstants constants;
					const auto* templatePass = FindLightingPass(property);
					if (templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, mainPassRenderFlags, constants)) {
						tables.geometryConstants[slot] = constants;
						tables.geometryConstantsValid[slot] = 1;
					}
					tables.geometryTemplate[slot] = property;
					tables.geometryTemplateNative[slot] = 1;
					++stats.templateUpgrades;
					timer.Add(BuildPart::PipelineEval);
				}
				pipelineSlot = pipelineIt->second;
				tables.pipelineLastUsed[pipelineSlot] = frame;

				// Material state as the engine's SetupMaterial produces it for this pass descriptor.
				const auto* material = property->material;
				auto materialIt = materialIndex.find(std::pair{ material, descriptors.pass });
				if (materialIt == materialIndex.end()) {
					timer.Add(BuildPart::Dedup);
					MaterialRecord record;
					if (!EvaluateMaterialForSlot(material, descriptors.pass, materialCacheOn, materialProbeAll, record)) {
						// No shader instance yet (nothing drawn so far): stay native this frame.
						derived.valid = false;
						continue;
					}
					const std::uint32_t slot = AllocateMaterialSlot();
					tables.materials[slot] = record;
					tables.materialVersion[slot] = ++materialVersions;
					tables.materialSlotKey[slot] = std::pair{ material, descriptors.pass };
					materialIt = materialIndex.emplace(std::pair{ material, descriptors.pass }, slot).first;
					timer.Add(BuildPart::MaterialEval);
				}
				materialSlot = materialIt->second;
				tables.materialLastUsed[materialSlot] = frame;
				timer.Add(BuildPart::DedupHit);

				staticFlags = (alphaTest ? kObjectAlphaTest : 0u) | (twoSided ? kObjectTwoSided : 0u) |
				              (ExternalEmittance::ShouldSuppress(interior, property, geometry) ? kObjectSuppressExternalEmittance : 0u) |
				              (descriptors.technique == kTechniqueTreeAnim ? kObjectTreeAnim : 0u) |
				              (alphaTest ? static_cast<std::uint32_t>(alpha->alphaThreshold) << kObjectAlphaThresholdShift : 0u) |
				              (descriptors.decalGroup ? kObjectDecal | (descriptors.decalGroup << kObjectDecalGroupShift) : 0u);
				if (derivedCache && accumulated) {
					if (derivedHit && derivedProbe) {
						++stats.derivedChecked;
						const bool same = derived.pipelineSlot == pipelineSlot && derived.materialSlot == materialSlot &&
						                  derived.staticFlags == staticFlags && derived.key == key && derived.descriptors.vertex == descriptors.vertex &&
						                  derived.descriptors.pixel == descriptors.pixel && derived.descriptors.pass == descriptors.pass;
						if (!same && stats.derivedDiffers++ == 0)
							logger::warn("[DCLF] derived cache: '{}' differs on recompute (slots {}/{} vs {}/{}, flags {:X} vs {:X})",
								geometry->name.c_str() ? geometry->name.c_str() : "?", derived.pipelineSlot, derived.materialSlot,
								pipelineSlot, materialSlot, derived.staticFlags, staticFlags);
					}
					derived.valid = true;
					derived.generation = tablesGeneration;
					derived.triShape = tables.geometrySlotKey[geometrySlot];
					derived.property = witnessProperty;
					derived.material = witnessMaterial;
					derived.fadeState = fadeState;
					derived.interior = interior;
					derived.alphaBelowOne = alphaBelowOne;
					derived.technique = accumulated->technique;
					derived.subPass = accumulated->subPass;
					derived.hint = accumulated->hint;
					derived.biasWitness = biasWitness;
					derived.descriptors = descriptors;
					derived.staticFlags = staticFlags;
					derived.key = key;
					derived.geometrySlot = geometrySlot;
					derived.pipelineSlot = pipelineSlot;
					derived.materialSlot = materialSlot;
				} else {
					derived.valid = false;
				}
			}

			// The patch. Everything above decided what this object draws with; here it goes into the
			// record the scene phase appended, at the index that phase fixed.
			object.materialIndex = materialSlot;
			object.pipelineIndex = pipelineSlot;
			object.flags = (object.flags & (kObjectSkinned | kObjectNoShadow)) | staticFlags | (accumulated ? kObjectNativeVisible : 0u);
			tables.draws[objectId].pipelineIndex = pipelineSlot;
			float emissiveMult = 1.0f;
			tables.shading[objectId] = MakeShading(*static_cast<RE::BSLightingShaderProperty*>(property), descriptors, mainPassRenderFlags, emissiveMult);
			tables.emissiveMult[objectId] = emissiveMult;
			if (descriptors.pass & kPassAdditionalAlphaMask) {
				if (fadingThisFrame++ == 0)
					++stats.fadingFrames;
				++stats.fadingDrawn;
			}
			ObjectLights lights;
			if (lightLimitFixLoaded) {
				lights.roomIndex = globals::features::lightLimitFix.GetRoomIndex(geometry);
				if (accumulated)
					lights.shadowBitMask = LightLimitFix::GetShadowBitMask(accumulated->pass);
			}
			tables.lights[objectId] = lights;
			// Per object, and only for trees: everything else keeps the pipeline template's values.
			ObjectTreeAnim tree{};
			if (object.flags & kObjectTreeAnim)
				DeriveTreeAnim(*property, tree);
			tables.treeAnim[objectId] = tree;
			// Extras rows for the objects that need them; filled at Prepass (RefreshObjectExtras), where
			// the main camera's state is current for the projection matrix.
			const bool landBlend = descriptors.technique == 8 || descriptors.technique == 19;
			if ((descriptors.projectedUV || landBlend) && tables.extraOffset[objectId] == kNoExtraRows) {
				object.flags |= (descriptors.projectedUV ? kObjectProjectedUV : 0u) | (landBlend ? kObjectLandBlend : 0u);
				stats.projectedUV += descriptors.projectedUV ? 1 : 0;
				stats.landBlend += landBlend ? 1 : 0;
				tables.extraOffset[objectId] = static_cast<std::uint32_t>(tables.extraRows.size() / 4);
				tables.extraRows.resize(tables.extraRows.size() + std::size_t(kExtraRows) * 4, 0.0f);
			}
			stats.nativeVisible += accumulated ? 1 : 0;
			if (descriptors.decalGroup && accumulated) {
				// The engine's draw order for decals: the opaque group first, then within a group the
				// technique buckets in ascending order, each bucket's lists 0-4, each list's chain.
				++stats.decals[(descriptors.decalGroup - 1) & 1];
				decalOrder.push_back({ (std::uint64_t(descriptors.decalGroup) << 60) | (std::uint64_t(accumulated->technique & 0x3FFFFFFF) << 28) |
										   (std::uint64_t(accumulated->subPass & 7) << 24) | (accumulated->chainIndex & 0xFFFFFF),
					objectId });
			}
			timer.Add(BuildPart::Record);
			(void)classifyProbe;
			(void)resolveBuffers;
			(void)gpu;
		}

		CheckObjectSlots(frameResolveBuffers);
		static const bool slotProbe = SwitchValue("CS_DCLF_SLOT_PROBE") == "1";
		if (slotProbe)
			ProbeSlots(frameResolveBuffers);
		stats.geometries = stats.pipelines = 0;
		stats.geometriesAlive = stats.pipelinesAlive = stats.materialsAlive = 0;
		for (const auto used : tables.geometryLastUsed) {
			stats.geometries += used == frame ? 1u : 0u;
			stats.geometriesAlive += used != Tables::kSlotFree ? 1u : 0u;
		}
		for (const auto used : tables.pipelineLastUsed) {
			stats.pipelines += used == frame ? 1u : 0u;
			stats.pipelinesAlive += used != Tables::kSlotFree ? 1u : 0u;
		}
		// Decal draw order: sort the frame's decals by the engine's key and hand each its slot in its
		// group. Tens to a few hundred entries; the sort is the whole cost.
		tables.decalOrdinal.assign(tables.objects.size(), ~0u);
		tables.decalCount = {};
		if (!decalOrder.empty()) {
			std::sort(decalOrder.begin(), decalOrder.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
			for (const auto& entry : decalOrder) {
				const std::uint32_t group = static_cast<std::uint32_t>(entry.key >> 60) - 1;
				tables.decalOrdinal[entry.object] = tables.decalCount[group & 1]++;
			}
		}
		// The 4c gate, checked over the finished tables rather than asserted from the election: no
		// native-visible object may draw on a pipeline whose lighting template came from a culled one.
		stats.templateDefects = 0;
		stats.pipelinesCulledOnly = 0;
		for (std::size_t p = 0; p < tables.geometryTemplateNative.size(); ++p)
			stats.pipelinesCulledOnly += (tables.PipelineUsed(p, frame) && !tables.geometryTemplateNative[p]) ? 1u : 0u;
		if (stats.pipelinesCulledOnly) {
			for (const auto& object : tables.objects) {
				if (!(object.flags & kObjectNativeVisible) || (object.flags & kObjectNoBindings))
					continue;
				if (object.pipelineIndex < tables.geometryTemplateNative.size() && !tables.geometryTemplateNative[object.pipelineIndex])
					++stats.templateDefects;
			}
		}
		stats.materials = 0;
		for (const auto used : tables.materialLastUsed) {
			stats.materials += used == frame ? 1u : 0u;
			stats.materialsAlive += used != Tables::kSlotFree ? 1u : 0u;
		}
		// The material cache is evicted with the material slots (SweepSlots), which is the only path
		// that touches a slot's lastUsed on the cached path.
		stats.materialCacheEntries = static_cast<std::uint32_t>(materialCache.size());
		ProcessMaterialWrites();
		RefreshTextureTransforms();
		ValidateMaterialSlice();
	}

	void SceneStore::InvalidateVerdicts()
	{
		AbandonSceneJob();
		// Every cached negative, candidate verdict and positive derivation: they witness the object, not
		// the toggles, so a toggle that enters the classification leaves all of them stale at once.
		for (auto& [geometry, entry] : tracked) {
			entry.verdict.cached = false;
			entry.candidateFrame = 0;
			entry.derived.valid = false;
		}
		++tablesGeneration;
	}

	void SceneStore::ResetSlotTables()
	{
		AbandonSceneJob();
		tables.Clear();
		geometryTouched.clear();
		geometryIndex.clear();
		pipelineIndex.clear();
		materialIndex.clear();
		++tablesGeneration;
	}

	void SceneStore::CheckObjectSlots(bool a_resolveBuffers)
	{
		stats.slotViolations = 0;
		for (std::size_t o = 0; o < tables.objects.size(); ++o) {
			auto& object = tables.objects[o];
			if (object.flags & kObjectNoBindings)
				continue;
			const char* what = nullptr;
			if (object.geometryIndex >= tables.geometries.size() || tables.geometryLastUsed[object.geometryIndex] != frame)
				what = "geometry slot not of this frame";
			else if (a_resolveBuffers && (!tables.geometries[object.geometryIndex].vertexAddress || !tables.geometries[object.geometryIndex].indexAddress))
				what = "geometry slot unresolved";
			else if (object.pipelineIndex >= tables.pipelines.size() || tables.pipelineLastUsed[object.pipelineIndex] != frame)
				what = "pipeline slot not of this frame";
			else if (object.materialIndex >= tables.materials.size() || tables.materialLastUsed[object.materialIndex] != frame)
				what = "material slot not of this frame";
			else if (!tables.geometryTemplate[object.pipelineIndex])
				what = "pipeline without a template";
			if (!what)
				continue;
			if (stats.slotViolations++ == 0) {
				const auto* geometry = tables.objectGeometry[o];
				logger::error("[DCLF] slot check: object {} '{}' {} (geometry {} used {}, pipeline {} used {}, material {} used {}, frame {})",
					o, geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", what,
					object.geometryIndex, object.geometryIndex < tables.geometryLastUsed.size() ? tables.geometryLastUsed[object.geometryIndex] : ~0u,
					object.pipelineIndex, object.pipelineIndex < tables.pipelineLastUsed.size() ? tables.pipelineLastUsed[object.pipelineIndex] : ~0u,
					object.materialIndex, object.materialIndex < tables.materialLastUsed.size() ? tables.materialLastUsed[object.materialIndex] : ~0u, frame);
			}
			// Neutralised: nothing downstream may draw from slots that are not this frame's.
			object.flags = (object.flags & ~kObjectNativeVisible) | kObjectNoBindings;
			object.geometryIndex = object.pipelineIndex = object.materialIndex = 0;
		}
	}

	void SceneStore::ProbeSlots(bool a_resolveBuffers)
	{
		static std::uint32_t logged = 0;
		if (logged >= 40)
			return;
		auto& evaluator = ConstantEvaluator::Get();
		auto& gpu = GpuResources::Get();
		std::uint32_t materialDiffers = 0, geometryDiffers = 0, materialsProbed = 0, geometriesProbed = 0;
		std::string first;
		if (evaluator.HasLightingShader()) {
			for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot) {
				if (tables.materialLastUsed[slot] != frame)
					continue;
				const auto key = tables.materialSlotKey[slot];
				MaterialRecord live;
				if (!key.first || !evaluator.EvaluateMaterial(key.first, key.second, live))
					continue;
				++materialsProbed;
				const auto& served = tables.materials[slot];
				for (std::size_t t = 0; t < served.textures.size(); ++t) {
					if (served.textures[t] != live.textures[t]) {
						if (materialDiffers++ == 0)
							first = fmt::format("material slot {} (pass {:X}) texture[{}] {} -> {}", slot, key.second, t,
								static_cast<const void*>(served.textures[t]), static_cast<const void*>(live.textures[t]));
						break;
					}
				}
			}
		}
		if (a_resolveBuffers) {
			for (std::uint32_t slot = 0; slot < tables.geometries.size(); ++slot) {
				if (tables.geometryLastUsed[slot] != frame)
					continue;
				++geometriesProbed;
				const auto& record = tables.geometries[slot];
				const auto* triShape = tables.geometrySlotKey[slot];
				const auto vertex = gpu.Resolve(record.vertexBuffer);
				const auto index = gpu.Resolve(record.indexBuffer);
				const bool same = triShape && vertex && index && vertex->address == record.vertexAddress && index->address == record.indexAddress &&
				                  reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer) == record.vertexBuffer &&
				                  reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer) == record.indexBuffer;
				if (!same && geometryDiffers++ == 0 && first.empty())
					first = fmt::format("geometry slot {} vb {:#x} -> {:#x} ib {:#x} -> {:#x} (trishape {} buffers {}/{} vs {}/{})", slot,
						record.vertexAddress, vertex ? vertex->address : 0ull, record.indexAddress, index ? index->address : 0ull,
						static_cast<const void*>(triShape), static_cast<const void*>(record.vertexBuffer), static_cast<const void*>(record.indexBuffer),
						triShape ? static_cast<const void*>(triShape->vertexBuffer) : nullptr, triShape ? static_cast<const void*>(triShape->indexBuffer) : nullptr);
			}
		}
		if (materialDiffers || geometryDiffers || frame < 12) {
			++logged;
			logger::info("[DCLF] slot probe frame {}: {} of {} used materials differ, {} of {} used geometries differ{}{}", frame, materialDiffers, materialsProbed,
				geometryDiffers, geometriesProbed, first.empty() ? "" : "; first: ", first);
		}
	}

	void SceneStore::SweepSlots()
	{
		if ((frame % 16) != 0)
			return;
		auto sweep = [&](std::vector<std::uint32_t>& a_lastUsed, std::vector<std::uint32_t>& a_free, auto&& a_erase) {
			for (std::uint32_t slot = 0; slot < a_lastUsed.size(); ++slot) {
				if (a_lastUsed[slot] == Tables::kSlotFree || frame - a_lastUsed[slot] <= Tables::kSlotIdleFrames)
					continue;
				a_erase(slot);
				a_lastUsed[slot] = Tables::kSlotFree;
				a_free.push_back(slot);
				++stats.slotsSwept;
			}
		};
		sweep(tables.geometryLastUsed, tables.geometryFree, [&](std::uint32_t a_slot) {
			geometryIndex.erase(tables.geometrySlotKey[a_slot]);
			tables.geometrySlotKey[a_slot] = nullptr;
		});
		sweep(tables.pipelineLastUsed, tables.pipelineFree, [&](std::uint32_t a_slot) {
			pipelineIndex.erase(tables.pipelines[a_slot]);
			tables.geometryTemplate[a_slot] = nullptr;
			tables.geometryConstantsValid[a_slot] = 0;
		});
		sweep(tables.materialLastUsed, tables.materialFree, [&](std::uint32_t a_slot) {
			materialIndex.erase(tables.materialSlotKey[a_slot]);
			if (materialCache.erase(tables.materialSlotKey[a_slot]))
				++stats.materialCacheEvicted;
			tables.materialSlotKey[a_slot] = { nullptr, 0u };
		});
	}

	void SceneStore::ValidateMaterialSlice()
	{
		// The standing alarm: a few records drawn this frame, re-evaluated live and compared outside their
		// frame-sourced components. A difference is a material writer the events do not cover; it is
		// reported, not repaired, because repairing it here is what hid the missing events before.
		if (!MaterialCacheEnabled() || tables.materials.empty())
			return;
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		std::uint32_t looked = 0;
		for (std::uint32_t n = 0; n < kMaterialValidationsPerFrame * kMaterialValidationStride && looked < kMaterialValidationsPerFrame; ++n) {
			const std::uint32_t slot = materialValidationCursor++ % static_cast<std::uint32_t>(tables.materials.size());
			if (tables.materialLastUsed[slot] != frame)
				continue;
			++looked;
			const auto key = tables.materialSlotKey[slot];
			MaterialRecord live;
			if (!key.first || !evaluator.EvaluateMaterial(key.first, key.second, live))
				continue;
			++stats.materialsValidated;
			MaterialRecord served = tables.materials[slot];
			MaterialSources::CopyFrameComponents(live, served, key.second);
			if (!(served == live))
				NoteStaleMaterial(slot, key, served, live);
		}
	}

	std::int32_t SceneStore::FindObject(const RE::BSGeometry* a_geometry) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		return it == tracked.end() || it->second.objectStamp != objectStamp ? -1 : static_cast<std::int32_t>(it->second.objectId);
	}

	Ineligible SceneStore::Classify(RE::BSGeometry* a_geometry) const
	{
		auto it = tracked.find(a_geometry);
		if (it == tracked.end())
			return Ineligible::NotTriShape;
		const Ineligible reason = ClassifyStatic(*a_geometry, nullptr);
		return reason != Ineligible::None ? reason : ClassifyFrame(it->second);
	}

	bool SceneStore::GetTrackInfo(const RE::BSGeometry* a_geometry, std::uint32_t& a_frame, TrackSource& a_source) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return false;
		a_frame = it->second.trackedFrame;
		a_source = it->second.trackedBy;
		return true;
	}

	bool SceneStore::GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const
	{
		const auto it = categoryFound.find(a_node);
		if (it == categoryFound.end())
			return false;
		a_frame = it->second.first;
		a_cause = it->second.second;
		return true;
	}

	bool SceneStore::IsTracked(const RE::BSGeometry* a_geometry) const
	{
		return tracked.contains(const_cast<RE::BSGeometry*>(a_geometry));
	}

	Ineligible SceneStore::ReasonThisFrame(const RE::BSGeometry* a_geometry, bool* a_accumulate) const
	{
		if (a_accumulate)
			*a_accumulate = false;
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return Ineligible::None;
		if (it->second.accumulateReasonFrame == frame) {
			if (a_accumulate)
				*a_accumulate = true;
			return it->second.accumulateReason;
		}
		return it->second.candidateReason;
	}
}
