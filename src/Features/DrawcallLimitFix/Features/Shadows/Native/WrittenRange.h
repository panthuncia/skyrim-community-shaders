#pragma once

// A block of memory a native port may write. A parity harness saves every range, runs one version, restores the ranges and runs the
// other, then compares. Shared by the native shadow ports in this folder.

#include <cstddef>

namespace DCLF::Features::Shadows::Native
{
	/** @brief One block a port may write: its address, its size in bytes and a short name for the harness's log. */
	struct WrittenRange
	{
		void* address;
		std::size_t size;
		const char* what;
	};
}
