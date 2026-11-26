// The shadow build. It writes the shadow list, the material rows and the frame record, mostly kept across builds in ShadowKept.
// Runs on DCLF's thread pool as part of the snapshot build. The sun and point-light exclusions are built elsewhere.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "RenderGraph/Fatal.h"
#include "Snapshot/EngineReads.h"

namespace DCLF::Draws
{
	namespace
	{
		/** @brief Whether the object is in DCLF's set for this mode. The mode's views draw only those objects. */
		bool SetCaster(const SceneTables& a_tables, std::uint32_t a_object, std::uint32_t a_mode)
		{
			return a_object < a_tables.setPhases.size() && (a_tables.setPhases[a_object] & Extensions::ShadowViewsSet().phaseOfMode[a_mode]) != 0;
		}

		/** @brief The key slot of the technique on the object's geometry, or ~0u if there is none yet. */
		std::uint32_t KeySlotOf(const SceneTables& a_tables, const Lookups& a_lookups, std::uint32_t a_object, std::uint32_t a_technique)
		{
			if (!a_technique)
				return ~0u;
			const auto& object = a_tables.objects[a_object];
			const ShadowPipelineKey key{ a_technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
				VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
			const auto it = a_lookups.shadowSlots.find(key);
			return it == a_lookups.shadowSlots.end() ? ~0u : it->second;
		}

		/** @brief An object's shadow list input for the modes in a_modes, bit m per mode. */
		DrawInput ShadowEntry(const SceneTables& a_tables, const Lookups& a_lookups, std::uint32_t a_object, std::uint32_t a_record, std::uint8_t a_modes,
			std::uint32_t a_stream)
		{
			const auto& object = a_tables.objects[a_object];
			// The record must fit the material field of the input's rows word, which BuildDrawsCS.hlsl reads.
			if (a_record > kRowMaterialMask)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("shadow material row {} past an input's row fields", a_record));
			std::uint32_t mask = 0;
			bool caster = false;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				if ((a_modes >> m) & 1) {
					mask |= ModeViewBits(m);
					caster |= !IsOcclusionMode(m);
				}
			const std::uint32_t casterKey = caster ? KeySlotOf(a_tables, a_lookups, a_object, BaseTechnique(a_tables, kSunShadowMode, a_object)) : ~0u;
			std::uint32_t occlusionKey[kOcclusionViews];
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				occlusionKey[v] = ((a_modes >> OcclusionModeOf(v)) & 1) ? KeySlotOf(a_tables, a_lookups, a_object, BaseTechnique(a_tables, OcclusionModeOf(v), a_object)) : ~0u;
			// Keep the focus-only marker, which shares a bit with kObjectDecal. Views use it to skip the object outside focus shadows.
			const std::uint32_t kept = ObjectFocusOnly(object.flags) ? kObjectFocusOnly : 0u;
			return { casterKey == ~0u ? 0u : casterKey, a_record, object.geometryIndex, (object.flags & ~(kObjectDecal | kObjectForward)) | kept | kInputDrawable,
				{ mask, casterKey, { occlusionKey[0], occlusionKey[1] } }, a_object, 0, PartitionsOf(a_tables, a_object), a_stream, FadeRootOf(a_tables, a_object),
				a_record, LodPartitionsOf(a_tables, a_object) };
		}

		/** @brief The texture-transform buffer the frame reads. The engine flips it each frame in Main::Update. */
		std::uint32_t ShadowTransformBuffer()
		{
			return CS_ENGINE_READ(ShaderManagerState, RecordTime, globals::game::smState) ? (CS_ENGINE_READ(ShaderManagerState, RecordTime, globals::game::smState)->textureTransformCurrentBuffer & 1) : 0u;
		}

		/**
		 * @brief The material's texcoord offset in xy and scale in zw. Read it now, not during the walk, because the float
		 * controllers move it between Main::Draw and BeforeShadowMaps. They write the buffer the frame does not read.
		 */
		std::array<float, 4> ShadowTexcoord(const RE::BSShaderMaterial& a_material, std::uint32_t a_buffer)
		{
			return { a_material.texCoordOffset[a_buffer].x, a_material.texCoordOffset[a_buffer].y, a_material.texCoordScale[a_buffer].x,
				a_material.texCoordScale[a_buffer].y };
		}

		/**
		 * @brief Marks rows whose texture transform moved. A slot stays watched while its two buffers differ or for 2 builds
		 * after its last move, so the value from both buffers reaches the row.
		 */
		template <class MarkRow>
		void WatchTransformMoves(ShadowKept& k, std::uint64_t a_build, MarkRow&& a_markRow)
		{
			std::vector<const RE::BSShaderMaterial*> moved;
			const bool lost = Extensions::DrainTransformMoves(moved);
			if (k.transformWatchBuild.size() < k.slotMaterial.size())
				k.transformWatchBuild.resize(k.slotMaterial.size(), 0);
			// Moves were dropped while no build ran, so watch every kept material.
			if (lost)
				for (std::uint32_t slot = 1; slot < k.slotMaterial.size(); ++slot)
					if (k.slotMaterial[slot]) {
						if (!k.transformWatchBuild[slot])
							k.transformWatch.push_back(slot);
						k.transformWatchBuild[slot] = a_build;
					}
			for (const auto* material : moved)
				if (const auto it = k.slotOf.find(material); it != k.slotOf.end()) {
					if (!k.transformWatchBuild[it->second])
						k.transformWatch.push_back(it->second);
					k.transformWatchBuild[it->second] = a_build;
				}
			for (std::size_t w = 0; w < k.transformWatch.size();) {
				const std::uint32_t slot = k.transformWatch[w];
				const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(k.slotMaterial[slot]);
				const bool keep = material && k.transformWatchBuild[slot] &&
				                  (a_build - k.transformWatchBuild[slot] <= 2 || material->texCoordOffset[0] != material->texCoordOffset[1] ||
									  material->texCoordScale[0] != material->texCoordScale[1]);
				if (!keep) {
					k.transformWatchBuild[slot] = 0;
					k.transformWatch[w] = k.transformWatch.back();
					k.transformWatch.pop_back();
					continue;
				}
				a_markRow(slot);
				++w;
			}
		}

