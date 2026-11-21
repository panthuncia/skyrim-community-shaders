// Water's rows beside Core's (plan section 3.2, items 3 and 4):
// - the water row: a table family with one WaterObjectRow per resident water object, named by the object record's
//   kWaterRecordWord as tree wind's slot is. Written on the scene lane from the property record, so only on events.
//   Its GPU table is a scene resource read at t42 (Indirect::kObjectRowsRegister), as tree wind's tree rows are: the commits
//   upload the adopted publication's rows whenever they changed, and the fit holds back an object whose row is past the table.
// - the water material pool: a frame-value pool of one WaterMaterialRow per water material, read at t58. The WaterMaterial
//   shading field names each slot's row. Each row is Port::EvaluateMaterial with this frame's inputs, since fog, the water
//   lighting and the timer move every frame.
// - the frame row: a one-row frame-value pool read at t59, Port::TechniqueRows plus the frame inputs the shader needs.
// The frame inputs are Port::CaptureFrame at the frame's start, on the render thread, in the shading field's capture.
// Advanced Skin's material table (Integrations/AdvancedSkin/SkinMaterials.cpp) and the landscape seams' quad table
// (Features/Landscape/LandscapeSeams.cpp) are the models.

#include "Features/DrawcallLimitFix/Features/Water/Scene/WaterScene.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneMirror.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Features/Water/Draw/WaterDraw.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"

