// Advanced Skin's per-geometry material for DCLF draws. A table of material rows is a frame-value pool read at t38, and each
// actor-owned slot's shading row names its row. Captured on the render thread at the frame's start, through Skin's own resolve.

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "RenderGraph/ShaderBindings.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <winrt/base.h>

#include "Features/Skin.h"
#include "Globals.h"
#include "RenderGraph/Fatal.h"

namespace DCLF::Integrations::AdvancedSkin
{
	namespace
	{
		static_assert(sizeof(MaterialRow::profile) == sizeof(Skin::SkinData));
		static_assert(sizeof(MaterialRow::materialFlags) == sizeof(float4));
		static_assert(SkinMaterials::TextureCount == kMaterialTextureRegisters.size());
		constexpr std::size_t kSkinDetailDescriptor = 2;        // t72's index in a row's descriptors
		constexpr std::size_t kSkinSamplerDescriptor = 3;       // s15's
		constexpr std::size_t kSkinDetailEnabled = 2 * 4 + 3;   // SkinData::skinDetailParams.w
		constexpr std::uint32_t kSkinViewEvictAfter = 300;      // captures a view may go unused before its binding is dropped
		// A kept slot is still resolved this often, so Skin keeps observing its geometry (it prunes after 600 frames) and its views
		// stay in use. It also bounds how long a change no key covers, such as a new owner on the same node, takes to show.
		constexpr std::uint32_t kSkinRefreshCaptures = 120;

