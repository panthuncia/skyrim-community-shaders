// Coordinator::UpdateMaterialBindings and its helpers. Scene lane.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

namespace DCLF::Scene
{
	namespace
	{
		// A character-light pass's t11 comes from the frame, so its record holds the modes but no view.
		constexpr std::uint32_t kCharacterLightTexture = 11;
		constexpr std::uint32_t kFeatureSourceTag = GpuTextures::Tag(GpuTextures::Source::Feature);
		constexpr std::size_t kMaterialLogEntries = std::size_t(1) << 15;
	}

	void Coordinator::UpdateMaterialBindings()
	{
		DCLF_ZONE("CS.DCLF.Scene.MaterialBindings");
		auto& mb = materialBindings;
		if (lookups.materials.size() < tables.materials.size())
			lookups.materials.resize(tables.materials.size());
		if (mb.held.size() < tables.materials.size())
			mb.held.resize(tables.materials.size(), 0);
		// A rejection is written too. An owner's last release runs on the cleanup queue, never on this thread.
		std::vector<std::uint32_t> waiting;
		mb.replies.Drain([&](GpuTextures::Reply&& a_reply) {
			if (a_reply.cookie != mb.cookie) {
				++mb.stats.stale;
				return;
			}
			++mb.stats.answered;
			if (const auto it = mb.inFlight.find(a_reply.view); it != mb.inFlight.end()) {
				for (const std::uint32_t slot : it->second)
					mb.waiting.erase(std::pair{ a_reply.view, slot });
				waiting.insert(waiting.end(), it->second.begin(), it->second.end());
				mb.inFlight.erase(it);
			}
			// The import was superseded by a move meanwhile, so the waiting slots ask again.
			if (a_reply.view && GpuTextures::Superseded(a_reply.binding.owner.get())) {
				++mb.stats.superseded;
				return;
			}
			if (a_reply.binding.owner) {
				auto& cached = mb.cache[a_reply.view];
				cached.owner = a_reply.binding.owner;
				cached.index = a_reply.binding.index;
				// Amortized cleanup of entries whose import nobody holds.
				for (std::uint32_t i = 0; i < 2 && !mb.cache.empty(); ++i) {
					mb.cacheSweep = (mb.cacheSweep + 1) % mb.cache.size();
					if (const auto swept = mb.cache.begin() + static_cast<std::ptrdiff_t>(mb.cacheSweep); swept->second.owner.expired())
						mb.cache.erase(swept);
				}
			} else {
				++mb.stats.rejected;
			}
			mb.arrived[a_reply.view] = std::move(a_reply.binding);
		});
		for (const std::uint32_t slot : waiting)
			ResolveMaterialBinding(slot);
		mb.arrived.clear();
		FollowRelocatedTextures();
		// A cursor behind the log resolves every used slot.
		const auto& log = tables.materialLog;
		if (!mb.cursor.Continues(log, 0)) {
			Tables::ForEachBit(tables.usedMaterialBits, [&](std::uint32_t a_slot) { ResolveMaterialBinding(a_slot); });
			mb.cursor.Restart(0);
		} else {
			for (const std::uint32_t slot : mb.cursor.Unread(log))
				ResolveMaterialBinding(slot);
		}
		mb.cursor.Advance(log);
		const auto& used = tables.usedMaterialBits;
		mb.usedSeen.resize(used.size(), 0);
		for (std::size_t word = 0; word < used.size(); ++word) {
			for (std::uint64_t added = used[word] & ~mb.usedSeen[word]; added; added &= added - 1)
				ResolveMaterialBinding(static_cast<std::uint32_t>(word * 64 + std::countr_zero(added)));
			mb.usedSeen[word] = used[word];
		}
	}

	struct Coordinator::MaterialTextureBinder
	{
		Coordinator& self;
		std::uint32_t slot;
		bool sameGeneration;
		bool wasResolved;
		const Lookups::MaterialOwners* owners;  // the owners held now (the entry's block, immutable)
		bool& changed;
		bool held = false, pending = false, ownersChanged = false;
		Lookups::MaterialOwners next;  // the owners that change, sealed into a new block after
		std::uint32_t texturesMoved = 0, featuresMoved = 0;