		/** @brief Adds o to a mark list once, or clears its mark. A cleared entry leaves the list at its next pass. */
		void SetMark(std::vector<std::uint32_t>& a_list, std::vector<std::uint8_t>& a_mark, std::uint32_t o, bool a_on)
		{
			if (a_mark.size() <= o)
				a_mark.resize(std::size_t(o) + 1, 0);
			if (a_on && !a_mark[o]) {
				a_mark[o] = 1;
				a_list.push_back(o);
			} else if (!a_on && a_mark[o]) {
				a_mark[o] = 0;
			}
		}

		/** @brief State shared by the stages of the kept shadow build. */
		struct KeptShadowBuild
		{
			const ShadowInputs& in;
			const SceneTables& tables;
			const Lookups& lookups;
			ShadowPayload& out;
			ShadowKept& k;
			ResidentRegion* scene;
			std::uint32_t objects;
			std::uint64_t build;
			std::uint32_t transformBuffer = 0;
			// Objects whose entries are rewritten after the modes update.
			std::vector<std::uint32_t> touched;

			void WriteRow(std::uint32_t a_slot, const ShadowMaterialRow& a_row)
			{
				if (a_slot >= k.rows.Size()) {
					auto& rows = k.rows.Mutable();
					rows.resize(std::size_t(a_slot) + 1);
					rows[a_slot] = a_row;
					k.rows.Mark(a_slot);
					++k.rowsWritten;
				} else if (k.rows.Set(a_slot, a_row)) {
					++k.rowsWritten;
				}
			}
			bool MaterialRow(std::uint32_t a_slot)
			{
				const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(k.slotMaterial[a_slot]);
				const auto textureIt = k.slotDiffuse[a_slot] ? lookups.shadowTextures.find(k.slotDiffuse[a_slot]) : lookups.shadowTextures.end();
				const bool ready = textureIt != lookups.shadowTextures.end() && textureIt->second != Lookups::kNone;
				k.slotReady[a_slot] = ready ? 1 : 0;
				if (!ready)
					return false;
				ShadowMaterialRow row;
				row.texcoord = ShadowTexcoord(*material, transformBuffer);
				row.diffuse = textureIt->second;
				WriteRow(a_slot, row);
				const auto owner = lookups.shadowTextureOwners.find(k.slotDiffuse[a_slot]);
				std::shared_ptr<const void> held = owner != lookups.shadowTextureOwners.end() ? owner->second : nullptr;
				if (k.slotOwner[a_slot] != held) {
					k.slotOwner[a_slot] = std::move(held);
					k.ownersChanged = true;
				}
				return true;
			}
			void MarkRow(std::uint32_t a_slot)
			{
				if (k.rowDirtyMark.size() <= a_slot)
					k.rowDirtyMark.resize(std::size_t(a_slot) + 1, 0);
				if (!k.rowDirtyMark[a_slot]) {
					k.rowDirtyMark[a_slot] = 1;
					k.rowDirty.push_back(a_slot);
				}
			}
			// Returns 0 for the plain row, the material's slot (acquired if new), or kNoRecord.
			std::uint32_t RecordOf(std::uint32_t o)
			{
				const auto& object = tables.objects[o];
				if (object.flags & kObjectFree)
					return ShadowKept::kNoRecord;
				const std::uint32_t occlusion = OcclusionTechniques(tables, in.modeUsed, o);
				if ((object.flags & kObjectNoShadow) && !occlusion)
					return ShadowKept::kNoRecord;
				if (!(((object.flags & kObjectNoShadow) ? 0u : tables.shadowTechnique[o]) & 0x80) && !(occlusion & 0x80))
					return 0;
				const auto* material = o < tables.shadowMaterial.size() ? tables.shadowMaterial[o] : nullptr;
				if (!material)
					return ShadowKept::kNoRecord;
				auto [it, fresh] = k.slotOf.try_emplace(material, 0u);
				if (fresh) {
					std::uint32_t slot;
					if (!k.freeSlots.empty()) {
						std::pop_heap(k.freeSlots.begin(), k.freeSlots.end(), std::greater<>());
						slot = k.freeSlots.back();
						k.freeSlots.pop_back();
					} else {
						slot = static_cast<std::uint32_t>(k.slotMaterial.size());
						k.slotMaterial.push_back(nullptr);
						k.slotDiffuse.push_back(nullptr);
						k.slotRefs.push_back(0);
						k.slotReady.push_back(0);
						k.slotOwner.emplace_back();
					}
					it->second = slot;
					k.slotMaterial[slot] = material;
					k.slotDiffuse[slot] = tables.shadowDiffuse[o];
					k.slotReady[slot] = 0;
					MarkRow(slot);
				}
				return it->second;
			}
			void ReleaseRecord(std::uint32_t o)
			{
				const std::uint32_t slot = k.objectRecord[o];
				if (slot != ShadowKept::kNoRecord && slot != 0 && slot < k.slotRefs.size() && k.objectMaterial[o] && --k.slotRefs[slot] == 0) {
					k.slotOf.erase(k.slotMaterial[slot]);
					k.slotMaterial[slot] = nullptr;
					k.slotDiffuse[slot] = nullptr;
					k.slotReady[slot] = 0;
					if (k.slotOwner[slot]) {
						k.slotOwner[slot].reset();
						k.ownersChanged = true;
					}
					if (slot < k.transformWatchBuild.size())
						k.transformWatchBuild[slot] = 0;
					k.freeSlots.push_back(slot);
					std::push_heap(k.freeSlots.begin(), k.freeSlots.end(), std::greater<>());
				}
				k.objectRecord[o] = ShadowKept::kNoRecord;
				k.objectMaterial[o] = nullptr;
			}
			void TakeRecord(std::uint32_t o)
			{
				ReleaseRecord(o);
				const std::uint32_t slot = RecordOf(o);
				k.objectRecord[o] = slot;
				if (slot != ShadowKept::kNoRecord && slot != 0) {
					++k.slotRefs[slot];
					k.objectMaterial[o] = k.slotMaterial[slot];
				}
			}

