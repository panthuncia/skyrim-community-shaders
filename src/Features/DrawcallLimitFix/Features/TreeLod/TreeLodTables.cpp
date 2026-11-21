// Tree LOD's scene resources, what they post and latch in the depth commit, and the depth commit's uploads into them.

// Helpers.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Gpu/NamedRefs.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodTables.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodMirror.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/Diagnostics/TreeLodEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/TreeWind.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/GraphHost.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <cstdint>
#include "Features/DrawcallLimitFix/Core/Gpu/SetupPrograms.h"

#include <cstring>
#include <memory>
#include <vector>

namespace DCLF::Features::TreeLod
{
	namespace
	{
		using TreeInstances::DrawRow;
		using TreeInstances::Instance;
		using TreeInstances::kMaxGroupInstances;
		using TreeInstances::kStageSlots;
		using TreeInstances::kStageWords;
		using TreeInstances::kVisibleHeaderWords;
		using TreeInstances::MeshRow;
		using TreeInstances::ShapeRow;
		using TreeInstances::VisibleHeader;

		Api::SceneResourcesId tablesId;  // install

		// The tables' first capacities (shape slots, mesh slots), doubled as the mirror needs.
		constexpr std::uint32_t kInitialShapes = 1024, kInitialMeshes = 256;

		std::uint32_t ShapeWords(std::uint32_t a_slots) { return a_slots * static_cast<std::uint32_t>(sizeof(ShapeRow) / 4); }
		std::uint32_t InstanceWords(std::uint32_t a_slots) { return a_slots * kMaxGroupInstances * static_cast<std::uint32_t>(sizeof(Instance) / 4); }
		std::uint32_t MeshWords(std::uint32_t a_slots) { return a_slots * static_cast<std::uint32_t>(sizeof(MeshRow) / 4); }
		std::uint32_t VisibleWords(std::uint32_t a_slots) { return kVisibleHeaderWords + 2 * a_slots * kMaxGroupInstances; }

		// The vertex stage reads every table by buffer view ref. These refs are made only to check that one can be. The draw row carries
		// each sealed version's own ref.
		RenderGraph::BufferRef RefOf(rhi::Device a_device, org::Buffer& a_buffer)
		{
			return RenderGraph::DeclaredRefOf(a_device, a_buffer);
		}

