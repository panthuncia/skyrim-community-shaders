#pragma once

// The engine's occlusion queries answered by the render graph, in graph mode (Engine::GraphMode).
//
// The engine keeps a ring of 64 D3D11 occlusion queries, used by the sun glare and the decals. In graph mode BeginQuery,
// EndQuery and GetQueryData are detoured. A port opens a Bracket around the engine call that issues a query. The query inside
// goes nowhere, and the port counts the draw with an ORG query instead. GetQueryData answers from the readback once that run's
// GPU work is done, and returns not-ready until then, as D3D11 does. A query outside any bracket is a tap error (EngineWork)
// and answers its last count. D3D11 never sees it.
//
// Free of ORG and Vulkan includes, except BasicRHI's rhi.h for the record-time helpers.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Render/RunRef.h>
#include <rhi.h>

namespace org
{
	class Resource;
}

namespace RenderGraph::Engine::OcclusionQueries
{
	inline constexpr std::uint32_t kRingSlots = 64;

	inline constexpr std::int32_t kNoSlot = -1;

	bool Active();

	/** @brief Main thread. With only CS_ORG_OCCLUSION_QUERY_LOG=1, hooks just GetQueryData to log D3D11's answers for comparison. */
	void Install();

	/**
	* @brief Render-thread scope around an engine call that begins a query, draws and ends it. That query goes to the graph instead.
	* The port must Commit or Skip the slot. At scope end, a slot that was neither, or whose run was not submitted, is skipped.
	*/
	class Bracket
	{
	public:
		/** @brief A skipped slot answers its last count, so the sun glare holds while a draw is skipped. */
		Bracket();
		/** @brief A skipped slot answers a_skipAnswer. For decals, 1 places the decal as visible. */
		explicit Bracket(std::uint32_t a_skipAnswer);
		~Bracket();
		Bracket(const Bracket&) = delete;
		Bracket& operator=(const Bracket&) = delete;

		/** @brief kNoSlot if the engine began none inside the scope. */
		std::int32_t Slot() const { return slot; }

		/** @brief Logged once per reason. */
		void Skip(std::string_view a_why);

		/** @brief Answers at once with no run, for a bracket that drew nothing. */
		void Answer(std::uint32_t a_count);

	private:
		friend struct Hooks;
		Bracket* outer = nullptr;
		std::int32_t slot = kNoSlot;
		bool hasSkipAnswer = false;
		std::uint32_t skipAnswer = 0;
	};

	/**
	* @brief The ORG queries of one client's runs. Each run's lease index owns a range of PerRun() queries.
	* Ranges come in chunks of kRunsPerChunk lease indices, each with its own pool and readback buffer. Chunks never move.
	* The readbacks are graph resources the pass declares, so a chunk made after a build is only resolved from the next build.
	* A range's counts are read at the next commit of its lease index, or earlier when GetQueryData finds the work done.
	*/
	class QueryRing
	{
	public:
		/** @brief Render thread, with the graph host up. Null and logged on failure. */
		static std::shared_ptr<QueryRing> Create(std::string_view a_name, std::uint32_t a_perRun);

		std::uint32_t PerRun() const;

		static constexpr std::uint32_t kRunsPerChunk = 64;

		/** @brief Enough lease indices for 256 runs in flight, more than play reaches. A later chunk is still made at a commit and refreshes the client. */
		static constexpr std::uint32_t kSetupChunks = 4;

		/** @brief In chunk order. The client's pass declares these as copy destinations. Any thread. */
		std::vector<std::shared_ptr<org::Resource>> Readbacks() const;

		/** @brief Record time, any thread. Invalid if the run's chunk was not made. */
		rhi::QueryPoolHandle Pool(const org::RunRef& a_run) const;
		std::uint32_t Index(const org::RunRef& a_run, std::uint32_t a_query) const;
		/** @brief Record time, outside any render pass, before any of the queries is begun. */
		void Reset(rhi::CommandList& a_commands, const org::RunRef& a_run, std::uint32_t a_first = 0, std::uint32_t a_count = ~0u) const;
		/**
		* @brief Record time, outside any render pass, after the draws. Ends empty queries for those not in a_begun, then resolves the range.
		* If the range's readback is not among the a_declared the pass knew, nothing is resolved and its slots are skipped.
		*/
		void Resolve(rhi::CommandList& a_commands, const org::RunRef& a_run, std::uint32_t a_begun, std::size_t a_declared, std::uint32_t a_first = 0,
			std::uint32_t a_count = ~0u) const;

		/**
		* @brief Render thread. Maps a_slots[i] to query a_first + i. A slot named by several queries answers their sum.
		* A second commit of the same run adds its queries. A staging run's slots read not-ready until the stage submits it.
		* @return Whether a chunk was made, in which case the client's passes declare it from their next build.
		*/
		bool Commit(const org::RunRef& a_run, std::span<const std::int32_t> a_slots, std::uint32_t a_first = 0);
		/** @brief Render thread. The run last committed was submitted. */
		void Executed();

		struct State;
		explicit QueryRing(std::shared_ptr<State> a_state) :
			state(std::move(a_state)) {}

	private:
		std::shared_ptr<State> state;
	};

	std::string Status();
}
