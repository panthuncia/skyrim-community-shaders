#pragma once

// The engine's occlusion queries answered by the render graph (CS_ORG_OCCLUSION_QUERIES=1, default off; docs/development/p5/occlusion-queries.md).
//
// The engine keeps a ring of 64 D3D11 occlusion queries (Renderer::Init, SE 524740 / AE 411344). AcquireOcclusionQuery (75565 / 77366)
// hands out the next entry; BeginQuery (75567 / 77368) and EndQuery (75568 / 77369) bracket the draws it counts; GetQueryData (75569 /
// 77370) reads the count: the polling form returns 0xFFFFFFFF until the result is there, the blocking form spins until it is. Users: the
// sun (Sun::SubmitOcclusionQuery -> SE 99958 / AE 106604, read by 99967 / 106605 for the sun glare) and the decals (SE 35563 / AE
// 36562, read by SE 29262 / AE 30117).
//
// With the switch on, the three functions are detoured. A port opens a Bracket around the engine's call that begins, draws and ends a
// query: the BeginQuery and EndQuery inside it go nowhere, the port draws the bracket's draw on the graph with an ORG occlusion query
// (RasterProgram::DrawDesc's), and its QueryRing resolves the count into a readback buffer. GetQueryData answers from it once that run's
// GPU work is done, and returns not-ready until then, as D3D11 does. A query begun outside any bracket is the engine's on D3D11, as
// before. Nothing about a bracket's query reaches D3D11.
//
// Header free of ORG and Vulkan includes, except BasicRHI's rhi.h for the record-time helpers.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#if defined(CS_HAS_RENDER_GRAPH)
#	include <rhi.h>
#endif

namespace RenderGraph::Engine::OcclusionQueries
{
	/** @brief The engine's ring of occlusion queries. */
	inline constexpr std::uint32_t kRingSlots = 64;

	/** @brief No ring slot. */
	inline constexpr std::int32_t kNoSlot = -1;

	/** @brief Whether CS_ORG_OCCLUSION_QUERIES=1 asks for the graph's occlusion queries (read once). */
	bool Requested();

	/** @brief Whether the detours are installed: a Bracket then takes the engine's query. */
	bool Active();

	/**
	 * @brief Main thread, Engine::InstallGameHooks: the BeginQuery, EndQuery and GetQueryData detours, when Requested(). With
	 * CS_ORG_OCCLUSION_QUERY_LOG=1 alone, only GetQueryData's, which logs what D3D11 answers (the reference for a comparison).
	 */
	void Install();

	/**
	 * @brief A render-thread scope around an engine call that begins a query, draws and ends it (Sun::SubmitOcclusionQuery's draw). The
	 * engine's first BeginQuery inside it takes its ring slot for the graph: that BeginQuery and its EndQuery go nowhere, and GetQueryData
	 * answers the slot from the graph. The port then commits the slot with its run (QueryRing::Commit), or skips it (Skip): a skipped slot
	 * answers the last count it had, at once. At the scope's end a slot that was neither, or whose run was not submitted, is skipped.
	 */
	class Bracket
	{
	public:
		Bracket();
		~Bracket();
		Bracket(const Bracket&) = delete;
		Bracket& operator=(const Bracket&) = delete;

		/** @brief The ring slot the engine began inside the scope, or kNoSlot. */
		std::int32_t Slot() const { return slot; }

		/** @brief The slot's draw is not drawn: it answers its last count (logged once per reason). */
		void Skip(std::string_view a_why);

	private:
		friend struct Hooks;
		Bracket* outer = nullptr;
		std::int32_t slot = kNoSlot;
	};

#if defined(CS_HAS_RENDER_GRAPH)
	/**
	 * @brief The ORG queries of one client's runs: a range of PerRun() occlusion queries per frame slot and a readback buffer of their
	 * counts, made once (the client's setup). Every recording of the client's pass resets its frame slot's range, counts each draw with
	 * one query (Index; RasterProgram::DrawDesc::occlusionPool), and resolves the range (Resolve). The commit names the engine slots its
	 * run counts (Commit); the executed callback marks the run submitted (Executed). A frame slot's counts are read when its next commit
	 * comes (the host has waited for its last work then), or earlier when GetQueryData finds that work done.
	 */
	class QueryRing
	{
	public:
		/** @brief Render thread, with the graph host up. Null, logged, when the pool or the readback cannot be made. */
		static std::shared_ptr<QueryRing> Create(std::string_view a_name, std::uint32_t a_perRun);

		std::uint32_t PerRun() const;

		/** @brief Record time (any thread): the pool. */
		rhi::QueryPoolHandle Pool() const;
		/** @brief Record time: query a_query of a_frameSlot's range. */
		std::uint32_t Index(std::uint32_t a_frameSlot, std::uint32_t a_query) const;
		/** @brief Record time, outside any render pass: a_frameSlot's range reset, before any of its queries is begun. */
		void Reset(rhi::CommandList& a_commands, std::uint32_t a_frameSlot) const;
		/**
		 * @brief Record time, outside any render pass, after the range's draws: each query not in a_begun (a bit per query) begun and
		 * ended with nothing in it, so all are available, then the range resolved into the readback.
		 */
		void Resolve(rhi::CommandList& a_commands, std::uint32_t a_frameSlot, std::uint32_t a_begun) const;

		/**
		 * @brief The client's commit (render thread): the run at a_frameSlot counts engine ring slot a_slots[i] with query i (kNoSlot:
		 * none). Reads the frame slot's last counts first.
		 */
		void Commit(std::uint32_t a_frameSlot, std::span<const std::int32_t> a_slots);
		/** @brief The client's executed callback (render thread): the run last committed was submitted. */
		void Executed();

		struct State;
		explicit QueryRing(std::shared_ptr<State> a_state) :
			state(std::move(a_state)) {}

	private:
		std::shared_ptr<State> state;
	};
#endif

	/** @brief One status line: the answers given by path, the skips and the waits. */
	std::string Status();
}
