#pragma once

#include "RenderGraph/BufferRef.h"

// The scene payload is seven parts. Each part is a chain of GPU-built buffer versions.
// For each part that changed, the snapshot builder uploads the journal's new runs and GPU-copies the rest from the
// last version on the streaming copy queue. The snapshot is posted only once every version is GPU ready,
// so the frame that installs it has nothing to catch up.

#include "Features/DrawcallLimitFix/Core/Extensions/PayloadProducts.h"

#include <array>
#include <memory>
#include <optional>

namespace org::runtime
{
	class IUploadService;
}

namespace org::services
{
	struct Backing;
	struct PublishedBufferVersion;
}

namespace org
{
	class RenderPassBuilder;
}

namespace DCLF::Draws
{
	/** @brief The payload's buffers, one version chain each. */
	enum class PayloadPart : std::uint8_t
	{
		Objects,       // BindlessObject per object (the stream views' objects)
		Extras,        // the extras rows (16-byte rows)
		Geometries,    // GeometryDraw per geometry slot (the newest main payload's geometryDraws, else the shadow payload's geometries)
		MaterialRows,  // MaterialRow per main material row; no addresses (the draws' sequences carry their blocks')
		PipelineRows,  // PipelineRow per main pipeline row; likewise no addresses
		Inputs,        // DrawInput: the scene list (region, then the main frame inputs, then the shadow inputs)
		ShadowRows,    // ShadowMaterialRow per shadow material row; rows hold their own address
		Count
	};
	inline constexpr std::size_t kPayloadParts = static_cast<std::size_t>(PayloadPart::Count);

	/** @brief One part's buffer as the epochs read it, by value. */
	struct PayloadBuffer
	{
		std::uint32_t srvIndex = 0;  // its SRV slot (0: the part is absent)
		std::uint64_t address = 0;   // its device address (the row parts; 0 for the others)
		RenderGraph::BufferRef ref;  // its buffer view ref (the row parts: shaders read rows through it; null for the others)
		std::uint64_t units = 0;     // the elements this payload holds (its rows: BindlessObject, extras row, GeometryDraw, DrawInput...)
	};

	/** @brief Where each part's rows come from for one publication. Immutable. Each version holds it until its upload completes. */
	struct PayloadSource
	{
		std::shared_ptr<const void> keep;  // the publication's draws (DrawPublication), which the pointers below are into
		const StreamViews* views = nullptr;
		const GeometryDrawsOut* geometries = nullptr;  // the main payload's geometryDraws, else the shadow payload's geometries
		const MainPayload* main = nullptr;             // NewestMainPayload (null: none)
		const ShadowPayload* shadow = nullptr;         // null: none
		const KeptView<DrawInput>* region = nullptr;   // the scene list's head: the main payload's resident, else the shadow payload's regionInputs
		const std::vector<DrawInput>* mainList = nullptr;
		const std::vector<DrawInput>* shadowList = nullptr;
		std::array<std::uint64_t, kPayloadParts> units{};  // each part's elements (PayloadBuffer::units)
		// The scene list layout in DrawInput units. The region is at the head, then main inputs, then shadow inputs.
		std::uint64_t regionCount = 0, mainInputsAt = 0, shadowInputsAt = 0;
		Extensions::PayloadProducts products;
		std::uint64_t serial = 0;  // process-unique per source
	};

	/** @brief A snapshot's payload. Every present part's GPU-ready version. Unchanged parts keep their last version. */
	struct ScenePayload
	{
		std::array<std::shared_ptr<const org::services::PublishedBufferVersion>, kPayloadParts> versions;  // null: the part is absent
		std::array<std::uint64_t, kPayloadParts> units{};  // the source's (PayloadBuffer::units)
		std::uint64_t regionCount = 0, mainInputsAt = 0, shadowInputsAt = 0;
		bool main = false;    // the main parts (material and pipeline rows, the main inputs) are present
		bool shadow = false;  // the shadow parts (shadow rows, the shadow inputs) are present
		Extensions::PayloadProducts products;  // the source's
		std::uint64_t serial = 0;              // the source's
	};

