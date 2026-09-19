#include "SceneStore.h"

#include "SceneTracker.h"

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
		geometryConstants.clear();
		geometryConstantsValid.clear();
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
		};

		if (auto* tes = RE::TES::GetSingleton()) {
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

	Ineligible SceneStore::ClassifyStatic(RE::BSGeometry& a_geometry, LightingDescriptors* a_descriptors)
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
		const Ineligible reason = DeriveLightingDescriptors(*property, a_geometry, descriptors);
		if (reason == Ineligible::None && a_descriptors)
			*a_descriptors = descriptors;
		return reason;
	}

	Ineligible SceneStore::ClassifyFrame(const Tracked& a_tracked) const
	{
		if (a_tracked.unsupportedParent)
			return Ineligible::UnsupportedParent;

		// App-culled or hidden anywhere between the leaf and its category node.
		for (const RE::NiAVObject* object = a_tracked.geometry.get(); object; object = object->parent) {
			if (IsHidden(object))
				return Ineligible::Hidden;
			if (object == a_tracked.categoryNode)
				break;
		}

		auto* property = a_tracked.geometry->GetGeometryRuntimeData().shaderProperty.get();
		if (property && property->fadeNode && property->fadeNode->GetRuntimeData().currentFade < 1.0f)
			return Ineligible::Fading;

		return Ineligible::None;
	}

	void SceneStore::BuildFrame()
	{
		++frame;
		RefreshLodFadeSettings();
		tables.Clear();
		objectIndex.clear();
		stats.ineligible.fill(0);

		ankerl::unordered_dense::map<const RE::BSGraphics::TriShape*, std::uint32_t> geometryIndex;
		ankerl::unordered_dense::map<std::uint64_t, std::uint32_t> pipelineIndex;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> materialIndex;
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			FindLightingShader();

		tables.objects.reserve(tracked.size());
		tables.objectGeometry.reserve(tracked.size());
		tables.draws.reserve(tracked.size());

		for (auto& [geometry, entry] : tracked) {
			LightingDescriptors descriptors;
			Ineligible reason = ClassifyStatic(*geometry, &descriptors);
			if (reason == Ineligible::None)
				reason = ClassifyFrame(entry);
			++stats.ineligible[static_cast<std::size_t>(reason)];
			if (reason != Ineligible::None)
				continue;

			auto& data = geometry->GetGeometryRuntimeData();
			auto* property = data.shaderProperty.get();
			const bool twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
			const auto* alpha = data.alphaProperty.get();
			const bool alphaTest = alpha && alpha->GetAlphaTesting();

			// Geometry, shared between every object drawing the same TriShape.
			auto* triShape = data.rendererData;
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
				tables.geometries.push_back(record);
			}

			PipelineKey key{ descriptors.vertex, descriptors.pixel, twoSided ? kRasterTwoSided : 0u, descriptors.pass };
			const std::uint64_t keyHash = (static_cast<std::uint64_t>(key.passDescriptor) << 32) ^ key.pixelDescriptor ^ (static_cast<std::uint64_t>(key.vertexDescriptor) << 13) ^
			                              (static_cast<std::uint64_t>(key.rasterFlags) << 62);
			auto [pipelineIt, newPipeline] = pipelineIndex.try_emplace(keyHash, static_cast<std::uint32_t>(tables.pipelines.size()));
			if (newPipeline) {
				tables.pipelines.push_back(key);
				// Per-frame PerGeometry values for this pass descriptor, from any object's lighting pass
				// (it supplies the scene light list the engine reads the sun from).
				GeometryConstants constants;
				const auto* templatePass = FindLightingPass(property);
				const bool valid = templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, mainPassRenderFlags, constants);
				tables.geometryConstants.push_back(constants);
				tables.geometryConstantsValid.push_back(valid ? 1 : 0);
			}

			// Material state as the engine's SetupMaterial produces it for this pass descriptor.
			const auto* material = property->material;
			auto [materialIt, newMaterial] = materialIndex.try_emplace(std::pair{ material, descriptors.pass }, static_cast<std::uint32_t>(tables.materials.size()));
			if (newMaterial) {
				MaterialRecord record;
				if (!evaluator.EvaluateMaterial(material, descriptors.pass, record)) {
					// No shader instance yet (nothing drawn so far): stay native this frame.
					materialIndex.erase(materialIt);
					++stats.ineligible[static_cast<std::size_t>(Ineligible::NotLightingShader)];
					--stats.ineligible[static_cast<std::size_t>(Ineligible::None)];
					continue;
				}
				tables.materials.push_back(record);
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
			               (alphaTest ? static_cast<std::uint32_t>(alpha->alphaThreshold) << kObjectAlphaThresholdShift : 0u);
			tables.objects.push_back(object);
			tables.objectGeometry.push_back(geometry);
			tables.shading.push_back(MakeShading(*static_cast<RE::BSLightingShaderProperty*>(property), descriptors, mainPassRenderFlags));
			objectIndex.emplace(geometry, objectId);

			const auto& geometryRecord = tables.geometries[object.geometryIndex];
			DrawSequence draw{};
			draw.pipelineIndex = object.pipelineIndex;
			draw.drawId = objectId;
			draw.indexBufferAddress = 0;  // resolved in Phase 2
			draw.indexBufferSize = geometryRecord.indexCount * 2;
			draw.indexFormat = kIndexFormatR16;
			draw.indexCount = geometryRecord.indexCount;
			draw.instanceCount = 1;
			draw.firstIndex = geometryRecord.firstIndex;
			draw.vertexOffset = 0;
			draw.firstInstance = 0;
			tables.draws.push_back(draw);
		}

		stats.objects = static_cast<std::uint32_t>(tables.objects.size());
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