		void Bind(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t& a_index, bool a_feature, std::uint32_t a_at,
			ID3D11ShaderResourceView*& a_seen)
		{
			const void* owner = owners ? (a_feature ? owners->features[a_at] : owners->textures[a_at]).get() : nullptr;
			// Asked anew if a move superseded its import, holding the old one meanwhile.
			if (sameGeneration && a_seen == a_view && owner && a_index != Lookups::kNone && !(a_view && GpuTextures::Superseded(owner)))
				return;
			auto binding = self.MaterialViewBinding(a_view, a_sourceTag, slot);
			if (binding.pending && wasResolved && owner && a_index != Lookups::kNone) {
				held = true;
				return;
			}
			pending |= binding.pending;
			const bool ownerChanged = owner != binding.owner.get();
			changed |= ownerChanged || a_index != binding.index;
			if (ownerChanged) {
				ownersChanged = true;
				(a_feature ? featuresMoved : texturesMoved) |= 1u << a_at;
				(a_feature ? next.features[a_at] : next.textures[a_at]) = std::move(binding.owner);
			}
			a_index = binding.index;
			a_seen = a_view;
		}

		void Clear(std::uint32_t& a_index, bool a_feature, std::uint32_t a_at, ID3D11ShaderResourceView*& a_seen)
		{
			const bool owner = owners && (a_feature ? owners->features[a_at] : owners->textures[a_at]);
			changed |= a_index != Lookups::kNone || owner;
			if (owner) {
				ownersChanged = true;
				(a_feature ? featuresMoved : texturesMoved) |= 1u << a_at;
			}
			a_index = Lookups::kNone;
			a_seen = nullptr;
		}
	};

	void Coordinator::FollowRelocatedTextures()
	{
		auto& mb = materialBindings;
		std::vector<ID3D11ShaderResourceView*> moved;
		const bool complete = GpuTextures::Get().RelocatedViews(mb.relocationCursor, moved);
		if (complete && moved.empty())
			return;
		// The log overran, so every slot checks its bindings.
		const ankerl::unordered_dense::set<ID3D11ShaderResourceView*> views(moved.begin(), moved.end());
		for (std::uint32_t slot = 0; slot < lookups.materials.size(); ++slot) {
			const auto& entry = lookups.materials[slot];
			if (!entry.key.first)
				continue;
			bool names = false;
			for (auto* view : entry.views)
				names = names || (view && views.contains(view));
			for (auto* view : entry.featureViews)
				names = names || (view && views.contains(view));
			// A null view's owner is the null descriptor, not an import.
			if (!complete && entry.bindingBlock) {
				const auto& block = *entry.bindingBlock;
				for (std::size_t t = 0; t < std::min(block.textures.size(), entry.views.size()); ++t)
					names = names || (entry.views[t] && GpuTextures::Superseded(block.textures[t].get()));
				for (std::size_t f = 0; f < std::min(block.features.size(), entry.featureViews.size()); ++f)
					names = names || (entry.featureViews[f] && GpuTextures::Superseded(block.features[f].get()));
			}
			if (!names)
				continue;
			++mb.stats.followed;
			// An unused slot draws nothing, so it lets its bindings and the old storage go until used again.
			if (slot < tables.materials.size() && tables.materialSlots.Alive(slot) && tables.MaterialUsed(slot))
				ResolveMaterialBinding(slot, true);
			else
				RetireMaterialBinding(slot);
		}
	}

