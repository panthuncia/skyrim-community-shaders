#pragma once

// DCLF's side of SceneWriterFramework. The engine store hooks belong to SWF and are shared with every plugin that embeds it.
// At kPostLoad every copy in the process elects one provider, which patches the engine and serves the events DCLF subscribes to.

#include <swf/swf.h>

namespace DCLF::SceneWriter
{
	/**
	 * @brief The capabilities DCLF cannot run without, since the mirror follows these writers by event alone. Moves and hidden bits
	 * are optional, at the cost of per-frame placement and verdicts.
	 */
	inline constexpr SWF_Caps kRequiredCaps = SWF_CAP_SWITCH_INDEX | SWF_CAP_FADE_RESET | SWF_CAP_STRUCTURE | SWF_CAP_SWITCH_STRUCTURAL |
	                                          SWF_CAP_FADE_CALLS | SWF_CAP_NODE_CALLS | SWF_CAP_PROPERTY_CALLS | SWF_CAP_GEOMETRY_CALLS |
	                                          SWF_CAP_MATERIAL_CALLS | SWF_CAP_ACTOR_CALLS | SWF_CAP_DECAL_CALLS | SWF_CAP_EFFECT_CALLS;

	/** @brief CS's kPostLoad, when DCLF will install. The newest SWF copy in the process patches the engine. */
	void Elect();

	/** @brief The elected provider's API, or null. */
	const SWF_Api* Api();

	/** @brief The provider's installed families, 0 without one. */
	SWF_Caps Caps();

	/** @brief Why DCLF cannot run on this provider, or empty. */
	std::string MissingReason();

	/** @brief Subscribes a_fn to a_event. False, logged, when the provider refuses. */
	bool Subscribe(std::uint32_t a_event, SWF_EventFn a_fn, std::uint32_t a_phases = SWF_PHASE_POST, std::int32_t a_priority = 0);

	/** @brief One of SWF's patches another plugin rewrote after SWF installed it. */
	struct IntegrityChange
	{
		std::string family;
		std::string module;  // the module the new bytes lead into, empty for none
		std::uint32_t kind = 0;
		std::uint32_t offset = 0;  // from the image base
	};

	/** @brief At kDataLoaded. Records the SWF patches rewritten since install, for the menu. A no-op without a provider. */
	void CheckIntegrity();

	/** @brief The last CheckIntegrity's changes. */
	const std::vector<IntegrityChange>& IntegrityChanges();
	bool IntegrityChecked();

	/** @brief The provider's host and version, or why there is none. */
	std::string ProviderLine();

	/** @brief The provider's installed families' names. */
	std::string CapsLine();

	/** @brief Adds a filter. False, logged, when the provider refuses. */
	bool AddFilter(std::uint32_t a_filter, SWF_FilterFn a_fn, std::int32_t a_priority = 0);

	/** @brief Whether the provider installed every family in a_caps. */
	inline bool Has(SWF_Caps a_caps) { return (Caps() & a_caps) == a_caps; }

	/** @brief Subscribes to an engine call hook's event, PRE and POST by default. */
	inline bool SubscribeCall(std::uint32_t a_hook, SWF_EventFn a_fn, std::uint32_t a_phases = SWF_PHASE_PRE | SWF_PHASE_POST)
	{
		return Subscribe(SWF_EVENT_CALL(a_hook), a_fn, a_phases);
	}

	/** @brief An engine call's arguments and result. */
	inline const SWF_CallEvent& CallOf(const SWF_EventHeader* a_event) { return *reinterpret_cast<const SWF_CallEvent*>(a_event); }
	template <class T>
	T Arg(const SWF_EventHeader* a_event, std::uint32_t a_index)
	{
		const std::uint64_t word = CallOf(a_event).args[a_index];
		if constexpr (std::is_pointer_v<T>)
			return reinterpret_cast<T>(static_cast<std::uintptr_t>(word));
		else if constexpr (std::is_same_v<T, float>)
			return std::bit_cast<float>(static_cast<std::uint32_t>(word));
		else
			return static_cast<T>(word);
	}
	inline bool IsPre(const SWF_EventHeader* a_event) { return a_event->phase == SWF_PHASE_PRE; }
}
