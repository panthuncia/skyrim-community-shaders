#pragma once

// Core's side of Api/TableFamilies.h: the families every SceneTables holds, the typed handle features read columns by,
// and the calls the tables' clear, retirement and publication make.

#include "Features/DrawcallLimitFix/Api/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF::Extensions
{
	/** @brief Most table families, because a SceneFit bounds each one's rows. Grow this before registering more. */
	inline constexpr std::uint32_t kTableFamilySlots = 4;
	/** @brief An object reference to no row. */
	inline constexpr std::uint32_t kNoFamilyRow = ~0u;
	/** @brief Most marks the replay log keeps. A copy further behind is copied whole. */
	inline constexpr std::size_t kMarkedRowsKept = 1u << 14;

	/** @brief One family in a tables: its stamp, the feature's columns, and what Core keeps beside them. */
	struct TableFamilyColumns
	{
		std::uint64_t stamp = 0;  // moved by every write, so publication copies the family only when it moved
		// Moved by NoteWrite but not by Mark. A copy at this value differs only in rows marked since, which publication replays.
		std::uint64_t written = 0;
		/** @brief Records any write other than a marked row's own columns. The next publication copies the family whole. */
		void NoteWrite()
		{
			++stamp;
			++written;
		}
		std::shared_ptr<void> columns;  // this tables' own, never shared with another tables
		// Per object slot, the row it references. kNoFamilyRow means none, and values from rowsEnd up carry the feature's own meaning.
		std::vector<std::uint32_t> objectRow;
		// Per row, its listing generation, carried in the record beside the row.
		std::vector<std::uint32_t> rowTag;
		// Copied from the registration so per-object readers never touch the registry.
		std::uint32_t rowsEnd = ~0u, recordWord = ~0u;
		std::uint32_t recordShift = 0, recordBias = 0;
		bool recordTag = true, journaled = false;
		// Rows written since what the commit hook's buffer holds. Copied with every publication. Unused unless journaled.
		ChangeJournal journal;
		// Every marked row in order, with chunks shared with the publications' copies. Trimmed past kMarkedRowsKept.
		SharedEventLog<std::uint32_t> markedRows;

		/** @brief Records a write of a_row's own columns only. Marks the journal and the replay log, and moves the stamp. */
		void Mark(std::uint32_t a_row)
		{
			journal.Mark(a_row);
			markedRows.Push(a_row);
			markedRows.Trim(kMarkedRowsKept);
			++stamp;
		}
		const ChangeJournal& Journal() const { return journal; }
		/** @brief The content epoch a commit hook holds once it has sent every run. */
		std::uint64_t ContentEpoch() const { return journal.ContentEpoch(); }

		std::uint32_t RowOf(std::uint32_t a_object) const { return a_object < objectRow.size() ? objectRow[a_object] : kNoFamilyRow; }
		/** @brief The record value for a_row: (row + bias) << shift, or 0 if it does not fit above the shift. */
		std::uint32_t RecordReferenceOf(std::uint32_t a_row) const
		{
			const std::uint64_t biased = std::uint64_t(a_row) + recordBias;
			return biased < (std::uint64_t(1) << (32 - recordShift)) ? static_cast<std::uint32_t>(biased << recordShift) : 0u;
		}
		/** @brief The two record words for a_object: its row reference and the row's tag, which is 0 without recordTag. */
		std::array<std::uint32_t, 2> RecordWordsOf(std::uint32_t a_object) const
		{
			const std::uint32_t row = RowOf(a_object);
			if (row == kNoFamilyRow)
				return { 0u, 0u };
			return { RecordReferenceOf(row), !recordTag ? 0u : row >= rowsEnd ? row : row < rowTag.size() ? rowTag[row] : 0u };
		}
		/** @brief ORs a_object's reference into a_record. Core's bits below the shift are kept. */
		void WriteRecordWords(std::uint32_t* a_record, std::uint32_t a_object) const
		{
			if (recordWord == ~0u)
				return;
			const auto words = RecordWordsOf(a_object);
			a_record[recordWord] |= words[0];
			if (recordTag)
				a_record[recordWord + 1] |= words[1];
		}
		/** @brief Whether a_object's reference fits within a_rows rows. If not, the object waits for the buffers to grow. */
		bool RowFits(std::uint32_t a_object, std::uint32_t a_rows) const
		{
			const std::uint32_t row = RowOf(a_object);
			return row >= rowsEnd || row < a_rows;
		}
	};

	/**
	 * @brief The families a SceneTables holds, by registration. Empty until the coordinator first writes one, and a missing family reads as empty.
	 * Copying runs each family's assign. Moves and swaps go by pointer.
	 */
	class TableFamilies
	{
	public:
		TableFamilies() = default;
		TableFamilies(const TableFamilies& a_other);
		TableFamilies& operator=(const TableFamilies& a_other);
		TableFamilies(TableFamilies&&) noexcept = default;
		TableFamilies& operator=(TableFamilies&&) noexcept = default;

		std::span<const TableFamilyColumns> All() const { return entries; }
		/** @brief a_id's family, or null. */
		const TableFamilyColumns* Find(Api::TableFamilyId a_id) const { return a_id.index < entries.size() ? &entries[a_id.index] : nullptr; }
		/** @brief a_id's columns as the feature's type, or null. */
		template <class T>
		const T* Columns(Api::TableFamilyId a_id) const
		{
			const auto* family = Find(a_id);
			return family ? static_cast<const T*>(family->columns.get()) : nullptr;
		}
		/** @brief Scene lane, live tables only. a_id's family, making every family first if none exist. */
		TableFamilyColumns& Mutable(Api::TableFamilyId a_id);
		template <class T>
		T& MutableColumns(Api::TableFamilyId a_id)
		{
			return *static_cast<T*>(Mutable(a_id).columns.get());
		}

		/** @brief Removes every family's rows, references and tags, and moves each stamp. */
		void Clear();
		/** @brief Puts a_family's retired a_row back on its free list. */
		void Recycle(std::uint32_t a_family, std::uint32_t a_row);
		/** @brief The sum of stamps. Any write changes it. */
		std::uint64_t StampSum() const;
		/**
		 * @brief One value covering what a revision's requirements read of the families. Used to decide if a revision can be reused.
		 * Each family contributes its row count if it reports one, and otherwise its stamp.
		 */
		std::uint64_t RequirementsWitness() const;
		/** @brief Whether every family's stamp matches a_other's. A family neither holds counts as 0. */
		bool SameStamps(const TableFamilies& a_other) const;
		/**
		 * @brief The publication's copy. Each family is kept if unchanged, replayed if only marks moved it, and otherwise copied.
		 * Returns the kept and replayed families as a bit per registration.
		 */
		std::uint32_t Publish(const TableFamilies& a_from, bool a_replayable, std::array<std::uint64_t, kTableFamilySlots>& a_kept);
		/** @brief Whether a_family matches a_other's rows, references and tags. Used by the replay parity. */
		bool Same(std::uint32_t a_family, const TableFamilies& a_other) const;
		std::uint64_t StampOf(std::uint32_t a_family) const { return a_family < entries.size() ? entries[a_family].stamp : 0; }
		/**
		 * @brief Scene lane, live tables. Trims each journaled family's journal to the content epoch its commit hook last reported sent.
		 * This forces no publication, because a trim changes no row and ForEachRun skips runs the holder already has.
		 */
		void TakeSent();

	private:
		void MakeAll();
		void Assign(std::size_t a_family, const TableFamilyColumns& a_from);
		/** @brief Brings a_family up to a_from by replaying rows marked since its copy. Returns false if it cannot. */
		bool ReplayMarked(std::size_t a_family, const TableFamilyColumns& a_from);

		std::vector<TableFamilyColumns> entries;
	};

	/**
	 * @brief Render thread. A journaled family's commit hook reports that its buffer holds a_id's rows at a_contentEpoch.
	 * Posted latest-wins with no wake. The next pass picks it up.
	 */
	void TableFamilySent(Api::TableFamilyId a_id, std::uint64_t a_contentEpoch);
	/** @brief Render thread, per revision. The rows that resources sized by a_family must hold, or 0. */
	std::uint32_t TableFamilyRows(const TableFamilies& a_tables, Api::TableFamilyId a_family);
	/** @brief How many publications replayed a_family's marked rows since the last call. */
	std::uint64_t TakeTableFamilyReplays(std::uint32_t a_family);
}
