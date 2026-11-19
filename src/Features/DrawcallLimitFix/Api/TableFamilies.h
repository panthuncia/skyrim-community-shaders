#pragma once

// Table families: a feature's own columns in the scene tables, beside Core's. Their rows are refcounted by the objects that reference them.
// Coordinator components write them, they are published with the tables, and the frame and builds read them.

#include <cstdint>
#include <memory>
#include <string_view>

namespace DCLF::Api
{
	/** @brief A registered family. Extensions::TableFamilies finds its columns in any tables by this id. */
	struct TableFamilyId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A table family. The coordinator's live tables and every publication's immutable copy each hold one.
	 * Core keeps a write stamp, so a publication copies the family only when it changed.
	 * Core also keeps each object's row reference and each row's tag, which lets the GPU tell a row's listing from the previous one.
	 * Freed rows return to the free list only once no publication names them.
	 * Callbacks run on the scene lane and never per object.
	 *
	 * - assignRow: optional, for journaled families. Copies one marked row, so a publication can replay only the marked rows.
	 * - same: optional. Used by the CS_DCLF_PERSISTENT_PARITY replay check.
	 */
	struct TableFamily
	{
		std::string_view name;  // plural, as in "<name> kept"
		std::shared_ptr<void> (*make)() = nullptr;
		void (*assign)(void* a_to, const void* a_from) = nullptr;
		void (*assignRow)(void* a_to, const void* a_from, std::uint32_t a_row) = nullptr;
		bool (*same)(const void* a_left, const void* a_right) = nullptr;
		void (*clear)(void* a_columns) = nullptr;
		void (*recycle)(void* a_columns, std::uint32_t a_row) = nullptr;
		// Reference values from here up name no row and carry the feature's own meaning, such as TreeWind's nodeless trees.
		std::uint32_t rowsEnd = ~0u;
		// The object record word that carries the row, with the tag in the next word. ~0u means none. Checked by Features/Manifest.cpp.
		std::uint32_t recordWord = ~0u;
		// The word holds (row + recordBias) << recordShift. A row that does not fit the bits above the shift is written as 0.
		std::uint32_t recordShift = 0, recordBias = 0;
		// false: one word and no tag, for families whose rows carry their own generation.
		bool recordTag = true;
		// Core keeps a journal of changed rows so a commit hook can send only what its buffer lacks.
		// The scene lane trims the journal to what was sent, without forcing a publication.
		bool journaled = false;
		// Optional, render thread, per revision. The rows that Core buffers sized by this family must hold.
		std::uint32_t (*rows)(const void* a_columns) = nullptr;
	};
}
