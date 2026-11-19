#pragma once

// The shadow payload. The shadow build makes it on DCLF's thread pool and the shadow epoch commits it on the render thread.
// Also the inputs the shadow shapes are made from. Internal.h includes this file in a fixed order.

namespace DCLF::Draws
{
	/**
	 * @brief One 256-byte row of the shadow material table. It is the Utility shader's PerMaterial block (b1) followed by the
	 * diffuse descriptor index. Row 0 serves every caster without alpha testing.
	 */
	struct ShadowMaterialRow
	{
		std::array<float, 4> texcoord{};  // offset xy, scale zw
		std::uint32_t diffuse = 0;
		std::array<std::uint32_t, 59> unused{};

		bool operator==(const ShadowMaterialRow& o) const { return texcoord == o.texcoord && diffuse == o.diffuse; }
	};
	static_assert(sizeof(ShadowMaterialRow) == kConstantAlignment && offsetof(ShadowMaterialRow, diffuse) == kShadowRowDiffuseOffset);

	/**
	 * @brief A shadow mode's rasterizer states, sorted, split into ordinary casters and volumetric-only casters.
	 * A caster becomes an input only when its pipeline is ready under every state for its class.
	 */
	struct ModeRasterStates
	{
		std::vector<std::uint32_t> casters, volumetric;

		void Add(std::uint32_t a_state, bool a_volumetric)
		{
			auto& ids = a_volumetric ? volumetric : casters;
			if (const auto it = std::lower_bound(ids.begin(), ids.end(), a_state); it == ids.end() || *it != a_state)
				ids.insert(it, a_state);
		}
		const std::vector<std::uint32_t>& Of(bool a_volumetric) const { return a_volumetric ? volumetric : casters; }
		bool Empty() const { return casters.empty() && volumetric.empty(); }
		/** @brief Every state of either class, once each. */
		std::vector<std::uint32_t> All() const
		{
			std::vector<std::uint32_t> all;
			std::set_union(casters.begin(), casters.end(), volumetric.begin(), volumetric.end(), std::back_inserter(all));
			return all;
		}
		bool operator==(const ModeRasterStates&) const = default;
	};

	struct ShadowInputs
	{
		std::uint32_t frameNumber = 0;
		std::array<bool, kShadowModeCount> modeUsed{};
		std::array<ModeRasterStates, kShadowModeCount> modeRasterStates{};
		// The sun's full-frustum processes as the engine's full-frustum cull (FUN_141511f30) just used them.
		// An object whose entry is outside all of them is never a candidate for the sun's cascade culls.
		std::vector<SunEntryProcess> sunEntryProcesses;
		// Sun entries DCLF can remove from the cascade culls. The build turns them into next frame's exclusion.
		std::shared_ptr<const EntrySpace> sunCandidates;
		// The same for point-light culls.
		std::shared_ptr<const EntrySpace> lightCandidates;
		ResourceAddresses addresses{};
		// Community Shaders' SharedData (b5) and FeatureData (b6).
		std::vector<std::byte> sharedData, featureData;
		std::uint32_t lookupGeneration = 0, tablesGeneration = 0;
		TablesContentEpochs tablesOldestContentEpochs;
		// How far back the shadow journals must keep changes. 0 means send everything whole.
		std::uint64_t inputsOldestContentEpoch = 0;
		std::uint64_t materialRowsOldestContentEpoch = 0;
	};

	struct ShadowPayload
	{
		ShadowInputs inputs;
		std::vector<std::shared_ptr<const void>> bindingOwners;
		std::shared_ptr<const StreamViews> streams;
		ConstantArena arena;
		DrawBindings frameRecord{};  // every texture and sampler except the diffuse
		// Unique per build. A commit stages the arena only into a buffer that does not already hold this serial.
		std::uint64_t arenaSerial = 0;
		KeptView<ShadowMaterialRow> materialRows;
		// The most draws each mode's inputs can produce. Skinned inputs draw once per partition.
		std::array<std::uint32_t, kShadowModeCount> modeDraws{};
		// The same per key slot. This sizes each view's buckets.
		std::array<std::vector<std::uint32_t>, kShadowModeCount> keySlotDraws;
		std::vector<std::uint32_t> objectRecord;  // material row per object, or ~0u when it cannot draw
		// One shadow list for every mode. The kept region comes first, then the per-frame inputs such as face shapes.
		bool kept = false;
		KeptView<DrawInput> regionInputs;
		std::vector<DrawInput> inputList;
		std::array<std::uint32_t, kShadowModeCount> modeBits{};
		std::array<std::uint32_t, kShadowModeCount> modeInputs{};
		// The build at which an object last joined or left each mode. 0 without the kept state.
		std::array<std::uint64_t, kShadowModeCount> membership{};
		// Objects that joined or left each mode in this build, so the exclusions need not reread the region.
		// movesValid is false for a mode that was rebuilt whole, and then its inputs must be read whole.
		std::uint64_t keptBuild = 0;
		std::array<std::vector<std::uint32_t>, kShadowModeCount> joined, left;
		std::array<bool, kShadowModeCount> movesValid{};
		// The main payloads' per-frame tail sits between the region and the shadow inputs in one buffer.
		// Shadow views scan all of it and their view bits pick their own.
		std::size_t mainTail = 0;
		std::size_t RegionCount() const { return regionInputs.Count(); }
		/** @brief How many inputs every view's dispatch scans. */
		std::size_t Inputs() const { return RegionCount() + mainTail + inputList.size(); }
		std::size_t ModeInputs(std::uint32_t a_mode) const { return modeInputs[a_mode]; }
		template <class F>
		void ForEachInput(F&& a_visit) const
		{
			if (regionInputs.elements)
				for (const auto& input : *regionInputs.elements)
					a_visit(input);
			for (const auto& input : inputList)
				a_visit(input);
		}
		/** @brief Visits the inputs whose mask has this mode's view bits. */
		template <class F>
		void ForEachInput(std::uint32_t a_mode, F&& a_visit) const
		{
			const std::uint32_t bits = modeBits[a_mode];
			ForEachInput([&](const DrawInput& a_input) {
				if (a_input.view.mask & bits)
					a_visit(a_input);
			});
		}
		/** @brief The mode's inputs copied into one list, for diagnostics. */
		std::vector<DrawInput> Flat(std::uint32_t a_mode) const
		{
			std::vector<DrawInput> out;
			out.reserve(ModeInputs(a_mode));
			ForEachInput(a_mode, [&](const DrawInput& a_input) { out.push_back(a_input); });
			return out;
		}
		GeometryDrawsOut geometries;
		// A view into the published tables, which outlive the payload.
		std::span<const Api::VertexStreamRow> streamRows;
		std::uint32_t skippedTexture = 0, skippedPipeline = 0, deferredTextures = 0, deferredPipelines = 0;
		// Set members a mode could not draw. This is a defect, because the engine withheld them.
		std::uint32_t setWaiting = 0;
		std::string setWaitingFirst;  // the first one, and why
		// Next frame's exclusions, built ahead and published by the epoch. Opaque here, owned by the exclusion contribution.
		std::shared_ptr<void> sunExclusion;
		std::shared_ptr<void> parabolicExclusion;

