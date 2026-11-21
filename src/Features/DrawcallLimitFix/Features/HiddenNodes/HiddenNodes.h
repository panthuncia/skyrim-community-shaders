#pragma once

// Hidden nodes: turns every engine store to NiAVObject's kHidden bit into an event, via SceneWriterFramework patches.
// Without these events the walk retakes every verdict each frame, and the LOD gates and kept scene lists stay off.
// Core owns the channel, the dependency tracking and the Hidden verdict. See dclf-architecture.md, "Hidden bits by event".

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::HiddenNodes
{
	Api::Feature Feature();

	/** @brief Subscribes to SWF's hidden stores and registers the show filter. Main thread, at PostPostLoad. */
	void InstallStores();
}
