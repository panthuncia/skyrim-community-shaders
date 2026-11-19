// Tree LOD drawn per face by a trees view: the per-face cull, the faces' draw pass, each epoch commit's row, and the lists' units.
// The lists, rows and latch belong to Core's view machinery and are sized by the faces' data.

// ViewEpoch.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodTables.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/DrawFamilies/FamilySlots.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"

#include "RenderGraph/ComputeProgram.h"
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF::Draws
{
	namespace
	{
		constexpr std::size_t kFamily = static_cast<std::size_t>(Api::DrawFamily::TreeInstances);

		std::shared_ptr<const ViewFrame> CurrentViewFrame(const org::PassPrepareContext& a_preparation, ViewKind a_kind)
		{
			if (const auto* shapes = RevisionShapesOf(a_preparation))
				return shapes->views[ViewIndex(a_kind)];
			return nullptr;
		}

		struct TreeBindings
		{
			org::DeclaredViewToken shapes, instances;
			std::array<org::DeclaredViewToken, kMaxViewFaces> rows, visible;
		};

		struct TreePrepared
		{
			std::shared_ptr<const ComputeProgram> program;
			std::shared_ptr<const org::LatchBlock> latch;
			Features::TreeLod::TreeLodCullConstants constants{};
			std::uint32_t faces = 0;
			std::uint32_t groups = 0;
		};

		/** @brief A view's tree LOD: phase 1 culling per face, with no occlusion, into the face's list. */
		class TreeCullPass final : public org::TypedRenderGraphPass<TreeCullPass, TreePrepared, TreeBindings>
		{
		public:
			TreeCullPass(std::shared_ptr<ViewResources> a_resources, std::shared_ptr<const Features::TreeLod::TreeLodTables> a_tables) :
				resources(std::move(a_resources)), tables(std::move(a_tables)) {}

			TreeBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				TreeBindings bindings{};
				bindings.shapes = a_builder.ShaderResource(*tables->shapes).View();
				bindings.instances = a_builder.ShaderResource(*tables->instances).View();
				for (std::uint32_t f = 0; f < resources->traits->faces; ++f) {
					bindings.rows[f] = a_builder.ShaderResource(resources->faceRows[kFamily][f]).View();
					bindings.visible[f] = a_builder.UnorderedAccess(*resources->faceLists[kFamily][f]).View();
				}
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>&) const {}

			TreePrepared Prepare(const TreeBindings&, const org::PassPrepareContext& a_preparation) const
			{
				TreePrepared prepared{};
				const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
				if (!frame || !frame->latch || !frame->faces[kFamily].groups || !tables->cull)
					return prepared;
				prepared.program = tables->cull;
				prepared.latch = frame->latch;
				prepared.groups = frame->faces[kFamily].groups;
				prepared.faces = resources->traits->faces;
				auto& constants = prepared.constants;
				constants.phase = 1;
				constants.retestOffset = static_cast<std::uint32_t>(sizeof(TreeInstances::VisibleHeader));  // never written: no occlusion test
				return prepared;
			}

			static void Record(const TreeBindings& a_bindings, const TreePrepared& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.program || !a_prepared.groups || !a_prepared.latch)
					return;
				auto viewed = a_prepared.constants;
				viewed.shapesIndex = a_recording.Index(a_bindings.shapes);
				viewed.instancesIndex = a_recording.Index(a_bindings.instances);
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_prepared.program->layout->GetHandle());
				commands.BindPipeline(a_prepared.program->Pipeline());
				const auto at = a_prepared.latch->At(a_recording.Run());
				const auto region = static_cast<std::uint32_t>(at.offset);
				for (std::uint32_t f = 0; f < a_prepared.faces; ++f) {
					auto constants = viewed;
					constants.latchIndex = at.srvIndex;
					constants.latchOffset = region + f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
					constants.drawIndex = a_recording.Index(a_bindings.rows[f]);
					constants.visibleIndex = a_recording.Index(a_bindings.visible[f]);
					commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, Features::TreeLod::kTreeLodCullConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
					commands.Dispatch(a_prepared.groups, 1, 1);
				}
			}

		private:
			std::shared_ptr<ViewResources> resources;
			std::shared_ptr<const Features::TreeLod::TreeLodTables> tables;
		};
	}
}

namespace DCLF::Features::TreeLod
{
	namespace
	{
		// Only a trees view with the faces' lists gets this cull.
		std::shared_ptr<org::RenderPass> FacesCull(const Api::PassContext& a_context)
		{
			if (a_context.Interior())  // no tree LOD in an interior: its passes are left out
				return nullptr;
			const auto& resources = a_context.View();
			constexpr std::size_t family = static_cast<std::size_t>(Api::DrawFamily::TreeInstances);
			if (!resources->traits->trees || !resources->faceLists[family][0])
				return nullptr;
			auto tables = SharedTablesOf(*a_context.Main()->scene);  // the build state's main resources (the view's live main may be replaced)
			if (!tables)
				return nullptr;
			return std::make_shared<Draws::TreeCullPass>(resources, std::move(tables));
		}

