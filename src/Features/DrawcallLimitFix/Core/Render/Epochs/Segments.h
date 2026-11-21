#pragma once

#include "RenderGraph/RenderGraphRuntime.h"

namespace DCLF
{
	/**
	* @brief Whether a render graph segment is one of DCLF's epochs, not a CS stage's.
	* The core knows them only as DCLF's fragment source's segments (Frame::Revision::RegisterFragmentSource).
	*/
	constexpr bool IsDclfSegment(RenderGraphRuntime::Segment a_segment)
	{
		return a_segment < RenderGraphRuntime::Segment::Declared;
	}
}