		void Reset()
		{
			setWaiting = 0;
			setWaitingFirst.clear();
			sunExclusion.reset();
			parabolicExclusion.reset();
			arena.Reset(kShadowConstantBytes);
			frameRecord = {};
			static std::atomic<std::uint64_t> arenaSerials{ 0 };
			arenaSerial = arenaSerials.fetch_add(1, std::memory_order_relaxed) + 1;
			materialRows.Reset();
			modeDraws = {};
			for (auto& slots : keySlotDraws)
				slots.clear();
			bindingOwners.clear();
			objectRecord.clear();
			inputList.clear();
			kept = false;
			regionInputs = {};
			mainTail = 0;
			modeBits = {};
			modeInputs = {};
			membership = {};
			keptBuild = 0;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				joined[m].clear();
				left[m].clear();
			}
			movesValid = {};
			geometries.Reset();
			streams.reset();
			streamRows = {};
			skippedTexture = skippedPipeline = deferredTextures = deferredPipelines = 0;
		}
	};

	/** @brief Where a shadow or occlusion view draws, and with which rasterizer state. The shape builds the rest of the view from it. */
	struct ShadowViewLayout
	{
		std::uint32_t slot = 0, modeIndex = 0, target = 0, slice = 0;
		std::uint32_t x = 0, y = 0, width = 0, height = 0;
		std::uint32_t rasterState = 0;
		bool operator==(const ShadowViewLayout&) const = default;
	};
	std::vector<ShadowViewLayout> LayoutOf(const ShadowFrame& a_frame);

	/** @brief Per mode, the most draws the scene's objects can cast, in total and per key slot. It bounds every frame's payload. */
	struct ShadowBounds
	{
		std::array<std::uint32_t, kShadowModeCount> modeDraws{};
		std::array<std::vector<std::uint32_t>, kShadowModeCount> keySlotDraws;
	};
	ShadowBounds ShadowBoundsOf(const DrawBoundStore& a_bound, const Lookups& a_lookups);

	/** @brief Everything the shadow shape is made from. The views list holds the placements first, then the occlusion maps. */
	struct ShadowShapeInputs
	{
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		ShadowIndirectState indirect{};
		std::vector<ShadowViewLayout> views;
		std::vector<RowBuckets> rows;  // per view
		const ShadowPayload* payload = nullptr;
		// Capacities are sized for these scene bounds, so the revision's draws stay within them.
		const ShadowBounds* bounds = nullptr;
		ShadowCapacity capacity;
		std::uint64_t viewBlocks = 0;
		std::shared_ptr<const ShadowFrame> previous;  // slot capacities only grow from this shape
		LatchedList latched;
		std::shared_ptr<const org::LatchBlock> latch, zeros;
		ShadowLatchLayout latchLayout;
		std::shared_ptr<const std::vector<ShadowSlotEntry>> slotLayouts;
	};
	std::shared_ptr<ShadowFrame> MakeShadowShape(const ShadowResources& a_resources, const ShadowShapeInputs& a_in);
	/** @brief The shadow commit's latched copies of CS's SharedData and FeatureData. */
	std::vector<LatchedCopy> ShadowLatchedLayout(const ShadowResources& a_resources);
	/**
	 * @brief Offsets in the shadow constants of the zero block, SharedData and FeatureData. Their sizes are fixed for the session.
	 * A block CS does not have reads the zero block.
	 */
	struct ShadowArenaBlocks
	{
		std::uint64_t zeros = 0, sharedData = 0, featureData = 0;
		std::uint32_t sharedBytes = 0, featureBytes = 0;
	};
	ShadowArenaBlocks ShadowArenaBlocksOf();
}
