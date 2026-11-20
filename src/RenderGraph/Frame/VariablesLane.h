#pragma once

#include "Features/Upscaling/PresentationConfig.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace org::services
{
	class VariableSet;
	class VariableSnapshot;
}

/**
* @brief The revision coordinator's side of the frame-wide variables (frame revision plan, step 5b). Revision.cpp and its input
* producers (Revision::RegisterInputProducer) use it, with the render graph. Render thread only: it is the variable set's one
* consumer.
*/
namespace RenderGraph::Frame::Vars::Lane
{
	/** @brief A CS::Setting's value as its variable holds it: its bytes, or a counter for a setting too big to carry. */
	struct SettingValue
	{
		static constexpr std::size_t kCapacity = 32;
		static constexpr std::uint32_t kTooBig = UINT32_MAX;
		std::array<std::byte, kCapacity> bytes{};
		std::uint32_t size = 0;  // kTooBig when bytes hold a counter
		friend bool operator==(const SettingValue&, const SettingValue&) = default;
	};

	/** @brief The index of the frame-wide variable named a_name, or UINT32_MAX for none. Any thread. */
	std::uint32_t IndexOf(std::string_view a_name);

	/**
	* @brief Whether the variable at a_index holds constants only (VariableTraits::sizes false: RenderSize, DynamicRatios,
	* ShaderGeneration). Such a variable may not size a texture or a buffer (decision 6a). Any thread.
	*/
	bool ConstantsOnly(std::uint32_t a_index);

	/**
	* @brief The index of a CS::Setting's variable, declared now with a_bytes when it has none yet. Any thread.
	* a_name is the setting's registry name. With null a_bytes nothing is declared, and UINT32_MAX means none.
	*/
	std::uint32_t SettingIndex(std::uint32_t a_settingId, std::string_view a_name, const void* a_bytes, std::size_t a_size);

	/**
	* @brief Render thread, at setup. Declares a_name, an unsigned variable derived from the variable at a_dependency. a_fn runs
	* in Take, on the lane, with the snapshot being built, and only when the dependency changed. Returns its index.
	*/
	std::uint32_t DeriveUint(std::string a_name, std::uint32_t a_dependency, std::function<std::uint32_t(const org::services::VariableSnapshot&)> a_fn);

	/** @brief The set itself, for the newest written values (VariableSet::Get). Any thread. */
	const org::services::VariableSet& Set();

	/** @brief The newest values. The same snapshot, with no allocation, while nothing changed since the last call. */
	std::shared_ptr<const org::services::VariableSnapshot> Take();

	/** @brief The snapshot of the revision adopted last, or null before the first adoption. */
	const std::shared_ptr<const org::services::VariableSnapshot>& Adopted();

	/** @brief Makes a_snapshot the adopted values. Inside the adoption point. */
	void Adopt(std::shared_ptr<const org::services::VariableSnapshot> a_snapshot);

	/** @brief The presentation a_values carries. */
	FrameGen::PresentationConfig PresentationConfigIn(const org::services::VariableSnapshot& a_values);

	/**
	* @brief The names of the variables a_newer changed since a_older, comma separated. Without a_older, the variables written
	* at least once. Empty when none.
	*/
	std::string ChangedNames(const org::services::VariableSnapshot& a_newer, const org::services::VariableSnapshot* a_older);
}
