// Coordinator::UpdateSharedBindings and its helpers. Scene lane.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

namespace DCLF::Scene
{
	namespace
	{
		constexpr std::uint32_t kProjectedSourceTag = GpuTextures::Tag(GpuTextures::Source::Projected);
		constexpr std::uint32_t kMaskSourceTag = GpuTextures::Tag(GpuTextures::Source::Mask);
		constexpr std::uint32_t kShadowSourceTag = GpuTextures::Tag(GpuTextures::Source::ShadowDiffuse);
	}

	void Coordinator::UpdateSharedBindings()
	{
		DCLF_ZONE("CS.DCLF.Scene.SharedBindings");
		auto& sb = sharedBindings;
		const std::uint32_t answeredKinds = DrainSharedReplies();
		// An answer for a texture the index let go of meanwhile is dropped.
		if (answeredKinds & (1u << SharedBindings::kShadow))
			for (const auto& answer : sb.arrived)
				ResolveShadowTextureBinding(answer.first);
		TakeFixedBindings();
		FollowRelocatedSharedTextures();
		if (auto capture = channels.projectedPosted.load(std::memory_order_acquire); capture != sb.projectedSeen) {
			sb.projectedSeen = std::move(capture);
			++sb.stats.projectedCaptures;
			ResolveProjectedBindings(0xf);
		} else if (sb.projectedPending && (answeredKinds & (1u << SharedBindings::kProjected))) {
			ResolveProjectedBindings(sb.projectedPending);
		}
		ResolveMaskBindings();
		std::vector<std::pair<ID3D11ShaderResourceView*, bool>> changes;
		tables.TakeShadowTextureChanges(changes);
		ApplyShadowTextureChanges(changes);
		sb.arrived.clear();
		SealSharedBindingBlock();
		TracyPlot("CS.DCLF.ShadowTextureChanges", static_cast<std::int64_t>(changes.size()));
		TracyPlot("CS.DCLF.ShadowTexturePending", static_cast<std::int64_t>(sb.shadowPending));
		TracyPlot("CS.DCLF.ShadowTextureOwners", static_cast<std::int64_t>(sb.shadowTextures.size() - sb.shadowPending));
	}

	void Coordinator::FollowRelocatedSharedTextures()
	{
		auto& sb = sharedBindings;
		// A superseded import holds its old storage while held here. Each asks for the new import and draws with the old one until answered.
		std::vector<ID3D11ShaderResourceView*> moved;
		for (const auto& [view, owner] : lookups.shadowTextureOwners)
			if (GpuTextures::Superseded(owner.get()))
				moved.push_back(view);
		for (auto* view : moved) {
			const auto it = sb.shadowTextures.find(view);
			if (it == sb.shadowTextures.end() || it->second.pending)
				continue;
			it->second.pending = true;
			++sb.shadowPending;
			ResolveShadowTextureBinding(view);
		}
		std::uint32_t projected = 0;
		for (std::uint32_t i = 0; i < lookups.projectedTextures.size(); ++i)
			if (GpuTextures::Superseded(lookups.projectedOwners[i].get()))
				projected |= 1u << i;
		if (projected)
			ResolveProjectedBindings(projected);
	}

