#pragma once

// Tracks which descriptors the bytes in GPU buffers currently name, by byte range. The tracker is OMS's
// (ORGModuleServices/ContentOwners.h); executions seal its snapshots with GpuTextures::Seal (ExecutionOwners::Seal).

#include <ORGModuleServices/ContentOwners.h>

namespace DCLF::Draws
{
	using ContentOwners = org::services::ContentOwners;
}