			int Evaluate(std::uint32_t m, std::uint32_t o)
			{
				// 0 not held, 1 held in the region, 2 waiting for a pipeline or texture, 3 in the per-frame list.
				const auto& object = tables.objects[o];
				const bool occlusionMode = IsOcclusionMode(m);
				if (object.flags & kObjectFree)
					return 0;
				if (occlusionMode ? !ModeTechnique(tables, m, o) : (object.flags & kObjectNoShadow) != 0)
					return 0;
				// A set member that cannot be drawn is a defect, so it is counted with what waits.
				if (!SetCaster(tables, o, m))
					return 0;
				const std::uint32_t record = k.objectRecord[o];
				if (record == ShadowKept::kNoRecord)
					return 0;
				auto wait = [&](const char* a_why, std::uint32_t a_value) {
					if (out.setWaitingFirst.empty())
						out.setWaitingFirst = fmt::format("object {} mode {}: {} {}", o, m, a_why, a_value);
					return 2;
				};
				// Waits for the scene buffers to grow. The resync after growth reads it again.
				if (!ObjectFits(tables, o, in.addresses.fit))
					return wait("scene buffers", o);
				if (record != 0 && !(record < k.slotReady.size() && k.slotReady[record]))
					return wait("material row", record);
				const bool volumetricOnly = VolumetricClass(m, object.flags);
				const auto& classStates = in.modeRasterStates[m].Of(volumetricOnly);
				if (classStates.empty())
					return 0;
				const std::uint32_t technique = BaseTechnique(tables, m, o);
				const std::uint32_t slot = KeySlotOf(tables, lookups, o, technique);
				if (slot == ~0u)
					return wait("no key slot for technique", technique);
				for (const std::uint32_t state : classStates)
					if (lookups.ShadowMapPipeline(m, state, slot) == Lookups::kNone)
						return wait("no pipeline under state", state);
				if (IsStreamObject(tables, o) || (VertexLayoutOf(tables.geometries[object.geometryIndex].vertexDesc) & kPositionInSecondStream))
					return 3;
				return 1;
			}
			void Touch(std::uint32_t o)
			{
				if (k.touchedMark.size() <= o)
					k.touchedMark.resize(std::size_t(o) + 1, 0);
				if (!k.touchedMark[o]) {
					k.touchedMark[o] = 1;
					touched.push_back(o);
				}
			}
			void Hold(std::uint32_t m, std::uint32_t o, bool a_held)
			{
				if (o < k.heldModes.size() && k.Holds(m, o) != a_held) {
					k.heldModes[o] ^= static_cast<std::uint8_t>(1u << m);
					k.modes[m].membership = build;
					// Record the move for the exclusions. An object flips at most once per build, since Evaluate reads nothing the modes write.
					if (out.movesValid[m])
						(a_held ? out.joined[m] : out.left[m]).push_back(o);
				}
				Touch(o);
			}
			void Take(std::uint32_t m, std::uint32_t o)
			{
				auto& mode = k.modes[m];
				const int result = o < objects ? Evaluate(m, o) : 0;
				SetMark(mode.waiting, mode.waitingMark, o, result == 2);
				const bool wasFace = o < mode.faceMark.size() && mode.faceMark[o];
				SetMark(mode.faces, mode.faceMark, o, result == 3);
				if (wasFace != (result == 3))
					mode.membership = build;
				Hold(m, o, result == 1);
			}

			/** @brief Writes o's shadow words into the shared region. The main build owns the rest of the entry. */
			void WriteRegionEntry(std::uint32_t o, std::uint8_t a_heldNow)
			{
				auto& r = *scene;
				r.Cover(objects);
				const std::uint32_t i = r.EntryOf(o);
				if (!a_heldNow) {
					if (i == kNoRegion || !(r.sideOf[i] & kSideShadow))
						return;
					if (r.sideOf[i] & kSideMain) {
						DrawInput entry = r.inputs.Get()[i];
						ClearShadowPart(entry);
						r.sideOf[i] = kSideMain;
						r.inputs.Set(i, entry);
					} else {
						r.RemoveEntry(o);
					}
					++k.entriesWritten;
					return;
				}
				const DrawInput part = ShadowEntry(tables, lookups, o, k.objectRecord[o], a_heldNow, ~0u);
				if (i == kNoRegion) {
					DrawInput entry{};
					SetShadowPart(entry, part, false);
					r.AddEntry(o, entry, kSideShadow);
					++k.entriesWritten;
					return;
				}
				DrawInput entry = r.inputs.Get()[i];
				SetShadowPart(entry, part, (r.sideOf[i] & kSideMain) != 0);
				r.sideOf[i] |= kSideShadow;
				if (r.inputs.Set(i, entry))
					++k.entriesWritten;
			}
			void WriteEntry(std::uint32_t o)
			{
				const std::uint8_t heldNow = o < k.heldModes.size() && o < objects ? k.heldModes[o] : std::uint8_t{ 0 };
				if (scene) {
					WriteRegionEntry(o, heldNow);
					return;
				}
				auto& own = k.scene;
				own.Cover(objects);
				const std::uint8_t modes = o < k.heldModes.size() ? k.heldModes[o] : std::uint8_t{ 0 };
				if (!modes || o >= objects) {
					if (own.Holds(o)) {
						own.Remove(o);
						++k.entriesWritten;
					}
					return;
				}
				const DrawInput input = ShadowEntry(tables, lookups, o, k.objectRecord[o], modes, ~0u);
				if (const std::uint32_t i = own.indexOf[o]; i == kNoRegion) {
					own.Add(o, input);
					++k.entriesWritten;
				} else if (own.inputs.Set(i, input)) {
					++k.entriesWritten;
				}
			}
		};