	std::uint32_t Coordinator::DrainSharedReplies()
	{
		auto& sb = sharedBindings;
		// An owner's last release runs on the cleanup queue, never on this thread.
		std::uint32_t answeredKinds = 0;
		sb.replies.Drain([&](GpuTextures::Reply&& a_reply) {
			if (a_reply.cookie != sb.cookie) {
				++sb.stats.stale;
				return;
			}
			std::uint32_t kinds = 0;
			if (const auto it = sb.inFlight.find(a_reply.view); it != sb.inFlight.end()) {
				kinds = it->second;
				sb.inFlight.erase(it);
			}
			for (std::uint32_t kind = 0; kind < SharedBindings::kKinds; ++kind) {
				if ((kinds >> kind) & 1) {
					++sb.stats.answered[kind];
					if (!a_reply.binding.owner)
						++sb.stats.rejected[kind];
				}
			}
			answeredKinds |= kinds;
			if (a_reply.binding.owner) {
				auto& cached = sb.cache[a_reply.view];
				cached.owner = a_reply.binding.owner;
				cached.index = a_reply.binding.index;
				// Amortized cleanup of entries whose import nobody holds.
				for (std::uint32_t i = 0; i < 2 && !sb.cache.empty(); ++i) {
					sb.cacheSweep = (sb.cacheSweep + 1) % sb.cache.size();
					if (const auto swept = sb.cache.begin() + static_cast<std::ptrdiff_t>(sb.cacheSweep); swept->second.owner.expired())
						sb.cache.erase(swept);
				}
			}
			sb.arrived[a_reply.view] = std::move(a_reply.binding);
		});
		return answeredKinds;
	}

	void Coordinator::TakeFixedBindings()
	{
		auto& sb = sharedBindings;
		// The null texture and the samplers are the shadow builds' too, so a change moves both generations.
		if (auto fixed = GpuTextures::Get().Fixed(); fixed != sb.fixed) {
			sb.fixed = std::move(fixed);
			const std::uint32_t nullTexture = sb.fixed ? sb.fixed->null.index : Lookups::kNone;
			std::array<std::uint32_t, GpuTextures::kSamplerCount> samplers;
			for (std::uint32_t i = 0; i < GpuTextures::kSamplerCount; ++i)
				samplers[i] = sb.fixed ? sb.fixed->samplers[i].index : Lookups::kNone;
			const bool samplersResolved = sb.fixed != nullptr;
			if (lookups.nullTexture != nullTexture) {
				lookups.nullTexture = nullTexture;
				lookups.sharedVersion = lookups.NextVersion();
				++lookups.generation;
				++lookups.shadowGeneration;
				++sb.stats.writtenShared;
			}
			if (lookups.samplersResolved != samplersResolved || !std::equal(samplers.begin(), samplers.end(), lookups.samplers.begin())) {
				std::copy(samplers.begin(), samplers.end(), lookups.samplers.begin());
				lookups.samplersResolved = samplersResolved;
				lookups.sharedVersion = lookups.NextVersion();
				++lookups.generation;
				++lookups.shadowGeneration;
				++sb.stats.writtenShared;
			}
			sb.sharedChanged = true;
			++sb.stats.fixedPublished;
		}
	}

