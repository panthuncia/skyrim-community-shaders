// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/ExternalWork.h"

#include "RenderGraph/Fatal.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/Runtime.h>

#include <atomic>
#include <mutex>
#include <string>

namespace RenderGraph::ExternalWork
{
	namespace
	{
		struct State
		{
			std::mutex lock;
			std::unique_ptr<org::ExternalClient> client;
			bool shutDown = false;  // nothing is submitted any more
			std::atomic<std::uint64_t> submissions{ 0 };
		};

		State& Get()
		{
			static State state;
			return state;
		}

		Access WholeIn(org::Resource& a_resource, rhi::ResourceAccessType a_access, rhi::ResourceLayout a_layout)
		{
			return { &a_resource, {}, { a_access, a_layout, rhi::ResourceSyncState::Copy } };
		}
	}

	Access CopySource(org::Resource& a_resource)
	{
		return WholeIn(a_resource, rhi::ResourceAccessType::CopySource, rhi::ResourceLayout::CopySource);
	}

	Access CopyDestination(org::Resource& a_resource)
	{
		return WholeIn(a_resource, rhi::ResourceAccessType::CopyDest, rhi::ResourceLayout::CopyDest);
	}

	void Start(std::shared_ptr<org::Runtime> a_runtime, rhi::Device a_device)
	{
		auto& state = Get();
		std::lock_guard guard(state.lock);
		if (state.client || state.shutDown)
			RenderGraph::Fatal("external work", "ORG's external client was made twice, or after its shutdown");
		try {
			org::ExternalClient::Desc desc{};
			desc.runtime = std::move(a_runtime);
			// The graphs' queue slots in their order, since a handoff matches queues by slot.
			desc.queues = { { a_device.GetQueue(rhi::QueueKind::Graphics), rhi::QueueKind::Graphics },
				{ a_device.GetQueue(rhi::QueueKind::Compute), rhi::QueueKind::Compute }, { a_device.GetQueue(rhi::QueueKind::Copy), rhi::QueueKind::Copy } };
			desc.name = "CS external work";
			desc.entryFullBarrier = true;
			// The context's operations, DCLF's commands and the presents share it, so a submission never waits for an earlier one's slot.
			desc.capacity = 256;
			state.client = std::make_unique<org::ExternalClient>(std::move(desc));
		} catch (const std::exception& e) {
			RenderGraph::Fatal("external work", fmt::format("ORG's external client could not be made: {}", e.what()));
		}
		logger::info("[ORG] external work: ORG's external client is ready with the native device (open submissions: each leaves what it used "
					 "in the state it declared, for the ledgers to carry on)");
	}

	bool Submit(std::string_view a_label, std::span<const Access> a_accesses, const std::function<void(rhi::CommandList&)>& a_record,
		std::shared_ptr<const void> a_keepAlive, std::span<const rhi::TimelinePoint> a_signals, std::span<const rhi::TimelinePoint> a_waits)
	{
		// Stages whose commit point this is commit first, since this work must follow them on the GPU.
		::FrameAnchors::BeforeGpuWork("external work");
		// So do the render owner's epochs still on the submission thread.
		RenderGraphRuntime::Get().DrainSubmissions("external work");
		auto& state = Get();
		// One submission at a time. The lock also covers Start and Shutdown.
		std::lock_guard guard(state.lock);
		// The window's close: nothing is submitted any more.
		if (state.shutDown)
			return false;
		if (!state.client)
			RenderGraph::Fatal("external work", fmt::format("'{}' has no ORG external client to submit through (it is made with the native device)", a_label));
		try {
			state.client->Submit(0, a_accesses, a_record, a_signals, std::move(a_keepAlive), a_waits);
		} catch (const std::exception& e) {
			RenderGraph::Fatal("external work", fmt::format("ORG's external client could not submit '{}': {}", a_label, e.what()));
		}
		state.submissions.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void Shutdown()
	{
		auto& state = Get();
		std::lock_guard guard(state.lock);
		state.shutDown = true;
		if (!state.client)
			return;
		try {
			state.client->ShutdownTaskWorkers();
			// The destructor waits for every submission.
			state.client.reset();
		} catch (const std::exception& e) {
			logger::error("[ORG] external work: shutdown failed: {}", e.what());
			(void)state.client.release();
		}
		logger::info("[ORG] external work: {} submissions in the session", state.submissions.load(std::memory_order_relaxed));
	}
}

