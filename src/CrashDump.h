#pragma once

// Crash and hang dumps, written out of process by CommunityShadersCrashDump.exe (tools/crash-dump-helper), which Start runs once at
// load.
//
// A crash: an unhandled exception filter, chained to the one installed before it, writes CommunityShaders.dmp beside the game for
// the session's first exception that no handler took, the one that ends the process. A vectored handler also logs the first
// faults (access violations and the like) at first chance, with module and offset: a handler of the code that raised them may still
// catch them, so they only log and never use up the dump. The helper keeps serving requests after a crash dump (a hang report that
// follows is still written).
//
// A hang: the watchdog (HangWatchdog.h) asks for the stuck thread's report. The helper writes every thread's stack as text and a
// minidump.

#include <cstdint>
#include <filesystem>

namespace CrashDump
{
	/** @brief SKSEPlugin_Load: starts the helper and installs the crash handler. */
	void Start();

	/** @brief Whether the helper runs (it is missing, or could not start, in a broken install). */
	bool Available();

	/** @brief What a hang report request came to. */
	enum class HangDumpResult
	{
		Written,     ///< the helper wrote the stacks and the minidump
		Failed,      ///< the helper answered, but the minidump was not written (HangDumpError has the Win32 error)
		NoHelper,    ///< the helper is not running
		NoAnswer,    ///< the helper did not answer within the timeout
	};

	/**
	 * @brief Any thread but a_threadId: the helper writes every thread's stack, a_threadId's first, to a_text, then a minidump to
	 * a_dump. Returns once it is done, or after a_timeoutMs. On Failed, a_error receives the helper's Win32 error.
	 */
	HangDumpResult RequestHangDump(std::uint32_t a_threadId, const std::filesystem::path& a_dump, const std::filesystem::path& a_text, std::uint32_t a_timeoutMs,
		std::uint32_t& a_error);
}
