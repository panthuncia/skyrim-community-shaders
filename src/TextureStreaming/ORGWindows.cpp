#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "TextureStreaming/ORGWindows.h"

#include "RenderGraph/Switches.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include "Globals.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/D3D.h"
#	include "Vram/Debug.h"

#	include <OpenRenderGraph/DeviceMemory.h>
#	include <OpenRenderGraph/GraphHost.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/TextureDescription.h>
#	include <rhi_helpers.h>

#	include <atomic>
#	include <format>
#	include <mutex>
#endif

namespace TextureStreaming::ORG
{
	bool Requested()
	{
		static const bool requested = Switches::On("CS_ORG_TEXTURE_STREAMING");
		return requested;
	}

#if defined(CS_HAS_RENDER_GRAPH)
	namespace
	{
		/** @brief A build's completion: done once its window is installed, or handed back to D3D11. */
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

		/** @brief What a let-go window leaves behind: its D3D11 objects first, then its graph texture and memory once D3D11 is past them. */
		struct Retired
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			winrt::com_ptr<ID3D11Resource> d3d11;
			std::shared_ptr<org::ExternalTextureResource> texture;
			std::shared_ptr<org::memory::AllocatedResource> image;
		};

		/** @brief The parts that outlive D3D11's last use, handed to ReleaseAfterD3D11. Members go in reverse: the descriptors, then the memory. */
		struct GpuParts
		{
			std::shared_ptr<org::memory::AllocatedResource> image;
			std::shared_ptr<org::ExternalTextureResource> texture;
		};

		/** @brief A window between its build and its install. */
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

		// Never destroyed: a window let go during the process's exit still finds it.
		State& S()
		{
			static auto* state = new State;
			return *state;
		}
	}

	/** @brief The image, its D3D11 view for the shell, and its graph texture for Drawcall Limit Fix. */
	class Window
	{
	public:
		std::shared_ptr<org::memory::AllocatedResource> image;
		winrt::com_ptr<ID3D11Resource> d3d11;
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		std::shared_ptr<org::ExternalTextureResource> texture;
		std::uint32_t index = 0;

		// Any thread: the render thread releases it (FrameTick).
		~Window()
		{
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

		/** @brief Hands a staged window back to the D3D11 path. Render thread. */
		void Retry(Staged& a_item)
		{
			S().retried.fetch_add(1, std::memory_order_relaxed);
			a_item.window = nullptr;
			if (a_item.retry)
				a_item.retry();
			a_item.completion->Finish();
		}

		/** @brief Turns the backend off for the session: every window it holds is built again on D3D11. Render thread. */
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
		 * @brief Makes the window: an image ORG's allocator holds, wrapped for D3D11 with its mips as initial data, and imported into the
		 * graph for its descriptor. DXVK writes the mips in its own stream (its initializer, ahead of any later D3D11 or graph work), so
		 * no graph pass copies them: nothing registers with the graph, and the graph is never built again for a window. Any thread, as
		 * D3D11's texture creation is. The import is safe off the render thread too, the same path engine textures take
		 * (Engine/Textures, Textures::Create): BasicRHI's resource registry allocates under its own lock, ORG's DescriptorHeapManager
		 * assigns the SRV slot under its mutation mutex, and the resource's ECS token is posted to ECSManager's queue, never made
		 * in the world on this thread. The host lives until DXVK destroys its device. False: the backend cannot make this window.
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
			// From here on DXVK's stream may touch the image: it goes only through the window's release.
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
			// In the layout DXVK rests it in. No pass of the graph names it, so the graph never moves it out of there.
			window->texture = RenderGraph::ImportDxvkTexture(host->Device(), info.image, std::move(desc), name.c_str());
			if (!window->texture)
				return false;
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
				// D3D11's objects first: DXVK's last use of the image is then behind the query ReleaseAfterD3D11 issues.
				parts.view = nullptr;
				parts.d3d11 = nullptr;
				runtime.ReleaseAfterD3D11(std::make_shared<GpuParts>(GpuParts{ std::move(parts.image), std::move(parts.texture) }));
			}
		}
	}

	void FrameTick()
	{
		// Off the render thread (a loading screen without CS_ORG_LOADING_THREAD=1): the made windows wait for the main thread's next
		// frame. With it, the loading loop's Presents run this too.
		if (!Requested() || !FrameAnchors::IsRenderOwner())
			return;
		auto& state = S();
		ServiceRetired();
		if (state.failed.load(std::memory_order_acquire))
			return;
		auto& runtime = RenderGraphRuntime::Get();
		const bool up = TextureStreaming::Enabled() && !TextureStreaming::FullMode() && runtime.IsActive() && runtime.CanWrapTexturesWithData();
		if (up != state.ready.load(std::memory_order_acquire)) {
			state.ready.store(up, std::memory_order_release);
			if (up)
				logger::info("[TextureStreaming] windows are made on the render graph: created with their mips in DXVK's stream, read by index");
		}
		std::vector<std::shared_ptr<Staged>> staged;
		{
			std::scoped_lock lock(state.mutex);
			staged.swap(state.staged);
		}
		if (!up) {
			// The graph went away with windows made: they are built on D3D11.
			if (!staged.empty())
				Fail("the render graph is not active", std::move(staged));
			return;
		}
		if (staged.empty())
			return;
		// Their mips are in DXVK's stream since their creation: the shells may name them from this frame on.
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
			// Like DXVK's windows: the spill pool first, system memory past the budget.
			allocation.flags = rhi::ma::AllocationFlagSpillable;
			allocation.owner = Vram::Debug::GameTextureOwner(a_upload.path);
			if (org::memory::CreateAllocatedResource(desc, allocation, image) != rhi::Result::Ok || !image)
				return nullptr;
		}

		auto item = std::make_shared<Staged>();
		item->path = std::move(a_upload.path);
		// The caller builds on D3D11 when this window cannot be made; the image goes with item.
		if (!MakeWindow(*item, a_upload.desc, std::move(image), a_upload.mips))
			return nullptr;
		item->install = std::move(a_upload.install);
		item->retry = std::move(a_upload.retry);
		item->completion = std::make_shared<Completion>();
		auto completion = item->completion;
		{
			auto& state = S();
			std::scoped_lock lock(state.mutex);
			// Turned off since the check above: the caller builds on D3D11, and the window goes with item.
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
#else
	class Window
	{
	};

	void FrameTick() {}
	bool Available() { return false; }
	ID3D11ShaderResourceView* ViewOf(const Window&) { return nullptr; }
	std::uint32_t IndexOf(const Window&) { return 0; }
	std::shared_ptr<PendingWindow> Queue(Upload) { return nullptr; }
	Counters GetCounters() { return {}; }
#endif
}
