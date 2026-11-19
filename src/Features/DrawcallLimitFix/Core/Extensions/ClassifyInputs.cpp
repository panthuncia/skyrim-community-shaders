// The classify inputs' samples and the scene passes' snapshots.

#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"

namespace DCLF::Extensions
{
	namespace
	{
		// The inputs a scene pass took at its start, installed on each thread it evaluates on. Null means live.
		thread_local const ClassifyInputSet* snapshot = nullptr;
	}

	ClassifyInputSet SampleClassifyInputs()
	{
		if (snapshot)
			return *snapshot;
		ClassifyInputSet set;
		const auto inputs = RegisteredClassifyInputs();
		for (std::size_t i = 0; i < inputs.size(); ++i)
			if (inputs[i].sample())
				set.bits |= static_cast<std::uint8_t>(1u << i);
		return set;
	}

	bool ClassifyInputOn(Api::ClassifyInputId a_input)
	{
		if (snapshot)
			return snapshot->On(a_input);
		const auto input = ResolvedClassifyInput(a_input);
		const auto inputs = RegisteredClassifyInputs();
		return input.index < inputs.size() && inputs[input.index].sample();
	}

	void BindClassifyInputNeeds(std::span<const std::string> a_needs)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the classify input needs were bound twice");
		// By name, like a toggle's needs. Names are unique.
		const auto inputs = RegisteredClassifyInputs();
		for (std::size_t n = 0; n < a_needs.size() && n < kClassifyInputNeedSlots; ++n)
			for (std::size_t i = 0; i < inputs.size(); ++i)
				if (inputs[i].name == a_needs[n]) {
					Detail::boundClassifyInputNeeds[n] = static_cast<std::uint32_t>(i);
					break;
				}
	}

	ClassifyInputsScope::ClassifyInputsScope(const ClassifyInputSet* a_inputs) :
		previous(snapshot)
	{
		snapshot = a_inputs;
	}

	ClassifyInputsScope::~ClassifyInputsScope() { snapshot = previous; }
}
