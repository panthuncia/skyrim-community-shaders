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
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
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
		constexpr std::string_view kClientId = "cs.texstream";
		// The most windows one copy pass carries, and the most staged bytes: what is left goes in the next one.
		constexpr std::size_t kMaxBatchWindows = 256;
		constexpr std::uint64_t kMaxBatchBytes = std::uint64_t(256) << 20;

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
			D3D11_TEXTURE2D_DESC desc{};
			std::string path;
			std::shared_ptr<org::memory::AllocatedResource> image;    // until its window holds it
			std::shared_ptr<org::memory::AllocatedResource> staging;  // until the copy pass is past it
			std::vector<rhi::helpers::TextureUploadFootprint> footprints;
			std::uint64_t bytes = 0;
			std::function<void(const std::shared_ptr<Window>&)> install;
			std::function<void()> retry;
			std::shared_ptr<Completion> completion;
			std::shared_ptr<Window> window;  // made on the render thread
		};

		/** @brief The windows one copy pass carries. Render thread. */
		struct Batch
		{
			std::vector<std::shared_ptr<Staged>> items;
			bool done = false;
		};

		struct State
		{
			std::mutex mutex;
			std::vector<std::shared_ptr<Staged>> staged;  // the workers'
			std::vector<Retired> retired;                 // any thread's
			std::shared_ptr<Batch> batch;                 // the render thread's: the one in flight
			// The render thread's: done batches, kept with their staging buffers and windows until nothing else holds them. A one-shot
			// pass may be recorded again (a ticket prepared ahead) after its run, so its copies' sources stay alive until then.
			std::vector<std::shared_ptr<Batch>> retiring;
			std::atomic<bool> ready{ false };
			std::atomic<bool> failed{ false };
			std::atomic<std::uint64_t> serial{ 0 };
			std::atomic<std::uint64_t> uploaded{ 0 }, batches{ 0 }, retried{ 0 };
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
		struct UploadBindings
		{
			std::vector<org::ResourceBindingToken> targets;
		};

		struct UploadFrame
		{
			std::shared_ptr<const Batch> batch;
		};

		/** @brief The batch's copies, each from its staging buffer into its window's mips. The graph moves each window to the copy layout and back. */
		class UploadPass final : public org::TypedRenderGraphPass<UploadPass, UploadFrame, UploadBindings>
		{
		public:
			explicit UploadPass(std::shared_ptr<const Batch> a_batch) :
				batch(std::move(a_batch)) {}

			UploadBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				UploadBindings bindings;
				bindings.targets.reserve(batch->items.size());
				for (const auto& item : batch->items)
					bindings.targets.push_back(a_builder.CopyDestination(item->window->texture));
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(batch.get())); }

			UploadFrame Prepare(const UploadBindings&, const org::PassPrepareContext&) const { return { batch }; }

			static void Record(const UploadBindings& a_bindings, const UploadFrame& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& items = a_frame.batch->items;
				for (std::size_t i = 0; i < items.size() && i < a_bindings.targets.size(); ++i) {
					const auto& item = *items[i];
					if (!item.staging)
						continue;
					const auto target = a_recording.Resolve(a_bindings.targets[i]).GetHandle();
					const auto staging = item.staging->Resource().GetHandle();
					for (const auto& footprint : item.footprints) {
						rhi::BufferTextureCopyFootprint copy{};
						copy.texture = target;
						copy.buffer = staging;
						copy.mip = footprint.mip;
						copy.arraySlice = 0;
						copy.footprint = { footprint.offset, footprint.rowPitch, footprint.width, footprint.height, 1 };
						a_recording.Commands().CopyBufferToTexture(copy);
					}
				}
			}

		private:
			std::shared_ptr<const Batch> batch;
		};

		std::string GraphName(std::uint64_t a_serial)
		{
			return std::format("cs.texstream.window-{}", a_serial);
		}

		/** @brief Hands a staged window back to the D3D11 path. Render thread. a_inBatch: its batch retires its staging buffer. */
		void Retry(Staged& a_item, bool a_inBatch = false)
		{
			S().retried.fetch_add(1, std::memory_order_relaxed);
			if (!a_inBatch)
				RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(a_item.staging));
			if (a_item.retry)
				a_item.retry();
			a_item.completion->Finish();
		}

		/**
		 * @brief Turns the backend off for the session: every window it holds is built again on D3D11, so nothing waits on a pass that
		 * will not come. Render thread. a_unregister: the client is still registered (not when the graph already dropped it).
		 */
		void Fail(std::string_view a_why, bool a_unregister = true)
		{
			auto& state = S();
			if (state.failed.exchange(true))
				return;
			state.ready.store(false, std::memory_order_release);
			logger::error("[TextureStreaming] graph windows are off for this session, windows are built on D3D11 again: {}", a_why);
			std::vector<std::shared_ptr<Staged>> staged;
			{
				std::scoped_lock lock(state.mutex);
				staged.swap(state.staged);
			}
			std::shared_ptr<Batch> batch;
			if (auto inFlight = std::exchange(state.batch, nullptr); inFlight && !inFlight->done) {
				inFlight->done = true;
				batch = std::move(inFlight);
				state.retiring.push_back(batch);
			}
			if (a_unregister)
				FrameAnchors::Unregister(kClientId);
			if (batch)
				for (const auto& item : batch->items)
					Retry(*item, true);
			for (const auto& item : staged)
				Retry(*item);
		}

		/** @brief Wraps a staged window for D3D11 and imports it into the graph. False: the backend cannot make windows. Render thread. */
		bool MakeWindow(Staged& a_item)
		{
			auto& runtime = RenderGraphRuntime::Get();
			const auto name = GraphName(S().serial.fetch_add(1, std::memory_order_relaxed));
			DxvkOrgInteropTextureWrap wrap{};
			wrap.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			wrap.width = a_item.desc.Width;
			wrap.height = a_item.desc.Height;
			wrap.depthOrArraySize = 1;
			wrap.mipLevels = a_item.desc.MipLevels;
			wrap.format = static_cast<std::uint32_t>(a_item.desc.Format);
			wrap.sampleCount = 1;
			wrap.bindFlags = D3D11_BIND_SHADER_RESOURCE;
			DxvkOrgInteropResourceInfo info{};
			auto window = std::make_shared<Window>();
			// From here on DXVK's stream may touch the image: it goes only through the window's release.
			window->image = std::move(a_item.image);
			// DXVK zeroes the image in its stream here: the copy pass comes later in the stream.
			window->d3d11 = runtime.WrapTexture(window->image->Resource(), wrap, info, a_item.path.c_str());
			if (!window->d3d11)
				return false;
			if (FAILED(globals::d3d::device->CreateShaderResourceView(window->d3d11.get(), nullptr, window->view.put())))
				return false;
			Util::SetResourceName(window->view.get(), "TextureStreaming::%s", a_item.path.c_str());

			const auto format = rhi::helpers::ToRHI(a_item.desc.Format);
			org::TextureDescription desc{};
			desc.type = rhi::ResourceType::Texture2D;
			desc.format = format;
			desc.hasSRV = true;
			desc.srvFormat = format;
			for (UINT mip = 0; mip < a_item.desc.MipLevels; ++mip)
				desc.imageDimensions.push_back({ (std::max)(a_item.desc.Width >> mip, 1u), (std::max)(a_item.desc.Height >> mip, 1u), 0, 0 });
			// In the layout DXVK rests it in: the graph moves it for the copy and gives it back there.
			window->texture = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), info.image, std::move(desc), name.c_str());
			if (!window->texture)
				return false;
			window->index = window->texture->GetSRVInfo(0).slot.index;
			a_item.window = std::move(window);
			return true;
		}

		/**
		 * @brief The batch's copies were submitted in DXVK's stream: its windows are installed. The batch keeps its staging buffers
		 * and windows until it retires (ServiceRetiringBatches). Render thread.
		 */
		void Install(const std::shared_ptr<Batch>& a_batch)
		{
			auto& state = S();
			a_batch->done = true;
			for (const auto& item : a_batch->items) {
				if (item->install)
					item->install(item->window);
				item->completion->Finish();
			}
			state.retiring.push_back(a_batch);
			state.uploaded.fetch_add(a_batch->items.size(), std::memory_order_relaxed);
			state.batches.fetch_add(1, std::memory_order_relaxed);
		}

		/**
		 * @brief Done batches that nothing else holds (no pass instance, no prepared ticket: their copies cannot be recorded again)
		 * give up their staging buffers and windows. The buffers go once D3D11's stream, which carried every copy, is past this
		 * point. Render thread.
		 */
		void ServiceRetiringBatches()
		{
			auto& retiring = S().retiring;
			std::erase_if(retiring, [](std::shared_ptr<Batch>& a_batch) {
				if (a_batch.use_count() > 1)
					return false;
				for (const auto& item : a_batch->items) {
					RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(item->staging));
					item->window = nullptr;
				}
				return true;
			});
		}

		/** @brief The next batch's client: its one-shot copy pass at the Present anchor. Replaces the last batch's. Render thread. */
		bool Register(const std::shared_ptr<Batch>& a_batch)
		{
			FrameAnchors::ClientDesc client;
			client.id = std::string(kClientId);
			client.passes.push_back(FrameAnchors::PassDesc{ .name = "cs.texstream.upload",
				.anchor = FrameAnchors::Anchor::Present,
				.order = 0,
				.kind = FrameAnchors::PassKind::Copy,
				.make = [batch = std::shared_ptr<const Batch>(a_batch)]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<UploadPass>(batch); },
				.once = true });
			// Only while the copy is to come: the build after it (the one-shot pass dropped) leaves the windows out of the graph.
			// Weak, as the other callbacks: a done batch is held only by its passes and tickets, so it can retire.
			client.registerResources = [weak = std::weak_ptr<Batch>(a_batch)](org::RenderGraph& a_graph) {
				const auto batch = weak.lock();
				if (!batch || batch->done)
					return;
				for (const auto& item : batch->items)
					if (item->window && item->window->texture)
						a_graph.RegisterResource(org::ResourceIdentifier(item->window->texture->GetName()), item->window->texture);
			};
			client.active = [batch = std::weak_ptr<Batch>(a_batch)] {
				const auto locked = batch.lock();
				return locked && !locked->done;
			};
			client.commit = [](const FrameAnchors::Point&) {};
			client.executed = [batch = std::weak_ptr<Batch>(a_batch)](const FrameAnchors::Point&) {
				auto& state = S();
				const auto locked = batch.lock();
				if (!locked || locked->done || state.batch != locked)
					return;
				Install(locked);
				state.batch = nullptr;
			};
			client.onFailed = [] { Fail("the render graph failed", false); };
			return FrameAnchors::Register(std::move(client));
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

	void Reserve()
	{
		if (!Requested() || !TextureStreaming::Enabled() || TextureStreaming::FullMode())
			return;
		if (!FrameAnchors::Reserve(FrameAnchors::Anchor::Present))
			logger::warn("[TextureStreaming] the Present anchor could not be reserved: windows stay on D3D11");
	}

	void FrameTick()
	{
		// A loading screen's Present: the staged windows wait for the main thread's next frame.
		if (!Requested() || !FrameAnchors::OnMainThread())
			return;
		auto& state = S();
		ServiceRetiringBatches();
		ServiceRetired();
		if (state.failed.load(std::memory_order_acquire))
			return;
		auto& runtime = RenderGraphRuntime::Get();
		const bool up = runtime.IsActive() && runtime.CanWrapTextures() && FrameAnchors::HasEpoch(FrameAnchors::Anchor::Present);
		if (up != state.ready.load(std::memory_order_acquire)) {
			state.ready.store(up, std::memory_order_release);
			if (up)
				logger::info("[TextureStreaming] windows are made on the render graph: uploaded at the Present anchor, read by index");
		}
		if (!up) {
			// The graph went away with windows staged: they are built on D3D11.
			bool holding = state.batch != nullptr;
			{
				std::scoped_lock lock(state.mutex);
				holding = holding || !state.staged.empty();
			}
			if (holding)
				Fail("the render graph is not active");
			return;
		}
		if (state.batch)
			return;  // one copy pass in flight at a time

		auto batch = std::make_shared<Batch>();
		{
			std::scoped_lock lock(state.mutex);
			std::uint64_t bytes = 0;
			std::size_t taken = 0;
			for (; taken < state.staged.size() && batch->items.size() < kMaxBatchWindows && (bytes < kMaxBatchBytes || batch->items.empty()); ++taken) {
				bytes += state.staged[taken]->bytes;
				batch->items.push_back(state.staged[taken]);
			}
			state.staged.erase(state.staged.begin(), state.staged.begin() + static_cast<std::ptrdiff_t>(taken));
		}
		if (batch->items.empty())
			return;
		for (const auto& item : batch->items) {
			if (!MakeWindow(*item)) {
				// The ones made so far go back with the rest.
				state.batch = batch;
				Fail(std::format("{} could not be wrapped for D3D11 or imported", item->path));
				return;
			}
		}
		state.batch = batch;
		if (!Register(batch))
			Fail("the copy pass could not be registered");
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

		std::vector<rhi::helpers::SubresourceData> sources;
		sources.reserve(a_upload.mips.size());
		for (const auto& mip : a_upload.mips)
			sources.push_back({ mip.pSysMem, mip.SysMemPitch, mip.SysMemSlicePitch });
		const rhi::Span<const rhi::helpers::SubresourceData> span{ sources.data(), static_cast<std::uint32_t>(sources.size()) };
		const auto plan = rhi::helpers::PlanTextureUploadSubresources(format, a_upload.desc.Width, a_upload.desc.Height, 1, a_upload.desc.MipLevels, 1, span);
		if (plan.footprints.size() != sources.size() || !plan.totalSize)
			return nullptr;

		auto item = std::make_shared<Staged>();
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
			if (org::memory::CreateAllocatedResource(desc, allocation, item->image) != rhi::Result::Ok || !item->image)
				return nullptr;
		}
		{
			rhi::ResourceDesc desc{};
			desc.type = rhi::ResourceType::Buffer;
			desc.heapType = rhi::HeapType::Upload;
			desc.resourceFlags = rhi::ResourceFlags::RF_None;
			desc.debugName = "TextureStreaming::WindowUpload";
			desc.buffer.sizeBytes = plan.totalSize;
			rhi::ma::AllocationDesc allocation{};
			allocation.heapType = rhi::HeapType::Upload;
			allocation.owner = "CS/TextureStreaming/Upload";
			if (org::memory::CreateAllocatedResource(desc, allocation, item->staging) != rhi::Result::Ok || !item->staging)
				return nullptr;
			void* mapped = nullptr;
			item->staging->Resource().Map(&mapped);
			if (!mapped)
				return nullptr;
			rhi::helpers::WriteTextureUploadSubresources(plan, span, mapped);
			item->staging->Resource().Unmap(0, plan.totalSize);
		}
		item->desc = a_upload.desc;
		item->path = std::move(a_upload.path);
		item->footprints = plan.footprints;
		item->bytes = plan.totalSize;
		item->install = std::move(a_upload.install);
		item->retry = std::move(a_upload.retry);
		item->completion = std::make_shared<Completion>();
		auto completion = item->completion;
		{
			auto& state = S();
			std::scoped_lock lock(state.mutex);
			// Turned off since the check above: the caller builds on D3D11, and the images go with item.
			if (state.failed.load(std::memory_order_acquire))
				return nullptr;
			state.staged.push_back(std::move(item));
		}
		return completion;
	}

	Counters GetCounters()
	{
		const auto& state = S();
		return { state.uploaded.load(std::memory_order_relaxed), state.batches.load(std::memory_order_relaxed), state.retried.load(std::memory_order_relaxed) };
	}
#else
	class Window
	{
	};

	void Reserve() {}
	void FrameTick() {}
	bool Available() { return false; }
	ID3D11ShaderResourceView* ViewOf(const Window&) { return nullptr; }
	std::uint32_t IndexOf(const Window&) { return 0; }
	std::shared_ptr<PendingWindow> Queue(Upload) { return nullptr; }
	Counters GetCounters() { return {}; }
#endif
}
