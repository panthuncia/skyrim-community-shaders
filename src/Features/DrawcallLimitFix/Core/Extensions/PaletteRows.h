#pragma once

// Core's side of palette rows: the classes' entries, resolved once at install, and what their callbacks get.
// Each callback object is valid for that call only. FrameValues runs the kick, the run and the chunks.

#include "Features/DrawcallLimitFix/Api/FrameValues.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <type_traits>

struct ID3D11Buffer;

namespace DCLF::Api
{
	/**
	 * @brief The geometry slot a class's draws read. units goes in and the rest comes out.
	 * Core resolves the slot by key, which must be the feature's own address and never an engine TriShape's.
	 */
	struct PaletteGeometrySource
	{
		std::uint32_t units = 0;
		const void* key = nullptr;
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		std::uint64_t vertexDesc = 0;
		std::uint32_t vertexCount = 0, indexCount = 0;
	};

	/** @brief A class's frame input. The kick writes it, and the run and chunks read it as the class's type. */
	struct PaletteRowsInput
	{
		alignas(16) std::array<std::byte, kPaletteRowsInputBytes> bytes{};

		template <class T>
		T& As()
		{
			static_assert(sizeof(T) <= kPaletteRowsInputBytes && alignof(T) <= 16 && std::is_trivially_copyable_v<T>);
			return *std::launder(reinterpret_cast<T*>(bytes.data()));
		}
		template <class T>
		const T& As() const
		{
			static_assert(sizeof(T) <= kPaletteRowsInputBytes && alignof(T) <= 16 && std::is_trivially_copyable_v<T>);
			return *std::launder(reinterpret_cast<const T*>(bytes.data()));
		}
	};

	/** @brief What a class's kick gets. */
	class PaletteRowsKick
	{
	public:
		std::uint64_t seq = 0;
		PaletteRowsInput& input;
	};

	/** @brief One item of the class with a block, in run order. */
	struct PaletteRowsItem
	{
		const void* geometry = nullptr;  // its BSGeometry as a key. The run holds it.
		void* state = nullptr;           // set by beginRun, handed back in the item's write
		std::uint32_t listed = 0;        // its place in the run's list
	};

	/** @brief What beginRun and endRun get: the frame's input and the run's items. */
	class PaletteRowsRun
	{
	public:
		const PaletteRowsInput& input;
		std::span<PaletteRowsItem> items;
	};

	/** @brief An item's write in a chunk. Core's sample goes in, and the block's rows and placement come out. */
	struct PaletteRowsWrite
	{
		void* state = nullptr;      // its PaletteRowsItem::state
		bool firstPerson = false;   // the render thread owns a first-person item's row
		std::uint32_t rows = 0;
		float* palette = nullptr;   // rows float4s, all for the class to write
		// Out: the placement row's world as row-major 3x4, its previous world, and bound. placed false keeps Core's sample.
		float world[12]{};
		float bound[4]{};
		bool placed = false;
	};

	/** @brief First-person writes since the report last took them. */
	struct PaletteRowsFirstPerson
	{
		std::atomic<std::uint64_t> writes{ 0 }, units{ 0 };
		std::atomic<std::uint32_t> farthest{ 0 };  // non-negative float bits, so they order as floats
	};

	/** @brief What a chunk gets: the frame's input and the chunk's writes, in run order. */
	class PaletteRowsChunk
	{
	public:
		const PaletteRowsInput& input;
		std::span<PaletteRowsWrite> writes;
		PaletteRowsFirstPerson& firstPerson;

		/** @brief Records a placed first-person write for the report. */
		void NoteFirstPerson(std::uint32_t a_units, float a_distance) const
		{
			firstPerson.writes.fetch_add(1, std::memory_order_relaxed);
			firstPerson.units.fetch_add(a_units, std::memory_order_relaxed);
			const std::uint32_t bits = std::bit_cast<std::uint32_t>(a_distance);
			for (std::uint32_t seen = firstPerson.farthest.load(std::memory_order_relaxed);
				bits > seen && !firstPerson.farthest.compare_exchange_weak(seen, bits, std::memory_order_relaxed);) {}
		}
	};
}

namespace DCLF::Extensions
{
	/** @brief A class's entry, with its name owned. */
	struct PaletteRowsEntry
	{
		std::string name;
		Api::PaletteRowsContribution contribution;
	};

	/** @brief Main thread, at the end of InstallFeatures. */
	void ResolvePaletteRows(const std::array<Api::PaletteRowsContribution, Api::kPaletteRowsClasses>& a_registered);
	/** @brief Any thread, after InstallFeatures. Null for None or no registration. */
	const PaletteRowsEntry* PaletteRows(Api::FrameValueItemClass a_class);
	/** @brief Scene lane. A block's rows for a geometry of a_units units, or 0 without a class. */
	std::uint32_t PaletteRowsOf(Api::FrameValueItemClass a_class, std::uint32_t a_units);
	/** @brief Scene lane, per record write. a_class's geometry slot. False when there is none yet. */
	bool PaletteGeometryOf(Api::FrameValueItemClass a_class, std::uint32_t a_units, Api::PaletteGeometrySource& a_source);
	/** @brief Render thread, at frame start. Every class's frameStart, in class order. */
	void PaletteRowsFrameStart();
}
