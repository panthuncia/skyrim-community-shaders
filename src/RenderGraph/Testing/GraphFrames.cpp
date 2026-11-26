#include "RenderGraph/Testing/GraphFrames.h"

#if !defined(CS_RELEASE_PROFILE)

#	include "RenderGraph/Frame/FrameDriver.h"
#	include "Testing/Testing.h"

#	include <atomic>
#	include <map>
#	include <mutex>
#	include <string>

namespace RenderGraph::TestSupport::GraphFrames
{
	/** @brief One client's counts. A frame counts once, however many taps it ran. The render owner writes; the report reads. */
	struct Counts
	{
		std::atomic<std::uint64_t> graph{ 0 };
		std::atomic<std::uint64_t> firstGraphFrame{ 0 };
		std::atomic<std::uint64_t> lastGraphFrame{ UINT64_MAX };
	};

	namespace
	{
		struct GraphFrameTable
		{
			std::mutex mutex;
			// Map nodes never move, so the pointers CountsOf hands out stay valid.
			std::map<std::string, Counts, std::less<>> clients;
		};

		GraphFrameTable& GraphFrameState()
		{
			static GraphFrameTable table;
			return table;
		}
	}

	Counts* CountsOf(std::string_view a_client)
	{
		auto& table = GraphFrameState();
		const std::lock_guard lock(table.mutex);
		auto it = table.clients.find(a_client);
		if (it == table.clients.end())
			it = table.clients.try_emplace(std::string(a_client)).first;
		return &it->second;
	}

	void NoteGraph(Counts* a_counts, std::uint64_t a_frame)
	{
		if (!a_counts || a_counts->lastGraphFrame.load(std::memory_order_relaxed) == a_frame)
			return;
		if (!a_counts->graph.load(std::memory_order_relaxed))
			a_counts->firstGraphFrame.store(a_frame, std::memory_order_relaxed);
		a_counts->lastGraphFrame.store(a_frame, std::memory_order_relaxed);
		a_counts->graph.fetch_add(1, std::memory_order_relaxed);
	}

	void Report()
	{
		auto& table = GraphFrameState();
		nlohmann::json clients = nlohmann::json::object();
		std::string line;
		{
			const std::lock_guard lock(table.mutex);
			for (const auto& [id, counts] : table.clients) {
				const std::uint64_t graph = counts.graph.load(std::memory_order_relaxed);
				// A registered client that never committed is left out.
				if (!graph)
					continue;
				clients[id] = { { "graph", graph }, { "first_graph_frame", counts.firstGraphFrame.load(std::memory_order_relaxed) } };
				line += fmt::format("{}{} {}", line.empty() ? "" : ", ", id, graph);
			}
		}
		const std::uint64_t frames = RenderGraph::Frame::Driver::Current().frame;
		logger::info("[ORG] graph frames at exit, of {} frames (client graph frames): {}", frames, line.empty() ? "none ran" : line);
		::Testing::SetReport("graph_frames", { { "frames", frames }, { "clients", std::move(clients) } });
	}
}

#endif
