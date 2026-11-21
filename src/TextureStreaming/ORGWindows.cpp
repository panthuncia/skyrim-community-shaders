// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "TextureStreaming/ORGWindows.h"

#include "RenderGraph/Switches.h"

#include "Globals.h"
#include "RenderGraph/DxvkImageImport.h"
#include "RenderGraph/ImageRegistry.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "Utils/D3D.h"
#include "Vram/Debug.h"

#include <OpenRenderGraph/DeviceMemory.h>
#include <OpenRenderGraph/GraphHost.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/TextureDescription.h>
#include <rhi_helpers.h>

#include <atomic>
#include <format>
#include <mutex>

namespace TextureStreaming::ORG
{
	namespace
	{
		/** @brief Done once its window is installed, or handed back to D3D11. */
		class Completion final : public PendingWindow
		{
		public:
			bool Done() const override
			{
				std::scoped_lock lock(mutex);
				return done;
			}

			void Subscribe(std::function<void()> a_wake) override
			{
				{
					std::scoped_lock lock(mutex);
					if (!done) {
						wakes.push_back(std::move(a_wake));
						return;
					}
				}
				a_wake();
			}

			void Finish()
			{
				std::vector<std::function<void()>> woken;
				{
					std::scoped_lock lock(mutex);
					if (done)
						return;
					done = true;
					woken.swap(wakes);
				}
				for (auto& wake : woken)
					wake();
			}

		private:
			mutable std::mutex mutex;
			bool done = false;
			std::vector<std::function<void()>> wakes;
		};

		/** @brief D3D11 objects go first, then the graph texture and memory once D3D11 is past them. */
		struct Retired
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			winrt::com_ptr<ID3D11Resource> d3d11;
			std::shared_ptr<org::ExternalTextureResource> texture;
			std::shared_ptr<org::memory::AllocatedResource> image;
		};

		/** @brief Handed to ReleaseAfterD3D11. Members go in reverse, descriptors then memory. */
		struct GpuParts
		{
			std::shared_ptr<org::memory::AllocatedResource> image;
			std::shared_ptr<org::ExternalTextureResource> texture;
		};

		struct Staged
		{
			std::string path;
			std::function<void(const std::shared_ptr<Window>&)> install;
			std::function<void()> retry;
			std::shared_ptr<Completion> completion;
			std::shared_ptr<Window> window;
		};

		struct State
		{
			std::mutex mutex;
			std::vector<std::shared_ptr<Staged>> staged;  // the workers'
			std::vector<Retired> retired;                 // any thread's
			std::atomic<bool> ready{ false };
			std::atomic<bool> failed{ false };
			std::atomic<std::uint64_t> serial{ 0 };
			std::atomic<std::uint64_t> made{ 0 }, installFrames{ 0 }, retried{ 0 };
		};