		const SetupPrograms::Compute kCullProgram{ "DCLF tree LOD cull", [] { return ComputeProgram::Desc{ .source = kTreeLodCullShader, .constantWords = kTreeLodCullConstantWords }; } };

		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto tables = std::make_shared<TreeLodTables>();
			tables->cull = kCullProgram.Get();
			const std::uint32_t shapes = a_setup.smallStart ? 4u : kInitialShapes;
			tables->shapes = Draws::MakeVersioned(Draws::CreateWords(ShapeWords(shapes), false, "cs.dclf.tree-lod-shapes"), { .extent = { 0, sizeof(ShapeRow), 4 } });
			tables->instances = Draws::MakeVersioned(Draws::CreateWords(InstanceWords(shapes), false, "cs.dclf.tree-lod-instances"),
				{ .extent = { 0, kMaxGroupInstances * sizeof(Instance), 4 } });
			tables->meshes = Draws::MakeVersioned(Draws::CreateWords(MeshWords(a_setup.smallStart ? 4u : kInitialMeshes), false, "cs.dclf.tree-lod-meshes"),
				{ .extent = { 0, sizeof(MeshRow), 4 } });
			tables->draw = Draws::CreateWords(sizeof(DrawRow) / 4, false, "cs.dclf.tree-lod-draw");
			tables->visible = Draws::MakeVersioned(Draws::CreateWords(VisibleWords(shapes), true, "cs.dclf.tree-lod-visible"),
				{ .extent = { kVisibleHeaderWords * 4, 2 * kMaxGroupInstances * 4, 4 } });
			const auto device = a_setup.device;
			tables->drawRef = RefOf(device, *tables->draw);
			if (!RefOf(device, *tables->shapes->Get()) || !RefOf(device, *tables->instances->Get()) || !RefOf(device, *tables->meshes->Get()) ||
				!tables->drawRef || !RefOf(device, *tables->visible->Get())) {
				logger::warn("[DCLF] Tree LOD's tables have no buffer view ref; tree LOD stays native");
				return nullptr;
			}
			if (debugStages) {
				tables->stages = Draws::CreateWords(kStageSlots * kStageWords, false, "cs.dclf.tree-lod-stages");
				// CS_DCLF_TREE_LOD_STAGES only. No pass declares this debug ring, so its ref stays raw.
				tables->stagesRef = RenderGraph::BufferRefOf(device, tables->stages->GetAPIResource().GetHandle());
			}
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				auto counts = std::make_shared<TreeLodCounts>();
				counts->readback = org::RunReadback::CreateRegions("cs.dclf.tree-lod-counts-readback", sizeof(VisibleHeader) + kStageSlots * kStageWords * 4,
					Draws::kReadbackRanges);
				counts->filled.assign(counts->readback->Regions(), 0u);
				tables->counts = std::move(counts);
			}
			return tables;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			const auto& tables = a_graph.State<TreeLodTables>();
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-shapes"), tables.shapes);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-instances"), tables.instances);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-meshes"), tables.meshes);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-draw"), tables.draw);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-visible"), tables.visible);
		}

		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			const auto& tables = a_requirements.State<TreeLodTables>();
			Draws::UnitGroup{}.Add(tables.shapes).Add(tables.instances).Add(tables.visible).Require(a_requirements.build, a_requirements.inputs[kShapeSlotsInput]);
			Draws::SceneBufferSet::Get().Require(a_requirements.build, tables.meshes, a_requirements.inputs[kMeshSlotsInput]);
		}

		void PostInputs(std::span<std::uint32_t> a_words)
		{
			const auto& mirror = RenderMirror();
			a_words[kShapeSlotsInput] = mirror.ShapeSlots();
			a_words[kMeshSlotsInput] = mirror.MeshSlots();
		}

		// Only the draw row and list header. The next-frame state's rows are read by the compute queue.
		void Latched(Api::SceneResourcesLatch& a_latch)
		{
			const auto& tables = a_latch.State<TreeLodTables>();
			a_latch.Latch(tables.draw, sizeof(DrawRow));
			a_latch.Latch(tables.visible, sizeof(VisibleHeader));
		}

		/** @brief Uploads the mirror's changes into the versions a_sealed names. Retired mesh leases go to the execution. */
		void UploadTreeLod(const Draws::NamedVersions& a_sealed, Mirror& a_mirror, TreeLodTables& a_tables, Draws::LatchedUploads& a_uploads,
			std::vector<std::shared_ptr<const void>>& a_owners, bool a_draw, std::uint32_t a_frame)
		{
			a_tables.ready.store(false, std::memory_order_release);
			if (!a_tables.cull || !a_tables.shapes)
				return;
			for (auto& owner : a_mirror.TakeRetired())
				a_owners.push_back(std::move(owner));
			const auto &shapes = a_sealed.Of(a_tables.shapes), &records = a_sealed.Of(a_tables.instances), &meshRows = a_sealed.Of(a_tables.meshes),
					   &visible = a_sealed.Of(a_tables.visible);
			const std::uint64_t visibleRecords = visible.bytes / 4 > kVisibleHeaderWords ? (visible.bytes / 4 - kVisibleHeaderWords) / 2 : 0;
			const auto shapeSlots = static_cast<std::uint32_t>(std::min({ shapes.bytes / sizeof(ShapeRow), records.bytes / (kMaxGroupInstances * sizeof(Instance)),
				visibleRecords / kMaxGroupInstances }));
			const auto meshSlots = static_cast<std::uint32_t>(meshRows.bytes / sizeof(MeshRow));
			// Capacities are reserved before the epoch. A mirror past them keeps its changes and draws nothing this commit.
			// A new backing holds nothing, so it is sent every slot and mesh.
			std::vector<std::uint32_t> slots, meshes;
			const bool fits = a_mirror.ShapeSlots() <= shapeSlots && a_mirror.MeshSlots() <= meshSlots;
			if (!fits) {
				a_draw = false;
			} else {
				auto& filled = a_tables.filled;
				if (!filled[0].Holds(shapes) || !filled[1].Holds(records) || !filled[2].Holds(meshRows))
					a_mirror.MarkAllChanged();
				a_mirror.TakeChanges(slots, meshes);
				filled[0].Filled(shapes);
				filled[1].Filled(records);
				filled[2].Filled(meshRows);
			}
			for (const std::uint32_t slot : slots) {
				const auto& row = a_mirror.SlotRow(slot);
				a_uploads(a_tables.shapes, &row, sizeof(row), std::uint64_t(slot) * sizeof(ShapeRow));
				if (const auto* instances = a_mirror.SlotInstances(slot); instances && !instances->empty())
					a_uploads(a_tables.instances, instances->data(), instances->size() * sizeof(Instance), std::uint64_t(slot) * kMaxGroupInstances * sizeof(Instance));
			}
			for (const std::uint32_t mesh : meshes) {
				const auto& row = a_mirror.MeshSlotRow(mesh);
				a_uploads(a_tables.meshes, &row, sizeof(row), std::uint64_t(mesh) * sizeof(MeshRow));
			}
			a_tables.slotsSent += slots.size();
			++a_tables.uploads;
			// The cull appends only while the row names shape slots, so both passes skip tree LOD together.
			OwnedRow<DrawRow> owned;
			auto& row = owned.Plain();
			row.instances = DCLF::RefOf(records);
			row.shapes = DCLF::RefOf(shapes);
			row.meshes = DCLF::RefOf(meshRows);
			row.visible = DCLF::RefOf(visible) + kVisibleHeaderWords * sizeof(std::uint32_t);
			row.alphaRef = 128.0f / 255.0f;
			row.maxIndices = std::max(a_mirror.MaxIndices(), 1u);
			row.stats = a_tables.stagesRef;
			row.frame = a_frame;
			// The frame's wind, set at the last Present like the engine's DistantTree b3. Params.z is 0 while Tree Wind is off.
			if (const auto& treeWind = globals::features::treeWind; treeWind.loaded)
				std::memcpy(row.wind, &treeWind.FrameWind(), sizeof(row.wind));
			// The engine's tree LOD draws use the worldspace's atlas and alpha reference 128/255.
			if (a_draw && a_mirror.MaxIndices() && TreeLodTextureBinding(owned))
				row.shapeSlots = shapeSlots;
			a_uploads(a_tables.draw, owned, 0);
			a_tables.row = row;
			VisibleHeader header{};
			header.phaseOne[0] = header.phaseTwo[0] = header.colour[0] = row.maxIndices;
			a_uploads(a_tables.visible, &header, sizeof(header), 0);
			if (row.shapeSlots)
				a_tables.ready.store(true, std::memory_order_release);
		}

		// The depth commit's tree LOD uploads, after the other features' hooks.
		void Commit(const Api::CommitContext& a_commit)
		{
			if (a_commit.kind != Api::CommitKind::Depth)
				return;
			auto* tables = TablesOf(a_commit.scene);
			if (!tables)
				return;
			auto& mirror = RenderMirror();
			const bool owned = IndirectDraws::Get().DrawFamilyOwned(Api::DrawFamily::TreeInstances);
			UploadTreeLod(a_commit.adopted, mirror, *tables, *a_commit.latched, a_commit.executionOwners, owned, a_commit.frame);
			// The engine's passes were withheld, so a commit that cannot draw leaves a hole.
			if (owned && !tables->ready.load(std::memory_order_acquire))
				Diagnostics::Notify(TreeLodMissed{ a_commit.frame, mirror.ShapeSlots(), a_commit.adopted.Of(tables->shapes).units });
		}
	}

	static_assert(sizeof(TreeInstances::DrawRow::wind) == sizeof(::TreeWind::WindCB), "the tree LOD draw row's wind is not TreeWindCB");

	TreeLodTables* TablesOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SceneResourcesOf<TreeLodTables>(a_scene, tablesId);
	}

	std::shared_ptr<TreeLodTables> SharedTablesOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SharedSceneResourcesOf<TreeLodTables>(a_scene, tablesId);
	}

	Api::SceneResourcesId TablesId()
	{
		return tablesId;
	}

	bool TreeLodTextureBinding(OwnedRow<TreeInstances::DrawRow>& a_row)
	{
		DCLF_ZONE("CS.DCLF.TreeLod.TextureBinding");
		using TreeInstances::DrawRow;
		const auto* texture = Engine::Global<RE::NiSourceTexture*>(Address::kTreeLodAtlas);
		auto* view = texture && texture->rendererTexture ? texture->rendererTexture->resourceView : nullptr;
		if (!view)
			return false;
		auto& textures = GpuTextures::Get();
		return a_row.Bind(&DrawRow::textureIndex, textures.ResolveBinding(view, 0)) && a_row.Bind(&DrawRow::samplerIndex, textures.SamplerBinding(0, 2));
	}

	bool TreeLodAtlasReady()
	{
		const auto* texture = Engine::Global<RE::NiSourceTexture*>(Address::kTreeLodAtlas);
		return texture && texture->rendererTexture && texture->rendererTexture->resourceView;
	}

	void RegisterTables(Api::InstallContext& a_context)
	{
		tablesId = a_context.RegisterSceneResources({ .name = "tree-lod", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .postInputs = &PostInputs,
			.inputWords = kInputWords, .latched = &Latched });
		a_context.RegisterCommitHook({ .name = "tree-lod", .commit = &Commit });
	}
}
