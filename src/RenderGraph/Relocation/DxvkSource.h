#pragma once

// Guest mode: DXVK's moves of the pinless imports, handed to ORG's relocation service.
// See docs/development/plans/org-relocation.md, section 4.

#include <Resources/Relocation.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

#include "RenderGraph/DxvkOrgInterop.h"

namespace org
{
	class ExternalTextureResource;
}

namespace RenderGraph::Relocation
{
	/**
	 * @brief The relocation source for DXVK's moves of tracked imports (dxvkTrackInteropResources), in guest mode.
	 *
	 * DXVK chooses the moves and copies the data in its stream. Each move hands back the old storage, which stays valid until it is
	 * released. ORG's service polls this source once per commit (the frame boundary, at CS's Present).
	 *
	 * Whoever imports a tracked resource registers the import by its cookie and the serial its description named:
	 * - Hold: the importer keeps raw handles or views of its own. The registration keeps the old storage of every later move until
	 *   the registration goes, and tells the importer once, so it imports again. Drawcall Limit Fix uses this.
	 * - Follow: an ExternalTextureResource that ORG moves to the new storage at the commit. Its identity stays; its descriptors rotate
	 *   and the journal names it. The old storage is released once the GPU is done with the old backing.
	 *
	 * A move can arrive between an import's description and its registration. So an importer begins a ticket before it describes,
	 * and keeps it until it registered. A move taken while a ticket that began before it is alive is kept, and a registration with an
	 * older serial takes it.
	 *
	 * Ordering of the first read of a followed resource after its move: the poll runs on the thread that presents, after the frame's
	 * epochs. Graph work on the graphics queue goes into DXVK's stream after the poll, so it runs after DXVK's copy. The compute and
	 * copy queues wait at their first submission of each epoch for a new point in DXVK's stream, enqueued then, so they come after the
	 * copy too. A follow move is only committed while neither queue is inside an epoch (RenderGraphRuntime::OffStreamQueuesIdle);
	 * otherwise it waits for the next commit. The relocations carry no ready point: ORG would wait for it with direct submissions on
	 * every queue, and on the compute and copy queues that would bypass that entry order.
	 */
	class DxvkSource final : public org::relocation::IRelocationSource
	{
	public:
		/** @brief An import in flight, from before its description until its registration. Move-only. Empty without the source. */
		class Ticket
		{
		public:
			Ticket() = default;
			Ticket(Ticket&& a_other) noexcept;
			Ticket& operator=(Ticket&& a_other) noexcept;
			Ticket(const Ticket&) = delete;
			Ticket& operator=(const Ticket&) = delete;
			~Ticket();

		private:
			friend class DxvkSource;
			void Reset() noexcept;
			std::shared_ptr<DxvkSource> source;
			std::uint64_t id = 0;
		};

		/** @brief One registered import. Its owner keeps it as long as the import lives, and drops it after ending the import. */
		class Registration : public std::enable_shared_from_this<Registration>
		{
		public:
			~Registration();
			Registration(const Registration&) = delete;
			Registration& operator=(const Registration&) = delete;

			/** @brief Any thread. A move past the import's description was taken: a Hold import reads an old storage now. */
			bool Superseded() const noexcept { return superseded.load(std::memory_order_acquire); }

		private:
			friend class DxvkSource;
			Registration(std::shared_ptr<DxvkSource> a_source, std::uint64_t a_cookie, std::uint64_t a_serial);

			struct Move
			{
				std::uint64_t serial = 0;
				DxvkOrgInteropResourceInfo next{};
				std::vector<std::shared_ptr<const void>> storages;
			};

			std::shared_ptr<DxvkSource> source;
			std::uint64_t cookie = 0;
			std::atomic<bool> superseded{ false };
			// Under the source's lock from here on.
			std::uint64_t serial = 0;  // the storage the import reads now
			std::function<void()> moved;  // Hold
			std::vector<std::shared_ptr<const void>> storages;  // old storages the import may still read
			std::weak_ptr<org::ExternalTextureResource> resource;  // Follow
			bool follows = false;
			bool cannotFollow = false;  // a move it could not follow: it stays on its storage
			std::optional<Move> move;  // Follow: the newest move not committed yet
			bool queued = false;  // in the source's follow list
			std::optional<rhi::ResourceHandle> adopting;  // Follow: the handle the last committed move gave it
		};

