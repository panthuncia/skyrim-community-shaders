#pragma once

// Hidden nodes: turns every engine store to NiAVObject's kHidden bit into an event, via SceneWriterFramework patches.
// CS_DCLF_HIDDEN_WATCH is a hardware watch that finds stores the patches miss.
// Without these events the walk retakes every verdict each frame, and the LOD gates and kept scene lists stay off.
// Core owns the channel, the dependency tracking and the Hidden verdict. See dclf-architecture.md, "Hidden bits by event".

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>
#include <string>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Features::HiddenNodes
{
	Api::Feature Feature();

	/** @brief Subscribes to SWF's hidden stores and registers the show filter. Main thread, at PostPostLoad. */
	void InstallStores();
	/** @brief The patched store whose SWF stub contains a_address, or 0. */
	std::uintptr_t StoreSiteOf(std::uintptr_t a_address);

	/**
	 * @brief CS_DCLF_HIDDEN_WATCH=1: hardware write watchpoints on up to four flags fields, on every thread.
	 * Armed when an actor's verdict changes with no hidden event, to name the store the events miss. Diagnostic only.
	 */
	void Arm(const std::array<const RE::NiAVObject*, 4>& a_nodes);
	/** @brief Returns what the watch recorded, or empty. Also disarms it when due. */
	std::string TakeReport(std::uint32_t a_frame);
	bool Enabled();
	void RegisterWatch(Api::InstallContext& a_context);
}
