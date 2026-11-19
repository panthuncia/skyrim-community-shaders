#include "Features/DrawcallLimitFix/Core/Gpu/TextureFeedback.h"
#include "RenderGraph/BufferRefs.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/ShaderTarget.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <atomic>
#include <memory>

namespace DCLF::TextureFeedback
{
	namespace
	{
		std::shared_ptr<org::Buffer> buffer;  // kept for the process
		rhi::ResourcePtr committed;  // D3D12's instead
		std::atomic<std::uint64_t> ref{ 0 };
		std::atomic<volatile std::uint32_t*> words{ nullptr };
	}

	void Ensure()
	{
		if (buffer || committed)
			return;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !host->Graph())
			return;
		const std::uint64_t bytes = std::uint64_t(kWords) * sizeof(std::uint32_t);
		rhi::Resource resource{};
		RenderGraph::BufferRef made{};
		if (::RenderGraph::ShaderTarget::IsSpirv()) {
			buffer = org::Buffer::CreateShared(rhi::HeapType::HostVisibleCoherent, bytes, true);
			buffer->SetName("cs.dclf.texture-feedback");
			resource = buffer->GetAPIResource();
			// No GPU pass reads the buffer, so none declares it. The validator counts its ref as host feedback.
			if (RenderGraph::UndeclaredAccess::On())
				RenderGraph::UndeclaredAccess::NoteHostFeedback(buffer);
			made = RenderGraph::BufferRefOf(*buffer);
		} else {
			// An UPLOAD buffer cannot have unordered access, and the graph's allocator takes only the standard heaps.
			rhi::ResourceDesc desc{};
			desc.type = rhi::ResourceType::Buffer;
			desc.heapType = rhi::HeapType::HostCached;
			desc.resourceFlags = rhi::RF_AllowUnorderedAccess;
			desc.debugName = "cs.dclf.texture-feedback";
			desc.buffer.sizeBytes = bytes;
			if (host->Device().CreateCommittedResource(desc, committed) != rhi::Result::Ok || !committed) {
				logger::critical("[DCLF] Texture streaming's feedback buffer could not be made");
				return;
			}
			resource = committed.Get();
			made = RenderGraph::BufferRefOf(host->Device(), resource.GetHandle());
		}
		void* mapped = nullptr;
		resource.Map(&mapped);
		if (!mapped || !made) {
			logger::warn("[DCLF] Texture streaming's feedback buffer has no {}: no feedback", mapped ? "buffer view ref" : "mapping");
			return;
		}
		auto* mappedWords = static_cast<volatile std::uint32_t*>(mapped);
		mappedWords[0] = 0;
		for (std::uint32_t i = 1; i < kWords; ++i)
			mappedWords[i] = kEmpty;
		words.store(mappedWords, std::memory_order_release);
		ref.store(made.Bits(), std::memory_order_release);
	}

	RenderGraph::BufferRef Ref()
	{
		const std::uint64_t bits = ref.load(std::memory_order_acquire);
		return { static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32) };
	}

	volatile std::uint32_t* Words()
	{
		return words.load(std::memory_order_acquire);
	}
}
