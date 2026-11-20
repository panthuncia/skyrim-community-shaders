#pragma once

// What a scene revision is made from and what it holds. Included by Internal.h, in its order.

namespace DCLF::Draws
{
	/** @brief The shadow view slot sizes the revision code asks the frame to make buffers for. The latest request wins. */
	struct ShadowSlotsRequest
	{
		std::uint32_t slots = 0, sequenceDraws = 0, bucketCountWords = 0;
	};

	/**
	 * @brief The frame state a scene revision reads, as the render thread last posted it. Immutable once posted, and the latest wins.
	 * The revision reads only this copy, never the live frame state.
	 */
	struct RevisionInputs
	{
		std::uint64_t generation = 0;
		// Setup can replace the main resources mid-frame. mainGeneration moves when it does.
		std::shared_ptr<Resources> main;
		std::uint64_t mainGeneration = 0;
		std::shared_ptr<ShadowResources> shadow;
		std::shared_ptr<SceneBuffers> scene;
		// epochBuilds is each segment's own build, which is what its recordings are of: its staged build while one is staged, else its
		// attached one. buildGeneration is their sum over DCLF's segments.
		std::uint64_t buildGeneration = 0;
		std::array<std::uint64_t, static_cast<std::size_t>(RenderGraphRuntime::Segment::Count)> epochBuilds{};
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		std::uint32_t targetsGeneration = 0;
		DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
		bool claims = false;       // !IndirectDraws::Failed
		std::uint32_t shadowRasterStates = 0;  // DrawPipelines::ShadowRasterStateCount
		std::uint32_t shadowCandidates = 0;
		std::vector<ShadowViewLayout> occlusionLayouts;
		std::vector<ShadowViewLayout> shadowPlacements;
		// The placements the last shadow epoch drew.
		std::uint64_t shadowActiveViews = 0;
		std::array<bool, 2> mainKnown{};
		std::array<MainViewport, 2> viewport{};
		std::array<FrameBlockSizes, 2> blockSizes{};
		std::array<ViewInputs, kViews> views;
		// Feature scene resource sizes, such as TreeLod and Portals, that the scene tables also size by.
		Extensions::SceneResourceInputs resourceInputs{};
		std::shared_ptr<const ShadowSlotBuffers> shadowSlots;
		bool asyncEpochs = false;
		// Weak so that no posted input keeps a graph's uploader alive.
		std::weak_ptr<org::runtime::IUploadService> uploads;
		// With the CPU pipeline only some epochs have passes, so the revision records only those.
		bool cpuPipeline = false;
		bool forwardDirect = false;
		bool firstPersonForwardDirect = false;
		// A revision is made only once every build of this state is staged or attached. Before that, a recording would be of the old graphs.
		std::shared_ptr<const BuildState> build;
	};

	/** @brief A latch layout's key in the scene's set. A different layout gets a different block. */
	template <class Layout>
	std::uint64_t LatchLayoutKey(const Layout& a_layout)
	{
		static_assert(std::is_trivially_copyable_v<Layout> && sizeof(Layout) % sizeof(std::uint32_t) == 0);
		std::array<std::uint32_t, sizeof(Layout) / sizeof(std::uint32_t)> words;
		std::memcpy(words.data(), &a_layout, sizeof(Layout));
		std::uint64_t key = 0xcbf29ce484222325ull;
		for (const std::uint32_t word : words)
			key = (key ^ word) * 0x100000001b3ull;
		return key;
	}

	/**
	 * @brief The revision producer's latch blocks in the scene's set. A layout change gets a new block at the next seal, so a frame
	 * in flight keeps the block it adopted.
	 */
	struct RevisionLatchHandles
	{
		// mainLatched is per segment, depth then colour.
		LatchHandle main;
		std::array<LatchHandle, 2> mainLatched;
		LatchHandle shadow, shadowZeros, shadowLatched;
		std::array<LatchHandle, kViews> views, viewZeros;
		[[nodiscard]] explicit operator bool() const { return static_cast<bool>(main); }
	};

