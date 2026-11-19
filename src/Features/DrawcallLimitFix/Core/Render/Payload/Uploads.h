#pragma once

// A commit's uploads on the render thread, either staged or latched into the epoch's latch block.
// Every write states which descriptors its bytes name, so they stay alive while the bytes are current.
// Features include this header directly, so it must stay self-contained.

#include "Features/DrawcallLimitFix/Core/Gpu/FrameData.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Gpu/OwnedRows.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/ContentOwners.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"

#include <Render/LatchBlock.h>
#include <Render/Runtime/IUploadService.h>
#include <Render/Runtime/StagedUploadBatch.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <Resources/Buffers/Buffer.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Draws
{
	/** @brief A readable name for the part of the frame-constants buffer at a_offset. */
	std::string FrameConstantsPart(std::uint64_t a_offset);
	template <class Target>
	std::string FrameDataWhere(std::string_view a_route, const Target& a_target, std::uint64_t a_offset)
	{
		std::string where(a_route);
		where += ": ";
		if constexpr (requires { a_target->GetName(); }) {
			const auto& name = a_target ? a_target->GetName() : std::string();
			if (name == "cs.dclf.frame-constants")
				return where + FrameConstantsPart(a_offset);
			where += name.empty() ? std::string("unnamed") : name;
		} else {
			where += "other";
		}
		return where;
	}

	/** @brief Reuses a released batch from a_pool, or adds a new one. */
	inline std::shared_ptr<org::runtime::StagedUploadBatch> AcquireStagedBatch(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool,
		std::size_t a_pageSize = std::size_t{ 4 } << 20)
	{
		DCLF_ZONE("CS.DCLF.Stage.AcquireBatch");
		std::shared_ptr<org::runtime::StagedUploadBatch> batch;
		for (const auto& candidate : a_pool) {
			if (candidate.use_count() == 1) {
				batch = candidate;
				break;
			}
		}
		if (!batch) {
			batch = org::runtime::StagedUploadBatch::Create(a_pageSize);
			a_pool.push_back(batch);
		}
		batch->Reset();
		return batch;
	}

	// A commit's own uploads, staged directly and submitted when the commit ends. Each is one memcpy and one copy.
	// A versioned buffer is written in the version the adopted set names, the one the epoch's recordings resolve.
	class CommitUploads
	{
	public:
		CommitUploads(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool, const NamedVersions& a_adopted, ContentOwners* a_content = nullptr) :
			batch(AcquireStagedBatch(a_pool, std::size_t{ 1 } << 20)), adopted(a_adopted), content(a_content)
		{}
		~CommitUploads()
		{
			if (!batch->Entries().empty())
				org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(batch));
		}
		CommitUploads(const CommitUploads&) = delete;
		CommitUploads& operator=(const CommitUploads&) = delete;

		template <class Target>
		void operator()(const Target& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (!a_data || !a_bytes)
				return;
			NoteContent(ContentKey(a_target), a_offset, a_bytes, {});
			Stage(a_target, a_data, a_bytes, a_offset);
		}
		// The row's descriptors become the range's content owners.
		template <class Target, class Row>
		void operator()(const Target& a_target, const OwnedRow<Row>& a_row, std::uint64_t a_offset)
		{
			Stage(a_target, a_row.Bytes(), OwnedRow<Row>::kBytes, a_offset);
			NoteContent(ContentKey(a_target), a_offset, OwnedRow<Row>::kBytes, a_row.Owners());
		}
		const std::shared_ptr<org::Buffer>& TargetOf(const Versioned& a_target) const { return adopted.Of(a_target).version->buffer; }

	private:
		friend class LatchedUploads;
		// Versioned buffers are keyed by Key() so owners survive growth to a new version.
		static const void* ContentKey(const Versioned& a_target) { return a_target->Key(); }
		template <class T>
		static const void* ContentKey(const std::shared_ptr<T>& a_target)
		{
			return a_target.get();
		}
		template <class Target>
		void Stage(const Target& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			batch->Stage(org::runtime::UploadTarget::FromShared(a_target), static_cast<std::size_t>(a_offset), a_data, a_bytes);
			if (FrameData::Enabled())
				FrameData::Note(FrameDataWhere("staged", a_target, a_offset), a_bytes);
		}
		// Every set sealed after a buffer's registration names it, so the adopted set always has a version here.
		void Stage(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset);
		void NoteContent(const void* a_key, std::uint64_t a_offset, std::uint64_t a_bytes, std::span<const std::shared_ptr<const void>> a_owners)
		{
			if (content)
				content->Set(a_key, a_offset, a_bytes, a_owners);
			else if (!a_owners.empty())
				stl::report_and_fail("Drawcall Limit Fix: a row naming descriptors was uploaded by a commit with no content owners (its descriptors would outlive their owners)");
		}

		std::shared_ptr<org::runtime::StagedUploadBatch> batch;
		const NamedVersions& adopted;
		ContentOwners* content = nullptr;
	};

	/**
	 * @brief A commit's per-frame values, written into its shape's latched copies and copied to their targets by the epoch's first pass.
	 * A value with no copy is staged by the fallback instead.
	 */
	class LatchedUploads
	{
	public:
		LatchedUploads(const LatchedList& a_layout, CommitUploads& a_fallback, const org::RunRef& a_run) :
			fallback(a_fallback), run(a_run), layout(a_layout), written(a_layout.copies.size(), 0)
		{}
		/** @brief Values that had no copy and were staged instead. */
		std::uint32_t Misses() const { return misses; }

		void operator()(const std::shared_ptr<org::Buffer>& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (!a_data || !a_bytes)
				return;
			fallback.NoteContent(a_target.get(), a_offset, a_bytes, {});
			Write(a_target, a_data, a_bytes, a_offset);
		}
		void operator()(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (!a_data || !a_bytes)
				return;
			fallback.NoteContent(a_target->Key(), a_offset, a_bytes, {});
			Write(a_target, a_data, a_bytes, a_offset);
		}
		// The row's descriptors become the range's content owners.
		template <class Target, class Row>
		void operator()(const Target& a_target, const OwnedRow<Row>& a_row, std::uint64_t a_offset)
		{
			Write(a_target, a_row.Bytes(), OwnedRow<Row>::kBytes, a_offset);
			fallback.NoteContent(CommitUploads::ContentKey(a_target), a_offset, OwnedRow<Row>::kBytes, a_row.Owners());
		}

	private:
		void Write(const std::shared_ptr<org::Buffer>& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (Latch(a_target.get(), a_data, a_bytes, a_offset, [&] { fallback.Stage(a_target, a_data, a_bytes, a_offset); }) && FrameData::Enabled())
				FrameData::Note(FrameDataWhere("latched", a_target, a_offset), a_bytes);
		}
		void Write(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (Latch(a_target->Key(), a_data, a_bytes, a_offset, [&] { fallback.Stage(a_target, a_data, a_bytes, a_offset); }) && FrameData::Enabled())
				FrameData::Note(FrameDataWhere("latched", fallback.TargetOf(a_target), a_offset), a_bytes);
		}
		// False when nothing was written or the fallback staged it.
		template <class Fallback>
		bool Latch(const void* a_key, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset, Fallback&& a_fallback)
		{
			if (!a_data || !a_bytes)
				return false;
			const auto& listed = layout.copies;
			for (std::size_t i = 0; i < listed.size(); ++i) {
				const auto& copy = listed[i];
				if (copy.target != a_key || copy.dstOffset != a_offset || a_bytes > copy.bytes || !layout.latch)
					continue;
				auto region = layout.latch->Bytes(run).subspan(copy.latchOffset, copy.bytes);
				std::memcpy(region.data(), a_data, a_bytes);
				std::memset(region.data() + a_bytes, 0, copy.bytes - a_bytes);
				written[i] = 1;
				return true;
			}
			++misses;
			a_fallback();
			return false;
		}

	public:
		/** @brief The frame's list, with every copy not written this commit zeroed. */
		LatchedList Finish()
		{
			// Zero unwritten copies so a published copy's region never replays an older run's bytes.
			for (std::size_t i = 0; i < written.size(); ++i)
				if (!written[i] && layout.latch) {
					const auto& copy = layout.copies[i];
					auto region = layout.latch->Bytes(run).subspan(copy.latchOffset, copy.bytes);
					std::memset(region.data(), 0, region.size());
				}
			return layout;
		}

	private:
		CommitUploads& fallback;
		org::RunRef run{};
		const LatchedList& layout;
		std::vector<std::uint8_t> written;
		std::uint32_t misses = 0;
	};
}
