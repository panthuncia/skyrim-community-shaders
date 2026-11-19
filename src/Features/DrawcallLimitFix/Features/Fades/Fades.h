#pragma once

// Fades ports the OnVisible fade step of BSFadeNode, BSLeafAnimNode and BSTreeNode to the GPU and produces the root states.
// It tracks the fade nodes that drawn geometry sits under, steps them in FadeStateCS and writes milestones back onto the nodes.
// It also records the engine's own fade writes so DCLF's state stays in step with them.
// See dclf-architecture.md, "Fades on the GPU", and skyrim-engine-notes.md, "Fade state".
// It must stay last in FeatureList.cpp so every shared registration point keeps its order.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Fades
{
	Api::Feature Feature();
}
