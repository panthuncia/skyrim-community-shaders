#pragma once

// A descriptor index in GPU-visible bytes must live as long as those bytes do. The rows are OMS's (ORGModuleServices/OwnedRows.h),
// bound with DCLF's GpuTextures bindings. The upload classes then keep those owners alive with the uploaded content.

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include <ORGModuleServices/OwnedRows.h>

namespace DCLF
{
	using DescriptorIndex = org::services::DescriptorIndex;
	/** @brief A row plus the owners of the descriptors it names, bound from GpuTextures. */
	template <class Row>
	using OwnedRow = org::services::OwnedRow<Row, GpuTextures::Binding>;
}
