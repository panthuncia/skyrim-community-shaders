#include "RenderGraph/Testing/GraphFrames.h"

#if !defined(CS_RELEASE_PROFILE)

#	include "RenderGraph/Frame/FrameDriver.h"
#	include "Testing/Testing.h"

#	include <map>
#	include <mutex>
#	include <string>

namespace RenderGraph::TestSupport::GraphFrames
{
	namespace
	{
		/** @brief One client's counts. A frame counts once, however many taps it ran. */
		struct GraphFrameCounts
		{
			std::uint64_t graph = 0;
			std::uint64_t firstGraphFrame = 0;
			std::uint64_t lastGraphFrame = UINT64_MAX;
		};

		struct GraphFrameTable
		{
			std::mutex mutex;
			std::map<std::string, GraphFrameCounts, std::less<>> clients;
		};

		GraphFrameTable& GraphFrameState()
		{
			static GraphFrameTable table;
			return table;
		}

		GraphFrameCounts& GraphFramesCountsOf(GraphFrameTable& a_table, std::string_view a_client)
		{
			auto it = a_table.clients.find(a_client);
			if (it == a_table.clients.end())
				it = a_table.clients.emplace(std::string(a_client), GraphFrameCounts{}).first;
			return it->second;
		}
	}

	void NoteGraph(std::string_view a_client, std::uint64_t a_frame)
	{
		auto& table = GraphFrameState();
		const std::lock_guard lock(table.mutex);
		auto& counts = GraphFramesCountsOf(table, a_client);
		if (counts.lastGraphFrame == a_frame)
			return;
		if (!counts.graph)
			counts.firstGraphFrame = a_frame;
		counts.lastGraphFrame = a_frame;
		++counts.graph;
	}

	void Report()
	{
		auto& table = GraphFrameState();
		nlohmann::json clients = nlohmann::json::object();
		std::string line;
		{
			const std::lock_guard lock(table.mutex);
			for (const auto& [id, counts] : table.clients) {
				clients[id] = { { "graph", counts.graph }, { "first_graph_frame", counts.firstGraphFrame } };
				line += fmt::format("{}{} {}", line.empty() ? "" : ", ", id, counts.graph);
			}
		}
		const std::uint64_t frames = RenderGraph::Frame::Driver::Current().frame;
		logger::info("[ORG] graph frames at exit, of {} frames (client graph frames): {}", frames, line.empty() ? "none ran" : line);
		::Testing::SetReport("graph_frames", { { "frames", frames }, { "clients", std::move(clients) } });
	}
}

#endif
