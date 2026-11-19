#pragma once

// Core's side of Api/FrameValues.h pools: what a pool's frame start, kick and producer get. Each object is valid for that call only.

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace DCLF
{
	struct FrameGlobals;
}

namespace org::services
{
	class BackingLease;
}

namespace DCLF::Api
{
	/** @brief What a pool's frame start gets. globals is null when none were captured. */
	class FrameValuePoolFrameStart
	{
	public:
		const std::shared_ptr<const FrameGlobals>& globals;
	};

	/** @brief What a pool's kick gets. */
	class FrameValuePoolKick
	{
	public:
		std::uint64_t seq = 0;                     // the frame's producer
		const FrameGlobals* globals = nullptr;     // null when none were captured
		std::uint64_t objectSlots = 0;             // object slots the frame's shading rows hold
	};

	/** @brief What a pool's producer gets during sample and send, and the way its rows reach the frame's backing. */
	class FrameValuePoolProducer
	{
	public:
		/** @brief Receives a row journal's runs since a_contentEpoch as (data, bytes, offset). */
		struct RowSink
		{
			void* owner = nullptr;
			void (*put)(void* a_owner, const void* a_data, std::size_t a_bytes, std::size_t a_offset) = nullptr;
			void operator()(const void* a_data, std::size_t a_bytes, std::size_t a_offset) const { put(owner, a_data, a_bytes, a_offset); }
		};
		/** @brief The backing a send fills. The sink's data must stay put until the send queues it. */
		struct SendTarget
		{
			std::uint64_t address = 0;
			std::uint64_t capacityUnits = 0;
			const org::services::BackingLease* lease = nullptr;
		};
		using EmitRowsFn = void (*)(const void* a_view, std::uint64_t a_contentEpoch, const SendTarget& a_target, const RowSink& a_sink);
		using SendRowsFn = void (*)(void* a_owner, std::size_t a_pool, std::size_t a_count, std::uint64_t a_contentEpoch, const void* a_view, EmitRowsFn a_emit);

		std::uint64_t seq = 0;                       // the frame's producer
		bool parity = false;                         // a parity frame, so the pool checks what it sends
		const FrameGlobals* globals = nullptr;       // null when none were captured
		const std::shared_ptr<const void>* input = nullptr;  // what the pool's kick handed over this frame

		/** @brief The texture-transform buffer index the frame's shaders read, as captured after Main::Update's flip. 0 without globals. */
		std::uint32_t TextureTransformBuffer() const;
		/** @brief The kick's input as the pool's type, or null. */
		template <class T>
		const T* Input() const
		{
			return static_cast<const T*>(input ? input->get() : nullptr);
		}
		/**
		 * @brief Whether this producer's kick is still the newest. Engine pointers held by its publication may be read under a lease only while it is.
		 * The publication is released no earlier than the next frame's start.
		 */
		bool Newest() const { return kickedSeq && kickedSeq->load(std::memory_order_acquire) == seq; }
		/** @brief Sample. Opens a_rows' build at the job's floor. Changes are forgotten when no backing holds content. */
		template <class Row>
		void BeginBuild(KeptWholeArray<Row>& a_rows) const
		{
			a_rows.BeginBuild(oldestContentEpoch ? *oldestContentEpoch : a_rows.ContentEpoch());
		}
		/** @brief Send. Writes the runs the backing lacks, straight from a_rows. Throws on failure, which fails the frame's values. */
		template <class Row>
		void Send(const KeptWholeArray<Row>& a_rows) const
		{
			send(owner, pool, a_rows.Size(), a_rows.ContentEpoch(), &a_rows, [](const void* a_view, std::uint64_t a_contentEpoch, const SendTarget&, const RowSink& a_sink) {
				static_cast<const KeptWholeArray<Row>*>(a_view)->Emit(a_contentEpoch, a_sink);
			});
		}
		/** @brief Sample. Counts a_rows rows seeded from frame-start engine reads. */
		void NoteSeeded(std::uint64_t a_rows) const
		{
			if (seeded && a_rows)
				seeded->fetch_add(a_rows);
		}

		// Set by FrameValues.
		std::optional<std::uint64_t> oldestContentEpoch;
		const std::atomic<std::uint64_t>* kickedSeq = nullptr;
		SendRowsFn send = nullptr;
		void* owner = nullptr;
		std::size_t pool = 0;
		std::atomic<std::uint64_t>* seeded = nullptr;
	};
}

namespace DCLF::Extensions
{
	/** @brief Render thread, from one kick to the next. The descriptor of a_pool's frame backing, or 0 without a producer. */
	std::uint32_t FrameValuePoolIndex(Api::FrameValuePoolId a_pool);
}
