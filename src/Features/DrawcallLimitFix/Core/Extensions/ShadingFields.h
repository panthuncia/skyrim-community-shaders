#pragma once

// Core's side of Api/ShadingFields.h: the fields' entries and published entry classes, and what a field's capture gets.
// Core zeroes a slot's fields when it leaves the class. FrameValues is the only writer of the values, and shading samples leave them alone.

#include "Features/DrawcallLimitFix/Api/ShadingFields.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace RE
{
	class BSGeometry;
}

namespace DCLF::Scene
{
	class FrameView;
}

namespace DCLF::Api
{
	/** @brief What a field's capture gets at the frame's start: the newest publication's geometry per slot, and Put for values. */
	class ShadingFieldCapture
	{
	public:
		ShadingRowField field = ShadingRowField::Count;
		std::span<RE::BSGeometry* const> objectGeometry;  // empty without a publication
		std::vector<ShadingFieldValue>& values;
		std::span<const std::uint32_t> changedObjects;  // slots whose shading was published since the last frame

		/** @brief Sets a_slot's value. Applied after the frame's shading sample. */
		void Put(std::uint32_t a_slot, std::span<const float> a_value, std::shared_ptr<const void> a_resources = {}) const
		{
			ShadingFieldValue value{ .slot = a_slot, .field = field, .resources = std::move(a_resources) };
			std::copy_n(a_value.begin(), std::min(a_value.size(), value.value.size()), value.value.begin());
			values.push_back(std::move(value));
		}
	};
}

namespace DCLF::Extensions
{
	/** @brief Number of entry classes the tables can keep members of. */
	inline constexpr std::size_t kPublishedEntryClasses = static_cast<std::size_t>(Api::EntryClass::Count);
	inline constexpr std::size_t kShadingRowFields = static_cast<std::size_t>(Api::ShadingRowField::Count);

	/** @brief A field's entry, with its name owned. */
	struct ShadingFieldEntry
	{
		std::string name;
		Api::ShadingFieldContribution contribution;
	};

	/** @brief Main thread, at the end of InstallFeatures. Resolves the fields and the published entry classes. */
	void ResolveShadingFields(std::span<const Api::ShadingFieldContribution> a_registered);
	/** @brief Any thread, after InstallFeatures. */
	std::span<const ShadingFieldEntry> ShadingFields();
	/** @brief Any thread, after InstallFeatures. The entry classes registered as published. */
	std::span<const Api::EntryClass> PublishedEntryClasses();
	/** @brief Render thread. Forwards one publication's membership changes to every field of a_class, and notes slots that left. */
	void ForwardClassMembers(Api::EntryClass a_class, std::span<const Api::ClassMemberChange> a_changes);
	/** @brief Render thread, at the frame's start. Every field's capture, each after zeroes for slots that left its class. */
	std::vector<Api::ShadingFieldValue> CaptureShadingFields(Scene::FrameView& a_view);
}
