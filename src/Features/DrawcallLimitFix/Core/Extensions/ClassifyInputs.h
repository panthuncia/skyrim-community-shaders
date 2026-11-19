#pragma once

// Core's side of Api/ClassifyInputs.h: the registered inputs, the value set a frame or scene pass takes,
// the per-thread snapshot a scene pass installs, and inputs needed by name.

#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace DCLF::Extensions
{
	/** @brief Most classify inputs, one bit each in a set. Grow this before registering more. */
	inline constexpr std::size_t kClassifyInputSlots = 8;
	/** @brief Most inputs needed by name. Grow this before needing more. */
	inline constexpr std::size_t kClassifyInputNeedSlots = 4;
	/** @brief Marks an id as a need, with its need slot in the low bits. A registered input's id is its index. */
	inline constexpr std::uint32_t kClassifyInputNeedBit = 0x8000'0000u;

	namespace Detail
	{
		// Per need slot, the bound input, or ~0u which reads false. Written only during InstallFeatures before DCLF's threads exist.
		inline std::array<std::uint32_t, kClassifyInputNeedSlots> boundClassifyInputNeeds = [] {
			std::array<std::uint32_t, kClassifyInputNeedSlots> bound{};
			bound.fill(~0u);
			return bound;
		}();
	}

	/** @brief The registered input a_input reads: itself, or a need's bound input. */
	inline Api::ClassifyInputId ResolvedClassifyInput(Api::ClassifyInputId a_input)
	{
		if (a_input.index == ~0u || !(a_input.index & kClassifyInputNeedBit))
			return a_input;
		const auto slot = a_input.index & ~kClassifyInputNeedBit;
		return { slot < kClassifyInputNeedSlots ? Detail::boundClassifyInputNeeds[slot] : ~0u };
	}

	/** @brief Every input's value at once, one bit each by id index. */
	struct ClassifyInputSet
	{
		std::uint8_t bits = 0;

		bool On(Api::ClassifyInputId a_input) const
		{
			const auto input = ResolvedClassifyInput(a_input);
			return input.index < kClassifyInputSlots && ((bits >> input.index) & 1u) != 0;
		}

		bool operator==(const ClassifyInputSet&) const = default;
	};
	static_assert(kClassifyInputSlots <= sizeof(ClassifyInputSet::bits) * 8);

	/** @brief A registered classify input, with its name owned. */
	struct ClassifyInputEntry
	{
		std::string name;
		decltype(Api::ClassifyInputContribution::sample) sample = nullptr;
	};
	/** @brief Any thread, after InstallFeatures. An index is the input's id. */
	std::span<const ClassifyInputEntry> RegisteredClassifyInputs();
	/** @brief Main thread, once, after every install. Binds each need to the input registered under its name. */
	void BindClassifyInputNeeds(std::span<const std::string> a_needs);

	/** @brief Any thread. Every input's value, from the thread's snapshot if a ClassifyInputsScope is installed, otherwise live. */
	ClassifyInputSet SampleClassifyInputs();
	/** @brief Any thread. a_input's value, as SampleClassifyInputs takes it. False when its feature is not installed. */
	bool ClassifyInputOn(Api::ClassifyInputId a_input);

	/**
	 * @brief Installs a_inputs as this thread's inputs while the scope lives.
	 * A scene pass uses it so its pool work keeps one answer even if the menu switches a feature meanwhile.
	 */
	class ClassifyInputsScope
	{
	public:
		explicit ClassifyInputsScope(const ClassifyInputSet* a_inputs);
		~ClassifyInputsScope();
		ClassifyInputsScope(const ClassifyInputsScope&) = delete;
		ClassifyInputsScope& operator=(const ClassifyInputsScope&) = delete;

	private:
		const ClassifyInputSet* previous;
	};
}
