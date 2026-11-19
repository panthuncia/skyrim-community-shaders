#pragma once

// The active trigger: the engine point whose DCLF work the render thread is in. Each point wraps that work in a TriggerScope.
// Revision submission compares it with its epoch's trigger. Scopes nest, with no allocation and no lock.

#include "Features/DrawcallLimitFix/Api/Epochs.h"

#include <cstdint>
#include <utility>

namespace DCLF::Extensions
{
	/** @brief No trigger is active. */
	inline constexpr std::uint32_t kNoTrigger = ~0u;

	namespace Detail
	{
		// Render thread only.
		inline std::uint32_t activeTrigger = kNoTrigger;
	}

	/** @brief Render thread. The active trigger's row, or kNoTrigger. */
	inline std::uint32_t ActiveTrigger() { return Detail::activeTrigger; }

	/** @brief Render thread. Makes a_trigger active for the scope's life, then restores the previous one. */
	class TriggerScope
	{
	public:
		explicit TriggerScope(Api::TriggerId a_trigger) :
			saved(std::exchange(Detail::activeTrigger, a_trigger.index)) {}
		~TriggerScope() { Detail::activeTrigger = saved; }
		TriggerScope(const TriggerScope&) = delete;
		TriggerScope& operator=(const TriggerScope&) = delete;

	private:
		std::uint32_t saved;
	};
}
