// The local map's scene for CS (LocalMapRenderer::Render, AE 16335 at 0x140242e00, SE 16094): its camera and roots as the render reads
// them, CS's visibility for its ortho camera (DCLF's SIMD sphere cull, no engine cull), and the pass each visible geometry makes.
// docs/development/plans/snap5-localmap2.md.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/CullSimd.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/ShaderDescriptor.h"

#include <array>
#include <cstring>
#include <vector>

namespace DCLF::Features::MenuViews::Scenes::Native
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		// LocalMapRenderer's fields the render reads.
		constexpr std::size_t kRendererCamera = 0x302b8;   // NiCamera*: the ortho local map camera
		constexpr std::size_t kRendererFogRoot = 0x30358;  // NiPointer<NiAVObject>: the fog of war's root
		// The fog of war batch runs while this byte is set (AE 0x141fd6f48, SE 0x141db3ec8).
		constexpr Engine::Offset kFogOfWar{ { 359696 }, { 501260 } };
		// A culling process's portal graph entry, the entry's graph, and the graph's list the render culls (FUN_140e28f70).
		constexpr std::size_t kProcessPortalEntry = 0x30190;
		constexpr std::size_t kEntryGraph = 0x10;
		constexpr std::size_t kGraphList = 0x58;
		// A cell's node children the render culls, in its order (the table at AE 0x14178d0b0). Child 2 is the land.
		constexpr std::array<std::uint32_t, 4> kCellChildren{ 2, 3, 6, 7 };
		constexpr std::uint32_t kLandChild = 2;
		// ShadowSceneNode[0]'s children the render culls after the portal graph's list, in its order.
		constexpr std::array<std::uint32_t, 2> kSceneChildren{ 9, 8 };

		RE::NiAVObject* ChildOf(RE::NiAVObject* a_object, std::uint32_t a_index)
		{
			auto* node = a_object ? a_object->AsNode() : nullptr;
			if (!node || a_index >= node->GetChildren().size())
				return nullptr;
			return node->GetChildren()[static_cast<std::uint16_t>(a_index)].get();
		}

		/** @brief FUN_140243ea0: a cell's node children {2 when the worldspace has land, 3, 6, 7}, culled shown by the frustum. */
		void AddCell(RE::TESObjectCELL* a_cell, bool a_land, std::vector<LocalMapRoot>& a_out)
		{
			if (!a_cell || !a_cell->GetRuntimeData().loadedData)
				return;
			auto* cell3D = a_cell->GetRuntimeData().loadedData->cell3D.get();
			for (const auto index : kCellChildren)
				if (index != kLandChild || a_land)
					if (auto* child = ChildOf(cell3D, index))
						a_out.push_back({ child, true });
		}
	}

	LocalMapScene CollectLocalMap(const void* a_renderer)
	{
		LocalMapScene scene;
		scene.camera = At<RE::NiCamera*>(a_renderer, kRendererCamera);
		scene.fogRoot = At<RE::NiPointer<RE::NiAVObject>>(a_renderer, kRendererFogRoot).get();
		scene.fogOfWar = Global<std::uint8_t>(kFogOfWar) != 0;
		auto* tes = RE::TES::GetSingleton();
		auto* interior = tes ? tes->interiorCell : nullptr;
		auto* worldSpace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		// Land draws unless the worldspace has fixed dimensions (flag 0x40); water only in an exterior that is not a small world
		// (worldspace flag 1).
		const bool land = !worldSpace || !worldSpace->flags.any(RE::TESWorldSpace::Flag::kFixedDimensions);
		scene.water = !interior && !(worldSpace && worldSpace->flags.any(RE::TESWorldSpace::Flag::kSmallWorld));
		// The loaded cells, or the interior cell; an exterior adds its worldspace's attached sky cell.
		if (interior)
			AddCell(interior, land, scene.roots);
		else if (tes && tes->gridCells) {
			const auto* grid = tes->gridCells;
			for (std::uint32_t x = 0; x < grid->length; ++x)
				for (std::uint32_t y = 0; y < grid->length; ++y)
					if (auto* cell = grid->GetCell(x, y); cell && cell->cellState == RE::TESObjectCELL::CellState::kAttached)
						AddCell(cell, land, scene.roots);
			if (auto* sky = worldSpace ? worldSpace->GetSkyCell() : nullptr; sky && sky->cellState == RE::TESObjectCELL::CellState::kAttached)
				AddCell(sky, land, scene.roots);
		}
		// The main camera's portal graph list (the first list process's entry).
		if (auto** processes = Global<std::byte**>(Engine::Address::kListProcesses); processes && processes[0])
			if (auto* entry = At<std::byte*>(processes[0], kProcessPortalEntry))
				if (auto* graph = At<std::byte*>(entry, kEntryGraph)) {
					const auto& list = At<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>(graph, kGraphList);
					for (const auto& object : list)
						if (object)
							scene.roots.push_back({ object.get(), false });
				}
		// ShadowSceneNode[0]'s children 9 and 8 (object and tree LOD).
		auto* sceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		for (const auto index : kSceneChildren)
			if (auto* child = ChildOf(sceneNode, index))
				scene.roots.push_back({ child, false });
		return scene;
	}

	void LocalMapVisible(std::span<const LocalMapRoot> a_roots, const RE::NiCamera& a_camera, std::vector<RE::BSGeometry*>& a_out)
	{
		a_out.clear();
		ShadowCameras::Planes planes;
		ShadowCameras::FrustumPlanes(const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2().viewFrustum, a_camera.world, planes);
		float active[6][4];
		std::uint32_t planeCount = 0;
		for (std::uint32_t i = 0; i < 6; ++i)
			if (planes.mask & (1u << i)) {
				for (std::uint32_t c = 0; c < 4; ++c)
					active[planeCount][c] = planes.plane[i][c];
				++planeCount;
			}
		static const auto wide = org::cpu::WideFrustum(Direct::Simd::Supported());
		const auto hidden = [](const RE::NiAVObject* a_object) { return a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden); };

		static const REL::Relocation<void (*)(RE::NiNode*, const RE::NiCamera*)> rotateToCamera{ RELOCATION_ID(69681, 71061) };
		// The objects a visible node passes on: a switch node's active child, else every child; hidden ones are left out.
		const auto children = [&](RE::NiNode& a_node, std::vector<RE::NiAVObject*>& a_out) {
			if (auto* switchNode = netimmerse_cast<RE::NiSwitchNode*>(&a_node)) {
				const auto index = switchNode->index;
				if (index >= 0 && static_cast<std::size_t>(index) < a_node.GetChildren().size())
					if (auto* child = a_node.GetChildren()[static_cast<std::uint16_t>(index)].get(); child && !hidden(child))
						a_out.push_back(child);
				return;
			}
			// A billboard turns to the culling camera in its OnVisible (AE 71063), which places its children for the draws.
			if (netimmerse_cast<RE::NiBillboardNode*>(&a_node))
				rotateToCamera(&a_node, &a_camera);
			for (auto& child : a_node.GetChildren())
				if (child && !hidden(child.get()))
					a_out.push_back(child.get());
		};

		// The process's own mode: every object outside hidden subtrees, in scene order.
		// Scratch whose capacity carries over between renders (the render owner's).
		thread_local std::vector<RE::NiAVObject*> level, next;
		next.clear();
		for (const auto& root : a_roots) {
			if (!root.object || root.cell || hidden(root.object))
				continue;
			level.assign(1, root.object);
			while (!level.empty()) {
				auto* object = level.back();
				level.pop_back();
				if (auto* geometry = object->AsGeometry())
					a_out.push_back(geometry);
				else if (auto* node = object->AsNode()) {
					next.clear();
					children(*node, next);
					level.insert(level.end(), next.rbegin(), next.rend());
				}
			}
		}

		// The cell roots: the frustum from their children down, a level at a time.
		level.clear();
		for (const auto& root : a_roots) {
			if (!root.object || !root.cell)
				continue;
			if (auto* geometry = root.object->AsGeometry())
				a_out.push_back(geometry);
			else if (auto* node = root.object->AsNode())
				children(*node, level);
		}
		thread_local std::vector<float> x, y, z, r;
		thread_local std::vector<std::uint8_t> pass;
		while (!level.empty()) {
			const auto count = static_cast<std::uint32_t>(level.size());
			x.resize(count);
			y.resize(count);
			z.resize(count);
			r.resize(count);
			pass.assign(count, 0);
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& bound = level[i]->worldBound;
				x[i] = bound.center.x;
				y[i] = bound.center.y;
				z[i] = bound.center.z;
				r[i] = bound.radius;
			}
			const org::cpu::Spheres spheres{ x.data(), y.data(), z.data(), r.data() };
			if (wide)
				wide(spheres, count, active, planeCount, nullptr, pass.data());
			else
				org::cpu::FrustumSse(spheres, count, active, planeCount, pass.data());
			next.clear();
			for (std::uint32_t i = 0; i < count; ++i) {
				if (!pass[i])
					continue;
				auto* object = level[i];
				if (auto* geometry = object->AsGeometry()) {
					a_out.push_back(geometry);
					continue;
				}
				if (auto* node = object->AsNode())
					children(*node, next);
			}
			std::swap(level, next);
		}
	}

	LocalMapPass LocalMapPassOf(const RE::BSGeometry& a_geometry, View a_view, bool a_water)
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		if (!property)
			return LocalMapPass::None;
		if (netimmerse_cast<const RE::BSLightingShaderProperty*>(property)) {
			// GetRenderPasses_LocalMap hands a landscape property to its own GetRenderPasses (vtable +0x150); the fog of war's mode has none.
			if (property->flags.any(Flag::kMultiTextureLandscape))
				return a_view == View::LocalMap ? LocalMapPass::Land : LocalMapPass::None;
			return LocalMapPass::Utility;
		}
		if (a_view != View::LocalMap)
			return LocalMapPass::None;
		if (netimmerse_cast<const RE::BSWaterShaderProperty*>(property))
			return a_water ? LocalMapPass::Water : LocalMapPass::None;
		if (netimmerse_cast<const RE::BSDistantTreeShaderProperty*>(property))
			return LocalMapPass::Tree;
		return LocalMapPass::None;
	}

	void SetLocalMapTreeDetail()
	{
		// FUN_140438580(BSTreeManager, 1) (AE 0x140438580): each loaded tree's switch node (+0x180) shows its child 1, or its last.
		auto* manager = RE::BSTreeManager::GetSingleton();
		if (!manager)
			return;
		for (const auto& tree : manager->loadedTrees) {
			auto* node = tree ? At<std::byte*>(tree.get(), 0x180) : nullptr;
			if (!node)
				continue;
			const auto size = At<std::uint16_t>(node, 0x124);
			if (!size)
				continue;
			const auto index = (std::min)(1, static_cast<int>(size) - 1);
			if (index > -2 && index < static_cast<int>(At<std::uint16_t>(node, 0x122)))
				At<std::int32_t>(node, 0x12C) = index;
		}
	}

	Inputs SampleLocalMapInputs(const RE::NiCamera& a_camera)
	{
		// The render makes ShadowSceneNode[0] its accumulator's scene node (+0x148): the land's lights and fog are the world's.
		auto in = SampleSceneInputs(RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0], a_camera, PreviousCamera{}, nullptr);
		in.localMap = true;
		// EyePosition is the current accumulator's eye less posAdjust (AE 0x1414de12c); the local map's accumulator keeps a zero eye, so
		// the land reads the camera's position negated.
		for (std::uint32_t c = 0; c < 3; ++c)
			in.pipeline.eyePosition[c] = 0.0f - (&in.eye.x)[c];
		in.pipeline.eye = true;
		return in;
	}

	Water::Port::FrameInputs SampleLocalMapWater(const Inputs& a_in, bool a_water)
	{
		auto f = Water::Port::CaptureFrame(nullptr);
		// SetCameraData put the local map camera in the shadow state: posAdjust is its eye, no previous one, its own matrices.
		f.posAdjust = { a_in.eye.x, a_in.eye.y, a_in.eye.z };
		f.previousPosAdjust = {};
		f.viewProj = a_in.camera.viewProj;
		f.proj00 = a_in.camera.proj[0];
		f.proj11 = a_in.camera.proj[5];
		// SubmitAccumulator makes the local map camera the current camera (CameraDataWater's planes); its accumulator's eye stays zero.
		f.cameraNear = a_in.camera.frustum.fNear;
		f.cameraFar = a_in.camera.frustum.fFar;
		f.eyePosition = {};
		f.waterEnabled = a_water;
		f.localMapPasses = true;
		f.renderMode = 0x12;
		f.rawWater = false;
		f.mapMenuOpen = true;
		return f;
	}

	namespace
	{
		// BSWaterShaderMaterial's first normal map (SetupGeometry).
		constexpr std::size_t kNormal1 = 0x40;
		// The water shader (GetRenderPasses' pass shader) and the white texture a normal map slot without a texture takes.
		constexpr Engine::Offset kWaterShader{ { 415108 }, { 528163 } };
		constexpr Engine::Offset kWhiteTexture{ { 411503 }, { 525021 } };

		/** @brief Writes a port row group into latch rows: the floats the port wrote, zero elsewhere. */
		template <std::size_t N>
		void PutRows(const std::array<Water::Port::Row, N>& a_rows, float (*a_out)[4])
		{
			for (std::size_t r = 0; r < N; ++r)
				for (std::uint32_t c = 0; c < 4; ++c)
					a_out[r][c] = ((a_rows[r].written >> c) & 1u) ? a_rows[r].v[c] : 0.0f;
		}
	}

	RE::BSShader* WaterShader()
	{
		return Global<RE::BSShader*>(kWaterShader);
	}

	bool ComputeWater(const Water::Port::FrameInputs& a_frame, const RE::BSGeometry& a_geometry, WaterResult& a_out, std::string& a_why)
	{
		namespace Port = Water::Port;
		a_out = {};
		const auto* property = netimmerse_cast<const RE::BSWaterShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
		const auto* material = property ? static_cast<const RE::BSWaterShaderMaterial*>(property->material) : nullptr;
		if (!material || material->GetType() != RE::BSShaderMaterial::Type::kWater || a_geometry.GetType().get() != RE::BSGeometry::Type::kTriShape) {
			a_why = "not a water TriShape with a water material";
			return false;
		}
		auto* shader = WaterShader();
		if (!shader) {
			a_why = "no water shader";
			return false;
		}
		const auto object = Port::CaptureObject(*property, a_geometry);
		// The local map's pass: LOD with normal texture coordinates, +1 with vertex colours. Observed: the engine's local map passes carry
		// no Reflections or Cubemap bit, whatever the property's flags 0x40 and 0x4000.
		const std::uint32_t descriptor = 0x4802u + (((object.vertexDescLow >> 0x16) & 0x3Cu) ? 1u : 0u);
		a_out.descriptor = descriptor;
		a_out.shader = shader;
		a_out.vertex = a_out.pixel = descriptor;
		globals::state->ModifyShaderLookup(*shader, a_out.vertex, a_out.pixel);
		const auto* vs = globals::shaderCache->GetVertexShader(*shader, a_out.vertex);
		const auto* ps = globals::shaderCache->GetPixelShader(*shader, a_out.pixel);
		if (!vs || !ps) {
			a_why = std::format("no water shader for vs {:#x} ps {:#x}", a_out.vertex, a_out.pixel);
			return false;
		}
		a_out.vertexDesc = vs->vertexDesc;
		Port::ConstantTables tables;
		std::memcpy(tables.vs.data(), vs->constantTable.data(), tables.vs.size());
		std::memcpy(tables.ps.data(), ps->constantTable.data(), tables.ps.size());
		const auto bytes = Port::BytesOf(*material);
		const auto rows = Port::Assemble(Port::EvaluateTechnique(descriptor, a_frame), Port::EvaluateMaterial(bytes, a_frame), Port::EvaluateObject(object, descriptor, a_frame),
			Port::EvaluateGeometry(object, descriptor, a_frame), Port::PassLights{}, tables);
		auto& c = a_out.constants;
		PutRows(rows.vsTechnique, c.vsTechnique);
		PutRows(rows.vsMaterial, c.vsMaterial);
		PutRows(rows.vsGeometry, c.vsGeometry);
		PutRows(rows.psTechnique, c.psTechnique);
		PutRows(rows.psMaterial, c.psMaterial);
		PutRows(rows.psGeometry, c.psGeometry);
		// Observed: the local map's water draws find TextureProj (PS b2 c0-c3) zero; LOD's pixel stage reads none of it.
		for (std::uint32_t r = 0; r < 4; ++r)
			for (std::uint32_t i = 0; i < 4; ++i)
				c.psGeometry[r][i] = 0.0f;
		// CS's permutation block, as its BeginTechnique hook sets it.
		const auto& permutation = globals::state->permutationData;
		c.descriptors[0] = descriptor;
		c.descriptors[1] = Util::ShaderDescriptor::UploadedPixelDescriptor(descriptor, a_out.pixel, State::GetRuntimePixelBits(*shader));
		c.descriptors[2] = permutation.ExtraShaderDescriptor;
		c.descriptors[3] = permutation.ExtraFeatureDescriptor;
		c.effectRadius = permutation.EffectRadius;
		// The states: SetupGeometry's water stencil test (mode 30, reference 1) and depth test without write; the batch leaves no blend,
		// colour writes (mode 1) and no culling.
		a_out.state.blend = 0;
		a_out.state.write = 1;
		a_out.state.depth = 1;
		a_out.state.cull = 0;
		a_out.state.stencil = 30;
		a_out.state.stencilRef = 1;
		// The texture LOD's pixel stage reads: t4, the first normal map (or the white texture). The local map's descriptor carries no
		// Reflections bit, so t3 stays null.
		const void* normals = nullptr;
		std::memcpy(&normals, bytes.data() + kNormal1, sizeof(normals));
		a_out.normals = { MaterialPort::ViewOf(normals ? normals : Global<const void*>(kWhiteTexture)), 3, 3 };
		return true;
	}
}