	/** @brief The revision producer's latch layouts. They only grow, and the blocks themselves belong to the sealed set. */
	struct RevisionLatchLayouts
	{
		MainLatchLayout main;
		bool mainLaidOut = false;
		ShadowLatchLayout shadow;
		bool shadowLaidOut = false;
		std::array<ViewLatchLayout, kViews> views;
		std::array<bool, kViews> viewsLaidOut{};
	};

	/** @brief The shapes the revision producer made last. */
	struct MadeShapes
	{
		std::array<std::shared_ptr<const PassFrame>, 2> main;  // per segment
		// The shadow epoch's base shape first, then one variant per set of active views.
		std::vector<std::shared_ptr<const ShadowFrame>> shadow;
		std::array<std::shared_ptr<const ViewFrame>, kViews> views;
	};

	/**
	 * @brief Cached buckets per shadow map row and rasterizer state. Valid while the lookups and the shadow pipeline set are
	 * unchanged.
	 */
	struct RowBucketsCache
	{
		std::uint64_t lookups = 0;
		std::uint32_t generation = 0;
		std::shared_ptr<const void> version;
		std::array<std::vector<std::optional<RowBuckets>>, kShadowModeCount> rows;  // by mode, then state
	};

	/** @brief Host data for one epoch's recording: the revision's versions and the epoch's shape. */
	struct EpochRevisionData final : org::IHostExecutionData
	{
		std::shared_ptr<const NamedVersions> versions;
		RevisionShapes shapes;
		const void* TryGet(std::type_index a_type) const noexcept override
		{
			if (a_type == typeid(RevisionShapes))
				return &shapes;
			if (a_type == typeid(NamedVersions))
				return versions.get();
			if (a_type == typeid(org::IResourceVersions))
				return static_cast<const org::IResourceVersions*>(versions.get());
			return nullptr;
		}
	};

	/** @brief The shadow epoch's shapes in a revision, one recording each. */
	struct ShadowVariants
	{
		std::vector<std::shared_ptr<const ShadowFrame>> shapes;
	};
	/**
	 * @brief An epoch's recordings for a revision, one per own variant times one per subset of absent hosted epochs.
	 * Index 0 has every hosted epoch present. The depth epoch hosting two views has 2 x 4 = 8 recordings.
	 */
	struct RevisionRecordings
	{
		std::vector<std::shared_ptr<const org::GraphHost::EpochRecording>> recordings;
		std::uint32_t hosted = 0;
		/** @brief The number of the host's own variants. */
		std::size_t Variants() const { return recordings.size() >> std::popcount(hosted); }
		/** @brief Whether the recordings include hosted epoch a_epoch's passes. */
		bool Carries(std::uint32_t a_epoch) const { return a_epoch < 32 && ((hosted >> a_epoch) & 1); }
		/** @brief The recording of own variant a_variant where only the hosted epochs in a_drawn record work. */
		std::size_t IndexOf(std::size_t a_variant, std::uint32_t a_drawn) const
		{
			std::size_t absent = 0, dimension = 0;
			for (std::uint32_t rest = hosted; rest; rest &= rest - 1, ++dimension)
				if (!(a_drawn & rest & (0u - rest)))
					absent |= std::size_t(1) << dimension;
			return a_variant + Variants() * absent;
		}
	};
	/**
	 * @brief An epoch's part of the selected revision. A commit trusts the shape and recording, because coverage was decided before
	 * the engine drew.
	 */
	struct EpochRevision
	{
		std::shared_ptr<const org::async::RevisionFragment> shape;
		std::shared_ptr<const RevisionRecordings> recordings;
		// With the CPU pipeline nothing is recorded and the direct sites draw from the shape.
		bool direct = false;
		explicit operator bool() const { return shape && (direct || (recordings && !recordings->recordings.empty())); }
	};
}
