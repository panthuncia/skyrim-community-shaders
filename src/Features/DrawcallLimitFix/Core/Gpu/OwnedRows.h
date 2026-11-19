#pragma once

// A descriptor index in GPU-visible bytes must live as long as those bytes do. Rows declare such fields DescriptorIndex, which only
// OwnedRow::Bind sets, together with the binding's owner. The upload classes then keep those owners alive with the uploaded content.

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace DCLF
{
	template <class Row>
	class OwnedRow;

	/** @brief A descriptor heap index in a GPU row. Set only by OwnedRow::Bind or as a sentinel meaning none. */
	class DescriptorIndex
	{
	public:
		constexpr DescriptorIndex() = default;
		/** @brief A value that names no descriptor, such as ~0u. */
		static constexpr DescriptorIndex Sentinel(std::uint32_t a_value)
		{
			DescriptorIndex index;
			index.value = a_value;
			return index;
		}
		constexpr std::uint32_t Value() const { return value; }

	private:
		template <class Row>
		friend class OwnedRow;
		std::uint32_t value = 0;
	};
	static_assert(sizeof(DescriptorIndex) == 4 && std::is_standard_layout_v<DescriptorIndex> && std::is_trivially_copyable_v<DescriptorIndex>);

	/**
	 * @brief A row plus the owners of the descriptors it names. Upload it whole so the owners travel with it.
	 * A copy of the plain row carries no owners, so never upload that alone once a field is bound.
	 */
	template <class Row>
	class OwnedRow
	{
		static_assert(std::is_trivially_copyable_v<Row>, "a GPU row is bytes");

	public:
		OwnedRow() = default;
		explicit OwnedRow(const Row& a_plain) :
			row(a_plain) {}

		Row& Plain() { return row; }
		const Row& Plain() const { return row; }

		/** @brief Writes a_binding's index into a_field and keeps its owner. False and nothing written without one. */
		bool Bind(DescriptorIndex Row::*a_field, GpuTextures::Binding a_binding)
		{
			if (a_binding.index == GpuTextures::kInvalid)
				return false;
			(row.*a_field).value = a_binding.index;
			if (a_binding.owner)
				owners.push_back(std::move(a_binding.owner));
			return true;
		}
		/** @brief Sets a_field to a sentinel. An owner bound before stays held until the row goes. */
		void Unbind(DescriptorIndex Row::*a_field, DescriptorIndex a_sentinel = {}) { row.*a_field = a_sentinel; }

		std::span<const std::shared_ptr<const void>> Owners() const { return owners; }
		const void* Bytes() const { return &row; }
		static constexpr std::size_t kBytes = sizeof(Row);

	private:
		Row row{};
		std::vector<std::shared_ptr<const void>> owners;
	};
}