	void Coordinator::ResolveMaterialBinding(std::uint32_t a_slot, bool a_force)
	{
		auto& mb = materialBindings;
		// Only a used slot, since the used set certifies the views a record holds. A freed slot keeps its entry until its retirement comes back.
		if (a_slot >= tables.materials.size() || a_slot >= tables.materialSlotKey.size() || a_slot >= tables.materialVersion.size() ||
			!tables.materialSlots.Alive(a_slot) || !tables.MaterialUsed(a_slot))
			return;
		const auto& key = tables.materialSlotKey[a_slot];
		if (!key.first)
			return;
		if (lookups.materials.size() < tables.materials.size())
			lookups.materials.resize(tables.materials.size());
		if (mb.held.size() < tables.materials.size())
			mb.held.resize(tables.materials.size(), 0);
		const auto& material = tables.materials[a_slot];
		const std::uint32_t textureGeneration = GpuTextures::Get().Generation();
		// Read before the entry is written, since a write copies its chunk when a publication shares it.
		if (const auto& current = lookups.materials[a_slot]; !a_force && current.key == key && current.resolved && !mb.held[a_slot] &&
															 current.recordVersion == tables.materialVersion[a_slot] && current.written == material.textureWritten &&
															 current.texturesGeneration == textureGeneration)
			return;
		auto& entry = lookups.materials.Mutable(a_slot);
		bool changed = false;
		if (entry.key != key) {
			entry = {};  // a predecessor incarnation contributes no binding
			entry.key = key;
			entry.textureIndex.fill(Lookups::kNone);
			entry.featureIndex.fill(Lookups::kNone);
			mb.held[a_slot] = 0;
			changed = true;
		}
		const bool sameGeneration = entry.texturesGeneration == textureGeneration;
		// A member joined because its material resolved, so the engine no longer draws it. Keep drawing with the previous view until the new one is answered.
		const bool wasResolved = entry.resolved && sameGeneration;
		MaterialTextureBinder binder{ *this, a_slot, sameGeneration, wasResolved, entry.bindingBlock.get(), changed };
		const std::uint32_t textureWritten = material.textureWritten & ~(MaterialSources::FrameCharacterLight(key.second) ? 1u << kCharacterLightTexture : 0u);
		for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
			if ((textureWritten >> t) & 1)
				binder.Bind(material.textures[t], t, entry.textureIndex[t], false, t, entry.views[t]);  // a null view: the null descriptor
			else
				binder.Clear(entry.textureIndex[t], false, t, entry.views[t]);
		}
		for (std::uint32_t f = 0; f < Extensions::kFeatureTextureSlots; ++f) {
			if (material.featureTextures[f])
				binder.Bind(material.featureTextures[f], kFeatureSourceTag + f, entry.featureIndex[f], true, f, entry.featureViews[f]);
			else
				binder.Clear(entry.featureIndex[f], true, f, entry.featureViews[f]);
		}
		// Its draws stay the engine's until the answer resolves it.
		const bool pending = binder.pending;
		if (entry.resolved == pending) {
			changed = true;
			if (!pending)
				++mb.stats.resolved;
		}
		entry.resolved = !pending;
		entry.written = material.textureWritten;  // the record's, so a character-light pass's is not resolved again every pass
		entry.texturesGeneration = textureGeneration;
		entry.recordVersion = tables.materialVersion[a_slot];
		mb.held[a_slot] = binder.held ? 1 : 0;
		if (binder.ownersChanged)
			entry.bindingBlock = SealMaterialOwners(key.first, binder.owners, binder.next, binder.texturesMoved, binder.featuresMoved);
		if (changed)
			mb.MarkChanged(a_slot);
	}

	std::shared_ptr<const Lookups::MaterialOwners> Coordinator::SealMaterialOwners(const RE::BSShaderMaterial* a_material, const Lookups::MaterialOwners* a_held,
		Lookups::MaterialOwners& a_next, std::uint32_t a_texturesMoved, std::uint32_t a_featuresMoved)
	{
		auto& mb = materialBindings;
		bool any = false;
		for (std::uint32_t t = 0; t < a_next.textures.size(); ++t) {
			if (!((a_texturesMoved >> t) & 1) && a_held)
				a_next.textures[t] = a_held->textures[t];
			any |= static_cast<bool>(a_next.textures[t]);
		}
		for (std::uint32_t f = 0; f < a_next.features.size(); ++f) {
			if (!((a_featuresMoved >> f) & 1) && a_held)
				a_next.features[f] = a_held->features[f];
			any |= static_cast<bool>(a_next.features[f]);
		}
		if (!any)
			return nullptr;
		// A material's passes often name the same textures, so its slots share one block.
		auto& shared = mb.sealed[a_material];
		if (auto block = shared.lock(); block && block->textures == a_next.textures && block->features == a_next.features) {
			++mb.work.sealShared;
			return block;
		}
		std::vector<std::shared_ptr<const void>> list;
		list.reserve(a_next.textures.size() + a_next.features.size());
		list.insert(list.end(), a_next.textures.begin(), a_next.textures.end());
		list.insert(list.end(), a_next.features.begin(), a_next.features.end());
		a_next.lease = GpuTextures::Get().Seal(std::move(list));
		auto block = std::make_shared<const Lookups::MaterialOwners>(std::move(a_next));
		shared = block;
		++mb.work.sealed;
		// Amortized cleanup of blocks nobody holds.
		for (std::uint32_t i = 0; i < 2 && !mb.sealed.empty(); ++i) {
			mb.sealedSweep = (mb.sealedSweep + 1) % mb.sealed.size();
			if (const auto swept = mb.sealed.begin() + static_cast<std::ptrdiff_t>(mb.sealedSweep); swept->second.expired())
				mb.sealed.erase(swept);
		}
		return block;
	}

	GpuTextures::Binding Coordinator::MaterialViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t a_slot)
	{
		auto& mb = materialBindings;
		if (const auto it = mb.arrived.find(a_view); it != mb.arrived.end())
			return it->second;
		if (const auto it = mb.cache.find(a_view); it != mb.cache.end()) {
			if (auto owner = it->second.owner.lock(); owner && !(a_view && GpuTextures::Superseded(owner.get()))) {
				++mb.stats.cached;
				return { it->second.index, std::move(owner) };
			}
			mb.cache.erase(it);
		}
		// The material does not keep its textures' views alive, so the slot pins each view. The request takes its own reference at once.
		auto [waiters, asked] = mb.inFlight.try_emplace(a_view);
		if (asked) {
			GpuTextures::Get().Request(a_view, a_sourceTag, mb.replies, mb.cookie);
			++mb.stats.requested;
		}
		if (mb.waiting.emplace(a_view, a_slot).second)
			waiters->second.push_back(a_slot);
		else
			++mb.work.waitersDeduped;
		return { Lookups::kNone, {}, true };
	}

	void Coordinator::RetireMaterialBinding(std::uint32_t a_slot)
	{
		auto& mb = materialBindings;
		if (a_slot >= lookups.materials.size())
			return;
		// The owners' last release runs on the cleanup queue, whichever publication holder lets go last.
		lookups.materials.Mutable(a_slot) = {};
		if (a_slot < mb.held.size())
			mb.held[a_slot] = 0;
		mb.MarkChanged(a_slot);
		++mb.stats.retired;
	}

	void Coordinator::ResetMaterialBindings()
	{
		auto& mb = materialBindings;
		mb.held.clear();
		mb.changed.clear();
		mb.changedMark.clear();
		mb.inFlight.clear();
		mb.waiting.clear();
		mb.sealed.clear();
		mb.sealedSweep = 0;
		mb.cache.clear();
		mb.arrived.clear();
		mb.usedSeen.clear();
		mb.cursor = {};
		++mb.cookie;
	}

	void Coordinator::VersionMaterialBindings()
	{
		DCLF_ZONE("CS.DCLF.Scene.VersionMaterialBindings");
		auto& mb = materialBindings;
		if (lookups.materialVersions.size() < lookups.materials.size())
			lookups.materialVersions.resize(lookups.materials.size(), 0);
		if (mb.changed.empty())
			return;
		for (const std::uint32_t slot : mb.changed) {
			mb.changedMark[slot] = 0;
			if (slot >= lookups.materials.size())
				continue;
			// A retired slot's empty entry has no version, as before it resolved.
			const std::uint32_t version = lookups.materials[slot].key.first ? lookups.NextVersion() : 0u;
			lookups.materials.Mutable(slot).version = version;
			lookups.materialVersions[slot] = version;
			lookups.materialLog.Push(slot);
		}
		mb.stats.versioned += mb.changed.size();
		mb.changed.clear();
		// A reader behind the trim reads every pair again.
		lookups.materialLog.Trim(kMaterialLogEntries);
		// A build against the old entries may have deferred draws they now resolve, or read an index they let go of.
		++lookups.generation;
	}
}