	void Coordinator::ResolveMaskBindings()
	{
		auto& sb = sharedBindings;
		// The row's view is the frame's shadow mask target, which the frame's capture holds.
		if (sb.masks.size() < tables.pipelines.size())
			sb.masks.resize(tables.pipelines.size());
		if (lookups.pipelines.size() < tables.pipelines.size())
			lookups.pipelines.resize(tables.pipelines.size());
		// Only the masks that may have moved. Every used one when a row's bindings or the texture generation moved.
		auto* frameMask = FrameGlobals::Current().shadowMaskHeld.get();
		{
			auto& w = sb.work;
			const std::uint32_t textureGeneration = GpuTextures::Get().Generation();
			bool sweep = textureGeneration != sb.textureGenerationSeen;
			sb.textureGenerationSeen = textureGeneration;
			const std::size_t rowsSeen = sb.techniqueSeen.size();
			sb.techniqueSeen.resize(tables.techniqueConstants.size(), 0);
			for (std::size_t r = 0; r < tables.techniqueConstants.size(); ++r) {
				const std::uint32_t version = tables.techniqueConstants[r].valid ? tables.techniqueConstants[r].bindingVersion : 0u;
				sweep |= r < rowsSeen && sb.techniqueSeen[r] != version;
				sb.techniqueSeen[r] = version;
			}
			const auto& used = tables.usedPipelineBits;
			sb.usedSeen.resize(used.size(), 0);
			for (std::size_t word = 0; word < used.size(); ++word) {
				for (std::uint64_t added = used[word] & ~sb.usedSeen[word]; added; added &= added - 1)
					sb.MarkMask(static_cast<std::uint32_t>(word * 64 + std::countr_zero(added)));
				sb.usedSeen[word] = used[word];
			}
			auto stands = [&](std::uint32_t a_slot) {
				const auto& mask = sb.masks[a_slot];
				const auto& technique = tables.TechniqueOf(a_slot).value;
				auto* view = technique.shadowMask ? technique.shadowMaskTexture : nullptr;
				const auto& entry = lookups.pipelines[a_slot];
				return mask.settled && mask.wanted == technique.shadowMask && entry.shadowMaskView == view && entry.shadowMaskTextureGeneration == textureGeneration;
			};
			auto resolve = [&](std::uint32_t a_slot) {
				if (a_slot >= tables.pipelines.size() || !tables.PipelineUsed(a_slot) || !tables.TechniqueConstantsValid(a_slot))
					return;  // marked again when used (the used set) or valid (its row's version moves)
				++w.masksChecked;
				w.masksResolved += ResolveMaskBinding(a_slot, frameMask) ? 1 : 0;
				if (!stands(a_slot))
					sb.MarkMask(a_slot);  // asked for, or held on its previous view: looked up again next pass
			};
			const auto dirty = std::exchange(sb.dirty, {});
			for (const std::uint32_t slot : dirty)
				if (slot < sb.masks.size())
					sb.masks[slot].listed = false;
			if (sweep) {
				++w.maskSweeps;
				Tables::ForEachBit(tables.usedPipelineBits, resolve);
			} else {
				for (const std::uint32_t slot : dirty)
					resolve(slot);
			}
		}
	}

	void Coordinator::ApplyShadowTextureChanges(const std::vector<std::pair<ID3D11ShaderResourceView*, bool>>& a_changes)
	{
		auto& sb = sharedBindings;
		for (const auto& [view, present] : a_changes) {
			if (present) {
				if (!sb.shadowTextures.try_emplace(view).second)
					continue;
				++sb.shadowPending;
				++sb.stats.shadowAdded;
				ResolveShadowTextureBinding(view);
			} else if (const auto it = sb.shadowTextures.find(view); it != sb.shadowTextures.end()) {
				// Its casters have gone. The owner's last release runs on the cleanup queue.
				if (it->second.pending)
					--sb.shadowPending;
				sb.shadowTextures.erase(it);
				if (lookups.shadowTextures.erase(view)) {
					++lookups.shadowGeneration;
					++sb.stats.writtenShadow;
				}
				lookups.shadowTextureOwners.erase(view);
				++sb.stats.shadowRemoved;
			}
		}
	}

	void Coordinator::SealSharedBindingBlock()
	{
		auto& sb = sharedBindings;
		if (sb.sharedChanged) {
			std::vector<std::shared_ptr<const void>> owners;
			owners.reserve(1 + GpuTextures::kSamplerCount + lookups.projectedOwners.size());
			if (sb.fixed) {
				owners.push_back(sb.fixed->null.owner);
				for (const auto& sampler : sb.fixed->samplers)
					owners.push_back(sampler.owner);
			}
			owners.insert(owners.end(), lookups.projectedOwners.begin(), lookups.projectedOwners.end());
			lookups.sharedBindingBlock = GpuTextures::Get().Seal(std::move(owners));
			++lookups.changes;
			sb.sharedChanged = false;
		}
	}

