#pragma once

// Shading fields: a shading row field whose value belongs to an owner, such as an actor, rather than to the object's property.
// It is captured once per owner at the frame's start and copied to the owner's members. Every other slot's field is zero.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/ShadingFields.h.
	class ShadingFieldCapture;

	/** @brief A shading row field filled from an owner's value. Core owns the byte layout. */
	enum class ShadingRowField : std::uint8_t
	{
		SkinPerGeometry,  // Advanced Skin's SkinPerGeometry (PS b7), a float4
		LandscapeSeams,   // per-quad weight, data and borrowed layer descriptors stored as index + 1, zero when inactive
		Count,
	};

	/** @brief A registered shading field. */
	struct ShadingFieldId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief One membership change for a slot of a published entry class. A group of 0 means the slot is not a member.
	 * clear drops all members. resetValue hands the value out again even if the owner's did not change.
	 */
	struct ClassMemberChange
	{
		std::uint32_t slot = 0;
		std::uint64_t group = 0, identity = 0;
		bool resetValue = false;
		bool clear = false;
	};

	/** @brief One publication's membership changes, in order. */
	struct ShadingFieldMembership
	{
		std::span<const ClassMemberChange> changes;
	};

	/** @brief Most floats a captured field value holds. */
	inline constexpr std::size_t kShadingFieldValueFloats = 8;

	/** @brief One member slot's captured value. */
	struct ShadingFieldValue
	{
		std::uint32_t slot = ~0u;
		ShadingRowField field = ShadingRowField::Count;
		std::array<float, kShadingFieldValueFloats> value{};
		std::shared_ptr<const void> resources;  // keeps the value's descriptors alive until the frame's GPU work completes
	};

	/**
	 * @brief A shading field and the entry class whose members carry it.
	 *
	 * - members: None means the capture tracks geometry itself and clears its own retired slots.
	 * - membership: render thread at the frame's start, once per publication with changes, in publication order.
	 * - capture: render thread at the frame's start. A value stays until the next put, or until the slot leaves the class.
	 */
	struct ShadingFieldContribution
	{
		std::string_view name;
		ShadingRowField field = ShadingRowField::Count;
		EntryClass members = EntryClass::None;
		void (*membership)(const ShadingFieldMembership& a_membership) = nullptr;
		void (*capture)(ShadingFieldCapture& a_capture) = nullptr;
	};
}
