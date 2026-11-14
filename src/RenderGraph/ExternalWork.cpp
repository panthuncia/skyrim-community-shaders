#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/ExternalWork.h"

#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <OpenRenderGraph/Runtime.h>

#	include <atomic>
#	include <mutex>
#	include <set>
#	include <string>

namespace RenderGraph::ExternalWork
{
	namespace
	{
		struct State
		{
			std::mutex lock;
			std::unique_ptr<org::ExternalClient> client;
			bool failed = false;    // the client could not be made: no more tries this session
			bool shutDown = false;  // after Shutdown: nothing is made again
			std::atomic<std::uint64_t> submissions{ 0 };
			std::set<std::string> loggedFailures;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		/** @brief The client, made on the first call with the render graph active. Null until then, and after a failure. With the lock. */
		org::ExternalClient* ClientLocked(State& a_state)
		{
			if (a_state.client || a_state.failed || a_state.shutDown)
				return a_state.client.get();
			auto& runtime = RenderGraphRuntime::Get();
			auto* host = runtime.Host();
			if (!runtime.IsActive() || !host)
				return nullptr;
			try {
				auto device = host->Device();
				org::ExternalClient::Desc desc{};
				desc.runtime = host->GetRuntime().shared_from_this();
				// The graphs' slots, in their order: a handoff between a graph and the client matches queues by slot.
				desc.queues = { { device.GetQueue(rhi::QueueKind::Graphics), rhi::QueueKind::Graphics },
					{ device.GetQueue(rhi::QueueKind::Compute), rhi::QueueKind::Compute }, { device.GetQueue(rhi::QueueKind::Copy), rhi::QueueKind::Copy } };
				desc.name = "CS external work";
				// Native mode has no home states: the client is open, and the ledgers carry what it leaves to the next user.
				desc.closed = host->ClosedExecutions() || host->GetDesc().externalHomeStates;
				desc.entryFullBarrier = true;
				const bool closed = desc.closed;
				a_state.client = std::make_unique<org::ExternalClient>(std::move(desc));
				logger::info("[ORG] external work: ORG's external client is ready ({})",
					closed ? "closed submissions: each returns what it used to its home state" :
							 "open submissions: each leaves what it used in the state it declared, for the ledgers to carry on");
			} catch (const std::exception& e) {
				a_state.failed = true;
				logger::error("[ORG] external work: ORG's external client could not be made: {}; such work keeps its own path", e.what());
			}
			return a_state.client.get();
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

	bool Submit(std::string_view a_label, std::span<const Access> a_accesses, const std::function<void(rhi::CommandList&)>& a_record,
		std::shared_ptr<const void> a_keepAlive, std::span<const rhi::TimelinePoint> a_signals)
	{
		// A region's held anchor runs go first: this work must follow them on the GPU.
		::FrameAnchors::FlushRegions("external work");
		auto& state = Get();
		// One submission at a time: the client serializes them anyway, and this lock also covers its creation and Shutdown.
		std::lock_guard guard(state.lock);
		auto* client = ClientLocked(state);
		if (!client)
			return false;
		try {
			client->Submit(0, a_accesses, a_record, a_signals, std::move(a_keepAlive));
		} catch (const std::exception& e) {
			if (state.loggedFailures.emplace(a_label).second)
				logger::error("[ORG] external work: '{}' could not be submitted: {}; it keeps its own path (logged once per kind)", a_label, e.what());
			return false;
		}
		state.submissions.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	std::uint64_t Submissions()
	{
		return Get().submissions.load(std::memory_order_relaxed);
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

#endif
