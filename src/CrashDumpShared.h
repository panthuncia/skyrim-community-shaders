#pragma once

// The shared section between the plugin (src/CrashDump.cpp) and the crash dump helper (tools/crash-dump-helper/main.cpp).
// Both build this one definition, so the layout cannot drift. Plain C++ only: the helper builds without the plugin's PCH.

#include <cstdint>

namespace CrashDump
{
	/** @brief What a request asks the helper for (Shared::kind). */
	enum class RequestKind : std::uint32_t
	{
		kCrash = 0,  ///< a minidump of the fault that is ending the process, to the path on the helper's command line
		kHang = 1    ///< every thread's stack as text to textPath, then a minidump to dumpPath
	};

	/** @brief The section's layout. The plugin writes a request, the helper writes the answer (written, error, bytes). */
	struct Shared
	{
		std::uint32_t threadId;           ///< a crash's faulting thread, or a hang's stuck thread
		RequestKind kind;                 ///< what to write
		std::uint64_t exceptionPointers;  ///< a crash's EXCEPTION_POINTERS* in the game's address space
		wchar_t dumpPath[1024];           ///< a hang's minidump
		wchar_t textPath[1024];           ///< a hang's stacks
		std::uint32_t written;            ///< 1 when the minidump was written
		std::uint32_t error;              ///< else the Win32 error it failed with (MiniDumpWriteDump's or CreateFile's)
		std::uint64_t bytes;              ///< the minidump's size
	};
}
