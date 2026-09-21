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

		ObjectShading MakeShading(const RE::BSLightingShaderProperty& a_property, const LightingDescriptors& a_descriptors, std::uint32_t a_renderFlags)
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
		lights.clear();
		geometryConstants.clear();
		geometryConstantsValid.clear();
		geometryTemplate.clear();
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

	void SceneStore::RefreshCategoryNodes()
	{
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
		if (rescanPending) {
			// RefreshCategoryNodes treats every category node as newly appeared and walks it, which is
			// exactly the full rescan wanted here.
			rescanPending = false;
			tracked.clear();
			categoryNodes.clear();
			validationCursor = 0;
		}

		RefreshCategoryNodes();

		SceneTracker::Event* events = tracker.Drain();
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

	Ineligible SceneStore::ClassifyStatic(RE::BSGeometry& a_geometry, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated)
	{
		if (a_geometry.GetType().get() != RE::BSGeometry::Type::kTriShape)
			return Ineligible::NotTriShape;

		auto& data = a_geometry.GetGeometryRuntimeData();
		if (data.skinInstance)
			return Ineligible::Skinned;
		if (!data.rendererData || !data.rendererData->vertexBuffer || !data.rendererData->indexBuffer)
			return Ineligible::NoRendererData;

		auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get());
		if (!property)
			return Ineligible::NotLightingShader;

		// GetRenderPasses treats material alpha below one as transparent.
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
		if (material && material->materialAlpha < 1.0f)
			return Ineligible::AlphaBlend;

		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*property, a_geometry, a_accumulated, descriptors);
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

	void SceneStore::RefreshFrameConstants()
	{
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
			tables.shading[o] = MakeShading(lighting, descriptors, mainPassRenderFlags);
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
		mainBatchRenderers.clear();
		mainBatchRenderers.insert(batch);
		addBatch(batch);
		// Geometry groups sort their passes in batch renderers of their own.
		for (auto* group : batch->geometryGroups) {
			if (group && group->batchRenderer) {
				mainBatchRenderers.insert(group->batchRenderer);
				addBatch(group->batchRenderer);
			}
		}
		// Published for the registration hook, which runs before this and so uses the previous frame's
		// set. These pointers are stable across frames, and an empty set on the first frame simply means
		// nothing is withheld yet.
		PassCapture::Get().SetMainBatchRenderers(
			std::make_shared<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>>(mainBatchRenderers));
	}

	void SceneStore::CompareCapturedPasses()
	{
		auto& capture = PassCapture::Get();
		if (!capture.Installed())
			return;
		const auto entries = capture.Drain();
		auto& captureStats = capture.MutableStats();
		captureStats.compared = captureStats.missing = captureStats.extra = captureStats.techniqueDiffers = captureStats.subPassDiffers = 0;

		// Only the main camera's registrations; the shadow cameras register into their own renderers.
		ankerl::unordered_dense::map<const RE::BSGeometry*, const PassCapture::Entry*> captured;
		for (const auto& entry : entries) {
			if (mainBatchRenderers.contains(entry.batch))
				captured.try_emplace(entry.geometry, &entry);
		}

		for (const auto& [geometry, accumulated] : accumulatedPasses) {
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
		for (const auto& [geometry, entry] : captured) {
			if (!accumulatedPasses.contains(geometry))
				++captureStats.extra;
		}

		// The tables are built from the capture rather than the accumulator walk once the two agree. The
		// walk stops working the moment a pass is withheld from the batch renderer, which is the whole
		// point of static ownership; the capture sees the registration regardless of what happens to it
		// afterwards. Falling back when the capture is empty keeps the first frame and any unexpected
		// path working.
		if (SwitchValue("CS_DCLF_PASS_SOURCE") == "accumulator" || captured.empty())
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
		// Adds the time since the last call to a_bucket.
		struct PartTimer
		{
			std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
			void Add(double& a_bucket)
			{
				const auto now = std::chrono::steady_clock::now();
				a_bucket += std::chrono::duration<double, std::milli>(now - last).count();
				last = now;
			}
		};
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
		PartTimer timer;
		RefreshLodFadeSettings();
		CollectAccumulatedPasses();
		CompareCapturedPasses();
		auto& gpu = GpuResources::Get();
		gpu.BeginFrame(frame);
		const bool resolveBuffers = gpu.Enabled();
		timer.Add(stats.partMs[0]);
		tables.Clear();
		objectIndex.clear();
		stats.ineligible.fill(0);
		stats.shadowMaskPipelines = 0;
		stats.derivationChecked = stats.derivationDiffers = stats.derivationBits = stats.derivationNative = 0;
		stats.derivationRuntimeDiffers = stats.derivationRuntimeBits = 0;
		stats.derivationBitCounts.fill(0);
		stats.materialsEvaluated = stats.materialsSkipped = 0;
		stats.materialsUnchanged = stats.materialsChanged = 0;

		ankerl::unordered_dense::map<const RE::BSGraphics::TriShape*, std::uint32_t> geometryIndex;
		ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash> pipelineIndex;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> materialIndex;
		auto& evaluator = ConstantEvaluator::Get();
		ConstantEvaluator::ResetFrameAudits();
		if (!evaluator.HasLightingShader())
			FindLightingShader();

		tables.objects.reserve(tracked.size());
		tables.objectGeometry.reserve(tracked.size());
		tables.draws.reserve(tracked.size());

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

		// Objects the engine kept are classified first, then the rest.
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
		// Ordering fixes it at the source and costs one extra pass over a hash map. It is not a tie-break
		// hack: an object the engine kept is by definition in the lighting situation being drawn, so it is
		// the correct template, and the ones that cannot be drawn should never displace it.
		std::vector<std::pair<RE::BSGeometry*, const Tracked*>> order;
		std::vector<std::pair<RE::BSGeometry*, const Tracked*>> culled;
		order.reserve(tracked.size());
		culled.reserve(tracked.size());
		for (auto& [trackedGeometry, entry] : tracked) {
			if (FindAccumulatedPass(trackedGeometry))
				order.emplace_back(trackedGeometry, &entry);
			else if (!accumulatedOnly)
				culled.emplace_back(trackedGeometry, &entry);
		}
		order.insert(order.end(), culled.begin(), culled.end());

		// An object the engine culled cannot be drawn while the draws are gated on the engine's own
		// visibility (IndirectDraws' RequireNativeVisible, i.e. CS_DCLF_CULL_INPUT=native). It is kept in
		// the tables so the GPU culling still has it as a candidate and can be measured against the
		// engine, but everything downstream of being drawn - the material's stand-in evaluation, the
		// pipeline's per-frame constants - is pure waste for it, and the stand-in evaluation is the
		// single most expensive thing in this loop.
		static const bool drawCulledCandidates = SwitchValue("CS_DCLF_CULL_INPUT") == "tracked";
		static const bool probeCache = SwitchValue("CS_DCLF_MATERIAL_CACHE") == "probe";

		for (auto& [geometry, trackedEntry] : order) {
			const auto& entry = *trackedEntry;
			const auto* accumulated = FindAccumulatedPass(geometry);
			const bool drawable = accumulated || drawCulledCandidates;
			LightingDescriptors descriptors;
			timer.Add(stats.partMs[1] /* the rest of the previous object counts as classification */);
			Ineligible reason = ClassifyStatic(*geometry, &descriptors, accumulated);
			if (reason == Ineligible::None)
				reason = ClassifyFrame(entry);
			// The renderer draws batch lists 1, 3 and 4 with alpha testing. When the technique the pass was
			// registered under lacks DoAlphaTest, the technique drawn sometimes gains it after the tables are
			// built (engine notes: batch renderer, open question); leave those to the native loop.
			if (reason == Ineligible::None && accumulated && accumulated->subPass != 0 && accumulated->subPass != 2 &&
				!(accumulated->technique & kDoAlphaTestBit))
				reason = Ineligible::AlphaTestState;
			++stats.ineligible[static_cast<std::size_t>(reason)];
			if (reason != Ineligible::None)
				continue;
			if (descriptors.derivedPass == kNotDerived) {
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

			auto& data = geometry->GetGeometryRuntimeData();
			auto* property = data.shaderProperty.get();
			const bool twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
			const auto* alpha = data.alphaProperty.get();
			const bool alphaTest = alpha && alpha->GetAlphaTesting();

			// Geometry, shared between every object drawing the same TriShape.
			auto* triShape = data.rendererData;
			const GpuResources::Buffer* vertexBuffer = nullptr;
			const GpuResources::Buffer* indexBuffer = nullptr;
			if (resolveBuffers) {
				// The render graph reads the game's buffers in place; they must never move (GpuResources).
				vertexBuffer = gpu.Resolve(reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer));
				indexBuffer = gpu.Resolve(reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer));
				if (!vertexBuffer || !indexBuffer) {
					--stats.ineligible[static_cast<std::size_t>(Ineligible::None)];
					++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
					continue;
				}
			}
			auto [geometryIt, newGeometry] = geometryIndex.try_emplace(triShape, static_cast<std::uint32_t>(tables.geometries.size()));
			if (newGeometry) {
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
			}

			const PipelineKey key{ descriptors.vertex, descriptors.pixel, twoSided ? kRasterTwoSided : 0u, descriptors.pass,
				VertexLayoutOf(tables.geometries[geometryIt->second].vertexDesc) };
			auto [pipelineIt, newPipeline] = pipelineIndex.try_emplace(key, static_cast<std::uint32_t>(tables.pipelines.size()));
			if (newPipeline && !drawable) {
				// Nothing would ever use it. Leaving the entry out keeps the pipeline table to what is
				// actually drawn, which is also what the ordering above relies on: a culled candidate must
				// never be the object that fixes a pipeline's per-frame lighting constants.
				pipelineIndex.erase(pipelineIt);
				pipelineIt = pipelineIndex.end();
			} else if (newPipeline) {
				timer.Add(stats.partMs[1]);
				tables.pipelines.push_back(key);
				// Per-frame PerGeometry values for this pass descriptor, from any object's lighting pass
				// (it supplies the scene light list the engine reads the sun from).
				GeometryConstants constants;
				const auto* templatePass = FindLightingPass(property);
				const bool valid = templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, mainPassRenderFlags, constants);
				tables.geometryConstants.push_back(constants);
				tables.geometryConstantsValid.push_back(valid ? 1 : 0);
				tables.geometryTemplate.push_back(property);

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
				timer.Add(stats.partMs[2]);
			}

			// Material state as the engine's SetupMaterial produces it for this pass descriptor.
			const auto* material = property->material;
			auto [materialIt, newMaterial] = materialIndex.try_emplace(std::pair{ material, descriptors.pass }, static_cast<std::uint32_t>(tables.materials.size()));
			if (newMaterial && !drawable) {
				++stats.materialsSkipped;
				materialIndex.erase(materialIt);
				materialIt = materialIndex.end();
			} else if (newMaterial) {
				timer.Add(stats.partMs[1]);
				MaterialRecord record;
				if (!evaluator.EvaluateMaterial(material, descriptors.pass, record)) {
					// No shader instance yet (nothing drawn so far): stay native this frame.
					materialIndex.erase(materialIt);
					++stats.ineligible[static_cast<std::size_t>(Ineligible::NotLightingShader)];
					--stats.ineligible[static_cast<std::size_t>(Ineligible::None)];
					continue;
				}
				++stats.materialsEvaluated;
				if (probeCache) {
					const std::pair probeKey{ material, descriptors.pass };
					if (auto it = materialProbe.find(probeKey); it != materialProbe.end()) {
						(it->second.record == record ? stats.materialsUnchanged : stats.materialsChanged) += 1;
						it->second.record = record;
					} else {
						auto& probe = materialProbe[probeKey];
						probe.material.reset(const_cast<RE::BSShaderMaterial*>(material));
						probe.record = record;
					}
				}
				tables.materials.push_back(record);
				timer.Add(stats.partMs[3]);
			}

			const auto objectId = static_cast<std::uint32_t>(tables.objects.size());
			ObjectRecord object{};
			StoreTransform(geometry->world, object.world);
			StoreTransform(geometry->previousWorld, object.previousWorld);
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
			               (ExternalEmittance::ShouldSuppress(property, geometry) ? kObjectSuppressExternalEmittance : 0u) |
			               (alphaTest ? static_cast<std::uint32_t>(alpha->alphaThreshold) << kObjectAlphaThresholdShift : 0u);
			tables.objects.push_back(object);
			tables.objectGeometry.push_back(geometry);
			tables.shading.push_back(MakeShading(*static_cast<RE::BSLightingShaderProperty*>(property), descriptors, mainPassRenderFlags));
			ObjectLights lights;
			if (globals::features::lightLimitFix.loaded) {
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
		}

		stats.objects = static_cast<std::uint32_t>(tables.objects.size());
		stats.nativeVisible = 0;
		for (const auto& object : tables.objects)
			stats.nativeVisible += (object.flags & kObjectNativeVisible) ? 1 : 0;
		stats.geometries = static_cast<std::uint32_t>(tables.geometries.size());
		stats.pipelines = static_cast<std::uint32_t>(tables.pipelines.size());
		stats.materials = static_cast<std::uint32_t>(tables.materials.size());
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
