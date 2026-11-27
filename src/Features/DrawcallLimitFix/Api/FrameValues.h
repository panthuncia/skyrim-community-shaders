#pragma once

// Frame-value pools: rows a feature samples every frame on DCLF's pool and sends to a backing the GPU reads by descriptor.
// Palette rows: a class of FrameValues items whose palette block a feature fills every frame, beside the skins' bone palettes.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/FrameValuePools.h.
	class FrameValuePoolFrameStart;
	class FrameValuePoolKick;
	class FrameValuePoolProducer;

	/** @brief A registered pool. FrameValues::PoolIndex finds the frame's backing by this id. */
	struct FrameValuePoolId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/** @brief What a pool's kick hands to the frame's producer. */
	struct FrameValueInput
	{
		// Released on DCLF's pool, so it must hold no engine reference. Kick runs every frame, so pass a pointer you already hold.
		std::shared_ptr<const void> input;
		// The backing holds at least this many rows. It grows with headroom and never shrinks.
		std::uint64_t rows = 0;
	};

	/**
	 * @brief A frame-value pool. The feature journals its rows, and each frame only the missing runs are sent.
	 *
	 * - frameStart: the game half, once per ticket, before it is known whether the ticket has a sampling job.
	 *   Read frame-start engine state here and drain the frame's queued requests.
	 * - kick: the game half with the ticket's sampling job (a perRender pool: the render thread with each upload job).
	 * - sample: on DCLF's pool, in the ticket's sampling job (perRender: in the render's upload job). Jobs run one at a time in kick
	 *   order, so state only they touch needs no lock. Read the engine only under an EngineReadWindow lease, and never in a perRender
	 *   pool: the render side takes no lease.
	 * - send: in each render's upload job, hands the rows to FrameValuePoolProducer::Send.
	 * A throw from sample or send fails the frame's values. The frame then draws what its backings already held.
	 */
	struct FrameValuePool
	{
		std::string_view name;    // backing pool name is cs.dclf.frame-values.<name>
		std::uint32_t stride = 0;  // bytes per row
		// The backings report their device address, for rows or passes that use it.
		bool deviceAddress = false;
		// The GPU writes to the rows. D3D12 needs this, or the UAV is a null view and every store is dropped.
		bool unorderedAccess = false;
		// Kicked and sampled with each render's upload, from render-side state (the render's view batches, its placement rows).
		bool perRender = false;
		void (*frameStart)(const FrameValuePoolFrameStart& a_start) = nullptr;
		FrameValueInput (*kick)(const FrameValuePoolKick& a_kick) = nullptr;
		void (*sample)(FrameValuePoolProducer& a_producer) = nullptr;
		void (*send)(FrameValuePoolProducer& a_producer) = nullptr;
	};

	// Defined in Core/Extensions/PaletteRows.h.
	struct PaletteGeometrySource;
	class PaletteRowsKick;
	class PaletteRowsRun;
	class PaletteRowsChunk;

	/** @brief A class of FrameValues items. ParticleSystem is a skinless particle system whose block holds its quads. */
	enum class FrameValueItemClass : std::uint8_t
	{
		None,
		ParticleSystem,
		Count,
	};
	inline constexpr std::size_t kPaletteRowsClasses = static_cast<std::size_t>(FrameValueItemClass::Count) - 1;  // None has none

	/** @brief Size of a palette rows class's frame input. It must be trivially copyable. */
	inline constexpr std::size_t kPaletteRowsInputBytes = 128;

	/**
	 * @brief A palette rows class. Its items are sampled every frame whether or not they moved, and the feature writes their blocks.
	 *
	 * - geometrySource: scene lane, per record write. Returns false when no geometry slot exists yet, so the record waits.
	 * - kick: the game half, with each ticket's sampling job. The input stays all zero without a kick.
	 * - beginRun: lists this run's items of the class and sets each one's state.
	 * - sampleChunk: runs concurrently with other chunks, and chunks hold distinct items. Writes every row of each block.
	 *   May also supply the placement row's world and bound. No engine reads, because no lease is held.
	 */
	struct PaletteRowsContribution
	{
		struct BlockRule
		{
			std::uint32_t rowsPerUnit = 0, minUnits = 0, maxUnits = 0;
			bool pow2 = false;
			// With pow2, sizes round up to one of this many equal steps per octave. Must be a power of two.
			// More steps waste fewer rows but create more sizes, and each size is its own geometry slot.
			std::uint32_t octaveSteps = 1;

			/** @brief The units a geometry with a_count units gets a block for. */
			constexpr std::uint32_t Units(std::uint32_t a_count) const
			{
				const std::uint32_t units = std::clamp(a_count, minUnits, maxUnits);
				if (!pow2)
					return units;
				const std::uint32_t octave = std::bit_floor(units);
				const std::uint32_t step = std::max(octave / std::max(octaveSteps, 1u), 1u);
				return (units + step - 1) / step * step;
			}
			/** @brief The block's rows in the frame's palettes. */
			constexpr std::uint32_t Rows(std::uint32_t a_count) const { return Units(a_count) * rowsPerUnit; }
		};

		std::string_view name;
		FrameValueItemClass itemClass = FrameValueItemClass::Count;
		BlockRule rule;
		bool (*geometrySource)(PaletteGeometrySource& a_source) = nullptr;
		void (*frameStart)() = nullptr;
		void (*kick)(const PaletteRowsKick& a_kick) = nullptr;
		void (*beginRun)(const PaletteRowsRun& a_run) = nullptr;
		void (*sampleChunk)(const PaletteRowsChunk& a_chunk) = nullptr;
		void (*endRun)(const PaletteRowsRun& a_run) = nullptr;
	};
}
