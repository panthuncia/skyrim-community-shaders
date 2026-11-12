#include "Features/DrawcallLimitFix/Core/Gpu/TextureFeedback.h"
#include "RenderGraph/BufferRefs.h"

#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <atomic>
#include <memory>

namespace DCLF::TextureFeedback
{
	namespace
	{
		std::shared_ptr<org::Buffer> buffer;  // the render thread's, kept for the process
		std::atomic<std::uint64_t> ref{ 0 };  // the buffer's view ref (BufferRef::Bits), which the pixel stages write through
		std::atomic<volatile std::uint32_t*> words{ nullptr };
	}

	void Ensure()
	{
		if (buffer)
			return;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !host->Graph())
			return;
		buffer = org::Buffer::CreateShared(rhi::HeapType::HostVisibleCoherent, std::uint64_t(kWords) * sizeof(std::uint32_t), true);
		buffer->SetName("cs.dclf.texture-feedback");
		auto resource = buffer->GetAPIResource();
		void* mapped = nullptr;
		resource.Map(&mapped);
		const RenderGraph::BufferRef made = RenderGraph::BufferRefOf(host->Device(), resource.GetHandle());
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
