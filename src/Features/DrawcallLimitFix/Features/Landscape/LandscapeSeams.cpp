// Landscape seam resources are per quad, captured into the object shading row without splitting terrain batches.
// The row holds the weights' and borrowed textures' descriptors, and slot 1 names the quad's row of a quad table, a frame-value
// pool read at t39. Row 0 is all zero, so its valid flag is off.
#include "EngineFixes/LandscapeSeamsFix.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Features/Landscape/Landscape.h"

#include <algorithm>
#include <array>
#include <fmt/ranges.h>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace DCLF::Features::Landscape
{
	namespace
	{
		struct Captured
		{
			std::shared_ptr<const LandscapeSeamsFix::Resources> source;
			std::array<GpuTextures::Binding, LandscapeSeamsFix::NumPSTextures> bindings;
			std::array<ID3D11ShaderResourceView*, LandscapeSeamsFix::NumPSTextures> views{};
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t row = 0;  // the quad table's row, 0 for none
		};
		// The quad data's slot in the shading row. It names a quad table row, not a descriptor.
		constexpr std::size_t kDataSlot = 1;
		using QuadData = LandscapeSeamsFix::QuadData;

		struct QuadRow
		{
			std::shared_ptr<const LandscapeSeamsFix::Resources> source;  // held so its address, the row's key, is not reused
			std::uint32_t users = 0;
		};

		// The quad table. Render thread only. It never shrinks, so a stale row index still reads inside it.
		struct QuadTable
		{
			std::vector<QuadRow> rows = std::vector<QuadRow>(1);
			std::vector<std::uint32_t> freeRows;
			std::unordered_map<const LandscapeSeamsFix::Resources*, std::uint32_t> rowOf;
			// Rows no kick handed over yet.
			std::vector<std::pair<std::uint32_t, LandscapeSeamsFix::QuadData>> unsent;
		};
		QuadTable table;

		std::uint32_t AcquireRow(const std::shared_ptr<const LandscapeSeamsFix::Resources>& a_source)
		{
			if (const auto it = table.rowOf.find(a_source.get()); it != table.rowOf.end()) {
				++table.rows[it->second].users;
				return it->second;
			}
			std::uint32_t row;
			if (!table.freeRows.empty()) {
				row = table.freeRows.back();
				table.freeRows.pop_back();
			} else {
				row = static_cast<std::uint32_t>(table.rows.size());
				table.rows.emplace_back();
			}
			table.rows[row] = { a_source, 1 };
			table.rowOf.emplace(a_source.get(), row);
			table.unsent.emplace_back(row, a_source->quad);
			return row;
		}

		// No slot names a released row, so its stale bytes are never read. Frames already recorded keep their own table version.
		void ReleaseRow(std::uint32_t a_row)
		{
			if (!a_row || --table.rows[a_row].users)
				return;
			table.rowOf.erase(table.rows[a_row].source.get());
			table.rows[a_row].source.reset();
			table.freeRows.push_back(a_row);
		}
		using Views = std::array<ID3D11ShaderResourceView*, LandscapeSeamsFix::NumPSTextures>;
		std::unordered_set<std::uint32_t> candidates;
		std::unordered_map<std::uint32_t, Captured> captured;
		std::uint32_t generation = ~0u;
		// While neither moves, a captured quad can only change through its slot, geometry or texture views.
		std::uint64_t seamsChanges = ~0ull;
		std::uint64_t relocationCursor = 0;
		std::vector<ID3D11ShaderResourceView*> moved;
		std::vector<std::uint32_t> dirty;  // Slots to capture again this frame.
		// Why this frame's captures recaptured, for the zone's text: new, device, source, views, forced, invalid, superseded.
		std::array<std::uint32_t, 7> reasons{};
		// This frame's invalid bindings by texture slot, and by ResolveFailure (none, rejected, pending, streamed window, no context).
		std::array<std::uint32_t, LandscapeSeamsFix::NumPSTextures> invalidSlots{};
		std::array<std::uint32_t, 5> invalidFailures{};

		Views ViewsOf(const LandscapeSeamsFix::Resources& a_source)
		{
			Views views{};
			views[0] = a_source.weights.get();
			// kDataSlot stays null: the quad table holds the data.
			for (std::size_t extra = 0; extra < a_source.extras.size(); ++extra)
				for (std::size_t t = 0; t < a_source.extras[extra].size(); ++t)
					if (const auto& texture = a_source.extras[extra][t]; texture && texture->rendererTexture)
						views[2 + extra * LandscapeSeamsFix::TexturesPerExtra + t] = texture->rendererTexture->resourceView;
			return views;
		}

		// Only valid when the seams fix changed nothing and the relocation log is complete.
		bool Unchanged(const Captured& a_previous, const RE::BSGeometry* a_geometry)
		{
			if (a_previous.geometry != a_geometry || !a_previous.source || ViewsOf(*a_previous.source) != a_previous.views)
				return false;
			for (std::size_t t = 0; t < a_previous.bindings.size(); ++t) {
				// An unresolved binding is tried again every frame.
				if (t != kDataSlot && a_previous.bindings[t].index == GpuTextures::kInvalid)
					return false;
				if (a_previous.views[t] && !moved.empty() && std::binary_search(moved.begin(), moved.end(), a_previous.views[t]))
					return false;
			}
			return true;
		}

		const RE::BSGeometry* GeometryOf(const Api::ShadingFieldCapture& a_capture, std::uint32_t a_slot)
		{
			return a_slot < a_capture.objectGeometry.size() ? a_capture.objectGeometry[a_slot] : nullptr;
		}

		// Returns false when the slot is not a land quad, which drops it from the candidates.
		bool CaptureSlot(Api::ShadingFieldCapture& a_capture, std::uint32_t a_slot, bool a_changedDevice, bool a_forced)
		{
			auto& textures = GpuTextures::Get();
			auto* geometry = a_slot < a_capture.objectGeometry.size() ? a_capture.objectGeometry[a_slot] : nullptr;
			auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			const bool land = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);
			auto source = land ? LandscapeSeamsFix::GetInstance().CaptureResources(geometry) : nullptr;
			if (!source) {
				if (const auto it = captured.find(a_slot); it != captured.end()) {
					ReleaseRow(it->second.row);
					captured.erase(it);
					a_capture.Put(a_slot, {});
				}
				return land;
			}
			auto [entry, inserted] = captured.try_emplace(a_slot);
			auto& previous = entry->second;
			const Views views = ViewsOf(*source);
			previous.geometry = geometry;
			bool invalid = false, superseded = false;
			for (std::size_t t = 0; t < previous.bindings.size(); ++t) {
				if (t == kDataSlot)
					continue;
				const auto& binding = previous.bindings[t];
				// Null descriptors have device owners, not imports that Superseded can inspect.
				invalid |= binding.index == GpuTextures::kInvalid;
				superseded |= previous.views[t] && GpuTextures::Superseded(binding.owner.get());
			}
			const bool sourceChanged = previous.source != source, viewsChanged = previous.views != views;
			const bool changed = inserted || a_changedDevice || sourceChanged || viewsChanged || a_forced || invalid || superseded;
			if (changed) {
				std::size_t r = 0;
				for (const bool why : { inserted, a_changedDevice, sourceChanged, viewsChanged, a_forced, invalid, superseded })
					reasons[r++] += why ? 1u : 0u;
			}
			if (!changed)
				return true;
			std::array<std::uint32_t, LandscapeSeamsFix::NumPSTextures> indices{};
			std::vector<std::shared_ptr<const void>> owners;
			owners.reserve(views.size());
			if (sourceChanged || !previous.row) {
				const std::uint32_t row = AcquireRow(source);
				ReleaseRow(previous.row);
				previous.row = row;
			}
			bool valid = views[0] != nullptr;
			// These are IMMUTABLE D3D11 resources or file textures, exempt from graph tracking, so one import per view is enough.
			for (std::size_t t = 0; t < views.size(); ++t) {
				if (t == kDataSlot) {
					indices[t] = previous.row;
					continue;
				}
				auto& binding = previous.bindings[t];
				// The land's textures stream. The binding follows the window there is now; a new window supersedes it, which the
				// relocation log names, and the quad is captured again then.
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				binding = textures.ResolveFollowingWindow(views[t], GpuTextures::Tag(GpuTextures::Source::FrameSlot, LandscapeSeamsFix::FirstPSTexture + static_cast<std::uint32_t>(t)), &failure);
				if (binding.index == GpuTextures::kInvalid) {
					++invalidSlots[t];
					++invalidFailures[std::min<std::size_t>(static_cast<std::size_t>(failure), invalidFailures.size() - 1)];
				}
				valid &= binding.index != GpuTextures::kInvalid;
				indices[t] = binding.index + 1;  // Zero is an unpopulated row; descriptor zero is still usable.
				owners.push_back(binding.owner);
			}
			previous.source = std::move(source);
			previous.views = views;
			std::array<float, Api::kShadingFieldValueFloats> value{};
			static_assert(sizeof(indices) == sizeof(value));
			if (valid)
				std::memcpy(value.data(), indices.data(), sizeof(indices));
			a_capture.Put(a_slot, value, textures.Seal(owners));
			return true;
		}

		void CaptureSeams(Api::ShadingFieldCapture& a_capture)
		{
			DCLF_ZONE("CS.DCLF.Landscape.Seams");
			reasons = {};
			invalidSlots = {};
			invalidFailures = {};
			// The zone's text: the pass, the quads it captured again and why. Called where the capture ends.
			const char* pass = "dirty";
			std::size_t dirtyCount = 0;
			const auto report = [&] {
				if (!TracyIsConnected)
					return;
				char text[320];
				const auto written = fmt::format_to_n(text, sizeof(text),
					"{}: quads {} dirty {} changed {}; recaptured new {} device {} source {} views {} forced {} invalid {} superseded {}; invalid by slot {}, by failure {}",
					pass, candidates.size(), dirtyCount, a_capture.changedObjects.size(), reasons[0], reasons[1], reasons[2], reasons[3], reasons[4], reasons[5], reasons[6],
					fmt::join(invalidSlots, "/"), fmt::join(invalidFailures, "/"));
				ZoneText(text, std::min<std::size_t>(written.size, sizeof(text)));
			};
			auto& textures = GpuTextures::Get();
			// Read before any quad, so a later change is seen at the next capture.
			const std::uint64_t changes = LandscapeSeamsFix::GetInstance().ResourceChanges();
			moved.clear();
			const bool relocationsComplete = textures.RelocatedViews(relocationCursor, moved);
			std::sort(moved.begin(), moved.end());
			const bool changedDevice = generation != textures.Generation();
			generation = textures.Generation();
			const bool seamsChanged = std::exchange(seamsChanges, changes) != changes;
			const bool full = changedDevice || !relocationsComplete || seamsChanged;
			if (full)
				pass = changedDevice ? "full (device)" : !relocationsComplete ? "full (relocations)" : "full (seams)";
			const std::unordered_set<std::uint32_t> reset(a_capture.changedObjects.begin(), a_capture.changedObjects.end());
			for (const auto slot : a_capture.changedObjects)
				candidates.insert(slot);
			if (full) {
				for (auto it = candidates.begin(); it != candidates.end();) {
					if (CaptureSlot(a_capture, *it, changedDevice, reset.contains(*it)))
						++it;
					else
						it = candidates.erase(it);
				}
				report();
				return;
			}
			// With no seams fix change, a quad without resources stays that way. Recapture changed slots and stale rows.
			dirty.assign(a_capture.changedObjects.begin(), a_capture.changedObjects.end());
			for (const auto& [slot, previous] : captured)
				if (!Unchanged(previous, GeometryOf(a_capture, slot)))
					dirty.push_back(slot);
			std::sort(dirty.begin(), dirty.end());
			dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
			dirtyCount = dirty.size();
			for (const auto slot : dirty)
				if (!CaptureSlot(a_capture, slot, false, reset.contains(slot)))
					candidates.erase(slot);
			report();
		}
	}

	namespace
	{
		Api::FrameValuePoolId quadPool;  // install

		struct QuadTableInput
		{
			std::vector<std::pair<std::uint32_t, QuadData>> rows;
			std::uint32_t count = 0;
		};
		std::shared_ptr<const QuadTableInput> idleTable;

		// The producers run one at a time in frame order.
		KeptWholeArray<QuadData> producedQuads;

		// Render thread, at Kick, after this frame's capture: every row made since the last kick.
		Api::FrameValueInput KickQuads(const Api::FrameValuePoolKick&)
		{
			const auto count = static_cast<std::uint32_t>(table.rows.size());
			if (!table.unsent.empty())
				return { std::make_shared<const QuadTableInput>(QuadTableInput{ std::exchange(table.unsent, {}), count }), count };
			if (!idleTable || idleTable->count != count)
				idleTable = std::make_shared<const QuadTableInput>(QuadTableInput{ {}, count });
			return { idleTable, count };
		}

		void SampleQuads(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.BeginBuild(producedQuads);
			auto& rows = producedQuads.Mutable();
			const auto* input = a_producer.Input<QuadTableInput>();
			const std::uint32_t count = input ? input->count : 0u;
			if (rows.size() < count) {
				const std::size_t first = rows.size();
				rows.resize(count, QuadData{});
				producedQuads.MarkRange(first, count - first);
			}
			if (input)
				for (const auto& [at, row] : input->rows) {
					if (at >= rows.size())
						continue;
					rows[at] = row;
					producedQuads.Mark(at);
				}
		}

		void SendQuads(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(producedQuads);
		}

		void SelectQuadTable(Api::FrameBindingSelect& a_select)
		{
			if (a_select.renderThread)
				a_select.index = FrameValues::Get().PoolIndex(quadPool);
		}
	}

	void InstallSeams(Api::InstallContext& a_context)
	{
		quadPool = a_context.RegisterFrameValuePool(
			{ .name = "landscape-seam-quads", .stride = sizeof(QuadData), .kick = &KickQuads, .sample = &SampleQuads, .send = &SendQuads });
		a_context.RegisterFrameBinding({ .name = "landscape seam quads SRV", .textureRegister = kSeamQuadsRegister, .select = &SelectQuadTable });
		a_context.RegisterShadingField({ .name = "landscape seams", .field = Api::ShadingRowField::LandscapeSeams, .capture = &CaptureSeams });
	}
}