		/**
		 * @brief Starts the kept build. Empties ShadowKept on a resync, when the log was trimmed or the identity or scene buffers moved.
		 * Returns whether it resynced.
		 */
		bool BeginKeptBuild(KeptShadowBuild& b)
		{
			const auto& in = b.in;
			auto& k = b.k;
			// The journal keeps what the oldest payload backing still lacks.
			k.rows.BeginBuild(in.materialRowsOldestContentEpoch);
			const bool resync = !k.cursor.Continues(b.tables.changeLog, in.tablesGeneration) || k.identity != in.addresses.identity ||
			                    k.objectRecord.size() > b.objects || k.fit != in.addresses.fit;
			if (resync) {
				k.Reset();
				k.cursor.Restart(in.tablesGeneration);
				k.identity = in.addresses.identity;
				k.fit = in.addresses.fit;
				++k.resyncs;
			}
			if (k.objectRecord.size() < b.objects) {
				k.objectRecord.resize(b.objects, ShadowKept::kNoRecord);
				k.objectMaterial.resize(b.objects, nullptr);
			}
			return resync;
		}

		/** @brief Starts the region half. On a resync it clears every shadow word in the shared region. */
		void BeginRegion(KeptShadowBuild& b, bool a_resync)
		{
			if (!b.scene)
				return;
			auto& r = *b.scene;
			r.Begin(b.in.inputsOldestContentEpoch);
			if (!a_resync)
				return;
			for (std::uint32_t i = static_cast<std::uint32_t>(r.sideOf.size()); i-- > 0;) {
				if (!(r.sideOf[i] & kSideShadow))
					continue;
				if (r.sideOf[i] & kSideMain) {
					DrawInput entry = r.inputs.Get()[i];
					ClearShadowPart(entry);
					r.sideOf[i] = kSideMain;
					r.inputs.Set(i, entry);
				} else {
					r.RemoveEntry(r.inputs.Get()[i].objectIndex);
				}
			}
		}

		/**
		 * @brief Collects the objects to read again and retakes their records. A view-state change rereads all of them,
		 * because a record depends on whether Skylighting's map is drawn.
		 */
		void TakeChangedRecords(KeptShadowBuild& b, bool a_resync, std::vector<std::uint32_t>& a_changed)
		{
			auto& k = b.k;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				a_resync |= k.modes[m].active && k.modes[m].rasterStates != (b.in.modeUsed[m] ? b.in.modeRasterStates[m] : ModeRasterStates{});
			if (a_resync) {
				a_changed.reserve(b.objects);
				for (std::uint32_t o = 0; o < b.objects; ++o)
					a_changed.push_back(o);
			} else {
				constexpr std::uint32_t kShadowCauses = kShadowChangeCauses;
				for (const auto& change : k.cursor.Unread(b.tables.changeLog))
					if ((change.causes & kShadowCauses) && change.slot < b.objects)
						a_changed.push_back(change.slot);
			}
			k.cursor.Advance(b.tables.changeLog);
			for (const std::uint32_t o : a_changed)
				b.TakeRecord(o);
		}

		/** @brief Rewrites the dirty material rows and rebuilds the owner list if it changed. */
		void UpdateMaterialRows(KeptShadowBuild& b)
		{
			auto& k = b.k;
			if (k.lookupsGeneration != b.lookups.shadowGeneration) {
				k.lookupsGeneration = b.lookups.shadowGeneration;
				for (std::uint32_t slot = 1; slot < k.slotMaterial.size(); ++slot)
					if (k.slotMaterial[slot])
						b.MarkRow(slot);
			}
			WatchTransformMoves(k, b.build, [&](std::uint32_t a_slot) { b.MarkRow(a_slot); });
			// A slot whose texture is not resolved stays dirty for the next build.
			for (std::size_t d = 0; d < k.rowDirty.size();) {
				const std::uint32_t slot = k.rowDirty[d];
				if (slot < k.slotMaterial.size() && k.slotMaterial[slot] && !b.MaterialRow(slot)) {
					++d;
					continue;
				}
				k.rowDirtyMark[slot] = 0;
				k.rowDirty[d] = k.rowDirty.back();
				k.rowDirty.pop_back();
			}
			if (k.ownersChanged) {
				auto owners = std::make_shared<std::vector<std::shared_ptr<const void>>>();
				for (const auto& owner : k.slotOwner)
					if (owner)
						owners->push_back(owner);
				k.owners = std::move(owners);
				k.ownersChanged = false;
			}
			b.out.bindingOwners.push_back(k.owners);
		}

		/** @brief Updates which objects each mode holds. A mode whose view states changed reads every object again. */
		void UpdateModes(KeptShadowBuild& b, const std::vector<std::uint32_t>& a_changed)
		{
			auto& k = b.k;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				auto& mode = k.modes[m];
				const ModeRasterStates states = b.in.modeUsed[m] ? b.in.modeRasterStates[m] : ModeRasterStates{};
				if (!mode.active || mode.rasterStates != states) {
					// The exclusions must then read this mode's inputs whole instead of its moves.
					b.out.movesValid[m] = false;
					mode.Reset();
					mode.active = true;
					mode.rasterStates = states;
					for (std::uint32_t o = 0; o < k.heldModes.size(); ++o)
						if (k.Holds(m, o))
							b.Hold(m, o, false);
					if (!states.Empty())
						for (std::uint32_t o = 0; o < b.objects; ++o)
							b.Take(m, o);
				} else if (!states.Empty()) {
					for (const std::uint32_t o : a_changed)
						b.Take(m, o);
					std::vector<std::uint32_t> waiting;
					waiting.swap(mode.waiting);
					for (const std::uint32_t o : waiting) {
						if (o >= mode.waitingMark.size() || !mode.waitingMark[o])
							continue;
						mode.waitingMark[o] = 0;
						b.Take(m, o);
					}
				}
			}
		}

