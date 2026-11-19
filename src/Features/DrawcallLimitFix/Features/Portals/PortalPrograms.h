#pragma once

// The portal programs' encoding, which PortalPrograms.hlsli's PortalTest reads. Core binds the buffer and never reads it.

#include <cstdint>

namespace DCLF
{
	/**
	 * @brief The main camera's portal programs for FadeStateCS's and BuildDrawsCS's view tests, uploaded with the depth commit.
	 * Layout: view planes (kPortalSetBytes), a header, one directory entry per program, 16-byte operator records in forward
	 * topological order, then the plane sets. Program 0 is unbound space, program 1 + i is room i.
	 */
	inline constexpr std::uint32_t kPortalSetBytes = 112;
	inline constexpr std::uint32_t kPortalHeaderBytes = kPortalSetBytes + 16;
	inline constexpr std::uint32_t kPortalDirectoryBytes = 32;
	inline constexpr std::uint32_t kPortalOpBytes = 16;
	inline constexpr std::uint32_t kPortalNoProgram = 0xFFFFFFFFu;
	inline constexpr std::uint32_t kPortalReached = 1u << 0;   // the walk reached it (else a room's children are not culled into)
	inline constexpr std::uint32_t kPortalSkipView = 1u << 1;  // skipViewFrustum: the view test is not made
}
