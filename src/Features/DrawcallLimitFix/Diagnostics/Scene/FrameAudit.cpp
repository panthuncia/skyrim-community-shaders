// The frame audit: the installed tables and the frame's placement rows against the engine now. A frame that draws the wrong scene
// tells from it whether the tables (stale buffers) or the rows (stale transforms) are wrong. Diagnostics only, at cs-test's shots.
#include "FrameAudit.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "TextureStreaming/Streamer.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DCLF::Diagnostics
{
	namespace
	{
		struct AuditCounts
		{
			std::uint32_t objects = 0, land = 0;
			std::uint32_t bufferChecked = 0, bufferMismatch = 0, landBufferMismatch = 0;
			std::uint32_t rowChecked = 0, rowMismatch = 0, landRowMismatch = 0;
			std::string firstMismatches;
			std::uint32_t named = 0;
		};

		bool SameWorld(const float (&a_a)[12], const float (&a_b)[12])
		{
			for (std::uint32_t i = 0; i < 12; ++i) {
				// Translation in units, rotation and scale in fractions.
				const float tolerance = (i % 4 == 3) ? 1.0f : 0.01f;
				if (!(std::abs(a_a[i] - a_b[i]) <= tolerance))
					return false;
			}
			return true;
		}

		void Name(AuditCounts& a_counts, const RE::BSGeometry& a_geometry, std::uint32_t a_slot, std::string_view a_what, std::string_view a_detail = {})
		{
			if (a_counts.named++ >= 4)
				return;
			a_counts.firstMismatches += fmt::format("{}{} slot {} '{}'{}", a_counts.firstMismatches.empty() ? "" : ", ", a_what, a_slot, a_geometry.name.c_str(), a_detail);
		}

		// A translation rounded to whole units, to find which object a row's transform belongs to.
		std::uint64_t PlaceKey(const float (&a_world)[12])
		{
			const auto unit = [](float a_value) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(a_value)) & 0x1FFFFF); };
			return unit(a_world[3]) | (unit(a_world[7]) << 21) | (unit(a_world[11]) << 42);
		}
	}

	namespace
	{
		/**
		 * @brief Each material an installed object draws with against the engine and the texture imports: whether the entry is the
		 * object's material now, and whether each texture index is its import's, the import not replaced, and a shell's import its
		 * current window. A frame that draws flat or black textures with right rows tells from it which binding is wrong.
		 */
		std::string AuditMaterialBindings(const Scene::FrameView::Tables& a_tables)
		{
			const auto& lookups = Scene::FrameView::Get().GetLookups();
			struct Counts
			{
				std::uint32_t materials = 0, land = 0, otherMaterial = 0, textures = 0, unresolved = 0, indexDiffers = 0, superseded = 0, staleWindow = 0;
				std::uint32_t landBad = 0, named = 0;
				std::string first;
			} c;
			const auto name = [&](const RE::BSGeometry& a_geometry, std::uint32_t a_material, std::string_view a_what) {
				if (c.named++ < 4)
					c.first += fmt::format("{}material {} of '{}': {}", c.first.empty() ? "" : ", ", a_material, a_geometry.name.c_str(), a_what);
			};
			std::unordered_set<std::uint32_t> seen;
			for (std::uint32_t slot = 0; slot < a_tables.objects.size() && slot < a_tables.objectGeometry.size(); ++slot) {
				const auto* geometry = a_tables.objectGeometry[slot];
				const auto& object = a_tables.objects[slot];
				if (!geometry || (object.flags & kObjectNoBindings) || object.materialIndex >= lookups.materials.size() || !seen.insert(object.materialIndex).second)
					continue;
				const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				const bool land = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);
				const auto& entry = lookups.materials[object.materialIndex];
				++c.materials;
				c.land += land ? 1u : 0u;
				bool bad = false;
				if (property && entry.key.first != property->material) {
					++c.otherMaterial;
					bad = true;
					name(*geometry, object.materialIndex, "another material's entry");
				}
				for (std::uint32_t t = 0; t < Lookups::kTextureSlots; ++t) {
					auto* view = entry.views[t];
					if (!view)
						continue;
					++c.textures;
					const void* owner = entry.bindingBlock ? entry.bindingBlock->textures[t].get() : nullptr;
					if (!entry.resolved || !owner) {
						++c.unresolved;
						continue;
					}
					std::string what;
					if (GpuTextures::ImportedIndex(owner) != entry.textureIndex[t]) {
						++c.indexDiffers;
						what = fmt::format("t{} index {} but its import's {}", t, entry.textureIndex[t], GpuTextures::ImportedIndex(owner));
					} else if (GpuTextures::Superseded(owner)) {
						++c.superseded;
						what = fmt::format("t{} import replaced", t);
					} else if (TextureStreaming::IdOf(view)) {
						winrt::com_ptr<ID3D11ShaderResourceView> window;
						window.attach(TextureStreaming::CurrentWindow(view));
						TextureStreaming::GraphWindow graph;
						if (GpuTextures::ImportedWindow(owner) != window.get()) {
							++c.staleWindow;
							what = fmt::format("t{} imported another window than the current one", t);
						} else if (window && TextureStreaming::GraphWindowOf(view, window.get(), graph) && graph.index != entry.textureIndex[t]) {
							++c.indexDiffers;
							what = fmt::format("t{} index {} but its window's {}", t, entry.textureIndex[t], graph.index);
						}
					}
					if (!what.empty()) {
						bad = true;
						name(*geometry, object.materialIndex, what);
					}
				}
				c.landBad += land && bad ? 1u : 0u;
			}
			return fmt::format("materials {} ({} land): {} another material's, {} land wrong; textures {}: {} unresolved, {} index differs, {} replaced, {} stale window{}{}",
				c.materials, c.land, c.otherMaterial, c.landBad, c.textures, c.unresolved, c.indexDiffers, c.superseded, c.staleWindow, c.first.empty() ? "" : "; first: ",
				c.first);
		}
	}

	std::string AuditFrameRows()
	{
		const auto& tables = Scene::FrameView::Get().GetTables();
		const auto* rows = FrameValues::Get().RowsIfDone();
		AuditCounts c;
		// Where each object is now, so a wrong row can name whose transform it holds.
		std::unordered_map<std::uint64_t, std::uint32_t> slotAt;
		std::vector<BindlessPlacement> now(tables.objects.size());
		std::vector<std::uint8_t> sampled(tables.objects.size(), 0);
		for (std::uint32_t slot = 0; slot < tables.objects.size() && slot < tables.objectGeometry.size(); ++slot)
			if (tables.objectGeometry[slot] && FrameValues::SampleSlot(tables, slot, now[slot])) {
				sampled[slot] = 1;
				slotAt.emplace(PlaceKey(now[slot].world), slot);
			}
		for (std::uint32_t slot = 0; slot < tables.objects.size() && slot < tables.objectGeometry.size(); ++slot) {
			const auto* geometry = tables.objectGeometry[slot];
			if (!geometry)
				continue;
			++c.objects;
			const auto& data = geometry->GetGeometryRuntimeData();
			const auto* property = data.shaderProperty.get();
			const bool land = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);
			c.land += land ? 1u : 0u;
			// Plain shapes only: a skin draws its partitions' buffers and particles build theirs.
			const auto g = tables.objects[slot].geometryIndex;
			if (geometry->GetType() == RE::BSGeometry::Type::kTriShape && !data.skinInstance && data.rendererData && g < tables.geometries.size()) {
				++c.bufferChecked;
				const auto& buffers = tables.geometries[g];
				if (buffers.vertexBuffer != reinterpret_cast<ID3D11Buffer*>(data.rendererData->vertexBuffer) ||
					buffers.indexBuffer != reinterpret_cast<ID3D11Buffer*>(data.rendererData->indexBuffer)) {
					++c.bufferMismatch;
					c.landBufferMismatch += land ? 1u : 0u;
					Name(c, *geometry, slot, "buffers");
				}
			}
			if (rows && slot < rows->size() && sampled[slot]) {
				++c.rowChecked;
				const auto& row = (*rows)[slot];
				if (!SameWorld(row.world, now[slot].world)) {
					++c.rowMismatch;
					c.landRowMismatch += land ? 1u : 0u;
					const auto owner = slotAt.find(PlaceKey(row.world));
					Name(c, *geometry, slot, "row",
						fmt::format(" holds ({:.0f} {:.0f} {:.0f}){}", row.world[3], row.world[7], row.world[11],
							owner != slotAt.end() ? fmt::format(", slot {}'s place", owner->second) : std::string(", no object's place")));
				}
			}
		}
		// The producer since the last shot (the reports take these too, with CS_DCLF_STATS).
		const auto p = FrameValues::Get().TakeCounters();
		const std::string materials = AuditMaterialBindings(tables);
		return fmt::format(
			"audit: {} objects ({} land); buffers {} checked, {} differ ({} land); rows {}, {} checked, {} differ ({} land){}{}; producer since the last take: "
			"{} plans ({} resets, {} items), {} sampled, {} settled, {} refused, {} failures; {}",
			c.objects, c.land, c.bufferChecked, c.bufferMismatch, c.landBufferMismatch, rows ? "held" : "not done", c.rowChecked, c.rowMismatch, c.landRowMismatch,
			c.firstMismatches.empty() ? "" : "; first: ", c.firstMismatches, p.plans, p.planResets, p.planItems, p.sampled, p.settled, p.refused, p.failures, materials);
	}
}