		// Never destroyed, since a window let go during process exit still finds it.
		State& S()
		{
			static auto* state = new State;
			return *state;
		}
	}

	class Window
	{
	public:
		std::shared_ptr<org::memory::AllocatedResource> image;
		winrt::com_ptr<ID3D11Resource> d3d11;
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		std::shared_ptr<org::ExternalTextureResource> texture;
		std::uint32_t index = 0;

		// Any thread. The render thread releases it in FrameTick.
		~Window()
		{
			// No import may find the texture after this.
			RenderGraph::ImageRegistry::Retire(texture.get());
			auto& state = S();
			std::scoped_lock lock(state.mutex);
			state.retired.push_back({ std::move(view), std::move(d3d11), std::move(texture), std::move(image) });
		}
	};

	namespace
	{
		std::string GraphName(std::uint64_t a_serial)
		{
			return std::format("cs.texstream.window-{}", a_serial);
		}

		/** @brief Render thread. */
		void Retry(Staged& a_item)
		{
			S().retried.fetch_add(1, std::memory_order_relaxed);
			a_item.window = nullptr;
			if (a_item.retry)
				a_item.retry();
			a_item.completion->Finish();
		}

		/** @brief For the session. Every window it holds is rebuilt on D3D11. Render thread. */
		void Fail(std::string_view a_why, std::vector<std::shared_ptr<Staged>> a_staged)
		{
			auto& state = S();
			if (!state.failed.exchange(true)) {
				state.ready.store(false, std::memory_order_release);
				logger::error("[TextureStreaming] graph windows are off for this session, windows are built on D3D11 again: {}", a_why);
			}
			{
				std::scoped_lock lock(state.mutex);
				a_staged.insert(a_staged.end(), state.staged.begin(), state.staged.end());
				state.staged.clear();
			}
			for (const auto& item : a_staged)
				Retry(*item);
		}

		/**
		* @brief DXVK writes the mips in its own stream ahead of later work, so no graph pass copies them and the graph never rebuilds.
		* Any thread. The import is safe off the render thread, the same path engine textures take.
		*/
		bool MakeWindow(Staged& a_item, const D3D11_TEXTURE2D_DESC& a_desc, std::shared_ptr<org::memory::AllocatedResource> a_image,
			const std::vector<D3D11_SUBRESOURCE_DATA>& a_mips)
		{
			auto& runtime = RenderGraphRuntime::Get();
			auto* host = runtime.Host();
			if (!host)
				return false;
			const auto name = GraphName(S().serial.fetch_add(1, std::memory_order_relaxed));
			DxvkOrgInteropTextureWrap wrap{};
			wrap.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			wrap.width = a_desc.Width;
			wrap.height = a_desc.Height;
			wrap.depthOrArraySize = 1;
			wrap.mipLevels = a_desc.MipLevels;
			wrap.format = static_cast<std::uint32_t>(a_desc.Format);
			wrap.sampleCount = 1;
			wrap.bindFlags = D3D11_BIND_SHADER_RESOURCE;
			DxvkOrgInteropResourceInfo info{};
			auto window = std::make_shared<Window>();
			// From here on DXVK's stream may touch the image, so it goes only through the window's release.
			window->image = std::move(a_image);
			window->d3d11 = runtime.WrapTexture(window->image->Resource(), wrap, info, a_item.path.c_str(), a_mips.data());
			if (!window->d3d11)
				return false;
			if (FAILED(globals::d3d::device->CreateShaderResourceView(window->d3d11.get(), nullptr, window->view.put())))
				return false;
			Util::SetResourceName(window->view.get(), "TextureStreaming::%s", a_item.path.c_str());

			const auto format = rhi::helpers::ToRHI(a_desc.Format);
			org::TextureDescription desc{};
			desc.type = rhi::ResourceType::Texture2D;
			desc.format = format;
			desc.hasSRV = true;
			desc.srvFormat = format;
			for (UINT mip = 0; mip < a_desc.MipLevels; ++mip)
				desc.imageDimensions.push_back({ (std::max)(a_desc.Width >> mip, 1u), (std::max)(a_desc.Height >> mip, 1u), 0, 0 });
			// No graph pass names it, so the graph never moves it out of DXVK's resting layout.
			window->texture = RenderGraph::ImportDxvkTexture(host->Device(), info.image, desc, name.c_str());
			if (!window->texture)
				return false;
			// An import of the window's texture returns this one.
			RenderGraph::ImageRegistry::Register(reinterpret_cast<std::uint64_t>(info.image.image), { window->texture, nullptr,
				std::make_shared<const org::TextureDescription>(desc), "ORGWindows", name, RenderGraph::ImageRegistry::Kind::Made });
			window->index = window->texture->GetSRVInfo(0).slot.index;
			a_item.window = std::move(window);
			return true;
		}

		void ServiceRetired()
		{
			std::vector<Retired> retired;
			{
				auto& state = S();
				std::scoped_lock lock(state.mutex);
				retired.swap(state.retired);
			}
			auto& runtime = RenderGraphRuntime::Get();
			for (auto& parts : retired) {
				// Release D3D11's objects first, so DXVK's last use is behind ReleaseAfterD3D11's query.
				parts.view = nullptr;
				parts.d3d11 = nullptr;
				runtime.ReleaseAfterD3D11(std::make_shared<GpuParts>(GpuParts{ std::move(parts.image), std::move(parts.texture) }));
			}
		}
	}

	void FrameTick()
	{
		// Off the render thread, made windows wait for the main thread's next frame. In the graph modes the loading loop runs this too.
		if (!RenderGraph::Engine::GraphMode() || !FrameAnchors::IsRenderOwner())
			return;
		auto& state = S();
		ServiceRetired();
		if (state.failed.load(std::memory_order_acquire))
			return;
		auto& runtime = RenderGraphRuntime::Get();
		const bool up = TextureStreaming::Enabled() && !TextureStreaming::FullMode() && runtime.IsActive();
		if (up != state.ready.load(std::memory_order_acquire)) {
			state.ready.store(up, std::memory_order_release);
			if (up)
				logger::info("[TextureStreaming] windows are made on the render graph: created with their mips in {}, read by index",
					RenderGraph::Engine::Native::Active() ? "the native device's uploads (native shells)" : "DXVK's stream");
		}
		std::vector<std::shared_ptr<Staged>> staged;
		{
			std::scoped_lock lock(state.mutex);
			staged.swap(state.staged);
		}
		if (!up) {
			// The graph went away with windows made, so build them on D3D11.
			if (!staged.empty())
				Fail("the render graph is not active", std::move(staged));
			return;
		}
		if (staged.empty())
			return;
		// Their mips are in DXVK's stream since creation, so the shells may name them now.
		for (const auto& item : staged) {
			if (item->install)
				item->install(item->window);
			item->completion->Finish();
		}
		state.made.fetch_add(staged.size(), std::memory_order_relaxed);
		state.installFrames.fetch_add(1, std::memory_order_relaxed);
	}

	bool Available()
	{
		const auto& state = S();
		return state.ready.load(std::memory_order_acquire) && !state.failed.load(std::memory_order_acquire);
	}

	ID3D11ShaderResourceView* ViewOf(const Window& a_window)
	{
		return a_window.view.get();
	}

	std::uint32_t IndexOf(const Window& a_window)
	{
		return a_window.index;
	}

	std::shared_ptr<PendingWindow> Queue(Upload a_upload)
	{
		if (!Available() || a_upload.mips.size() != a_upload.desc.MipLevels || a_upload.mips.empty())
			return nullptr;
		const auto format = rhi::helpers::ToRHI(a_upload.desc.Format);
		if (format == rhi::Format::Unknown)
			return nullptr;

		std::shared_ptr<org::memory::AllocatedResource> image;
		{
			rhi::ResourceDesc desc{};
			desc.type = rhi::ResourceType::Texture2D;
			desc.heapType = rhi::HeapType::DeviceLocal;
			desc.resourceFlags = rhi::ResourceFlags::RF_None;
			desc.debugName = a_upload.path.c_str();
			desc.texture.format = format;
			desc.texture.width = a_upload.desc.Width;
			desc.texture.height = a_upload.desc.Height;
			desc.texture.depthOrLayers = 1;
			desc.texture.mipLevels = static_cast<std::uint16_t>(a_upload.desc.MipLevels);
			desc.texture.sampleCount = 1;
			rhi::ma::AllocationDesc allocation{};
			allocation.heapType = rhi::HeapType::DeviceLocal;
			// Like DXVK's windows, the spill pool first, system memory past the budget.
			allocation.flags = rhi::ma::AllocationFlagSpillable;
			allocation.owner = Vram::Debug::GameTextureOwner(a_upload.path);
			if (org::memory::CreateAllocatedResource(desc, allocation, image) != rhi::Result::Ok || !image)
				return nullptr;
		}

		auto item = std::make_shared<Staged>();
		item->path = std::move(a_upload.path);
		// The image goes with item.
		if (!MakeWindow(*item, a_upload.desc, std::move(image), a_upload.mips))
			return nullptr;
		item->install = std::move(a_upload.install);
		item->retry = std::move(a_upload.retry);
		item->completion = std::make_shared<Completion>();
		auto completion = item->completion;
		{
			auto& state = S();
			std::scoped_lock lock(state.mutex);
			// Turned off since the check above. The window goes with item.
			if (state.failed.load(std::memory_order_acquire))
				return nullptr;
			state.staged.push_back(std::move(item));
		}
		return completion;
	}

	Counters GetCounters()
	{
		const auto& state = S();
		return { state.made.load(std::memory_order_relaxed), state.installFrames.load(std::memory_order_relaxed), state.retried.load(std::memory_order_relaxed) };
	}
}