		/** @brief Builds the per-frame list of face shapes with this walk's positions, one entry per object for all its modes. */
		void BuildFaceList(KeptShadowBuild& b)
		{
			auto& k = b.k;
			auto& out = b.out;
			std::vector<std::uint32_t> faceObjects;
			ankerl::unordered_dense::map<std::uint32_t, std::uint8_t> faceModes;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				auto& mode = k.modes[m];
				if (!b.in.modeUsed[m])
					continue;
				std::size_t keptFaces = 0;
				for (const std::uint32_t o : mode.faces) {
					if (o >= mode.faceMark.size() || !mode.faceMark[o])
						continue;
					mode.faces[keptFaces++] = o;
					auto [it, fresh] = faceModes.try_emplace(o, std::uint8_t{ 0 });
					if (fresh)
						faceObjects.push_back(o);
					it->second |= static_cast<std::uint8_t>(1u << m);
				}
				mode.faces.resize(keptFaces);
				out.membership[m] = mode.membership;
				out.deferredPipelines += static_cast<std::uint32_t>(mode.waiting.size());
				out.skippedPipeline += static_cast<std::uint32_t>(mode.waiting.size());
				for (const std::uint32_t o : mode.waiting)
					out.setWaiting += o < mode.waitingMark.size() && mode.waitingMark[o] ? 1 : 0;
			}
			for (const std::uint32_t o : faceObjects) {
				const std::uint32_t streamIndex = StreamGeometry(b.tables, o, b.in.addresses.streamBuffer);
				if (streamIndex == ~0u)
					continue;  // no positions this walk, so the engine draws it
				out.inputList.push_back(ShadowEntry(b.tables, b.lookups, o, k.objectRecord[o], faceModes[o], streamIndex));
			}
		}
	}

	/** @brief The kept path's shared half. Updates rows and records in ShadowKept and notes the objects for the region half. */
	void BuildKeptShadowShared(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out, ShadowKept& k,
		const ShadowMaterialRow& a_plain, ShadowBuildSplit& a_split)
	{
		DCLF_ZONE("CS.DCLF.BuildShadow.KeptShared");
		const std::uint32_t objects = static_cast<std::uint32_t>(a_tables.objects.size());
		++k.builds;
		const std::uint64_t build = ++k.build;
		KeptShadowBuild b{ a_in, a_tables, a_lookups, a_out, k, nullptr, objects, build };
		// If the last region half never ran because its build was abandoned, carry its work into this build.
		const bool carried = a_split.pending;
		b.touched.swap(a_split.touched);
		if (!carried) {
			b.touched.clear();
			a_split.changed.clear();
		}
		const bool resync = BeginKeptBuild(b);
		if (resync) {
			// Every object is read and touched again below.
			b.touched.clear();
			a_split.changed.clear();
		}
		a_out.keptBuild = build;
		a_out.movesValid.fill(!resync);
		if (k.slotMaterial.empty()) {
			k.slotMaterial.push_back(nullptr);  // slot 0 is the plain row
			k.slotDiffuse.push_back(nullptr);
			k.slotRefs.push_back(0);
			k.slotReady.push_back(1);
			k.slotOwner.emplace_back();
		}
		b.WriteRow(0, a_plain);
		b.transformBuffer = ShadowTransformBuffer();

		if (k.heldModes.size() < objects)
			k.heldModes.resize(objects, 0);
		auto& changed = a_split.changed;
		TakeChangedRecords(b, resync, changed);
		UpdateMaterialRows(b);
		for (const std::uint32_t o : changed)
			b.Touch(o);
		a_split.build = build;
		a_split.transformBuffer = b.transformBuffer;
		a_split.resync = resync || (carried && a_split.resync);
		b.touched.swap(a_split.touched);
	}

	/** @brief The kept path's region half. Updates each mode, writes the region entries and builds the per-frame list. */
	void BuildKeptShadowRegion(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out, ShadowKept& k,
		ResidentRegion* a_scene, ShadowBuildSplit& a_split)
	{
		DCLF_ZONE("CS.DCLF.BuildShadow.Kept");
		const std::uint32_t objects = static_cast<std::uint32_t>(a_tables.objects.size());
		KeptShadowBuild b{ a_in, a_tables, a_lookups, a_out, k, a_scene, objects, a_split.build };
		b.transformBuffer = a_split.transformBuffer;
		b.touched.swap(a_split.touched);
		BeginRegion(b, a_split.resync);
		// The main build's resync emptied the region, so rewrite every held object.
		if (a_scene && k.sceneResets != a_scene->resets) {
			k.sceneResets = a_scene->resets;
			for (std::uint32_t o = 0; o < k.heldModes.size() && o < objects; ++o)
				if (k.heldModes[o])
					b.Touch(o);
		}
		UpdateModes(b, a_split.changed);
		for (const std::uint32_t o : b.touched) {
			b.WriteEntry(o);
			k.touchedMark[o] = 0;
		}
		b.touched.clear();
		a_out.regionInputs = a_scene ? a_scene->inputs.View() : k.scene.inputs.View();
		BuildFaceList(b);
		a_out.kept = true;
		a_out.materialRows = k.rows.View();
		b.touched.swap(a_split.touched);
	}

	namespace
	{
		// CountModeDraws goes parallel past kCountFanoutMin inputs, in chunks of kCountGrain.
		constexpr std::size_t kCountFanoutMin = 8192;
		constexpr std::size_t kCountGrain = 4096;

		/**
		 * @brief Brings the kept counts up to date by recounting only the entries the region's journal changed.
		 * Sums wrap mod 2^32 like a full count, so the result does not depend on order.
		 */
		void UpdateRegionCounts(ShadowKept::ModeCounts& a_counts, const ShadowPayload& a_out, const void* a_source, std::uint64_t& a_counted)
		{
			auto& c = a_counts;
			const auto& region = a_out.regionInputs;
			const std::size_t regionCount = region.elements ? region.elements->size() : 0;
			if (!c.contentEpoch || c.source != a_source || c.modeBits != a_out.modeBits) {
				c.source = a_source;
				c.modeBits = a_out.modeBits;
				c.entries.clear();
				c.draws = {};
				c.inputs = {};
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					c.keyDraws[m].clear();
					c.keyInputs[m].clear();
				}
				c.contentEpoch = 0;
			}
			const auto apply = [&](const ShadowKept::ModeCounts::Entry& a_entry, bool a_add) {
				const std::uint32_t draws = PartitionDraws(a_entry.partitions);
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					if (!(a_entry.view.mask & c.modeBits[m]))
						continue;
					const std::uint32_t key = ShadowKeyOf(m, a_entry.view);
					if (key == ~0u)
						continue;
					auto& slots = c.keyDraws[m];
					auto& inputs = c.keyInputs[m];
					if (key >= slots.size()) {
						slots.resize(std::size_t(key) + 1, 0u);
						inputs.resize(std::size_t(key) + 1, 0u);
					}
					if (a_add) {
						++c.inputs[m];
						c.draws[m] += draws;
						slots[key] += draws;
						++inputs[key];
					} else {
						--c.inputs[m];
						c.draws[m] -= draws;
						slots[key] -= draws;
						--inputs[key];
					}
				}
			};
			const auto recount = [&](std::size_t a_entry) {
				auto& entry = c.entries[a_entry];
				apply(entry, false);
				const DrawInput& input = (*region.elements)[a_entry];
				entry = { input.view, input.partitions };
				apply(entry, true);
				++a_counted;
			};
			const std::size_t held = c.entries.size();
			for (std::size_t n = regionCount; n < held; ++n)
				apply(c.entries[n], false);
			c.entries.resize(regionCount);  // a new entry has mask 0 and counts nothing until recounted
			if (!c.contentEpoch) {
				for (std::size_t n = 0; n < regionCount; ++n)
					recount(n);
			} else {
				region.changes.ForEachRun(c.contentEpoch, regionCount, [&](std::uint64_t a_first, std::uint64_t a_count) {
					for (std::uint64_t n = a_first; n < a_first + a_count; ++n)
						recount(static_cast<std::size_t>(n));
				});
				// Recount appended entries too. Recounting twice is harmless.
				for (std::size_t n = held; n < regionCount; ++n)
					recount(n);
			}
			c.contentEpoch = region.ContentEpoch();
			// Trim the key slots to the last used one, matching what a full count would size them to.
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				auto& slots = c.keyDraws[m];
				auto& inputs = c.keyInputs[m];
				while (!inputs.empty() && !inputs.back()) {
					inputs.pop_back();
					slots.pop_back();
				}
			}
		}

		/**
		 * @brief Counts the draws each used mode can produce, in total and per key slot. Skinned inputs draw once per partition.
		 * These sizes set the draw buckets.
		 */
		void CountModeDraws(ShadowPayload& a_out, ShadowKept::ModeCounts* a_counts = nullptr, const void* a_source = nullptr, std::uint64_t* a_counted = nullptr)
		{
			DCLF_ZONE("CS.DCLF.BuildShadow.CountModeDraws");
			std::uint32_t used = 0, usedBits = 0;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				a_out.keySlotDraws[m].clear();
				a_out.modeInputs[m] = 0;
				if (a_out.inputs.modeUsed[m]) {
					used |= 1u << m;
					usedBits |= a_out.modeBits[m];
				}
			}
			struct Counts
			{
				std::array<std::uint64_t, kShadowModeCount> draws{};
				std::array<std::uint32_t, kShadowModeCount> inputs{};
				std::array<std::vector<std::uint32_t>, kShadowModeCount> keySlots;
			};
			const std::size_t regionCount = a_out.regionInputs.elements ? a_out.regionInputs.elements->size() : 0;
			const std::size_t count = regionCount + a_out.inputList.size();
			auto countRange = [&](std::size_t a_begin, std::size_t a_end, Counts& a_counts) {
				for (std::size_t n = a_begin; n < a_end; ++n) {
					const DrawInput& input = n < regionCount ? (*a_out.regionInputs.elements)[n] : a_out.inputList[n - regionCount];
					if (!(input.view.mask & usedBits))
						continue;
					const std::uint32_t inputDraws = PartitionDraws(input.partitions);
					for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
						if (!((used >> m) & 1) || !(input.view.mask & a_out.modeBits[m]))
							continue;
						const std::uint32_t key = ShadowKeyOf(m, input);
						if (key == ~0u)
							continue;
						++a_counts.inputs[m];
						a_counts.draws[m] += inputDraws;
						auto& slots = a_counts.keySlots[m];
						if (key >= slots.size())
							slots.resize(std::size_t(key) + 1, 0u);
						slots[key] += inputDraws;
					}
				}
			};
			std::vector<Counts> chunks;
			if (a_counts) {
				// Chunk 0 is the region from the kept counts. Chunk 1 is the payload's own inputs.
				UpdateRegionCounts(*a_counts, a_out, a_source, *a_counted);
				chunks.resize(2);
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					if (!((used >> m) & 1))
						continue;
					chunks[0].draws[m] = a_counts->draws[m];
					chunks[0].inputs[m] = a_counts->inputs[m];
					chunks[0].keySlots[m] = a_counts->keyDraws[m];
				}
				countRange(regionCount, count, chunks[1]);
			} else if (count >= kCountFanoutMin) {
				chunks.resize((count + kCountGrain - 1) / kCountGrain);
				::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.BuildShadow.CountModeDraws", count, kCountGrain, [&](std::size_t a_begin, std::size_t a_end) {
					DCLF_ZONE("CS.DCLF.BuildShadow.CountModeDrawsShards");
					countRange(a_begin, a_end, chunks[a_begin / kCountGrain]);
				});
			} else {
				chunks.resize(1);
				countRange(0, count, chunks[0]);
			}
			std::array<std::uint64_t, kShadowModeCount> draws{};
			for (const auto& chunk : chunks)
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					draws[m] += chunk.draws[m];
					a_out.modeInputs[m] += chunk.inputs[m];
					auto& slots = a_out.keySlotDraws[m];
					if (slots.size() < chunk.keySlots[m].size())
						slots.resize(chunk.keySlots[m].size(), 0u);
					for (std::size_t key = 0; key < chunk.keySlots[m].size(); ++key)
						slots[key] += chunk.keySlots[m][key];
				}
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				a_out.modeDraws[m] = static_cast<std::uint32_t>(std::min<std::uint64_t>(draws[m], UINT32_MAX));
		}

		/** @brief Writes the geometry table, the frame record and the shared blocks. Returns the null texture's index. */
		std::uint32_t WriteSharedBlocks(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
			std::shared_ptr<const StreamViews>& a_streams)
		{
			a_out.Reset();
			a_out.inputs = a_in;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				a_out.modeBits[m] = ModeViewBits(m);
			if (a_lookups.sharedBindingBlock)
				a_out.bindingOwners.push_back(a_lookups.sharedBindingBlock);
			const std::uint64_t base = a_in.addresses.constants;
			auto& arena = a_out.arena;
			auto block = [&](const void* a_data, std::size_t a_size) -> std::uint64_t {
				const auto offset = arena.Allocate(a_size);
				if (offset == ~0ull)
					return 0;
				if (a_data)
					std::memcpy(arena.At(offset, a_size).data(), a_data, a_size);
				return base + offset;
			};

			TakeGeometryDraws(a_streams.get(), a_tables, a_in.tablesGeneration, a_in.frameNumber, a_out.geometries);
			a_out.streams = std::move(a_streams);
			AppendStreamRows(a_tables, a_in.addresses.streamBuffer, a_out.geometries);
			if (a_in.addresses.streamBuffer)
				a_out.streamRows = a_tables.streamRows;  // a view, safe because the published tables outlive the payload
			// No register the Utility shaders declare may sit at address zero. A late-compiled pipeline reading one would lose
			// the device, so unsupplied registers point at a zero block.
			const auto blocks = ShadowArenaBlocksOf();
			const std::uint64_t end = std::max({ blocks.zeros + kShadowZeroBlockBytes, blocks.sharedData + blocks.sharedBytes, blocks.featureData + blocks.featureBytes });
			if (block(nullptr, static_cast<std::size_t>(end)) != base)
				::RenderGraph::Fatal("Drawcall Limit Fix", "the shadow arena's blocks were not laid out from its start");
			if (a_in.sharedData.size() == blocks.sharedBytes)
				std::memcpy(arena.At(blocks.sharedData, blocks.sharedBytes).data(), a_in.sharedData.data(), blocks.sharedBytes);
			if (blocks.featureBytes && a_in.featureData.size() == blocks.featureBytes)
				std::memcpy(arena.At(blocks.featureData, blocks.featureBytes).data(), a_in.featureData.data(), blocks.featureBytes);
			// The diffuse comes from the material row. Everything else is here.
			auto& frameRecord = a_out.frameRecord;
			const std::uint32_t nullIndex = a_lookups.nullTexture == Lookups::kNone ? 0u : a_lookups.nullTexture;
			for (auto& index : frameRecord.textures)
				index = nullIndex;
			frameRecord.textures[kPlacementBufferRegister] = a_in.addresses.placementsIndex;
			frameRecord.textures[kPaletteBufferRegister] = a_in.addresses.palettesIndex;
			frameRecord.textures[kShadingBufferRegister] = a_in.addresses.shadingIndex;  // a tree's wind until its entry
			Extensions::WriteFrameBindings(a_in.addresses.frameBindings, frameRecord.textures);
			const std::uint32_t wrapAnisotropic = a_lookups.Sampler(static_cast<std::uint32_t>(RE::BSGraphics::TextureAddressMode::kWrapSWrapT),
				static_cast<std::uint32_t>(RE::BSGraphics::TextureFilterMode::kAnisotropic));
			for (auto& index : frameRecord.samplers)
				index = wrapAnisotropic == Lookups::kNone ? 0u : wrapAnisotropic;
			std::memcpy(arena.At(kShadowFrameRecordOffset, sizeof(DrawBindings)).data(), &frameRecord, sizeof(DrawBindings));
			return nullIndex;
		}

		/** @brief Non-kept path. Builds a record for every caster and Skylighting occluder, and the material rows. */
		void BuildMaterialRows(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
			const ShadowMaterialRow& a_plain)
		{
			std::vector<ShadowMaterialRow> rows;
			rows.push_back(a_plain);
			auto& objectRecord = a_out.objectRecord;
			objectRecord.assign(a_tables.objects.size(), ~0u);
			ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::uint32_t> recordByMaterial;
			const bool haveShadowMaterials = a_tables.shadowMaterial.size() == a_tables.objects.size();
			for (std::size_t o = 0; o < a_tables.objects.size(); ++o) {
				const auto& object = a_tables.objects[o];
				const std::uint32_t occlusion = OcclusionTechniques(a_tables, a_in.modeUsed, o);
				if ((object.flags & kObjectNoShadow) && !occlusion)
					continue;
				if (!(((object.flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[o]) & 0x80) && !(occlusion & 0x80)) {
					objectRecord[o] = 0;
					continue;
				}
				// Alpha-tested: one record per material.
				const auto* material = haveShadowMaterials ? a_tables.shadowMaterial[o] : nullptr;
				if (!material) {
					++a_out.skippedTexture;
					continue;
				}
				if (auto it = recordByMaterial.find(material); it != recordByMaterial.end()) {
					objectRecord[o] = it->second;
					continue;
				}
				auto* srv = a_tables.shadowDiffuse[o];
				const auto textureIt = srv ? a_lookups.shadowTextures.find(srv) : a_lookups.shadowTextures.end();
				if (textureIt == a_lookups.shadowTextures.end()) {
					if (srv)
						++a_out.deferredTextures;
					++a_out.skippedTexture;
					continue;
				}
				if (textureIt->second == Lookups::kNone) {
					++a_out.skippedTexture;
					continue;
				}
				ShadowMaterialRow row;
				row.texcoord = ShadowTexcoord(*material, ShadowTransformBuffer());
				row.diffuse = textureIt->second;
				if (const auto owner = a_lookups.shadowTextureOwners.find(srv); owner != a_lookups.shadowTextureOwners.end())
					a_out.bindingOwners.push_back(owner->second);
				const auto index = static_cast<std::uint32_t>(rows.size());
				rows.push_back(row);
				recordByMaterial.emplace(material, index);
				objectRecord[o] = index;
			}
			a_out.materialRows = { std::make_shared<const std::vector<ShadowMaterialRow>>(std::move(rows)), {} };
		}

		/** @brief Non-kept path. Finds the modes that draw each object, then writes one input per object. */
		void BuildModeInputs(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out)
		{
			const auto& objectRecord = a_out.objectRecord;
			std::vector<std::uint8_t> heldModes(a_tables.objects.size(), 0);
			std::vector<std::uint32_t> streams(a_tables.objects.size(), ~0u);
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				if (!a_in.modeUsed[m])
					continue;
				const bool occlusionMode = IsOcclusionMode(m);
				for (std::size_t o = 0; o < a_tables.objects.size(); ++o) {
					const auto& object = a_tables.objects[o];
					if (occlusionMode ? !ModeTechnique(a_tables, m, o) : (object.flags & kObjectNoShadow) != 0)
						continue;
					// A set member skipped below is a defect and is counted.
					const bool member = SetCaster(a_tables, static_cast<std::uint32_t>(o), m);
					if (!member)
						continue;
					struct MemberSkip
					{
						bool armed;
						std::uint32_t& count;
						~MemberSkip() { count += armed ? 1 : 0; }
					} memberSkip{ member, a_out.setWaiting };
					if (objectRecord[o] == ~0u || !ObjectFits(a_tables, static_cast<std::uint32_t>(o), a_in.addresses.fit)) {
						continue;
					}
					const bool volumetricOnly = VolumetricClass(m, object.flags);
					const auto& classStates = a_in.modeRasterStates[m].Of(volumetricOnly);
					if (volumetricOnly && classStates.empty()) {
						memberSkip.armed = false;
						continue;
					}
					// No view of its class draws it, so nothing is withheld.
					if (classStates.empty())
						memberSkip.armed = false;
					const std::uint32_t technique = ModeTechnique(a_tables, m, o);
					const ShadowPipelineKey key{ BaseTechnique(a_tables, m, o), (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
						VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
					const auto slotIt = a_lookups.shadowSlots.find(key);
					if (slotIt == a_lookups.shadowSlots.end()) {
						++a_out.deferredPipelines;
						++a_out.skippedPipeline;
						continue;
					}
					// Every view of the mode must be able to draw it, because the engine's pass is withheld from all of them.
					bool deferred = false, missing = false;
					for (const std::uint32_t state : classStates) {
						if (a_lookups.ShadowMapPipeline(m, state, slotIt->second) == Lookups::kNone) {
							// shadowPipelines tells "not resolved yet" apart from "resolved to nothing".
							const auto pipelineIt = a_lookups.shadowPipelines.find({ technique, key.rasterFlags, key.vertexLayout, state });
							(pipelineIt == a_lookups.shadowPipelines.end() ? deferred : missing) = true;
						}
					}
					if (deferred || missing || classStates.empty()) {
						a_out.deferredPipelines += deferred ? 1 : 0;
						++a_out.skippedPipeline;
						continue;
					}
					// Faces and second-stream casters need this walk's positions, or the engine draws them.
					const std::uint32_t streamIndex = StreamGeometry(a_tables, o, a_in.addresses.streamBuffer);
					if (streamIndex == ~0u && (IsStreamObject(a_tables, o) || (key.vertexLayout & kPositionInSecondStream))) {
						continue;
					}
					// No plane test here. BuildDraws tests the sun entry sphere on the GPU.
					heldModes[o] |= static_cast<std::uint8_t>(1u << m);
					streams[o] = streamIndex;
					memberSkip.armed = false;
				}
			}
			for (std::uint32_t o = 0; o < heldModes.size(); ++o)
				if (heldModes[o])
					a_out.inputList.push_back(ShadowEntry(a_tables, a_lookups, o, objectRecord[o], heldModes[o], streams[o]));
		}
	}

	void BuildShadowPayloadShared(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
		std::shared_ptr<const StreamViews> a_streams, ShadowKept* a_kept, ShadowBuildSplit& a_split)
	{
		DCLF_ZONE("CS.DCLF.BuildShadowPayload.Shared");
		const std::uint32_t nullIndex = WriteSharedBlocks(a_in, a_tables, a_lookups, a_out, a_streams);
		// Row 0 serves every caster without alpha testing.
		ShadowMaterialRow plain;
		plain.diffuse = nullIndex;
		if (a_kept)
			BuildKeptShadowShared(a_in, a_tables, a_lookups, a_out, *a_kept, plain, a_split);
		else
			BuildMaterialRows(a_in, a_tables, a_lookups, a_out, plain);
		a_split.pending = true;
	}

	void BuildShadowPayloadRegion(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out, ShadowKept* a_kept,
		ResidentRegion* a_scene, ShadowBuildSplit& a_split)
	{
		DCLF_ZONE("CS.DCLF.BuildShadowPayload.Region");
		a_split.pending = false;
		if (a_kept) {
			BuildKeptShadowRegion(a_in, a_tables, a_lookups, a_out, *a_kept, a_scene, a_split);
			CountModeDraws(a_out, &a_kept->modeCounts, a_scene ? static_cast<const void*>(a_scene) : &a_kept->scene, &a_kept->entriesCounted);
			Diagnostics::Notify(Diagnostics::ShadowBuilt{ a_in, a_tables, a_lookups, a_out, *a_kept });
			return;
		}
		BuildModeInputs(a_in, a_tables, a_lookups, a_out);
		CountModeDraws(a_out);
	}

	void BuildShadowPayload(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
		std::shared_ptr<const StreamViews> a_streams, ShadowKept* a_kept, ResidentRegion* a_scene)
	{
		DCLF_ZONE("CS.DCLF.BuildShadowPayload");
		ShadowBuildSplit split;
		BuildShadowPayloadShared(a_in, a_tables, a_lookups, a_out, std::move(a_streams), a_kept, split);
		BuildShadowPayloadRegion(a_in, a_tables, a_lookups, a_out, a_kept, a_scene, split);
	}
}