#include <ORGModuleServices/GrowableBufferTypes.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DCLF::Features::Water::WaterScene
{
	namespace
	{
		// ---- The water row's table family.

		Api::TableFamilyId rowFamily;           // install
		Api::CoordinatorComponentId component;  // install

		/**
		 * @brief The family's columns: the rows, their free list and the generation counter the tags come from. contentEpoch moves at
		 * every row write, so a commit knows when the GPU table must be uploaded again.
		 */
		struct WaterRows
		{
			std::vector<WaterObjectRow> rows;
			std::vector<std::uint32_t> free;
			std::uint32_t generations = 0;
			std::uint64_t contentEpoch = 0;
		};

		std::shared_ptr<void> MakeRows() { return std::make_shared<WaterRows>(); }

		void AssignRows(void* a_to, const void* a_from)
		{
			*static_cast<WaterRows*>(a_to) = *static_cast<const WaterRows*>(a_from);
		}

		bool SameRows(const void* a_left, const void* a_right)
		{
			const auto& x = *static_cast<const WaterRows*>(a_left);
			const auto& y = *static_cast<const WaterRows*>(a_right);
			return x.generations == y.generations && x.free == y.free && x.rows.size() == y.rows.size() &&
			       (x.rows.empty() || std::memcmp(x.rows.data(), y.rows.data(), x.rows.size() * sizeof(WaterObjectRow)) == 0);
		}

		void ClearRows(void* a_columns)
		{
			auto& rows = *static_cast<WaterRows*>(a_columns);
			rows.rows.clear();
			rows.free.clear();
		}

		void RecycleRow(void* a_columns, std::uint32_t a_row)
		{
			static_cast<WaterRows*>(a_columns)->free.push_back(a_row);
		}

		std::shared_ptr<void> MakeRowsComponent() { return std::make_shared<int>(0); }

		std::uint32_t RowCount(const void* a_columns)
		{
			return static_cast<std::uint32_t>(static_cast<const WaterRows*>(a_columns)->rows.size());
		}

		/** @brief The row a water property record holds now. Pure. */
		WaterObjectRow RowOf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
		{
			WaterObjectRow row;
			const auto& values = a_property.projected;
			row.cellX = std::bit_cast<std::int32_t>(values[kValueOffsets + 0]);
			row.cellY = std::bit_cast<std::int32_t>(values[kValueOffsets + 1]);
			row.flowX = std::bit_cast<std::int32_t>(values[kValueOffsets + 2]);
			row.flowY = std::bit_cast<std::int32_t>(values[kValueOffsets + 3]);
			row.plane = { values[kValuePlane + 0], values[kValuePlane + 1], values[kValuePlane + 2], values[kValuePlane + 3] };
			const auto classification = ClassifyLeaf(a_property, a_leaf);
			row.waterFlags = classification.waterFlags;
			row.stencilBit = classification.stencilBit;
			row.surfaceDescriptor = classification.surfaceDescriptor;
			return row;
		}

		/** @brief A resident's row given back, if it has one. */
		void UnlistWater(const Api::CoordinatorComponentContext& a_context, std::uint32_t a_slot)
		{
			auto& tables = a_context.tables;
			// Every other resident passes here, so it only reads until a row is found.
			if (const auto* found = tables.families.Find(rowFamily); !found || found->RowOf(a_slot) == Extensions::kNoFamilyRow)
				return;
			auto& family = tables.families.Mutable(rowFamily);
			auto& current = family.objectRow[a_slot];
			family.NoteWrite();
			tables.Retire(SceneTables::kRetiredFamilyRow, current, rowFamily.index);
			current = Extensions::kNoFamilyRow;
			a_context.NoteTouched(a_slot);
		}

		/** @brief A resident's row: taken for a water object and kept up to date, given back for anything else. */
		void ListWater(const Api::CoordinatorComponentContext& a_context, std::uint32_t a_slot)
		{
			auto& tables = a_context.tables;
			const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
			const auto leaf = geometry ? a_context.Mirror().Leaf(geometry) : SceneCapture::LeafView{};
			const auto* property = SceneCapture::LeafView::Port(leaf.property);
			if (!property || property->port != kMaterialKindWater || !(property->feature & kFeatureCaptured)) {
				UnlistWater(a_context, a_slot);
				return;
			}
			auto& family = tables.families.Mutable(rowFamily);
			auto& rows = *static_cast<WaterRows*>(family.columns.get());
			if (family.objectRow.size() <= a_slot) {
				family.NoteWrite();
				family.objectRow.resize(std::size_t(a_slot) + 1, Extensions::kNoFamilyRow);
			}
			WaterObjectRow row = RowOf(*property, leaf);
			auto& current = family.objectRow[a_slot];
			if (current != Extensions::kNoFamilyRow && current < rows.rows.size()) {
				row.generation = rows.rows[current].generation;
				if (std::memcmp(&row, &rows.rows[current], sizeof(row)) == 0)
					return;
				rows.rows[current] = row;
				++rows.contentEpoch;
				family.NoteWrite();
				return;
			}
			std::uint32_t at;
			if (!rows.free.empty()) {
				at = rows.free.back();
				rows.free.pop_back();
			} else {
				at = static_cast<std::uint32_t>(rows.rows.size());
				rows.rows.emplace_back();
				family.rowTag.push_back(0);
			}
			row.generation = ++rows.generations;
			rows.rows[at] = row;
			++rows.contentEpoch;
			family.rowTag[at] = row.generation;
			current = at;
			family.NoteWrite();
			a_context.NoteTouched(a_slot);
		}

		// ---- The rows' GPU table at t42. Setup and the commits on the render thread, the requirement on the snapshot builder.

		Api::SceneResourcesId rowsScene;  // install
		constexpr std::uint32_t kInitialWaterRows = 256;

		/** @brief The rows' table, made once at setup. Commits write it on the render thread. */
		struct WaterRowsScene
		{
			Draws::Versioned rows;
			org::services::FilledContents<> filled;  // WaterRows::contentEpoch of the version last uploaded
		};

		const WaterRows* AdoptedRowsOf(const SceneTables& a_tables) { return a_tables.families.Columns<WaterRows>(rowFamily); }

		std::shared_ptr<void> SetupRowsScene(const Api::SceneResourcesSetup& a_setup)
		{
			auto scene = std::make_shared<WaterRowsScene>();
			const std::uint32_t rows = a_setup.smallStart ? 4u : kInitialWaterRows;
			scene->rows = Draws::MakeVersioned(a_setup.StructuredBuffer(rows, sizeof(WaterObjectRow), "cs.dclf.water-rows"),
				{ .extent = { 0, sizeof(WaterObjectRow), sizeof(WaterObjectRow) } });
			return scene;
		}

		void RegisterRowsGraph(const Api::SceneResourcesGraph& a_graph)
		{
			if (const auto& scene = a_graph.State<WaterRowsScene>(); scene.rows)
				Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.water-rows"), scene.rows);
		}

		void RequireRows(const Api::SceneResourcesRequirements& a_requirements)
		{
			const auto& scene = a_requirements.State<WaterRowsScene>();
			const auto* rows = AdoptedRowsOf(a_requirements.tables);
			if (scene.rows && rows)
				Draws::UnitGroup{}.Add(scene.rows).Require(a_requirements.build, rows->rows.size());
		}

		void FitRows(Api::SceneResourcesFit& a_fit)
		{
			const auto& scene = a_fit.State<WaterRowsScene>();
			if (scene.rows)
				a_fit.fit.familyRows[rowFamily.index] = static_cast<std::uint32_t>(std::min<std::uint64_t>(Draws::NamedUnits(a_fit.sealed, scene.rows), UINT32_MAX));
		}

		WaterRowsScene* RowsSceneOf(const Draws::SceneBuffers& a_scene) { return Extensions::SceneResourcesOf<WaterRowsScene>(a_scene, rowsScene); }

		/** @brief Every commit uploads the adopted rows into the version it writes, once per change of their contents. */
		void CommitRows(const Api::CommitContext& a_commit)
		{
			auto* scene = RowsSceneOf(a_commit.scene);
			const auto* rows = AdoptedRowsOf(a_commit.tables);
			if (!scene || !scene->rows || !rows)
				return;
			const auto& version = a_commit.adopted.Of(scene->rows);
			if (scene->filled.Since(version) == rows->contentEpoch || rows->rows.size() > version.units)
				return;
			if (!rows->rows.empty())
				a_commit.uploads(scene->rows, rows->rows.data(), rows->rows.size() * sizeof(WaterObjectRow), 0);
			scene->filled.Filled(version, rows->contentEpoch);
		}

		void SelectRows(Api::FrameBindingSelect& a_select)
		{
			if (const auto* scene = RowsSceneOf(a_select.scene); scene && scene->rows)
				a_select.buffer = &scene->rows;
		}

		// ---- The frame inputs, the material pool and the frame row. Render thread, apart from the producers.

		/** @brief A pool row: the material it evaluates and how many member slots name it. */
		struct MaterialEntry
		{
			const void* material = nullptr;
			std::uint32_t users = 0;
		};

		struct MemberSlot
		{
			const RE::BSGeometry* geometry = nullptr;
			const void* material = nullptr;
			std::uint32_t row = 0;
		};

		struct PoolState
		{
			// Row 0 is no material. A slot naming it reads zeros.
			std::vector<MaterialEntry> rows = std::vector<MaterialEntry>(1);
			std::vector<std::uint32_t> freeRows;
			std::unordered_map<const void*, std::uint32_t> rowOf;
			std::unordered_map<std::uint32_t, MemberSlot> members;
			Port::FrameInputs frame;
			bool haveFrame = false;
		};

		PoolState& Pool()
		{
			static PoolState state;
			return state;
		}

		std::uint32_t AcquireRow(PoolState& a_state, const void* a_material)
		{
			if (const auto it = a_state.rowOf.find(a_material); it != a_state.rowOf.end()) {
				++a_state.rows[it->second].users;
				return it->second;
			}
			std::uint32_t row;
			if (!a_state.freeRows.empty()) {
				row = a_state.freeRows.back();
				a_state.freeRows.pop_back();
			} else {
				row = static_cast<std::uint32_t>(a_state.rows.size());
				a_state.rows.emplace_back();
			}
			a_state.rows[row] = { a_material, 1 };
			a_state.rowOf.emplace(a_material, row);
			return row;
		}

		// No slot names a released row, so its stale bytes are never read. The pool never shrinks.
		void ReleaseRow(PoolState& a_state, std::uint32_t a_row)
		{
			if (!a_row || --a_state.rows[a_row].users)
				return;
			a_state.rowOf.erase(a_state.rows[a_row].material);
			a_state.rows[a_row] = {};
			a_state.freeRows.push_back(a_row);
		}

		/** @brief a_slot's water material now, or null when its geometry is not water. */
		const void* WaterMaterialOf(const RE::BSGeometry* a_geometry)
		{
			const auto* property = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			if (!property || property->GetRTTI() != Extensions::MaterialPorts()[kMaterialKindWater].rtti)
				return nullptr;
			return property->material;
		}

		void PutRow(const Api::ShadingFieldCapture& a_capture, std::uint32_t a_slot, std::uint32_t a_row)
		{
			float value[1];
			std::memcpy(value, &a_row, sizeof(a_row));
			a_capture.Put(a_slot, value);
		}

		/** @brief Brings a_slot's row up to date. Returns false when it is not a water member. */
		bool ResolveSlot(PoolState& a_state, const Api::ShadingFieldCapture& a_capture, std::uint32_t a_slot)
		{
			auto* geometry = a_slot < a_capture.objectGeometry.size() ? a_capture.objectGeometry[a_slot] : nullptr;
			const void* material = WaterMaterialOf(geometry);
			const auto it = a_state.members.find(a_slot);
			if (!material) {
				if (it != a_state.members.end()) {
					ReleaseRow(a_state, it->second.row);
					a_state.members.erase(it);
					PutRow(a_capture, a_slot, 0);
				}
				return false;
			}
			if (it != a_state.members.end() && it->second.geometry == geometry && it->second.material == material)
				return true;
			const std::uint32_t row = AcquireRow(a_state, material);
			if (it != a_state.members.end())
				ReleaseRow(a_state, it->second.row);
			a_state.members.insert_or_assign(a_slot, MemberSlot{ geometry, material, row });
			PutRow(a_capture, a_slot, row);
			return true;
		}

		void CaptureMaterials(Api::ShadingFieldCapture& a_capture)
		{
			auto& state = Pool();
			static REL::Relocation<RE::BSShaderAccumulator**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
			state.frame = Port::CaptureFrame(*mainAccumulator);
			state.haveFrame = true;
			for (const auto slot : a_capture.changedObjects)
				ResolveSlot(state, a_capture, slot);
			// A material swap or a reused slot shows here even when its shading was not published again.
			std::vector<std::uint32_t> slots;
			slots.reserve(state.members.size());
			for (const auto& [slot, member] : state.members)
				slots.push_back(slot);
			for (const auto slot : slots)
				ResolveSlot(state, a_capture, slot);
		}

		Api::FrameValuePoolId materialPool, framePool;  // install

		struct MaterialTableInput
		{
			std::vector<std::pair<std::uint32_t, WaterMaterialRow>> rows;
			std::uint32_t count = 0;
		};

		WaterMaterialRow GpuRowOf(const Port::MaterialRow& a_row)
		{
			WaterMaterialRow out;
			const auto copy = [](const Port::Row& a_from, std::array<float, 4>& a_to) {
				for (std::uint32_t c = 0; c < 4; ++c)
					a_to[c] = (a_from.written & (1u << c)) ? a_from.v[c] : 0.0f;
			};
			for (std::size_t r = 0; r < out.vs.size(); ++r)
				copy(a_row.vs[r], out.vs[r]);
			for (std::size_t r = 0; r < out.ps.size(); ++r)
				copy(a_row.ps[r], out.ps[r]);
			return out;
		}

		// The producers run one at a time in frame order.
		KeptWholeArray<WaterMaterialRow> producedMaterials;
		KeptWholeArray<WaterFrameRow> producedFrame;

		/** @brief Render thread, right after this frame's capture: every live row, evaluated with this frame. */
		Api::FrameValueInput KickMaterials(const Api::FrameValuePoolKick&)
		{
			auto& state = Pool();
			const auto count = static_cast<std::uint32_t>(state.rows.size());
			auto input = std::make_shared<MaterialTableInput>();
			input->count = count;
			if (state.haveFrame)
				for (std::uint32_t r = 1; r < count; ++r)
					if (const auto* material = static_cast<const RE::BSWaterShaderMaterial*>(state.rows[r].material))
						input->rows.emplace_back(r, GpuRowOf(Port::EvaluateMaterial(Port::BytesOf(*material), state.frame)));
			return { std::move(input), count };
		}

		void SampleMaterials(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.BeginBuild(producedMaterials);
			auto& rows = producedMaterials.Mutable();
			const auto* input = a_producer.Input<MaterialTableInput>();
			const std::uint32_t count = input ? input->count : 0u;
			if (rows.size() < count) {
				const std::size_t first = rows.size();
				rows.resize(count, WaterMaterialRow{});
				producedMaterials.MarkRange(first, count - first);
			}
			if (input)
				for (const auto& [at, row] : input->rows) {
					if (at >= rows.size() || std::memcmp(&rows[at], &row, sizeof(row)) == 0)
						continue;
					rows[at] = row;
					producedMaterials.Mark(at);
				}
		}

		void SendMaterials(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(producedMaterials);
		}

		void SelectMaterials(Api::FrameBindingSelect& a_select)
		{
			if (a_select.renderThread)
				a_select.index = FrameValues::Get().PoolIndex(materialPool);
		}

		std::array<float, 4> Float4(const Port::Row& a_row)
		{
			std::array<float, 4> out{};
			for (std::uint32_t c = 0; c < 4; ++c)
				out[c] = (a_row.written & (1u << c)) ? a_row.v[c] : 0.0f;
			return out;
		}

		std::array<float, 4> Bits(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z, std::uint32_t a_w)
		{
			return { std::bit_cast<float>(a_x), std::bit_cast<float>(a_y), std::bit_cast<float>(a_z), std::bit_cast<float>(a_w) };
		}

		WaterFrameRow FrameRowOf(const Port::FrameInputs& a_frame)
		{
			using enum FrameRowEntry;
			WaterFrameRow out;
			auto at = [&](FrameRowEntry a_entry) -> std::array<float, 4>& { return out.rows[static_cast<std::size_t>(a_entry)]; };
			// With the Depth bit, so VPOSOffset is written. The other rows do not depend on the descriptor.
			const auto technique = Port::EvaluateTechnique(Port::Flag::kDepth, a_frame);
			at(QPosAdjust) = Float4(technique.qPosAdjust);
			at(VposOffset) = Float4(technique.vposOffset);
			at(PosAdjust) = Float4(technique.posAdjust);
			at(CameraData) = Float4(technique.cameraData);
			at(SunDir) = Float4(technique.sunDir);
			at(SunColor) = Float4(technique.sunColor);
			at(ProjData) = { a_frame.proj00, a_frame.proj11, 0.0f, 0.0f };
			at(PreviousPosAdjust) = { a_frame.previousPosAdjust[0], a_frame.previousPosAdjust[1], a_frame.previousPosAdjust[2], a_frame.invFrameBufferRange };
			at(Eye) = { a_frame.eyePosition[0], a_frame.eyePosition[1], a_frame.eyePosition[2], a_frame.timer };
			const auto& uw = a_frame.unifiedWater;
			at(Flowmap) = { static_cast<float>(a_frame.flowmapSize), a_frame.meshFlowCellOffset[0], a_frame.meshFlowCellOffset[1], uw.active ? 1.0f : 0.0f };
			at(UnifiedWaterFlow) = { static_cast<float>(uw.width), static_cast<float>(uw.height), static_cast<float>(uw.offsetX), static_cast<float>(uw.offsetY) };
			at(DisplacementOffset) = a_frame.displacementOffset;
			at(WaterLighting) = a_frame.waterLighting;
			at(Flags) = Bits(a_frame.cameraUnderwater, a_frame.interior, a_frame.rawWater, a_frame.linearLighting);
			for (std::size_t r = 0; r < 4; ++r)
				for (std::size_t c = 0; c < 4; ++c)
					out.rows[static_cast<std::size_t>(ViewProj0) + r][c] = a_frame.viewProj[r * 4 + c];
			return out;
		}

		Api::FrameValueInput KickFrame(const Api::FrameValuePoolKick&)
		{
			const auto& state = Pool();
			return { std::make_shared<const WaterFrameRow>(state.haveFrame ? FrameRowOf(state.frame) : WaterFrameRow{}), 1 };
		}

		void SampleFrame(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.BeginBuild(producedFrame);
			auto& rows = producedFrame.Mutable();
			if (rows.empty()) {
				rows.resize(1, WaterFrameRow{});
				producedFrame.MarkRange(0, 1);
			}
			if (const auto* input = a_producer.Input<WaterFrameRow>(); input && std::memcmp(&rows[0], input, sizeof(WaterFrameRow)) != 0) {
				rows[0] = *input;
				producedFrame.Mark(0);
			}
		}

		void SendFrame(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(producedFrame);
		}

		void SelectFrame(Api::FrameBindingSelect& a_select)
		{
			if (a_select.renderThread)
				a_select.index = FrameValues::Get().PoolIndex(framePool);
		}
	}

	PoolSizes CurrentPoolSizes()
	{
		const auto& state = Pool();
		return { static_cast<std::uint32_t>(state.rowOf.size()), static_cast<std::uint32_t>(state.members.size()), state.haveFrame && state.frame.cameraUnderwater };
	}

	void RegisterRows(Api::InstallContext& a_context)
	{
		rowFamily = a_context.RegisterTableFamily({ .name = "water rows", .make = &MakeRows, .assign = &AssignRows, .same = &SameRows, .clear = &ClearRows,
			.recycle = &RecycleRow, .recordWord = kWaterRecordWord, .rows = &RowCount });
		component = a_context.RegisterCoordinatorComponent({ .name = "water-rows", .make = &MakeRowsComponent, .residency = { .resident = &ListWater, .left = &UnlistWater } });
		rowsScene = a_context.RegisterSceneResources({ .name = "water-rows", .setup = &SetupRowsScene, .registerGraph = &RegisterRowsGraph, .require = &RequireRows, .fit = &FitRows });
		a_context.RegisterCommitHook({ .name = "water-rows", .commit = &CommitRows });
		a_context.RegisterFrameBinding({ .name = "water object rows SRV", .textureRegister = Indirect::kObjectRowsRegister, .select = &SelectRows });
		framePool = a_context.RegisterFrameValuePool({ .name = "water-frame", .stride = sizeof(WaterFrameRow), .kick = &KickFrame, .sample = &SampleFrame, .send = &SendFrame });
		materialPool = a_context.RegisterFrameValuePool(
			{ .name = "water-materials", .stride = sizeof(WaterMaterialRow), .kick = &KickMaterials, .sample = &SampleMaterials, .send = &SendMaterials });
		a_context.RegisterFrameBinding({ .name = "water frame row SRV", .textureRegister = kFrameRowRegister, .select = &SelectFrame });
		a_context.RegisterFrameBinding({ .name = "water materials SRV", .textureRegister = kMaterialPoolRegister, .select = &SelectMaterials });
		a_context.RegisterShadingField({ .name = "water material", .field = Api::ShadingRowField::WaterMaterial, .capture = &CaptureMaterials });
	}
}
