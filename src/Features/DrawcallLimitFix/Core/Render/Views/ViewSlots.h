#pragma once

// Core's fixed slot per engine view, whichever feature takes the view. Every per-view loop runs over this table in its order.
// Adding a view takes the enum value, a row here, its epochs in Schedule/Frame.cpp, and the feature's registration.
// Included by Internal.h after Impl.h, because it names the producer's key groups.

namespace DCLF::Draws
{
	/** @brief A view's slot: the shapes key group its inputs mix into, and its graph extension's id. */
	struct ViewSlot
	{
		ViewKind view;
		std::size_t keyGroup;
		const char* extensionId;
	};
	inline constexpr std::array<ViewSlot, kViews> kViewSlots = { {
		{ ViewKind::Reflection, ImplProducer::kKeyReflection, kReflectionExtensionId },
		{ ViewKind::FirstPerson, ImplProducer::kKeyFirstPerson, kFirstPersonExtensionId },
	} };
	static_assert(kViewSlots[ViewIndex(ViewKind::Reflection)].view == ViewKind::Reflection && kViewSlots[ViewIndex(ViewKind::FirstPerson)].view == ViewKind::FirstPerson,
		"kViewSlots is by view");
}
