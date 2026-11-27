#pragma once

// The client lifecycle every DCLF engine draw family shares.
// A family keeps one Family::Client: its client states and their declarations, the owners its runs keep, and its counts.
// A family's own client state derives from Family::ClientState. Its passes, commit, batch checks and programs stay the family's.
// Every client is declared through RenderGraph::Frame::DeclareClient, so it registers with a frame revision, never mid-frame.
// Render thread unless a function says otherwise.

#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/FrameAnchors.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace DCLF::Family
{
	/** @brief What every family client hands from the render thread to the graph. A family's client state derives from it. */
	template <class Run>
	struct ClientState
	{
		std::string id;
		FrameAnchors::Anchor anchor = FrameAnchors::Anchor::EngineDraws;
		// Written by the render thread, read by the host's thread through Current().
		std::shared_ptr<const Run> run;
		// True while the client's anchor runs for its withheld batch. The registration's active reads it.
		bool pending = false;
		std::atomic<std::uint64_t> executed{ 0 };  // runs submitted with this client's passes

		/** @brief Any thread. The run the next commit takes. */
		std::shared_ptr<const Run> Current() const { return std::atomic_load(&run); }

		/** @brief The host's thread, from the registration's executed. Counts a run. True for the first, which the family logs. */
		bool CountExecuted() { return executed.fetch_add(1, std::memory_order_relaxed) == 0; }
	};

	/** @brief The key of a client whose registration depends on nothing that changes: its targets are imported once at setup. */
	struct NoKey
	{
		bool operator==(const NoKey&) const = default;
	};

	namespace Detail
	{
		/** @brief Whether the render graph runs. */
		bool GraphActive();
		/** @brief Whether the graph has a host and this thread owns rendering. */
		bool CanSetUp();
		/** @brief Logs a client's declaration, or that it could not be declared and why that leaves the draws to the engine. */
		void LogDeclared(std::string_view a_family, std::string_view a_id, bool a_declared, std::string_view a_refusal);
	}

	/**
	 * @brief One family's clients and the state they share. Run needs draws, owners, revision and Hash(), the revision of what its
	 * recording depends on. State derives from ClientState<Run>. kCount is the number of client slots, one per view.
	 */
	template <class Run, class State, std::size_t kCount = 1>
	class Client
	{
		static_assert(std::is_base_of_v<ClientState<Run>, State>, "a family's client state derives from Family::ClientState");

	public:
		/** @brief a_family names the family in logs and must outlive it (a literal). */
		explicit Client(std::string_view a_family) :
			family(a_family) {}

		/** @brief Whether the render graph runs. Every anchor is a stage's, so a running graph runs them all. */
		bool Available() const { return Detail::GraphActive(); }

		/**
		 * @brief Main thread, at the start of the family's Setup. Empty when setup begins now, which happens once.
		 * Otherwise Setup's result: true once begun, false while there is no host or render owner or the graph is not running.
		 */
		std::optional<bool> BeginSetup()
		{
			if (setUp)
				return true;
			if (!Detail::CanSetUp() || !Available())
				return false;
			setUp = true;
			return std::nullopt;
		}

		/** @brief Whether setup has begun. */
		bool SetUp() const { return setUp; }

		/**
		 * @brief Main thread, at setup. Declares a_client at slot a_index through DeclareClient and keeps both. The client registers
		 * with the next frame revision. a_refusal ends the log line when it cannot be declared ("the sky stays the engine's").
		 * False, logged, when it was not declared.
		 */
		template <class Key>
		bool Declare(std::size_t a_index, std::shared_ptr<State> a_client, ::RenderGraph::Frame::Dependencies a_dependencies,
			std::function<std::optional<Key>(const ::RenderGraph::Frame::Inputs&)> a_key, std::function<bool(const Key&, FrameAnchors::ClientDesc&)> a_make,
			std::string_view a_refusal)
		{
			if (a_index >= kCount || !a_client)
				return false;
			auto declared = ::RenderGraph::Frame::DeclareClient<Key>(a_client->id, std::move(a_dependencies), std::move(a_key), std::move(a_make));
			Detail::LogDeclared(family, a_client->id, static_cast<bool>(declared), a_refusal);
			if (!declared)
				return false;
			declarations[a_index] = std::move(declared);
			clients[a_index] = std::move(a_client);
			return true;
		}

		/** @brief The client at a_index, or null when it was not declared. */
		const std::shared_ptr<State>& At(std::size_t a_index) const { return a_index < kCount ? clients[a_index] : None(); }

		/**
		 * @brief The client at a_index once a revision registered it. Until then null, and the engine draws the batch. Registration
		 * happens only at a revision's adoption, so this engine state switches only there.
		 */
		const std::shared_ptr<State>& Registered(std::size_t a_index) const
		{
			return a_index < kCount && clients[a_index] && declarations[a_index].Registered() ? clients[a_index] : None();
		}

		/** @brief Whether the graph draws a_client's batch now, at its anchor in segment a_segment (HeldForBatch). */
		bool Held(const State& a_client, std::uint32_t a_segment) const { return Family::HeldForBatch(family, a_client.id, a_segment, a_client.anchor); }

		/** @brief Registered and Held for the client at a_index. False means the engine draws the batch. */
		bool CanTake(std::size_t a_index, std::uint32_t a_segment) const
		{
			const auto& client = Registered(a_index);
			return client && Held(*client, a_segment);
		}

		/** @brief Keeps a batch's imports and leases as long as a run would, when it ran nothing. */
		void Keep(Owners&& a_owners) { kept.Keep(std::move(a_owners)); }

		/**
		 * @brief Hands a_run to the client at a_index and runs its anchor for the withheld batch. Sets the run's revision from Hash() and
		 * keeps its owners. False when there is no run or client, or the run did not submit (logged).
		 */
		bool Submit(std::size_t a_index, std::shared_ptr<Run> a_run)
		{
			if (!a_run)
				return false;
			const auto& client = At(a_index);
			if (!client)
				return false;
			a_run->revision = a_run->Hash();
			kept.Keep(std::move(a_run->owners));
			++runs;
			draws += a_run->draws.size();
			std::atomic_store(&client->run, std::shared_ptr<const Run>(std::move(a_run)));
			return Family::RunWithheld(family, client->id, client->anchor, client->pending);
		}

		/** @brief The runs submitted. */
		std::uint64_t Runs() const { return runs; }

		/** @brief The draws of the runs submitted. */
		std::uint64_t Draws() const { return draws; }

		/** @brief Any thread. The runs every client's passes executed. */
		std::uint64_t Executed() const
		{
			std::uint64_t executed = 0;
			for (const auto& client : clients)
				if (client)
					executed += client->executed.load(std::memory_order_relaxed);
			return executed;
		}

	private:
		static const std::shared_ptr<State>& None()
		{
			static const std::shared_ptr<State> none;
			return none;
		}

		std::string_view family;
		bool setUp = false;
		std::array<std::shared_ptr<State>, kCount> clients{};
		std::array<::RenderGraph::Frame::DeclaredClient, kCount> declarations{};
		std::uint64_t runs = 0, draws = 0;
		KeptOwners kept;
	};
}
