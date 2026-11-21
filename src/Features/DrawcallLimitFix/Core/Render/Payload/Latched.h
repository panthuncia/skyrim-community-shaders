#pragma once

// Latched copies. A commit writes per-frame values into a latch block and its epoch's first pass copies them to their targets.
// The types are ORG's (Render/LatchedCopies.h), and so is the copy pass (RenderPasses/LatchedCopiesPass.h).
// Features include this header directly, so it must stay self-contained.

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"

#include <Render/LatchBlock.h>
#include <Render/LatchedCopies.h>

namespace DCLF::Draws
{
	using LatchedCopy = org::LatchedCopy;
	using LatchedList = org::LatchedList;
	using LatchRegionsHeld = org::LatchRegionsHeld;
	using LatchedTarget = org::LatchedTarget;
	using org::SetLatchWords;
}