		void FacesDeclare(Api::DrawFamilyDeclare& a_declare)
		{
			const auto* tables = TablesOf(a_declare.scene);
			const auto& view = *a_declare.view;
			constexpr std::size_t family = static_cast<std::size_t>(Api::DrawFamily::TreeInstances);
			if (!tables || !view.faceLists[family][0])
				return;
			const std::span<const org::SrvView> noViews{};
			auto& builder = a_declare.builder;
			for (const auto& table : { tables->shapes, tables->instances, tables->meshes })
				builder.ShaderResource(*table, noViews);
			for (std::uint32_t f = 0; f < view.traits->faces; ++f) {
				builder.ShaderResource(view.faceRows[family][f], noViews);
				a_declare.arguments[f] = builder.IndirectArguments(*view.faceLists[family][f]).Resource();
				builder.ShaderResource(*view.faceLists[family][f], noViews);
			}
		}

		void FacesRecord(const Api::DrawFamilyRecord& a_record)
		{
			std::uint32_t words[kDrawPushWords]{ a_record.row.viewSlot, a_record.row.byteOffset, 0u };
			a_record.commands.BindPipeline(a_record.pipeline);
			a_record.commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
			a_record.commands.ExecuteIndirect(a_record.signature, a_record.arguments, offsetof(TreeInstances::VisibleHeader, phaseOne), {}, 0, 1);
		}

		// The row as the last depth commit uploaded it, its texture bound again for this execution.
		// Without owners, only answer whether the row names shape slots and its pipelines and tables are in.
		bool FacesRow(Api::DrawFamilyFaceRow& a_row)
		{
			const auto& scene = a_row.scene;
			const auto* tables = TablesOf(scene);
			const auto treeLod = Draws::FamilyPipelinesOf(scene, Api::DrawFamily::TreeInstances);
			OwnedRow<TreeInstances::DrawRow> owned(tables ? tables->row : TreeInstances::DrawRow{});
			auto& row = owned.Plain();
			const bool trees = a_row.viewDraws && treeLod && tables;
			if (!a_row.owners) {
				std::memcpy(a_row.row.data(), &row, sizeof(row));
				return trees && row.shapeSlots;
			}
			// Only the view's execution reads its own latched copy, so the row's owners are the execution's.
			if (!(trees && row.shapeSlots && TreeLodTextureBinding(owned)))
				row.shapeSlots = 0;
			a_row.owners->insert(a_row.owners->end(), owned.Owners().begin(), owned.Owners().end());
			std::memcpy(a_row.row.data(), owned.Bytes(), sizeof(row));
			return row.shapeSlots != 0;
		}

		void FacesLists(Api::DrawFamilyLists& a_lists)
		{
			const auto* tables = TablesOf(a_lists.scene);
			if (!tables)
				return;
			if (a_lists.tables)
				a_lists.tables->Add(tables->shapes).Add(tables->instances).Add(tables->visible);
			if (!a_lists.inputs.empty())
				a_lists.required = a_lists.inputs[kShapeSlotsInput];
			const auto buffer = tables->shapes ? tables->shapes->Get() : nullptr;
			a_lists.current = buffer ? static_cast<std::uint32_t>(std::min<std::uint64_t>(buffer->GetSize() / sizeof(TreeInstances::ShapeRow), UINT32_MAX)) : 0u;
		}

		void FacesFace(Api::DrawFamilyFaceWrite& a_face)
		{
			TreeInstances::DrawRow commitRow{};
			std::memcpy(&commitRow, a_face.commitRow.data(), sizeof(commitRow));
			TreeInstances::DrawRow row = commitRow;
			row.visible = a_face.listRef + TreeInstances::kVisibleHeaderWords * sizeof(std::uint32_t);
			row.shapeSlots = a_face.drawn ? commitRow.shapeSlots : 0u;
			row.stats = {};  // the main draws' stage stamps only
			std::memcpy(a_face.row.data(), &row, sizeof(row));
			TreeInstances::VisibleHeader header{};
			header.phaseOne[0] = header.phaseTwo[0] = header.colour[0] = std::max(row.maxIndices, 1u);
			std::memcpy(a_face.header.data(), &header, sizeof(header));
		}
	}

	Api::DrawFamilyContribution::Faces FacesContribution()
	{
		return { .declare = &FacesDeclare,
			.record = &FacesRecord,
			.row = &FacesRow,
			.lists = &FacesLists,
			.rowName = "tree-row",
			.listName = "tree-visible",
			.rowBytes = static_cast<std::uint32_t>(sizeof(TreeInstances::DrawRow)),
			.headerBytes = static_cast<std::uint32_t>(sizeof(TreeInstances::VisibleHeader)),
			.unitWords = TreeInstances::kMaxGroupInstances,
			.cullGroup = kTreeLodCullGroup,
			.root = kFacesRoot,
			.face = &FacesFace };
	}

	void RegisterFacesCull(Api::InstallContext& a_context)
	{
		a_context.RegisterPass({ .key = "tree-lod.faces-cull", .passName = "{view}.tree-lod-cull", .make = &FacesCull, .cpuPipeline = true });
	}

	static_assert(sizeof(TreeInstances::DrawRow) == 144 && sizeof(TreeInstances::VisibleHeader) == 64 && TreeInstances::kMaxGroupInstances == 75 &&
	                  kTreeLodCullGroup == 64,
		"tree LOD's faces' sizes are not the shaders' (TreeInstancesLayout.h, TreeLodCullCS.hlsl)");
}
