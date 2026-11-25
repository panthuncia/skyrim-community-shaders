#include "RenderGraph/KeptBindings.h"

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/UndeclaredAccess.h"

#include <algorithm>

namespace RenderGraph
{
	namespace
	{
		// A view unused for this many frames is dropped, which releases its reference and its import.
		constexpr std::uint64_t kIdleFrames = 60;
	}

	const KeptBindings::Kept* KeptBindings::Resolve(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::source_location a_where)
	{
		if (!a_view)
			return nullptr;
		auto& textures = ::DCLF::GpuTextures::Get();
		const std::uint32_t generation = textures.Generation();
		const std::uint64_t frame = Frame::Driver::Current().frame;
		if (frame != prunedFrame) {
			prunedFrame = frame;
			std::erase_if(entries, [&](const Entry& a_entry) { return a_entry.generation != generation || a_entry.lastFrame + kIdleFrames < frame; });
		}
		const auto found = std::ranges::find_if(entries, [&](const Entry& a_entry) { return a_entry.key.get() == a_view; });
		if (found != entries.end() && !UndeclaredAccess::On() && found->generation == generation && !::DCLF::GpuTextures::Superseded(found->kept.owner.get())) {
			found->lastFrame = frame;
			return &found->kept;
		}
		auto binding = textures.ResolveBinding(a_view, a_sourceTag, nullptr, a_where);
		if (!binding || binding.pending) {
			if (found != entries.end())
				entries.erase(found);
			return nullptr;
		}
		Entry* entry = found != entries.end() ? &*found : &entries.emplace_back();
		if (!entry->key) {
			entry->key.copy_from(a_view);
			// One reference for every frame that holds this view, released once the last of them retires.
			a_view->AddRef();
			entry->kept.view = std::shared_ptr<const void>(a_view, [](const void* a_held) {
				static_cast<ID3D11ShaderResourceView*>(const_cast<void*>(a_held))->Release();
			});
		}
		entry->kept.index = binding.index;
		entry->kept.owner = std::move(binding.owner);
		entry->generation = generation;
		entry->lastFrame = frame;
		return &entry->kept;
	}
}