	GpuTextures::Binding Coordinator::SharedViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, SharedBindings::Kind a_kind)
	{
		auto& sb = sharedBindings;
		if (const auto it = sb.arrived.find(a_view); it != sb.arrived.end())
			return it->second;
		// A caster's diffuse is its material's t0. A live owner holds the view, so its address still names the same view.
		auto known = [&](auto& a_cache, GpuTextures::Binding& a_out) {
			const auto it = a_cache.find(a_view);
			if (it == a_cache.end())
				return false;
			if (auto owner = it->second.owner.lock(); owner && !GpuTextures::Superseded(owner.get())) {
				a_out = { it->second.index, std::move(owner) };
				return true;
			}
			a_cache.erase(it);
			return false;
		};
		if (GpuTextures::Binding binding; known(sb.cache, binding) || known(materialBindings.cache, binding)) {
			++sb.stats.cached[a_kind];
			return binding;
		}
		// The request takes its own reference on the view at once.
		auto [waiting, asked] = sb.inFlight.try_emplace(a_view, 0u);
		if (asked) {
			GpuTextures::Get().Request(a_view, a_sourceTag, sb.replies, sb.cookie);
			++sb.stats.requested[a_kind];
		}
		waiting->second |= 1u << a_kind;
		return { Lookups::kNone, {}, true };
	}

	void Coordinator::ResolveProjectedBindings(std::uint32_t a_textures)
	{
		auto& sb = sharedBindings;
		for (std::uint32_t i = 0; i < lookups.projectedTextures.size(); ++i) {
			if (!((a_textures >> i) & 1))
				continue;
			// Unresolvable until captured, which defers the ProjectedUV pipelines' draws.
			auto* view = sb.projectedSeen ? sb.projectedSeen->views[i].get() : nullptr;
			auto binding = view ? SharedViewBinding(view, kProjectedSourceTag + i, SharedBindings::kProjected) : GpuTextures::Binding{ Lookups::kNone, {} };
			if (binding.pending)
				sb.projectedPending |= 1u << i;
			else
				sb.projectedPending &= ~(1u << i);
			const bool ownerChanged = lookups.projectedOwners[i].get() != binding.owner.get();
			const bool indexChanged = lookups.projectedTextures[i] != binding.index;
			if (!ownerChanged && !indexChanged)
				continue;
			if (ownerChanged)
				++lookups.generation;
			if (indexChanged)
				++lookups.generation;
			lookups.sharedVersion = lookups.NextVersion();
			lookups.projectedTextures[i] = binding.index;
			lookups.projectedOwners[i] = std::move(binding.owner);
			sb.sharedChanged = true;
			++sb.stats.writtenShared;
		}
	}

	bool Coordinator::ResolveMaskBinding(std::uint32_t a_slot, ID3D11ShaderResourceView* a_frameMask)
	{
		auto& sb = sharedBindings;
		auto& mask = sb.masks[a_slot];
		const auto& entry = lookups.pipelines[a_slot];
		const auto& technique = tables.TechniqueOf(a_slot).value;
		const bool wanted = technique.shadowMask;
		auto* view = wanted ? technique.shadowMaskTexture : nullptr;
		const std::uint32_t textureGeneration = GpuTextures::Get().Generation();
		if (mask.wanted == wanted && entry.shadowMaskView == view && entry.shadowMaskTextureGeneration == textureGeneration && mask.settled)
			return false;
		GpuTextures::Binding binding{ Lookups::kNone, {} };
		if (wanted && view) {
			if (view != a_frameMask)
				++sb.stats.masksUnheld;
			binding = SharedViewBinding(view, kMaskSourceTag, SharedBindings::kMask);
		} else if (wanted) {
			// A technique that binds the mask while the target has no view reads the null descriptor.
			if (sb.fixed)
				binding = sb.fixed->null;
			else
				binding.pending = true;
		}
		// Held while the new view is asked for, since the set's members draw with it.
		if (binding.pending && entry.shadowMaskOwner && entry.shadowMaskIndex != Lookups::kNone && entry.shadowMaskTextureGeneration == textureGeneration) {
			++sb.stats.masksHeld;
			return true;
		}
		mask.settled = !binding.pending;
		mask.wanted = wanted;
		auto& written = lookups.pipelines.Mutable(a_slot);
		written.shadowMaskView = view;
		written.shadowMaskTextureGeneration = textureGeneration;
		const bool ownerChanged = written.shadowMaskOwner.get() != binding.owner.get();
		const bool indexChanged = written.shadowMaskIndex != binding.index;
		if (!ownerChanged && !indexChanged)
			return true;
		if (ownerChanged) {
			++lookups.generation;
			written.shadowMaskOwner = std::move(binding.owner);
		}
		if (indexChanged)
			++lookups.generation;
		written.shadowMaskIndex = binding.index;
		written.version = lookups.NextVersion();
		++sb.stats.writtenMasks;
		return true;
	}

	void Coordinator::ResolveShadowTextureBinding(ID3D11ShaderResourceView* a_view)
	{
		auto& sb = sharedBindings;
		const auto it = sb.shadowTextures.find(a_view);
		if (it == sb.shadowTextures.end() || !it->second.pending)
			return;
		// The view is alive because a member's record names it.
		auto binding = SharedViewBinding(a_view, kShadowSourceTag, SharedBindings::kShadow);
		if (binding.pending)
			return;  // not in the lookups' table until answered, so its casters wait (deferredTextures)
		// A rejection is permanent, so neither outcome is asked again.
		it->second.pending = false;
		--sb.shadowPending;
		auto [slot, inserted] = lookups.shadowTextures.try_emplace(a_view, binding.index);
		const auto owner = lookups.shadowTextureOwners.find(a_view);
		const void* heldOwner = owner == lookups.shadowTextureOwners.end() ? nullptr : owner->second.get();
		if (inserted || slot->second != binding.index || heldOwner != binding.owner.get()) {
			++lookups.shadowGeneration;
			++sb.stats.writtenShadow;
		}
		slot->second = binding.index;
		if (binding.owner)
			lookups.shadowTextureOwners[a_view] = std::move(binding.owner);
		else if (owner != lookups.shadowTextureOwners.end())
			lookups.shadowTextureOwners.erase(owner);
	}

	void Coordinator::RetireMaskBinding(std::uint32_t a_slot)
	{
		// The owner's last release runs on the cleanup queue.
		if (a_slot < sharedBindings.masks.size())
			sharedBindings.masks[a_slot] = {};
		if (a_slot >= lookups.pipelines.size())
			return;
		if (const auto& entry = lookups.pipelines[a_slot]; !entry.shadowMaskOwner && entry.shadowMaskIndex == Lookups::kNone && !entry.shadowMaskView)
			return;
		auto& written = lookups.pipelines.Mutable(a_slot);
		const bool ownerChanged = written.shadowMaskOwner != nullptr;
		const bool indexChanged = written.shadowMaskIndex != Lookups::kNone;
		written.shadowMaskView = nullptr;
		written.shadowMaskTextureGeneration = 0;
		written.shadowMaskOwner.reset();
		written.shadowMaskIndex = Lookups::kNone;
		if (!ownerChanged && !indexChanged)
			return;
		if (ownerChanged)
			++lookups.generation;
		if (indexChanged)
			++lookups.generation;
		written.version = lookups.NextVersion();
		++sharedBindings.stats.writtenMasks;
	}

	void Coordinator::ResetSharedBindings()
	{
		// The posted projected capture is the render thread's and stays for the next pass.
		auto& sb = sharedBindings;
		sb.projectedSeen.reset();
		sb.projectedPending = 0;
		sb.fixed.reset();
		sb.sharedChanged = false;
		sb.masks.clear();
		sb.dirty.clear();
		sb.usedSeen.clear();
		sb.techniqueSeen.clear();
		sb.textureGenerationSeen = ~0u;
		sb.shadowTextures.clear();
		sb.shadowPending = 0;
		sb.inFlight.clear();
		sb.cache.clear();
		sb.arrived.clear();
		++sb.cookie;
	}
}
