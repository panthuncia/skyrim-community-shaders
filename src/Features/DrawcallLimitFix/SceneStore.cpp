#include "SceneStore.h"

#include "GpuResources.h"
#include "SceneTracker.h"
#include "VertexInput.h"

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

	void SceneStore::ProcessEvents()
	{
		RefreshCategoryNodes();

		auto& tracker = SceneTracker::Get();
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

	void SceneStore::CollectAccumulatedPasses()
	{
		accumulatedPasses.clear();
		auto* accumulator = *globals::game::currentAccumulator.get();
		auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
		if (!batch)
			return;

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
		PartTimer timer;
		RefreshLodFadeSettings();
		CollectAccumulatedPasses();
		auto& gpu = GpuResources::Get();
		gpu.BeginFrame(frame);
		const bool resolveBuffers = gpu.Enabled();
		timer.Add(stats.partMs[0]);
		tables.Clear();
		objectIndex.clear();
		stats.ineligible.fill(0);
		stats.shadowMaskPipelines = 0;
		stats.derivationChecked = stats.derivationDiffers = stats.derivationBits = stats.derivationNative = 0;

		ankerl::unordered_dense::map<const RE::BSGraphics::TriShape*, std::uint32_t> geometryIndex;
		ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash> pipelineIndex;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> materialIndex;
		auto& evaluator = ConstantEvaluator::Get();
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
		for (auto& [trackedGeometry, entry] : tracked) {
			auto* geometry = trackedGeometry;
			const auto* accumulated = FindAccumulatedPass(geometry);
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
			} else {
				++stats.derivationChecked;
				if (const std::uint32_t bits = (descriptors.derivedPass ^ descriptors.pass) & ~kRuntimePassBits) {
					++stats.derivationDiffers;
					stats.derivationBits |= bits;
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
			if (newPipeline) {
				timer.Add(stats.partMs[1]);
				tables.pipelines.push_back(key);
				// Per-frame PerGeometry values for this pass descriptor, from any object's lighting pass
				// (it supplies the scene light list the engine reads the sun from).
				GeometryConstants constants;
				const auto* templatePass = FindLightingPass(property);
				const bool valid = templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, mainPassRenderFlags, constants);
				tables.geometryConstants.push_back(constants);
				tables.geometryConstantsValid.push_back(valid ? 1 : 0);

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
			if (newMaterial) {
				timer.Add(stats.partMs[1]);
				MaterialRecord record;
				if (!evaluator.EvaluateMaterial(material, descriptors.pass, record)) {
					// No shader instance yet (nothing drawn so far): stay native this frame.
					materialIndex.erase(materialIt);
					++stats.ineligible[static_cast<std::size_t>(Ineligible::NotLightingShader)];
					--stats.ineligible[static_cast<std::size_t>(Ineligible::None)];
					continue;
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
			object.materialIndex = materialIt->second;
			object.pipelineIndex = pipelineIt->second;
			object.flags = (alphaTest ? kObjectAlphaTest : 0u) | (twoSided ? kObjectTwoSided : 0u) |
			               (accumulated ? kObjectNativeVisible : 0u) |
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
