#pragma once

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief Read-only view of IndirectDraws' implementation for the draws observers, read at hook points after the main path is done with it.
	 */
	struct DrawsView
	{
		using Impl = IndirectDraws::Impl;

		static const Impl& Get() { return *IndirectDraws::Get().impl; }
		/** @brief The implementation, or null before it is made. */
		static const Impl* Find() { return IndirectDraws::Get().impl.get(); }
	};
}