		/** @brief Totals since the start. Any thread. */
		struct Stats
		{
			std::uint64_t images = 0, buffers = 0;  // moves taken
			std::int64_t storagesHeld = 0;          // old storages not released yet
			std::uint64_t held = 0;                 // Hold imports told of a move
			std::uint64_t followed = 0;             // follow moves handed to ORG
			std::uint64_t followFailed = 0;         // of those, moves ORG did not adopt
			std::uint64_t cannotFollow = 0;         // follow moves kept on the old storage (another layout, a failed import)
			std::uint64_t unclaimed = 0;            // moves kept for imports in flight, now
		};

		/** @brief Guest mode with pinless imports, when the runtime starts: makes the source and adds it to ORG's service. */
		static void Install();
		/** @brief At the runtime's shutdown: removes it from ORG's service. Registrations still alive keep it until they go. */
		static void Uninstall();
		/** @brief Any thread. The installed source, or null. */
		static std::shared_ptr<DxvkSource> Get();
		static Stats GetStats();

		/** @brief Any thread. Begins an import, before its description. Empty without the source. */
		static Ticket Begin();
		/**
		 * @brief Any thread. Registers an import that keeps raw handles or views: the registration keeps the old storage of every move
		 * past a_serial, and a_moved runs once, on the committing thread, at the first such move. When Superseded() is already true
		 * on return, a_moved is not run: the caller handles it. Null without the source.
		 */
		static std::shared_ptr<Registration> Hold(std::uint64_t a_cookie, std::uint64_t a_serial, std::function<void()> a_moved);
		/**
		 * @brief Any thread. Registers a texture ORG moves itself. a_resource is an ExternalTextureResource made over ImportDxvkImage
		 * of the description that named a_serial, not registered in ImageRegistry (its image changes). Null without the source.
		 */
		static std::shared_ptr<Registration> Follow(std::uint64_t a_cookie, std::uint64_t a_serial, std::shared_ptr<org::ExternalTextureResource> a_resource);

		size_t Poll(std::span<org::relocation::Relocation> a_out) override;

		~DxvkSource() override;

	private:
		DxvkSource() = default;
		struct Unclaimed
		{
			std::uint64_t serial = 0;
			std::uint64_t mark = 0;  // tickets up to this began before the move was taken
			DxvkOrgInteropResourceInfo next{};
			std::shared_ptr<const void> storage;
		};

		/** @brief Takes DXVK's moves and routes them. Fills ready with the follow moves to commit. */
		void Collect();
		/** @brief Under the lock. Hands a move to a follow registration. */
		void AddMove(Registration& a_registration, std::uint64_t a_serial, const DxvkOrgInteropResourceInfo& a_next, std::shared_ptr<const void> a_storage);
		/** @brief Under the lock. The relocation of a follow registration's pending move, or none when it cannot be followed. */
		std::optional<org::relocation::Relocation> MakeRelocation(Registration& a_registration);
		void Forget(Registration& a_registration);
		void EndTicket(std::uint64_t a_id);

		std::mutex mutex;
		std::unordered_map<std::uint64_t, std::vector<Registration*>> byCookie;
		std::unordered_map<std::uint64_t, std::vector<Unclaimed>> unclaimed;
		std::set<std::uint64_t> tickets;
		std::uint64_t nextTicket = 0;
		std::vector<std::weak_ptr<Registration>> follows;  // with a pending move
		std::vector<std::weak_ptr<Registration>> adopting;  // follow moves of the last commit
		std::vector<org::relocation::Relocation> ready;  // the committing thread
		bool collected = false;  // this commit polled DXVK
	};
}
