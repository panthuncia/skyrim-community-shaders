#pragma once

// SceneTables holds the scene's object, geometry, pipeline and material slots and every column parallel to them, plus the change
// logs and the retirement list. The coordinator writes the live copy on the scene lane. A publication carries an immutable copy
// that the render thread and the builds read.

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Api/MainRanges.h"
#include "Features/DrawcallLimitFix/Api/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include <ORGModuleServices/Async/SlotTable.h>
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF
{
	inline constexpr std::uint32_t kNoObjectSlot = ~0u;

	/** @brief What a change-log entry changed, grouped by the columns its consumers read. */
	enum ChangeCause : std::uint32_t
	{
		kChangeBindings = 1u << 0,    // flags, material, pipeline (the draw's too), the fade distance, whether it has a fade node
		kChangeAncestorValue = 1u << 1,  // the ancestor value (Integrations/LightLimitFix's room index)
		kChangeRetired2 = 1u << 2,    // retired (was the record's tree animation, never written); noted only within kChangeAll
		kChangeSkin = 1u << 3,        // skin partitions, palette block (offset and rows)
		kChangeExtras = 1u << 4,      // the extras block and its rows
		kChangeShadow = 1u << 5,      // shadow technique and reason, sky technique, shadow material and diffuse
		kChangeGeometry = 1u << 6,    // geometry slot, the draw's geometry half, face stream, the object's geometry
		kChangeMembership = 1u << 7,  // residency
	};
	// Placement, palette and shading are FrameValues' rows, never a column.
	inline constexpr std::uint32_t kChangeCauseCount = 8;
	// A caster's bound and sun entry come from its record, so placement is not a shadow input.
	inline constexpr std::uint32_t kShadowChangeCauses = kChangeShadow | kChangeBindings | kChangeGeometry | kChangeSkin | kChangeMembership;
	// kChangeAll notes still set the retired bit, so it keeps its name in reports.
	inline constexpr std::array<const char*, kChangeCauseCount> kChangeCauseNames{ "bindings", "lights", "tree", "skin", "extras", "shadow", "geometry",
		"membership" };
	inline constexpr std::uint32_t kChangeAll = (1u << kChangeCauseCount) - 1;

	/** @brief The flags the accumulate patch keeps from the scene phase. It owns the rest. */
	inline constexpr std::uint32_t kSceneKeptFlags = kObjectPaletteBlock | kObjectNoShadow | kObjectVolumetricOnly | kObjectShadowOnly;

	/** @brief The coordinator writes its own copy. The frame and the draws read a publication's immutable copy. */
	struct SceneTables
	{
		std::vector<ObjectRecord> objects;
		std::vector<RE::BSGeometry*> objectGeometry;  // parallel to objects
		std::vector<GeometryBuffers> geometries;
		struct GeometryImport
		{
			std::uint64_t vertexGeneration = 0, indexGeneration = 0;
			std::shared_ptr<const void> vertexOwner, indexOwner;
		};
		// Import leases are independent of GpuResources' age-based cache.
		std::vector<GeometryImport> geometryImports;  // parallel to geometries
		std::vector<PipelineKey> pipelines;
		std::vector<MaterialRecord> materials;
		// Session-unique, new whenever the record is evaluated again but not when only its frame floats change.
		// Consumers compare this instead of the 2.3 KB record.
		std::vector<std::uint32_t> materialVersion;
		// New whenever only the record's frame floats are written.
		std::vector<std::uint32_t> materialFrameVersion;
		// What MainBuild's kept bindings key on. New whenever a pipeline's PerGeometry floats or what its pairs read of it change.
		std::vector<std::uint32_t> pipelineBindingVersion;    // parallel to pipelines
		// Bumped by every write of the pipeline family, so the publication copies the family only when it moved.
		std::uint64_t pipelinesStamp = 0;
		void NotePipelinesWrite() { ++pipelinesStamp; }
		template <class T, class F>
		static void PipelineFamily(T& a_left, T& a_right, F&& a_member)
		{
			a_member(a_left.pipelines, a_right.pipelines);
			a_member(a_left.pipelineBindingVersion, a_right.pipelineBindingVersion);
			a_member(a_left.pipelineTechnique, a_right.pipelineTechnique);
			a_member(a_left.permutations, a_right.permutations);
		}
		std::uint32_t versionCounter = 0;
		std::uint32_t NextVersion() { return ++versionCounter; }
		// Shading values are not here. They are FrameValues' rows, sampled at frame start for the slots the walk names.
		std::vector<ObjectAncestorValue> ancestorValue;       // parallel to objects
		// The features' table families. A family is copied into a publication only when its stamp moved.
		Extensions::TableFamilies families;
		// Members of the published entry classes, whose shading fields belong to their owner. The values themselves are FrameValues'.
		struct ClassMembers
		{
			std::vector<std::uint32_t> objects;  // sorted by object index (the walk sorts it)
			// By slot, the owner's group (0 when not a member) and identity.
			struct Owner
			{
				std::uint64_t group = 0, identity = 0;
			};
			std::vector<Owner> owners;
			// Moved onto the publication log for the render thread's fields.
			std::vector<Api::ClassMemberChange> changes;
			bool Grouped(std::uint32_t a_slot) const { return a_slot < owners.size() && owners[a_slot].group; }
		};
		// Write only through SetClassMembers / ClearClassMembers.
		std::array<ClassMembers, Extensions::kPublishedEntryClasses> classMembers;
		/** @brief Sets a_slot's membership in a_class and clears it in every other published class. */
		void SetClassMembers(std::uint32_t a_slot, Api::EntryClass a_class, std::uint64_t a_group, std::uint64_t a_identity, bool a_resetValue = false)
		{
			for (const auto c : Extensions::PublishedEntryClasses()) {
				auto& members = classMembers[static_cast<std::size_t>(c)];
				const std::uint64_t group = c == a_class ? a_group : 0;
				if (members.owners.size() <= a_slot)
					members.owners.resize(std::size_t(a_slot) + 1);
				members.owners[a_slot] = { group, a_identity };
				members.changes.push_back({ a_slot, group, a_identity, a_resetValue, false });
			}
		}
		void ClearClassMembers()
		{
			for (const auto c : Extensions::PublishedEntryClasses()) {
				auto& members = classMembers[static_cast<std::size_t>(c)];
				members.owners.clear();
				members.changes.clear();
				members.changes.push_back({ 0, 0, 0, false, true });
			}
		}
		bool ClassMemberChangesPending() const
		{
			for (const auto& members : classMembers)
				if (!members.changes.empty())
					return true;
			return false;
		}
		// Bit i draws partition i along nextPartition from geometryIndex, 0 for one geometry. A LOD skin holds the union of its levels.
		std::vector<std::uint16_t> skinPartitions;            // parallel to objects
		// Byte L is level L's partitions. BuildDraws picks by FadeStateCS's level, never the node's, so the record is written once.
		std::vector<std::uint32_t> skinLodPartitions;         // parallel to objects
		// The property that supplied each pipeline's per-frame constants. The coordinator's alone, empty in a publication.
		std::vector<RE::BSShaderProperty*> geometryTemplate;  // parallel to pipelines
		// One row per TechniqueKey, shared by its pipelines. Never freed, there are only a few dozen.
		std::vector<std::uint32_t> techniqueKeys;
		ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> techniqueRow;  // TechniqueKey -> row
		std::vector<std::uint32_t> pipelineTechnique;  // parallel to pipelines: its row
		// The constants packed into pipeline rows. Without them a pipeline is not drawable.
		struct PipelineConstantsRow
		{
			GeometryConstants constants{};
			PipelineKey key{};         // what it was evaluated for, with the slot's binding version
			std::uint32_t binding = 0;
			std::uint32_t version = 0;  // NextVersion, whenever it is written
			bool valid = false;
		};
		std::vector<PipelineConstantsRow> pipelineConstants;  // parallel to pipelines
		struct TechniqueConstantsRow
		{
			TechniqueConstants value{};
			std::uint32_t constantsVersion = 0, bindingVersion = 0;  // the floats', the bindings' (filter modes, the shadow mask)
			std::uint32_t passDescriptor = 0;  // a pass descriptor of the row's key (the first): what it is evaluated again for
			TechniqueInputs inputs{};          // the frame sample its last write was made from (the parity's: late or differ)
			bool valid = false;
		};
		std::vector<TechniqueConstantsRow> techniqueConstants;  // parallel to techniqueKeys
		// The publication copies the constants only when this moved. The set's readiness reads it too.
		std::uint64_t constantsStamp = 0;
		void NoteConstantsWrite() { ++constantsStamp; }
		bool PipelineConstantsCurrent(std::size_t a_pipeline) const
		{
			if (a_pipeline >= pipelineConstants.size() || a_pipeline >= pipelines.size() || a_pipeline >= pipelineBindingVersion.size())
				return false;
			const auto& row = pipelineConstants[a_pipeline];
			return row.valid && row.key == pipelines[a_pipeline] && row.binding == pipelineBindingVersion[a_pipeline];
		}
		bool TechniqueConstantsValid(std::size_t a_pipeline) const
		{
			return a_pipeline < pipelineTechnique.size() && pipelineTechnique[a_pipeline] < techniqueConstants.size() &&
			       techniqueConstants[pipelineTechnique[a_pipeline]].valid;
		}
		const TechniqueConstantsRow& TechniqueOf(std::size_t a_pipeline) const { return techniqueConstants[pipelineTechnique[a_pipeline]]; }
		bool TechniqueShadowMask(std::size_t a_pipeline) const { return (techniqueKeys[pipelineTechnique[a_pipeline]] & 1u) != 0; }
		std::vector<PipelinePermutation> permutations;        // parallel to pipelines
		std::vector<DrawSequence> draws;  // one per object (templates: pipelineIndex is the table index)
		// Each object's fixed slot in its main range, in the order the range's feature gives, or ~0u. The colour epoch writes a draw
		// or a zero-count one to that slot so overlapping objects keep their order, which an atomic append would not.
		std::vector<std::uint32_t> rangeOrdinal;  // parallel to objects (sized by the writer: Features/Decals' OrderDecals)
		std::array<std::uint32_t, Api::kFixedMainRanges> rangeCount{};
		// Each skinned object's block in FrameValues' palettes, three float4 rows a bone. Capacity grows in kBoneGrowRows steps and moves no block.
		static constexpr std::uint32_t kBoneGrowRows = 4096;
		std::vector<std::uint32_t> boneOffset;  // parallel to objects, in rows
		std::vector<std::uint32_t> boneRows;    // parallel to objects
		// Free rows below boneTop, coalesced as first row -> rows. Blocks of any size take the first run that fits, so skins and particle
		// systems reuse each other's rows instead of raising boneTop.
		std::map<std::uint32_t, std::uint32_t> boneFree;
		/** @brief Merges with its neighbours and lowers boneTop past a free top. */
		void ReturnBones(std::uint32_t a_offset, std::uint32_t a_rows);
		std::uint32_t boneTop = 0;       // rows handed out (past the last block in use)
		std::uint32_t boneCapacity = 0;  // rows the blocks span (FrameValues' palettes hold twice as many)
		std::uint32_t BoneCapacity() const { return boneCapacity; }
		/** @brief Keeps the slot's own block when the size is the same. Returns its first row. */
		std::uint32_t PlaceBones(std::uint32_t a_slot, std::uint32_t a_rows);
		void FreeBones(std::uint32_t a_slot);
		// Per-object extras, filled at Prepass and sent to the extras buffer (t126). A block persists while the object's patch holds it.
		std::vector<float> extraRows;
		std::vector<std::uint32_t> extraOffset;  // parallel to objects
		std::vector<std::uint32_t> extraFree;    // freed blocks' row offsets
		// Every block written, allocated or freed, for the published tables to replay.
		SharedEventLog<std::uint32_t> extrasBlockLog;
		void NoteExtrasBlock(std::uint32_t a_offset) { extrasBlockLog.Push(a_offset); }
		std::uint32_t AllocateExtras()
		{
			if (!extraFree.empty()) {
				const std::uint32_t offset = extraFree.back();
				extraFree.pop_back();
				std::fill_n(extraRows.begin() + std::ptrdiff_t(offset) * 4, std::size_t(kExtraRows) * 4, 0.0f);
				NoteExtrasBlock(offset);
				return offset;
			}
			const auto offset = static_cast<std::uint32_t>(extraRows.size() / 4);
			extraRows.resize(extraRows.size() + std::size_t(kExtraRows) * 4, 0.0f);
			NoteExtrasBlock(offset);
			return offset;
		}
		void FreeExtras(std::uint32_t a_slot)
		{
			if (a_slot < extraOffset.size() && extraOffset[a_slot] != kNoExtraRows) {
				NoteExtrasBlock(extraOffset[a_slot]);
				Retire(kRetiredExtras, extraOffset[a_slot]);
				extraOffset[a_slot] = kNoExtraRows;
			}
		}
		// The Utility technique each object casts with, and why the engine would not draw it into a shadow map. Decided by the scene
		// phase, because every shadow view is drawn before the accumulate phase.
		std::vector<std::uint32_t> shadowTechnique;  // parallel to objects
		std::vector<std::uint8_t> shadowReject;      // parallel to objects (ShadowReject)
		// Per occlusion view, the Utility technique the map draws each object with, or 0.
		std::array<std::vector<std::uint32_t>, kOcclusionViews> occlusionTechnique;
		// The object's entry in the sun's full-frustum cull objectArray. Null when never tested, as for actors.
		std::vector<const RE::NiAVObject*> listedRoot;  // parallel to objects
		std::vector<std::uint8_t> hasFadeNode;  // parallel to objects
		// Meaningless without kObjectFadeTest. Positive values scale with the camera's LOD factor, negative ones do not.
		std::vector<float> fadeDistance;  // parallel to objects
		std::vector<std::uint8_t> residentSlot;  // parallel to objects
		// Written by CommitSet alone, and every build selects by it. kObjectMember is the main phase's copy for the GPU.
		std::vector<std::uint8_t> setPhases;  // parallel to objects
		// Each write that changes a slot's columns appends the slot and causes. A consumer one behind the trim, or on a new tables
		// generation, rereads everything. Published snapshots share these logs' chunks.
		struct Change
		{
			std::uint32_t slot = 0;
			std::uint32_t causes = 0;  // ChangeCause bits
		};
		SharedEventLog<Change> changeLog;
		std::array<std::uint64_t, kChangeCauseCount> changeCounts{};  // notes by cause, for the report
		void NoteChange(std::uint32_t a_slot, std::uint32_t a_causes)
		{
			if (!a_causes)
				return;
			changeLog.Push({ a_slot, a_causes });
			for (std::uint32_t bits = a_causes; bits; bits &= bits - 1)
				++changeCounts[std::countr_zero(bits)];
		}
		SharedEventLog<std::uint32_t> geometryLog;
		void NoteGeometry(std::uint32_t a_slot) { geometryLog.Push(a_slot); }
		SharedEventLog<std::uint32_t> materialLog;
		void NoteMaterial(std::uint32_t a_slot) { materialLog.Push(a_slot); }
		// Object slots whose published columns changed with no change-log note. Only the publication's replay reads it.
		SharedEventLog<std::uint32_t> publicationLog;
		void NotePublished(std::uint32_t a_slot) { publicationLog.Push(a_slot); }
		void InvalidateChangeLog()
		{
			changeLog.Invalidate();
			materialLog.Invalidate();
			extrasBlockLog.Invalidate();
			publicationLog.Invalidate();
		}
		/** @brief Everything a persistent consumer builds from, for writers to compare against. */
		struct Columns
		{
			ObjectRecord object{};
			DrawSequence draw{};
			ObjectAncestorValue ancestorValue{};
			std::array<float, kExtraRows * 4> extras{};
			const RE::BSGeometry* geometry = nullptr;
			const RE::NiAVObject* listedRoot = nullptr;
			std::uint64_t identity = 0;
			std::uint64_t groupIdentity = 0;
			ID3D11ShaderResourceView* shadowDiffuse = nullptr;
			const RE::BSShaderMaterial* shadowMaterial = nullptr;
			float fadeDistance = 0.0f;
			std::uint32_t boneOffset = 0, boneRows = 0, extraOffset = kNoExtraRows, shadowTechnique = 0, streamRow = kNoStreamRow;
			std::array<std::uint32_t, kOcclusionViews> occlusionTechnique{};
			std::uint32_t sceneFlags = 0;
			std::uint16_t skinPartitions = 0;
			std::uint32_t skinLodPartitions = 0;
			std::uint8_t shadowReject = 0, resident = 0, hasFadeNode = 0;
		};
		Columns ColumnsOf(std::uint32_t a_slot) const;
		static std::uint32_t CausesBetween(const Columns& a_before, const Columns& a_after);
		void NoteWrite(std::uint32_t a_slot, const Columns& a_before) { NoteChange(a_slot, CausesBetween(a_before, ColumnsOf(a_slot))); }
		// The vertex stream rows, written in place by the component that follows the stream's entry class.
		std::vector<Api::VertexStreamRow> streamRows;
		std::vector<std::uint32_t> streamRow;  // parallel to objects: index in streamRows, or kNoStreamRow
		std::vector<std::uint32_t> streamRowFree;
		std::vector<std::pair<const void*, std::uint32_t>> streamRowsReleased;
		void SetStreamRow(std::uint32_t a_slot, Api::VertexStreamRow a_row);
		void ClearStreamRow(std::uint32_t a_slot);
		// An alpha-tested caster's diffuse view and material. The shadow build reads only the material's texture transform, which
		// BSLightingShaderPropertyFloatController moves between Main::Draw and BeforeShadowMaps.
		std::vector<ID3D11ShaderResourceView*> shadowDiffuse;    // parallel to objects
		std::vector<const RE::BSShaderMaterial*> shadowMaterial;  // parallel to objects
		// Distinct views, so lookups refresh per view rather than per caster.
		std::vector<ID3D11ShaderResourceView*> shadowTextureSet;
		ankerl::unordered_dense::set<ID3D11ShaderResourceView*> shadowTextureSeen;
		std::vector<std::pair<ID3D11ShaderResourceView*, bool>> shadowTextureChanges;
		void TakeShadowTextureChanges(std::vector<std::pair<ID3D11ShaderResourceView*, bool>>& a_out)
		{
			a_out.clear();
			a_out.swap(shadowTextureChanges);
		}
		std::vector<ShadowPipelineKey> shadowKeysUsed;
		// Occluders use complete techniques, RenderDepth included.
		std::array<std::vector<ShadowPipelineKey>, kOcclusionViews> occlusionKeysUsed;  // per occlusion view

		/**
		 * @brief The three shared tables keep their slots across frames.
		 *
		 * A slot lives while objects reference it. The drain runs before the loop, so no object of the frame points at a reused slot.
		 * Being in a used set certifies a slot's raw engine pointers for evaluation. CheckObjectSlots holds every bound record to it.
		 */
		static constexpr std::uint32_t kSlotFree = ~0u;  // a geometryLastUsed never written; ResolveGeometrySlot's "no slot"
		org::async::SlotTable geometrySlots, pipelineSlots, materialSlots;
		std::vector<std::uint32_t> geometryLastUsed;  // parallel to geometries: the walk's last write (PassStamp)
		std::vector<const RE::BSGraphics::TriShape*> geometrySlotKey;
		// A layer's geometry key is the shape's second index list, never read as a TriShape.
		std::vector<std::uint8_t> geometryLayerKey;  // parallel to geometries
		std::vector<std::pair<const RE::BSShaderMaterial*, std::uint32_t>> materialSlotKey;
		std::vector<std::uint64_t> usedMaterialBits, usedPipelineBits;
		static bool BitSet(const std::vector<std::uint64_t>& a_bits, std::size_t a_slot) { return a_slot / 64 < a_bits.size() && ((a_bits[a_slot / 64] >> (a_slot % 64)) & 1); }
		template <class F>
		static void ForEachBit(const std::vector<std::uint64_t>& a_bits, F&& a_function)
		{
			for (std::size_t word = 0; word < a_bits.size(); ++word)
				for (std::uint64_t remaining = a_bits[word]; remaining; remaining &= remaining - 1)
					a_function(static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining)));
		}
		bool MaterialUsed(std::size_t a_slot) const { return BitSet(usedMaterialBits, a_slot); }
		void UnmarkMaterial(std::uint32_t a_slot) { if (a_slot / 64 < usedMaterialBits.size()) usedMaterialBits[a_slot / 64] &= ~(1ull << (a_slot % 64)); }
		void UnmarkPipeline(std::uint32_t a_slot) { if (a_slot / 64 < usedPipelineBits.size()) usedPipelineBits[a_slot / 64] &= ~(1ull << (a_slot % 64)); }
		/** @brief The used pipeline set cut to a_slots (pipelineSlots.TrimTop): no bit past the table. */
		void TrimPipelineUsed(std::uint32_t a_slots)
		{
			usedPipelineBits.resize(std::min<std::size_t>(usedPipelineBits.size(), (std::size_t(a_slots) + 63) / 64));
			if (a_slots % 64 && a_slots / 64 < usedPipelineBits.size())
				usedPipelineBits[a_slots / 64] &= (1ull << (a_slots % 64)) - 1;
		}
		void ClearUsed()
		{
			std::fill(usedMaterialBits.begin(), usedMaterialBits.end(), 0);
			std::fill(usedPipelineBits.begin(), usedPipelineBits.end(), 0);
		}
		void MarkMaterialUsed(std::uint32_t a_slot);
		void MarkPipelineUsed(std::uint32_t a_slot);
		void SetMaterialUsed(std::uint32_t a_slot, bool a_used)
		{
			if (a_used)
				MarkMaterialUsed(a_slot);
			else if (a_slot / 64 < usedMaterialBits.size())
				usedMaterialBits[a_slot / 64] &= ~(1ull << (a_slot % 64));
		}
		void SetPipelineUsed(std::uint32_t a_slot, bool a_used)
		{
			if (a_used)
				MarkPipelineUsed(a_slot);
			else if (a_slot / 64 < usedPipelineBits.size())
				usedPipelineBits[a_slot / 64] &= ~(1ull << (a_slot % 64));
		}
		bool PipelineUsed(std::size_t a_slot) const { return BitSet(usedPipelineBits, a_slot); }
		// Each table's columns. a_column(vector, initial value...) runs for every vector parallel to it.
		template <class F>
		void GeometryColumns(F&& a_column)
		{
			a_column(geometries);
			a_column(geometryImports);
			a_column(geometryLastUsed, kSlotFree);
			a_column(geometrySlotKey);
			a_column(geometryLayerKey, std::uint8_t(0));
		}
		template <class F>
		void PipelineColumns(F&& a_column)
		{
			a_column(pipelines);
			a_column(geometryTemplate);
			a_column(pipelineTechnique);
			a_column(permutations);
			a_column(pipelineBindingVersion);
			a_column(pipelineConstants);
		}
		template <class F>
		void MaterialColumns(F&& a_column)
		{
			a_column(materials);
			a_column(materialVersion);
			a_column(materialFrameVersion);
			a_column(materialSlotKey);
		}

		// Object slots persist while the object has a record. A walk sweeps the slots it did not write.
		// objectSeen is the coordinator's alone and holds zeros in a publication.
		std::vector<std::uint32_t> objectSeen;
		// New on every tracked insertion, including detach and reattach and pointer reuse.
		std::vector<std::uint64_t> objectIdentity;  // parallel to objects; zero for a free slot
		std::vector<std::uint64_t> objectGroup;  // actor siblings share this ID
		// The flags as the scene phase wrote them. The delta walk restores each patched slot's scene half from here.
		std::vector<std::uint32_t> sceneFlags;
		// A multi-index shape's layer is an object of its own. layerBase maps layer to base and layerOf base to layer, else kNoObjectSlot.
		// The publication replays both ends of a link when either end has a change-log note.
		std::vector<std::uint32_t> layerBase;
		std::vector<std::uint32_t> layerOf;
		bool IsLayer(std::uint32_t a_slot) const { return a_slot < layerBase.size() && layerBase[a_slot] != kNoObjectSlot; }
		std::vector<std::uint32_t> objectFree;
		std::uint32_t liveObjects = 0;
		/**
		 * @brief Freed slots wait here until no published version can name them. PublishTables moves them into the retirement chain.
		 */
		enum RetiredKind : std::uint8_t
		{
			kRetiredObject,
			kRetiredFamilyRow,  // a table family's row (families); extra: the family's index
			kRetiredExtras,     // slot: the block's row offset
			kRetiredBones,      // slot: the block's offset, extra: its rows
			kRetiredStreamRow,  // a vertex stream row (streamRows)
			kRetiredMaterialLookup,  // a material slot's bindings to drop (Coordinator::RetireMaterialBinding)
			kRetiredPipelineLookup,  // a pipeline slot's lookups to drop (its mask binding: Coordinator::RetireMaskBinding)
			kRetiredGeometrySlot,    // a SlotTable's slot; extra: its generation when freed
			kRetiredMaterialSlot,
			kRetiredPipelineSlot,
			kRetiredComponentRange,  // a coordinator component's range (RetireRange; owner: the component), slot: its first unit, extra: its units
		};
		struct RetiredSlot
		{
			RetiredKind kind = kRetiredObject;
			std::uint8_t owner = 0;  // kRetiredComponentRange's component (Api::CoordinatorComponentId::index)
			std::uint32_t slot = 0, extra = 0;
			std::uint32_t epoch = 0;  // slotEpoch when retired: a clear since drops it
		};
		std::vector<RetiredSlot> retiring;
		std::uint32_t objectsRetiring = 0;  // object slots among them, and in the chain (neither live nor free)
		std::uint32_t slotEpoch = 0;        // moves on when every slot is cleared
		void Retire(RetiredKind a_kind, std::uint32_t a_slot, std::uint32_t a_extra = 0, std::uint8_t a_owner = 0)
		{
			retiring.push_back({ a_kind, a_owner, a_slot, a_extra, slotEpoch });
		}
		/** @brief Call ResetObject first. */
		void RetireObject(std::uint32_t a_slot)
		{
			Retire(kRetiredObject, a_slot);
			++objectsRetiring;
		}
		std::uint32_t CountLive() const { return static_cast<std::uint32_t>(objects.size() - objectFree.size() - objectsRetiring); }
		// The retirement chain node is held by publications, not tables. A reader that keeps only tables keeps no slot from reuse.
		std::uint64_t publishedSequence = 0;  // the scene publication it was published with (the retirement report's held snapshots)
		void GrowObjects(std::size_t a_count);
		/** @brief Returns a slot to the free state, but not to the free list. */
		void ResetObject(std::uint32_t a_slot);

		void ClearFrame();
		void Clear();
	};
}