	/** @brief The frame's payload. Render thread. The epochs read it by value. */
	struct PayloadVersions
	{
		std::array<PayloadBuffer, kPayloadParts> parts{};
		std::uint64_t regionCount = 0, mainInputsAt = 0, shadowInputsAt = 0;
		bool main = false;    // the main parts (material and pipeline rows, the main inputs) are present
		bool shadow = false;  // the shadow parts (shadow rows, the shadow inputs) are present
		std::uint64_t serial = 0;  // the source's

		[[nodiscard]] const PayloadBuffer& operator[](PayloadPart a_part) const { return parts[static_cast<std::size_t>(a_part)]; }
	};

	/**
	 * @brief Version chains for the payload parts. Each part has its own backing pool, and a build waits while none is free.
	 * The journals keep changes back to the oldest position any backing of the part holds.
	 */
	namespace PayloadPools
	{
		/** @brief Snapshot builder. Makes a publication's source. */
		std::shared_ptr<const PayloadSource> MakeSource(std::shared_ptr<const void> a_keep, const StreamViews& a_views, const MainPayload* a_main,
			const ShadowPayload* a_shadow);
		/** @brief Snapshot builder. Takes finished builds into the chains. True when none is in flight. Nothing is posted while false. */
		bool Settle();
		/** @brief Any thread, read racily for stall messages. The parts with a version in flight, or "none". */
		std::string PendingParts();
		/** @brief Snapshot builder, with Settle true. Posts a new version of each changed part. False when nothing could be posted. */
		bool Post(const std::shared_ptr<const PayloadSource>& a_source, const std::shared_ptr<org::runtime::IUploadService>& a_uploads);
		/** @brief Snapshot builder, once Settle is true after Post. Null when a part failed. */
		std::shared_ptr<const ScenePayload> Finish(const PayloadSource& a_source);
		/** @brief Render thread, at adoption. Null leaves the parts empty. */
		void Install(std::shared_ptr<const ScenePayload> a_payload);
		/**
		 * @brief Render thread. Fills the frame's payload from the installed versions. False without one.
		 * Each version becomes the current version of its late-bound buffer, so its refs belong to declared buffers.
		 */
		bool MakeFramePayload(PayloadVersions& a_out);
		/** @brief Graph build. Declares each part's late-bound buffer, so changing versions needs no graph rebuild. */
		void DeclareReads(org::RenderPassBuilder& a_builder);
		/** @brief Render thread. The installed backings are not reused before a_point. Also covers the backing an adoption replaces. */
		void BindInstalled(std::uint64_t a_point);
		/** @brief Render thread. Needs an uploader with a dedicated streaming queue. */
		bool Available();
		/** @brief Any thread. The oldest journal position any backing of a_part holds. nullopt means everything may be trimmed. */
		std::optional<std::uint64_t> OldestContentEpoch(PayloadPart a_part);

		/** @brief One part's report since the last take, then the pool's census. */
		struct PartReport
		{
			std::uint64_t built = 0, uploadedBytes = 0, copiedBytes = 0, wholeCopies = 0, suspensions = 0, failures = 0;
			std::uint64_t liveBackings = 0, liveBytes = 0;
			std::optional<std::uint64_t> floor;
		};
		std::array<PartReport, kPayloadParts> TakeReport();
		inline constexpr std::array<const char*, kPayloadParts> kPartNames{ "objects", "extras", "geometries", "material rows", "pipeline rows", "inputs",
			"shadow rows" };
	}

	struct PayloadFloors
	{
		[[nodiscard]] std::optional<std::uint64_t> OldestContentEpoch(PayloadPart a_part) const { return PayloadPools::OldestContentEpoch(a_part); }
	};
}