		struct MaterialRowHash
		{
			std::size_t operator()(const MaterialRow& a_row) const
			{
				return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(&a_row), sizeof(a_row)));
			}
		};
		struct MaterialRowEqual
		{
			bool operator()(const MaterialRow& a_left, const MaterialRow& a_right) const { return std::memcmp(&a_left, &a_right, sizeof(a_left)) == 0; }
		};

		/** @brief A texture's descriptor. The view is held so its address, the cache's key, cannot be reused while cached. */
		struct MaterialViewBinding
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			GpuTextures::Binding binding;  // kInvalid until the first reply
			bool asked = false;
			bool failed = false;     // the import failed: the material draws without this texture and it is not asked again
			std::uint32_t used = 0;  // the capture that last used it
		};

		struct MaterialTableEntry
		{
			MaterialRow row{};
			std::uint32_t users = 0;
			std::shared_ptr<const void> owners;  // the row's descriptors, handed out with every put of it
		};

		/** @brief What Skin's resolve reads from a geometry, besides its owner. The same key and Skin revision give the same material. */
		struct SurfaceKey
		{
			const void* geometry = nullptr;
			const void* property = nullptr;
			const void* material = nullptr;
			const void* textureSet = nullptr;
			const void* diffuse = nullptr;
			const void* normal = nullptr;
			const void* parent = nullptr;
			const void* alpha = nullptr;
			std::uint64_t flags = 0;
			bool blending = false;
			bool active = false;
			bool operator==(const SurfaceKey&) const = default;
		};

		SurfaceKey KeyOf(RE::BSGeometry* a_geometry, bool a_active)
		{
			SurfaceKey key{ .geometry = a_geometry, .active = a_active };
			if (!a_active || !a_geometry)
				return key;
			const auto& data = a_geometry->GetGeometryRuntimeData();
			auto* property = data.shaderProperty.get();
			key.parent = a_geometry->parent;
			key.property = property;
			if (const auto* alpha = data.alphaProperty.get()) {
				key.alpha = alpha;
				key.blending = alpha->GetAlphaBlending();
			}
			if (!property)
				return key;
			key.flags = property->flags.underlying();
			key.material = property->material;
			if (SkinMaterials::IsCompatible(property)) {
				const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
				key.textureSet = material->textureSet.get();
				key.diffuse = material->diffuseTexture.get();
				key.normal = material->normalTexture.get();
			}
			return key;
		}

		struct MaterialSlotState
		{
			bool member = false;
			bool force = false;     // put the row again even if its index did not change
			bool complete = false;  // every view the material names is bound, or failed for good
			bool kept = false;      // key and revision hold what the last resolve saw
			std::uint32_t row = 0;
			std::uint32_t at = 0;  // position in MaterialState::members
			std::array<ID3D11ShaderResourceView*, 3> views{};
			SurfaceKey key;
			std::uint64_t revision = 0;  // Skin::MaterialRevision at the last resolve
		};

		// Render thread only, apart from the replies the import thread pushes.
		struct MaterialState
		{
			GpuTextures::Replies replies;
			std::uint64_t cookie = 1;
			std::uint32_t generation = ~0u;
			std::uint32_t captures = 0;
			std::uint64_t relocationCursor = 0;
			std::vector<ID3D11ShaderResourceView*> relocated;
			std::uint64_t rowZeroRevision = ~0ull;
			std::unordered_map<ID3D11ShaderResourceView*, MaterialViewBinding> views;
			org::OwnedDescriptorBinding sampler;
			// Row 0 is the disabled profile, which a slot with no skin surface names. It is never freed.
			std::vector<MaterialTableEntry> table = std::vector<MaterialTableEntry>(1);
			std::vector<std::uint32_t> freeRows;
			std::unordered_map<MaterialRow, std::uint32_t, MaterialRowHash, MaterialRowEqual> rowIndex;
			std::vector<MaterialSlotState> slots;
			std::vector<std::uint32_t> members;
			// Rows no kick handed over yet, and the most rows named. The table never shrinks.
			std::vector<std::pair<std::uint32_t, MaterialRow>> unsent;
			std::uint32_t rows = 1;
		};

		MaterialState& SkinMaterialState()
		{
			static MaterialState state;
			return state;
		}

		void AcquireRow(MaterialState& a_state, std::uint32_t a_row)
		{
			if (a_row)
				++a_state.table[a_row].users;
		}

		void ReleaseRow(MaterialState& a_state, std::uint32_t a_row)
		{
			if (!a_row)
				return;
			auto& entry = a_state.table[a_row];
			if (--entry.users)
				return;
			// No slot names it, so its stale bytes are never read. Puts already made keep their owners until the GPU is done.
			a_state.rowIndex.erase(entry.row);
			entry.owners.reset();
			a_state.freeRows.push_back(a_row);
		}

		void JoinSlot(MaterialState& a_state, std::uint32_t a_slot)
		{
			if (a_state.slots.size() <= a_slot)
				a_state.slots.resize(std::size_t(a_slot) + 1);
			auto& slot = a_state.slots[a_slot];
			slot.force = true;
			if (slot.member)
				return;
			slot = { .member = true, .force = true, .at = static_cast<std::uint32_t>(a_state.members.size()) };
			a_state.members.push_back(a_slot);
		}

		// Core zeroes the field of a slot that leaves the class.
		void LeaveSlot(MaterialState& a_state, std::uint32_t a_slot)
		{
			if (a_slot >= a_state.slots.size() || !a_state.slots[a_slot].member)
				return;
			auto& slot = a_state.slots[a_slot];
			ReleaseRow(a_state, slot.row);
			const std::uint32_t moved = a_state.members.back();
			a_state.members[slot.at] = moved;
			a_state.slots[moved].at = slot.at;
			a_state.members.pop_back();
			slot = {};
		}

		void MaterialMembership(const Api::ShadingFieldMembership& a_membership)
		{
			auto& state = SkinMaterialState();
			for (const auto& change : a_membership.changes) {
				if (change.clear) {
					while (!state.members.empty())
						LeaveSlot(state, state.members.back());
				} else if (change.group) {
					JoinSlot(state, change.slot);
				} else {
					LeaveSlot(state, change.slot);
				}
			}
		}

		// Every binding and row is for the old device. The rows' indices stay, and every member is put again.
		void ResetMaterialDevice(MaterialState& a_state, std::uint32_t a_generation)
		{
			a_state.generation = a_generation;
			++a_state.cookie;
			a_state.views.clear();
			a_state.sampler = {};
			for (std::uint32_t r = 1; r < a_state.table.size(); ++r)
				a_state.table[r].owners.reset();
			for (const auto slot : a_state.members) {
				ReleaseRow(a_state, a_state.slots[slot].row);
				a_state.slots[slot].row = 0;
				a_state.slots[slot].force = true;
			}
		}

		void DrainMaterialReplies(MaterialState& a_state)
		{
			a_state.replies.Drain([&](GpuTextures::Reply&& a_reply) {
				if (a_reply.cookie != a_state.cookie)
					return;
				const auto it = a_state.views.find(a_reply.view);
				if (it == a_state.views.end())
					return;
				it->second.asked = false;
				// Skin validated the DDS when it loaded it, so a failed import is a bug. The skin still draws, without this texture.
				if (!a_reply.binding.owner) {
					it->second.failed = true;
					logger::error("[DCLF] Advanced Skin's texture {} could not be imported for DCLF's draws; its skin draws without it",
						fmt::ptr(a_reply.view));
					return;
				}
				// Moved meanwhile: the next capture asks again, and the old binding stays in use until then.
				if (GpuTextures::Superseded(a_reply.binding.owner.get()))
					return;
				it->second.binding = std::move(a_reply.binding);
			});
		}

		// The view's binding, or null while it is still being imported.
		const GpuTextures::Binding* BindView(MaterialState& a_state, const winrt::com_ptr<ID3D11ShaderResourceView>& a_view)
		{
			auto [it, inserted] = a_state.views.try_emplace(a_view.get());
			auto& entry = it->second;
			if (inserted)
				entry.view = a_view;
			entry.used = a_state.captures;
			const bool bound = entry.binding.index != GpuTextures::kInvalid;
			if (!entry.asked && !entry.failed && (!bound || GpuTextures::Superseded(entry.binding.owner.get()))) {
				GpuTextures::Get().Request(a_view.get(), GpuTextures::Tag(GpuTextures::Source::Feature), a_state.replies, a_state.cookie);
				entry.asked = true;
			}
			return bound ? &entry.binding : nullptr;
		}

		const org::OwnedDescriptorBinding& DetailSamplerBinding(MaterialState& a_state)
		{
			if (!a_state.sampler) {
				a_state.sampler = ShaderBindings::CreateSamplerFromD3D11(Skin::DetailSamplerDesc());
				if (!a_state.sampler)
					::RenderGraph::Fatal("Drawcall Limit Fix", "Advanced Skin's detail sampler could not be created for DCLF's draws");
			}
			return a_state.sampler;
		}

		MaterialRow RowOfMaterial(const Skin::GeometryMaterial& a_material)
		{
			MaterialRow row{};
			std::memcpy(row.materialFlags, &a_material.materialFlags, sizeof(row.materialFlags));
			std::memcpy(row.profile, &a_material.profile, sizeof(row.profile));
			if (a_material.previewWetness) {
				row.wetnessOverride[0] = *a_material.previewWetness;
				row.wetnessOverride[3] = 1.0f;
			}
			return row;
		}

		// The table index of a_row, added if new. a_owners only counts for a new row.
		std::uint32_t InternRow(MaterialState& a_state, const MaterialRow& a_row, std::vector<std::shared_ptr<const void>>&& a_owners)
		{
			if (MaterialRowEqual{}(a_row, a_state.table[0].row))
				return 0;
			if (const auto it = a_state.rowIndex.find(a_row); it != a_state.rowIndex.end())
				return it->second;
			std::uint32_t index;
			if (!a_state.freeRows.empty()) {
				index = a_state.freeRows.back();
				a_state.freeRows.pop_back();
			} else {
				index = static_cast<std::uint32_t>(a_state.table.size());
				a_state.table.emplace_back();
			}
			auto& entry = a_state.table[index];
			entry.row = a_row;
			entry.users = 0;
			entry.owners = GpuTextures::Get().Seal(std::move(a_owners));
			a_state.rowIndex.emplace(a_row, index);
			a_state.unsent.emplace_back(index, a_row);
			a_state.rows = std::max(a_state.rows, index + 1);
			return index;
		}

		// a_slot's row now. Views still being imported are left out, so their flags and the detail switch are off until they arrive.
		std::uint32_t ResolveMaterialSlot(MaterialState& a_state, Skin& a_skin, RE::BSGeometry* a_geometry, MaterialSlotState& a_slot)
		{
			auto* property = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			// The cheap test first: most actor-owned slots are armour, which Skin leaves at the disabled profile.
			if (!SkinMaterials::IsCompatible(property))
				return 0;
			auto material = a_skin.ResolveGeometryMaterial(a_geometry, property);
			MaterialRow row = RowOfMaterial(material);
			std::vector<std::shared_ptr<const void>> owners;
			for (std::size_t i = 0; i < material.views.size(); ++i) {
				if (!material.views[i])
					continue;
				a_slot.views[i] = material.views[i].get();
				if (const auto* binding = BindView(a_state, material.views[i])) {
					row.descriptors[i] = binding->index + 1;
					owners.push_back(binding->owner);
				} else {
					// A failed import is settled: the slot keeps drawing without it and is not resolved again for it.
					if (const auto it = a_state.views.find(material.views[i].get()); it == a_state.views.end() || !it->second.failed)
						a_slot.complete = false;
					if (i < 2)
						row.materialFlags[i] = 0.0f;
				}
			}
			if (row.descriptors[kSkinDetailDescriptor]) {
				const auto& sampler = DetailSamplerBinding(a_state);
				row.descriptors[kSkinSamplerDescriptor] = sampler.Index() + 1;
				owners.push_back(sampler.Owner());
			} else {
				row.profile[kSkinDetailEnabled] = 0.0f;
			}
			return InternRow(a_state, row, std::move(owners));
		}

		void EvictMaterialViews(MaterialState& a_state)
		{
			if (a_state.captures % 60)
				return;
			std::erase_if(a_state.views, [&](const auto& a_item) { return !a_item.second.asked && a_state.captures - a_item.second.used > kSkinViewEvictAfter; });
		}

		void CaptureMaterials(Api::ShadingFieldCapture& a_capture)
		{
			// Skin's state is the render thread's, and SetupGeometry never runs for DCLF's draws, so this is where they resolve.
			DCLF_ZONE("CS.DCLF.Capture.SkinMaterials");
			auto& state = SkinMaterialState();
			auto& skin = globals::features::skin;
			++state.captures;
			if (const auto generation = GpuTextures::Get().Generation(); generation != state.generation)
				ResetMaterialDevice(state, generation);
			DrainMaterialReplies(state);
			const bool active = skin.loaded;
			const std::uint64_t revision = skin.MaterialRevision();
			// Row 0 follows the common settings.
			if (std::exchange(state.rowZeroRevision, revision) != revision) {
				if (const auto disabled = RowOfMaterial(skin.ResolveGeometryMaterial(nullptr, nullptr)); !MaterialRowEqual{}(disabled, state.table[0].row)) {
					state.table[0].row = disabled;
					state.unsent.emplace_back(0, disabled);
				}
			}
			// A moved texture's binding is superseded, so every slot resolves again and asks for the new one.
			state.relocated.clear();
			bool resolveAll = !GpuTextures::Get().RelocatedViews(state.relocationCursor, state.relocated);
			for (auto* view : state.relocated)
				resolveAll |= state.views.contains(view);
			const auto geometry = a_capture.objectGeometry;
			for (const auto o : state.members) {
				auto& slot = state.slots[o];
				auto* mesh = o < geometry.size() ? geometry[o] : nullptr;
				// Skin's resolve is the capture's cost. A slot keeps its row while its key and Skin's revision hold.
				const auto key = KeyOf(mesh, active);
				const bool refresh = (state.captures + o) % kSkinRefreshCaptures == 0;
				if (slot.kept && slot.complete && !slot.force && !resolveAll && !refresh && slot.revision == revision && slot.key == key)
					continue;
				slot.key = key;
				slot.revision = revision;
				slot.kept = true;
				slot.views = {};
				slot.complete = true;
				const std::uint32_t row = active && mesh ? ResolveMaterialSlot(state, skin, mesh, slot) : 0u;
				if (row == slot.row && !slot.force)
					continue;
				AcquireRow(state, row);
				ReleaseRow(state, slot.row);
				slot.row = row;
				slot.force = false;
				float value[1];
				std::memcpy(value, &row, sizeof(row));
				a_capture.Put(o, value, row ? state.table[row].owners : nullptr);
			}
			EvictMaterialViews(state);
		}

		Api::FrameValuePoolId materialPool;  // install

		struct MaterialTableInput
		{
			std::vector<std::pair<std::uint32_t, MaterialRow>> rows;
			std::uint32_t count = 0;
		};
		std::shared_ptr<const MaterialTableInput> idleTable;

		// The producers run one at a time in frame order.
		KeptWholeArray<MaterialRow> producedRows;

		// Render thread, at Kick, after this frame's capture: every row made since the last kick.
		Api::FrameValueInput KickMaterials(const Api::FrameValuePoolKick&)
		{
			DCLF_ZONE("CS.DCLF.FV.Kick.SkinMaterials");
			auto& state = SkinMaterialState();
			if (!state.unsent.empty())
				return { std::make_shared<const MaterialTableInput>(MaterialTableInput{ std::exchange(state.unsent, {}), state.rows }), state.rows };
			if (!idleTable || idleTable->count != state.rows)
				idleTable = std::make_shared<const MaterialTableInput>(MaterialTableInput{ {}, state.rows });
			return { idleTable, state.rows };
		}

		void SampleMaterials(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.BeginBuild(producedRows);
			auto& rows = producedRows.Mutable();
			const auto* input = a_producer.Input<MaterialTableInput>();
			const std::uint32_t count = input ? input->count : 0u;
			if (rows.size() < count) {
				const std::size_t first = rows.size();
				rows.resize(count, MaterialRow{});
				producedRows.MarkRange(first, count - first);
			}
			if (input)
				for (const auto& [at, row] : input->rows) {
					if (at >= rows.size())
						continue;
					rows[at] = row;
					producedRows.Mark(at);
				}
		}

		void SendMaterials(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(producedRows);
		}

		void SelectMaterialTable(Api::FrameBindingSelect& a_select)
		{
			if (a_select.renderThread)
				a_select.index = FrameValues::Get().PoolIndex(materialPool);
		}
	}

	void InstallMaterials(Api::InstallContext& a_context)
	{
		materialPool = a_context.RegisterFrameValuePool(
			{ .name = "skin-materials", .stride = sizeof(MaterialRow), .kick = &KickMaterials, .sample = &SampleMaterials, .send = &SendMaterials });
		a_context.RegisterFrameBinding({ .name = "skin materials SRV", .textureRegister = kMaterialTableRegister, .select = &SelectMaterialTable });
		// Actor-owned surfaces only, by decision. A skin surface no actor owns keeps row 0 (skin off), unlike Skin's D3D11 path, which
		// gives it a material by its texture (docs/development/plans/dclf-skin-per-geometry.md, 2.3).
		a_context.RegisterShadingField({ .name = "skin material", .field = Api::ShadingRowField::SkinMaterial, .members = Api::EntryClass::ActorOwned,
			.membership = &MaterialMembership, .capture = &CaptureMaterials });
		// SkinPerGeometry (b7), bound per draw. DCLF's draws take it from the shading row and the material table instead.
		a_context.DeclarePerDrawBuffer({ .name = "SkinPerGeometry", .stage = Api::ShaderStage::Pixel, .constantRegister = kSkinPerGeometryRegister });
	}

	bool SlotMaterialOf(std::uint32_t a_slot, SlotMaterial& a_out)
	{
		const auto& state = SkinMaterialState();
		if (a_slot >= state.slots.size() || !state.slots[a_slot].member)
			return false;
		const auto& slot = state.slots[a_slot];
		a_out = { slot.views, &state.table[slot.row].row, slot.complete };
		return true;
	}

	std::vector<std::string> MaterialGrowthLines()
	{
		const auto& state = SkinMaterialState();
		return { fmt::format("advanced skin material members {}", state.members.size()),
			fmt::format("advanced skin material rows {} ({} free)", state.table.size(), state.freeRows.size()),
			fmt::format("advanced skin material views {}", state.views.size()) };
	}
}
