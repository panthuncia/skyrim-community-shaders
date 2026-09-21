#include "SceneStore.h"

#include "Switches.h"

#include "GpuResources.h"
#include "PassCapture.h"
#include "SceneTracker.h"
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
		constexpr std::array<std::uint32_t, 3> kDrawnCategories{ 3 /*Static*/, 4 /*Dynamic*/, 5 /*MultiBound*/ };
		constexpr std::uint32_t kMaxParentDepth = 64;
		constexpr std::size_t kValidationSlice = 256;
		constexpr std::uint32_t kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT
		constexpr std::uint32_t kSpecularBit = 0x200;  // pass descriptor Specular
		constexpr std::uint32_t kTechniqueEnvmap = 1;
		constexpr std::uint32_t kDoAlphaTestBit = 1u << 20;  // pass descriptor DoAlphaTest

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
	}

	void SceneStore::Tables::Clear()
	{
		objects.clear();
		objectGeometry.clear();
		geometries.clear();
		pipelines.clear();
		materials.clear();
		shading.clear();
		emissiveMult.clear();
		lights.clear();
		geometryConstants.clear();
		geometryConstantsValid.clear();
		geometryTemplate.clear();
		geometryTemplateNative.clear();
		techniqueConstants.clear();
		permutations.clear();
		draws.clear();
	}

	SceneStore& SceneStore::Get()
	{
		static SceneStore store;
		return store;
	}

	void SceneStore::Clear()
	{
		tracked.clear();
		categoryNodes.clear();
		tables.Clear();
		objectIndex.clear();
		// They hold raw pointers into game allocations now that they outlive the frame, so the teardown
		// paths have to drop them rather than leave them to the next BuildFrame.
		geometryIndex.clear();
		pipelineIndex.clear();
		materialIndex.clear();
		materialCache.clear();
		materialPatched.clear();
		materialPatchValues.clear();
		materialPatchValuesFresh = false;
		materialPatchSource.reset();
		validationCursor = 0;
	}

	RE::NiNode* SceneStore::FindCategoryNode(RE::NiAVObject* a_object, bool* a_unsupportedParent) const
	{
		bool unsupported = false;
		RE::NiNode* node = a_object ? a_object->parent : nullptr;
		for (std::uint32_t depth = 0; node && depth < kMaxParentDepth; ++depth, node = node->parent) {
			if (categoryNodes.contains(node)) {
				if (a_unsupportedParent)
					*a_unsupportedParent = unsupported;
				return node;
			}
			// A switch node draws one child at a time and an ordered node depends on draw order;
			// neither survives being drawn out of the native loop.
			if (netimmerse_cast<RE::NiSwitchNode*>(node) || netimmerse_cast<RE::BSOrderedNode*>(node))
				unsupported = true;
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
		for (auto* node : added) {
			for (auto& child : node->GetChildren()) {
				if (child)
					AddSubtree(child.get());
			}
		}
	}

	void SceneStore::AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, bool a_unsupportedParent)
	{
		auto& entry = tracked[a_geometry];
		entry.geometry.reset(a_geometry);
		entry.categoryNode = a_categoryNode;
		entry.unsupportedParent = a_unsupportedParent;
	}

	void SceneStore::AddSubtree(RE::NiAVObject* a_root)
	{
		// The attach event is drained a frame or more after it was queued, so its node can already be gone
		// (a cell transition releases the subtree). Walking it then dereferences null.
		if (!a_root)
			return;
		bool unsupportedAbove = false;
		RE::NiNode* category = FindCategoryNode(a_root, &unsupportedAbove);
		if (!category)
			return;

		// Walk down, carrying whether a switch/ordered node lies between the category node and the leaf.
		std::vector<std::pair<RE::NiAVObject*, bool>> stack;
		stack.emplace_back(a_root, unsupportedAbove);
		while (!stack.empty()) {
			auto [object, unsupported] = stack.back();
			stack.pop_back();
			if (!object)
				continue;
			if (auto* geometry = object->AsGeometry()) {
				AddGeometry(geometry, category, unsupported);
				continue;
			}
			if (auto* node = object->AsNode()) {
				const bool below = unsupported || netimmerse_cast<RE::NiSwitchNode*>(node) || netimmerse_cast<RE::BSOrderedNode*>(node);
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
			bool unsupported = false;
			if (FindCategoryNode(it->first, &unsupported) != it->second.categoryNode)
				stale.push_back(it->first);
			else
				it->second.unsupportedParent = unsupported;
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
		RefreshCategoryNodes(sawDetach || rescanned);

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
		if (data.skinInstance)
			return Ineligible::Skinned;
		if (!data.rendererData || !data.rendererData->vertexBuffer || !data.rendererData->indexBuffer)
			return Ineligible::NoRendererData;

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
		if (reason == Ineligible::None && a_descriptors)
			*a_descriptors = descriptors;
		return reason;
	}

	Ineligible SceneStore::ClassifyFrame(const Tracked& a_tracked) const
	{
		if (a_tracked.unsupportedParent)
			return Ineligible::UnsupportedParent;

		// App-culled or hidden anywhere between the leaf and its category node; part of an actor.
		for (const RE::NiAVObject* object = a_tracked.geometry.get(); object; object = object->parent) {
			if (IsHidden(object))
				return Ineligible::Hidden;
			if (object == a_tracked.categoryNode)
				break;
			if (auto* ref = object->GetUserData(); ref && ref->IsActor())
				return Ineligible::Actor;
		}

		auto* property = a_tracked.geometry->GetGeometryRuntimeData().shaderProperty.get();
		if (property && property->fadeNode && property->fadeNode->GetRuntimeData().currentFade < 1.0f)
			return Ineligible::Fading;

		return Ineligible::None;
	}

	void SceneStore::RefreshMaterialPatch()
	{
		if (!MaterialCacheEnabled() || materialPatched.empty() || !materialPatchSource || tables.materials.empty())
			return;
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		MaterialRecord live;
		if (!evaluator.EvaluateMaterial(materialPatchSource.get(), materialPatchSourcePass, live))
			return;
		// The patched positions are shader-level, so one sample answers for every record. If that ever
		// stops being true the rolling validator says so, because it compares a served record against a
		// live evaluation of that material.
		for (std::size_t i = 0; i < materialPatched.size(); ++i) {
			const std::uint32_t index = materialPatched[i];
			const float value = live.ps.floats[index];
			materialPatchValues[i] = value;
			for (auto& record : tables.materials)
				record.ps.floats[index] = value;
		}
		++stats.materialPatchResamples;
	}

	void SceneStore::RefreshFrameConstants()
	{
		RefreshMaterialPatch();
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		for (std::size_t i = 0; i < tables.pipelines.size() && i < tables.geometryTemplate.size(); ++i) {
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
		}
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
					for (auto* pass = group.passes[subPass]; pass; pass = pass->passGroupNext) {
						if (pass->geometry && pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting)
							accumulatedPasses.try_emplace(pass->geometry, AccumulatedPass{ pass, PassDescriptorOf(technique), subPass, pass->passEnum });
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
			if (PassDescriptorOf(it->second->technique) != accumulated.technique)
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
			accumulatedPasses.try_emplace(geometry,
				AccumulatedPass{ entry->pass, PassDescriptorOf(entry->technique), entry->subPass, entry->passEnum });
		}
	}

	const AccumulatedPass* SceneStore::FindAccumulatedPass(const RE::BSGeometry* a_geometry) const
	{
		auto it = accumulatedPasses.find(a_geometry);
		return it == accumulatedPasses.end() ? nullptr : &it->second;
	}

	namespace
	{
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
	}

	bool SceneStore::MaterialCacheEnabled()
	{
		// Default ON. It is not merely parity-neutral, it is parity-*better* than evaluating every
		// material: because RefreshMaterialPatch resamples the frame's lighting floats at Prepass rather
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

	void SceneStore::NotePatchedFloat(std::uint32_t a_index)
	{
		if (std::find(materialPatched.begin(), materialPatched.end(), a_index) != materialPatched.end())
			return;
		materialPatched.push_back(a_index);
		materialPatchValues.push_back(0.0f);
	}

	bool SceneStore::ProfileEnabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_PROFILE");
		return enabled;
	}

	void SceneStore::BuildFrame()
	{
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
		if (IsLoadingScreenUp()) {
			tables.Clear();
			objectIndex.clear();
			accumulatedPasses.clear();
			stats.objects = 0;
			stats.nativeVisible = 0;
			stats.geometries = 0;
			stats.pipelines = 0;
			stats.materials = 0;
			return;
		}
		PartTimer timer(stats.partMs);
		RefreshLodFadeSettings();
		// The pass table is filled from the capture, which is the source that keeps working once passes
		// are withheld from the batch renderer. The accumulator walk is the cross-check, and it used to
		// run every frame: the walk filled the table, the comparison built a second map of the same size,
		// and then the table was discarded and refilled from it - three ~600-entry maps a frame to end up
		// with the capture's answer. Now the walk runs only for the comparison, or as the first-frame
		// fallback when the capture has nothing yet.
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
		auto& gpu = GpuResources::Get();
		gpu.BeginFrame(frame);
		const bool resolveBuffers = gpu.Enabled();
		timer.Add(BuildPart::Walk);
		tables.Clear();
		objectIndex.clear();
		stats.ineligible.fill(0);
		stats.shadowMaskPipelines = 0;
		stats.derivationChecked = stats.derivationDiffers = stats.derivationBits = stats.derivationNative = 0;
		stats.derivationRuntimeDiffers = stats.derivationRuntimeBits = 0;
		stats.derivationBitCounts.fill(0);
		stats.materialsEvaluated = stats.materialsSkipped = 0;
		stats.materialsUnchanged = stats.materialsChanged = stats.materialDiffMask = 0;
		stats.materialsFromCache = stats.materialsValidated = stats.materialCacheStale = 0;
		// Relearned every frame: the whole point of the drift is that it moves, and a stale set of
		// positions patched into a served record is exactly the defect this cache could produce.
		materialPatchValuesFresh = false;
		stats.templateUpgrades = stats.templateDefects = stats.pipelinesCulledOnly = 0;
		stats.nativeVisible = 0;  // counted as the objects are built, not in a second pass over the table
		stats.classifyHits = stats.classifyChecked = stats.classifyDiffers = stats.castResolved = 0;

		// Members: cleared rather than constructed, so the buckets are reused instead of being allocated
		// and freed every frame.
		geometryIndex.clear();
		pipelineIndex.clear();
		materialIndex.clear();
		auto& evaluator = ConstantEvaluator::Get();
		ConstantEvaluator::ResetFrameAudits();
		if (!evaluator.HasLightingShader())
			FindLightingShader();

		// Every per-object container, not only three of them. The three that were left out reallocated
		// their way back up every frame, and objectIndex - cleared just above - rehashed its way up to
		// ~2900 entries in the Whiterun exterior, which the profile billed to `record`.
		tables.objects.reserve(tracked.size());
		tables.objectGeometry.reserve(tracked.size());
		tables.draws.reserve(tracked.size());
		tables.shading.reserve(tracked.size());
		tables.emissiveMult.reserve(tracked.size());
		tables.lights.reserve(tracked.size());
		objectIndex.reserve(tracked.size());
		geometryIndex.reserve(tracked.size());

		// The whole tracked set is classified, not only what the main-camera accumulator holds. The
		// accumulator has already run the engine's culling, so building from it left the GPU culling nothing
		// to reject and no way to be measured; Phase 5 needs the whole set in any case, once objects stop
		// going into the accumulator at all. An object the accumulator does not hold is still a candidate
		// here: it carries kObjectNativeVisible unset, and whether it is actually drawn is BuildDrawsCS's
		// decision (RequireNativeVisible), not this loop's.
		//
		// Objects outside the accumulator have no pass to read the per-frame bits from, so their descriptors
		// come from the property derivation alone, with its guesses for kRuntimePassBits and no shadow bit
		// mask. That is exactly the derivation Phase 5 has to stand on, and the derivation counters below
		// measure it against the accumulated objects every frame.
		// CS_DCLF_TABLES=accumulated restores the pre-Phase-4 behaviour, where the tables held only what the
		// engine's accumulator kept. It exists to tell a defect in the culling apart from one caused merely
		// by classifying and evaluating three times as many objects, which is a different kind of change:
		// the stand-in evaluation runs the engine's own SetupMaterial and SetupGeometry.
		static const bool accumulatedOnly = SwitchValue("CS_DCLF_TABLES") == "accumulated";

		// One pass over the tracked set, in whatever order the map holds.
		//
		// It used to be two: engine-kept objects into `order`, the rest into `culled`, then `culled`
		// appended - up to a ~6000-entry memcpy a frame - purely so that an engine-kept object would
		// always reach a pipeline first and fix its per-frame lighting constants. That guarantee is now
		// made explicitly by the template election below, which does not depend on iteration order and
		// therefore survives a table that persists across frames.
		//
		// Several of the tables are per-pipeline rather than per-object, and the object that *creates* a
		// pipeline decides their contents for every object that shares it - in particular
		// tables.geometryConstants, which comes from FindLightingPass(property) of whichever object got
		// there first and carries that object's scene light list. While the tables held only the
		// accumulator's output that was harmless, because every candidate was visible and in the same
		// lighting situation. Widening them to the whole tracked set made it a defect: a candidate the
		// engine culled - in another room, or unlit - would often create the pipeline and hand its lights
		// to the visible objects drawn on it, which is the blown-out interior lighting this produced.
		//
		// The election below fixes it at the source, and it is not a tie-break hack: an object the engine
		// kept is by definition in the lighting situation being drawn, so it is the correct template, and
		// the ones that cannot be drawn must never displace it. Ordering used to enforce that implicitly;
		// stating it as a rule about the objects is what lets the table outlive the frame.
		// The pass each object was found under is carried along rather than looked up again in the loop:
		// nothing mutates accumulatedPasses while the loop runs, so the pointer stays good, and this is
		// the difference between two hash lookups per tracked object per frame and one.
		//
		// The vector is a member so its capacity survives the frame. It was a local, so a 9000-object
		// exterior allocated and freed two ~140 KB buffers every frame to hold the same thing.
		order.clear();
		order.reserve(tracked.size());
		for (auto& [trackedGeometry, entry] : tracked) {
			const auto* pass = FindAccumulatedPass(trackedGeometry);
			if (pass || !accumulatedOnly)
				order.push_back({ trackedGeometry, &entry, pass });
		}

		// An object the engine culled cannot be drawn while the draws are gated on the engine's own
		// visibility (IndirectDraws' RequireNativeVisible, i.e. CS_DCLF_CULL_INPUT=native). It is kept in
		// the tables so the GPU culling still has it as a candidate and can be measured against the
		// engine, but everything downstream of being drawn - the material's stand-in evaluation, the
		// pipeline's per-frame constants - is pure waste for it, and the stand-in evaluation is the
		// single most expensive thing in this loop.
		static const bool drawCulledCandidates = SwitchValue("CS_DCLF_CULL_INPUT") == "tracked";
		// CS_DCLF_MATERIAL_CACHE=off|on|probe. `on` serves a cached record instead of calling
		// EvaluateMaterial; `probe` serves it AND evaluates, comparing the two.
		//
		// EvaluateMaterial is the most expensive call in this loop - a heap allocation, a full
		// RendererShadowState memcpy, six ~1 KB ConstantBlock resets, 28 COM releases and the engine's real
		// SetupMaterial with every CS hook on it - and it ran ~104 times a frame in Dragonsreach.
		//
		// It is NOT a pure function of (material, pass descriptor), and the reason is worth stating because
		// the first attempt at this cache was wrecked by it. Decompiling BSLightingShader::SetupMaterial
		// (vtable slot 4) shows that the PS PerMaterial group's variable 29 - IBLParams, ShaderCache.h - is
		// not read from the material at all: it comes from fields of the BSLightingShader object itself
		// (this+0xcc, and this+0xd0/0xd8 or this+0xe0/0xe8 chosen by a day/night flag at this+0xf0). So it
		// holds the same value for every material in a frame and moves as the frame's lighting does, which
		// is why min == max across all 104 materials while every record still differed between frames.
		//
		// Those positions are therefore patched from one live evaluation a frame, and the set of positions
		// is cumulative - see materialPatched, which documents the step-change failure that taught it.
		// The values are sampled here and then RESAMPLED at Prepass by RefreshMaterialPatch, because
		// sampling them at EarlyPrepass alone leaves them a fraction of a frame behind what the native
		// draws read - visible as IBLParams differing in the sixth decimal across a fast lighting
		// transition. With the resample, capture parity is exact in steady state and better than the
		// uncached path across a transition.
		const bool materialCacheOn = MaterialCacheEnabled();
		// =probe validates EVERY served record against a live evaluation instead of the production
		// sample. The sample is 8 entries behind a stride off a cursor that restarts each frame, so with
		// a stable iteration order it re-checks the same 8 materials for ever - which is exactly how it
		// reported "0 stale" while capture parity was failing on 80% of draws.
		static const bool materialProbeAll = SwitchValue("CS_DCLF_MATERIAL_CACHE") == "probe";
		// The derivation counters feed exactly one log line, which only CS_DCLF_STATS prints. Computing
		// them per object per frame when nothing reads them was pure overhead.
		// CS_DCLF_DERIVE_PROBE, not CS_DCLF_STATS. The counters feed one log line, but computing them also
		// forces the property derivation to run for every accumulated object, and for such an object that
		// derivation has no other effect at all. Gating them on CS_DCLF_STATS meant every reporting run
		// measured a configuration nobody ships.
		static const bool derivationStats = SwitchEnabled("CS_DCLF_DERIVE_PROBE");
		// CS_DCLF_CLASSIFY_CACHE=off|on|probe. `probe` uses the cached verdict and *also* recomputes it,
		// comparing the two; it is the gate, and it costs more than either path alone.
		static const std::string classifyCacheMode = SwitchValue("CS_DCLF_CLASSIFY_CACHE");
		static const bool classifyCache = classifyCacheMode != "off";
		static const bool classifyProbe = classifyCacheMode == "probe";
		// Frame-globals that were being read per object. ShouldSuppress in particular is three terms, and
		// only one of them is per object - the interior test is the same answer for every object in the
		// frame.
		const bool interior = Util::IsInterior();
		const bool lightLimitFixLoaded = globals::features::lightLimitFix.loaded;

		timer.Add(BuildPart::PassLookup);
		for (auto& [geometry, trackedEntry, accumulated] : order) {
			const auto& entry = *trackedEntry;
			// Per TRACKED object, not per eligible one: for a rejected object this is its `continue` and
			// the iteration itself. It used to be billed to `record`, which is a large part of why `record`
			// looked like the biggest cost in the loop.
			timer.Add(BuildPart::LoopTail);
			const bool drawable = accumulated || drawCulledCandidates;
			LightingDescriptors descriptors;
			// The cached-negative fast path. The witnesses come off one cache line of the geometry's
			// runtime data, and an object that has already been shown undrawable never reaches the RTTI
			// cast, the flag tests or the fade metric again.
			auto& verdict = trackedEntry->verdict;
			auto& runtime = geometry->GetGeometryRuntimeData();
			auto* witnessProperty = runtime.shaderProperty.get();
			const auto* witnessMaterial = witnessProperty ? witnessProperty->material : nullptr;
			const std::uint8_t fadeState = FadeStateOf(witnessProperty);
			// Resolve the RTTI cast once per property pointer rather than once per frame. Both outcomes
			// are remembered: castResult stays null for a property that is not a lighting one, and the
			// pointer witness is what makes that null trustworthy.
			if (trackedEntry->castProperty != witnessProperty) {
				trackedEntry->castProperty = witnessProperty;
				trackedEntry->castResult = netimmerse_cast<RE::BSLightingShaderProperty*>(witnessProperty);
				++stats.castResolved;
			}
			RE::BSLightingShaderProperty* castCache = trackedEntry->castResult;
			const bool hit = classifyCache && verdict.cached && verdict.rendererData == runtime.rendererData &&
			                 verdict.property == witnessProperty && verdict.material == witnessMaterial &&
			                 verdict.fadeState == fadeState;
			Ineligible reason;
			if (hit && !classifyProbe) {
				reason = verdict.reason;
				++stats.classifyHits;
			} else {
				reason = ClassifyStatic(*geometry, &descriptors, accumulated, derivationStats, &castCache);
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
			// The renderer draws batch lists 1, 3 and 4 with alpha testing. When the technique the pass was
			// registered under lacks DoAlphaTest, the technique drawn sometimes gains it after the tables are
			// built (engine notes: batch renderer, open question); leave those to the native loop.
			if (reason == Ineligible::None && accumulated && accumulated->subPass != 0 && accumulated->subPass != 2 &&
				!(accumulated->technique & kDoAlphaTestBit))
				reason = Ineligible::AlphaTestState;
			++stats.ineligible[static_cast<std::size_t>(reason)];
			if (reason != Ineligible::None)
				continue;
			// Only computed when something will report them: this whole block, including the popcount
			// loop, exists to feed one log line, and it ran per object per frame regardless.
			if (!derivationStats) {
				// nothing to do
			} else if (descriptors.derivedPass == kNotDerived) {
				++stats.derivationNative;
			} else if (accumulated) {
				// Only objects the accumulator holds can be compared at all. Without one, descriptors.pass
				// IS the derivation (LightingDescriptors.cpp: "the derivation's guesses stand"), so counting
				// those would compare the derivation against itself and report a difference of zero for
				// every one of them - which is how the first version of this counter turned 588 differing
				// objects out of 915 comparable ones into "588 of 3057".
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
			}
			timer.Add(BuildPart::Diagnostics);

			auto& data = geometry->GetGeometryRuntimeData();
			auto* property = data.shaderProperty.get();
			const bool twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
			const auto* alpha = data.alphaProperty.get();
			const bool alphaTest = alpha && alpha->GetAlphaTesting();

			// Geometry, shared between every object drawing the same TriShape.
			timer.Add(BuildPart::Record /* the property and alpha reads above */);
			auto* triShape = data.rendererData;
			auto [geometryIt, newGeometry] = geometryIndex.try_emplace(triShape, static_cast<std::uint32_t>(tables.geometries.size()));
			if (newGeometry) {
				// The buffers are resolved once per TRISHAPE, not once per object.
				//
				// Both Resolve results were only ever read inside this branch; for an object whose
				// geometry was already in the table they were computed and thrown away. With ~3.8 objects
				// per TriShape in the Whiterun exterior that is most of the calls, and in steady state a
				// Resolve is a hash probe (DescribeResource only runs on first insert), so it was ~0.33 ms
				// a frame of pure repetition.
				//
				// Skipping the call altogether is NOT safe and is why this is a move rather than a cache:
				// GpuResources holds a reference on each buffer so its address cannot be reused, and it
				// drops that reference when an entry goes kEvictFrames without a Resolve. Resolving per
				// TriShape still touches every entry DCLF depends on every frame, so nothing is evicted
				// out from under the tables.
				const GpuResources::Buffer* vertexBuffer = nullptr;
				const GpuResources::Buffer* indexBuffer = nullptr;
				if (resolveBuffers) {
					// The render graph reads the game's buffers in place; they must never move (GpuResources).
					vertexBuffer = gpu.Resolve(reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer));
					indexBuffer = gpu.Resolve(reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer));
					if (!vertexBuffer || !indexBuffer) {
						// Leave the slot unclaimed so the next object sharing this TriShape retries,
						// exactly as it did when every object resolved for itself.
						geometryIndex.erase(geometryIt);
						--stats.ineligible[static_cast<std::size_t>(Ineligible::None)];
						++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
						timer.Add(BuildPart::Resolve);
						continue;
					}
				}
				const auto& shape = static_cast<RE::BSTriShape*>(geometry)->GetTrishapeRuntimeData();
				GeometryRecord record;
				record.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer);
				record.indexBuffer = reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer);
				record.vertexDesc = std::bit_cast<std::uint64_t>(triShape->vertexDesc);
				record.vertexStride = triShape->vertexDesc.GetSize();
				record.vertexCount = shape.vertexCount;
				record.indexCount = static_cast<std::uint32_t>(shape.triangleCount) * 3;
				record.firstIndex = 0;
				if (vertexBuffer && indexBuffer) {
					record.vertexAddress = vertexBuffer->address;
					record.vertexBytes = vertexBuffer->size;
					record.indexAddress = indexBuffer->address;
					record.indexBytes = indexBuffer->size;
				}
				tables.geometries.push_back(record);
				timer.Add(BuildPart::Resolve);
			}

			const PipelineKey key{ descriptors.vertex, descriptors.pixel, twoSided ? kRasterTwoSided : 0u, descriptors.pass,
				VertexLayoutOf(tables.geometries[geometryIt->second].vertexDesc) };
			auto [pipelineIt, newPipeline] = pipelineIndex.try_emplace(key, static_cast<std::uint32_t>(tables.pipelines.size()));
			if (newPipeline && !drawable) {
				// Nothing would ever use it: leaving the entry out keeps the pipeline table, and the
				// per-pipeline evaluations that go with it, to what is actually drawn. It also means a
				// culled candidate cannot create a pipeline in the first place, so in the default
				// configuration the election below never has to take a template over - which is exactly
				// what its counter reading 0 says.
				pipelineIndex.erase(pipelineIt);
				pipelineIt = pipelineIndex.end();
			} else if (newPipeline) {
				timer.Add(BuildPart::Dedup);
				tables.pipelines.push_back(key);
				// Per-frame PerGeometry values for this pass descriptor, from any object's lighting pass
				// (it supplies the scene light list the engine reads the sun from).
				GeometryConstants constants;
				const auto* templatePass = FindLightingPass(property);
				const bool valid = templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, mainPassRenderFlags, constants);
				tables.geometryConstants.push_back(constants);
				tables.geometryConstantsValid.push_back(valid ? 1 : 0);
				tables.geometryTemplate.push_back(property);
				tables.geometryTemplateNative.push_back(accumulated ? 1 : 0);

				TechniqueConstants technique;
				EvaluateTechnique(descriptors.pass, technique);
				stats.shadowMaskPipelines += technique.shadowMask ? 1 : 0;
				tables.techniqueConstants.push_back(technique);

				PipelinePermutation permutation;
				permutation.vertexShaderDescriptor = descriptors.rawVertex;
				permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
				permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
				// Extended Translucency disables its material model for opaque geometry.
				permutation.extraFeatureDescriptor = globals::features::extendedTranslucency.loaded ?
				                                         static_cast<std::uint32_t>(ExtendedTranslucency::MaterialModel::DescriptorDisabled)
				                                             << ExtendedTranslucency::ExtraFeatureDescriptorShift :
				                                         0u;
				tables.permutations.push_back(permutation);
				timer.Add(BuildPart::PipelineEval);
			} else if (accumulated && pipelineIt != pipelineIndex.end() &&
			           pipelineIt->second < tables.geometryTemplateNative.size() &&
			           !tables.geometryTemplateNative[pipelineIt->second]) {
				// The election. This pipeline's per-frame lighting template belongs to an object the
				// engine culled, and here is one it kept: take the template over. An object the engine
				// kept is by definition in the lighting situation being drawn, so it is the correct
				// template, and this is the ordering guarantee stated as a rule about the objects rather
				// than as a rule about the order they are visited in - which is what a persistent
				// pipeline table needs, because it has no visit order to rely on.
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

			// Material state as the engine's SetupMaterial produces it for this pass descriptor.
			const auto* material = property->material;
			auto [materialIt, newMaterial] = materialIndex.try_emplace(std::pair{ material, descriptors.pass }, static_cast<std::uint32_t>(tables.materials.size()));
			if (newMaterial && !drawable) {
				++stats.materialsSkipped;
				materialIndex.erase(materialIt);
				materialIt = materialIndex.end();
			} else if (newMaterial) {
				timer.Add(BuildPart::Dedup);
				MaterialRecord record;
				// The cross-frame material cache.
				//
				// EvaluateMaterial is the single most expensive call in this loop: a heap allocation, a full
				// RendererShadowState memcpy, six ~1 KB ConstantBlock resets, 28 COM releases and the engine's
				// real SetupMaterial with every CS hook on it. It ran ~104 times a frame in Dragonsreach for a
				// result that is almost entirely the same every time.
				//
				// "Almost": measurement found exactly three adjacent PS floats that move between frames, and
				// they hold the SAME value for every material in a frame (min == max across all 104), drifting
				// as the time of day advances. So the record is cached, and those frame-global floats are
				// patched from one live evaluation per frame. Their positions are LEARNED rather than written
				// in here, because the reason a value is frame-global belongs to the engine, not to this file,
				// and a hard-coded index would silently rot when a feature changes the constant layout.
				const std::pair cacheKey{ material, descriptors.pass };
				auto cached = materialCache.find(cacheKey);
				const bool canServe = materialCacheOn && cached != materialCache.end() && materialPatchValuesFresh;
				// One live evaluation a frame teaches the drift; the validator below re-evaluates a rolling
				// slice so a material that starts varying in some OTHER field cannot go unnoticed.
				const bool validating = materialCacheOn && cached != materialCache.end() && materialPatchValuesFresh &&
				                        (materialProbeAll ||
				                            (stats.materialsValidated < kMaterialValidationsPerFrame &&
				                                (materialValidationCursor++ % kMaterialValidationStride) == 0));
				if (canServe && !validating) {
					record = cached->second.record;
					for (std::size_t i = 0; i < materialPatched.size(); ++i)
						record.ps.floats[materialPatched[i]] = materialPatchValues[i];
					++stats.materialsFromCache;
				} else {
					if (!evaluator.EvaluateMaterial(material, descriptors.pass, record)) {
						// No shader instance yet (nothing drawn so far): stay native this frame.
						materialIndex.erase(materialIt);
						++stats.ineligible[static_cast<std::size_t>(Ineligible::NotLightingShader)];
						--stats.ineligible[static_cast<std::size_t>(Ineligible::None)];
						continue;
					}
					++stats.materialsEvaluated;
					if (cached != materialCache.end()) {
						const auto& previous = cached->second.record;
						if (!materialPatchValuesFresh) {
							// Any position that differs from this material's own cached copy joins the set
							// permanently; then every known position takes this frame's live value.
							for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f) {
								if (previous.ps.floats[f] != record.ps.floats[f])
									NotePatchedFloat(f);
							}
							for (std::size_t i = 0; i < materialPatched.size(); ++i)
								materialPatchValues[i] = record.ps.floats[materialPatched[i]];
							materialPatchValuesFresh = true;
							materialPatchSource.reset(const_cast<RE::BSShaderMaterial*>(material));
							materialPatchSourcePass = descriptors.pass;
							stats.materialDriftFloats = static_cast<std::uint32_t>(materialPatched.size());
						} else if (validating) {
							// The standing alarm: what the cache WOULD have served, against a live evaluation.
							// It runs in production, not only under a probe switch, because the failure it
							// guards against is a material quietly rendering with another material's constants.
							MaterialRecord served = previous;
							for (std::size_t i = 0; i < materialPatched.size(); ++i)
								served.ps.floats[materialPatched[i]] = materialPatchValues[i];
							++stats.materialsValidated;
							// Stage 3's probe shape: the frame must behave exactly as it would with the cache
							// on, or the probe measures a configuration nobody ships. The live record is
							// only the yardstick; what goes into the tables is what the cache would serve.
							const MaterialRecord fresh = record;
							record = served;
							++stats.materialsFromCache;
							if (!(served == fresh)) {
								++stats.materialCacheStale;
								// Self-healing: a float the cache got wrong is, by definition, one that is not
								// a fixed property of the material. Adopt it into the patched set so the next
								// frame serves it live instead of from the cache.
								for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f) {
									if (served.ps.floats[f] != fresh.ps.floats[f]) {
										NotePatchedFloat(f);
										for (std::size_t i = 0; i < materialPatched.size(); ++i) {
											if (materialPatched[i] == f)
												materialPatchValues[i] = fresh.ps.floats[f];
										}
									}
								}
								if (served.vs.floats != fresh.vs.floats)
									stats.materialDiffMask |= 1u << 0;
								if (served.ps.floats != fresh.ps.floats)
									stats.materialDiffMask |= 1u << 1;
								if (served.textures != fresh.textures)
									stats.materialDiffMask |= 1u << 2;
								if (served.addressModes != fresh.addressModes)
									stats.materialDiffMask |= 1u << 3;
								if (served.filterModes != fresh.filterModes)
									stats.materialDiffMask |= 1u << 4;
								if (served.textureWritten != fresh.textureWritten)
									stats.materialDiffMask |= 1u << 5;
								if (!stats.materialDiffLogged) {
									stats.materialDiffLogged = true;
									std::string moved;
									for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f) {
										if (served.vs.floats[f] != fresh.vs.floats[f])
											moved += fmt::format(" vs[{}]=c{}.{} ({} -> {})", f, f / 4, "xyzw"[f % 4], served.vs.floats[f], fresh.vs.floats[f]);
										if (served.ps.floats[f] != fresh.ps.floats[f])
											moved += fmt::format(" ps[{}]=c{}.{} ({} -> {})", f, f / 4, "xyzw"[f % 4], served.ps.floats[f], fresh.ps.floats[f]);
									}
									if (served.textures != fresh.textures)
										moved += " textures";
									if (served.addressModes != fresh.addressModes || served.filterModes != fresh.filterModes)
										moved += " samplers";
									logger::warn("[DCLF] material cache STALE for a material:{}", moved);
								}
							}
						}
					}
					auto& cacheEntry = cached != materialCache.end() ? cached->second : materialCache[cacheKey];
					// The reference is what makes the key safe: BSShaderMaterial is intrusively ref-counted, so
					// holding one means a freed material cannot be mistaken for a new allocation at the same
					// address - which is the one way this cache could hand an object another material's state.
					if (!cacheEntry.material)
						cacheEntry.material.reset(const_cast<RE::BSShaderMaterial*>(material));
					cacheEntry.record = record;
				}
				if (auto it = materialCache.find(cacheKey); it != materialCache.end())
					it->second.lastUsed = frame;
				tables.materials.push_back(record);
				timer.Add(BuildPart::MaterialEval);
			}
			// The geometry, pipeline and material probes above, on the path where all three hit - which is
			// nearly every object. The Dedup part only ever fired inside the miss branches, so these three
			// hashes (one of them over the five-field PipelineKey) fell through to the next iteration and
			// were billed to `record`.
			timer.Add(BuildPart::DedupHit);

			const auto objectId = static_cast<std::uint32_t>(tables.objects.size());
			ObjectRecord object{};
			// CS_DCLF_TRANSFORM_PROBE=skip: a COST BOUND, not a mode. It renders wrong - every object
			// collapses to the origin - and exists only to answer how much of `record` the two transform
			// stores are, which is what decides whether Stage 5's transform witness can pay for itself.
			// A witness would read two 52-byte NiTransforms to avoid 18 multiplies, so the answer has to
			// be large for it to be worth the extra memory traffic.
			static const bool skipTransforms = SwitchValue("CS_DCLF_TRANSFORM_PROBE") == "skip";
			if (!skipTransforms) {
				StoreTransform(geometry->world, object.world);
				StoreTransform(geometry->previousWorld, object.previousWorld);
			}
			object.boundCenter[0] = geometry->worldBound.center.x;
			object.boundCenter[1] = geometry->worldBound.center.y;
			object.boundCenter[2] = geometry->worldBound.center.z;
			object.boundRadius = geometry->worldBound.radius;
			object.geometryIndex = geometryIt->second;
			// A candidate that cannot be drawn has no material or pipeline entry. The indices are left at
			// zero rather than at a sentinel because nothing reads them: kObjectNativeVisible is unset, so
			// BuildDrawsCS rejects it before it ever looks at them. All of the parallel per-object arrays
			// are still appended below, which is what the first attempt at this got wrong - skipping one
			// of them shifts every later object's index.
			const bool hasBindings = materialIt != materialIndex.end() && pipelineIt != pipelineIndex.end();
			object.materialIndex = hasBindings ? materialIt->second : 0u;
			object.pipelineIndex = hasBindings ? pipelineIt->second : 0u;
			// kObjectNoBindings, not a zero index: the pipeline and material tables can be empty (the
			// first frame after a teleport has tracked geometry but nothing accumulated), so index 0 is
			// out of bounds as readily as any other.
			object.flags = (alphaTest ? kObjectAlphaTest : 0u) | (twoSided ? kObjectTwoSided : 0u) |
			               (accumulated ? kObjectNativeVisible : 0u) | (hasBindings ? 0u : kObjectNoBindings) |
			               (ExternalEmittance::ShouldSuppress(interior, property, geometry) ? kObjectSuppressExternalEmittance : 0u) |
			               (alphaTest ? static_cast<std::uint32_t>(alpha->alphaThreshold) << kObjectAlphaThresholdShift : 0u);
			stats.nativeVisible += accumulated ? 1 : 0;
			tables.objects.push_back(object);
			tables.objectGeometry.push_back(geometry);
			float emissiveMult = 1.0f;
			tables.shading.push_back(MakeShading(*static_cast<RE::BSLightingShaderProperty*>(property), descriptors, mainPassRenderFlags, emissiveMult));
			tables.emissiveMult.push_back(emissiveMult);
			ObjectLights lights;
			if (lightLimitFixLoaded) {
				lights.roomIndex = globals::features::lightLimitFix.GetRoomIndex(geometry);
				if (accumulated)
					lights.shadowBitMask = LightLimitFix::GetShadowBitMask(accumulated->pass);
			}
			tables.lights.push_back(lights);
			objectIndex.emplace(geometry, objectId);

			const auto& geometryRecord = tables.geometries[object.geometryIndex];
			DrawSequence draw{};
			draw.pipelineIndex = object.pipelineIndex;
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

		stats.objects = static_cast<std::uint32_t>(tables.objects.size());
		stats.geometries = static_cast<std::uint32_t>(tables.geometries.size());
		stats.pipelines = static_cast<std::uint32_t>(tables.pipelines.size());
		// The 4c gate, checked over the finished tables rather than asserted from the election: no
		// native-visible object may draw on a pipeline whose lighting template came from a culled one.
		stats.templateDefects = 0;
		stats.pipelinesCulledOnly = 0;
		for (const auto native : tables.geometryTemplateNative)
			stats.pipelinesCulledOnly += native ? 0u : 1u;
		if (stats.pipelinesCulledOnly) {
			for (const auto& object : tables.objects) {
				if (!(object.flags & kObjectNativeVisible) || (object.flags & kObjectNoBindings))
					continue;
				if (object.pipelineIndex < tables.geometryTemplateNative.size() && !tables.geometryTemplateNative[object.pipelineIndex])
					++stats.templateDefects;
			}
		}
		stats.materials = static_cast<std::uint32_t>(tables.materials.size());
		// The eviction the probe never had. Without it a long session accumulates every material of every
		// cell it has visited, each holding a reference that keeps the material itself alive - a slow leak
		// rather than a crash, which is why it needs a counter and not just a comment.
		if ((frame % kMaterialCacheIdleFrames) == 0) {
			for (auto it = materialCache.begin(); it != materialCache.end();) {
				if (frame - it->second.lastUsed > kMaterialCacheIdleFrames) {
					it = materialCache.erase(it);
					++stats.materialCacheEvicted;
				} else {
					++it;
				}
			}
		}
		stats.materialCacheEntries = static_cast<std::uint32_t>(materialCache.size());
	}

	std::int32_t SceneStore::FindObject(const RE::BSGeometry* a_geometry) const
	{
		auto it = objectIndex.find(a_geometry);
		return it == objectIndex.end() ? -1 : static_cast<std::int32_t>(it->second);
	}

	Ineligible SceneStore::Classify(RE::BSGeometry* a_geometry) const
	{
		auto it = tracked.find(a_geometry);
		if (it == tracked.end())
			return Ineligible::NotTriShape;
		const Ineligible reason = ClassifyStatic(*a_geometry, nullptr);
		return reason != Ineligible::None ? reason : ClassifyFrame(it->second);
	}

	bool SceneStore::IsTracked(const RE::BSGeometry* a_geometry) const
	{
		return tracked.contains(const_cast<RE::BSGeometry*>(a_geometry));
	}
}
