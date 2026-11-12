#pragma once

namespace DCLF
{
	/**
	 * @brief Main thread, PostPostLoad, once DCLF installed: registers DCLF's test commands ("dclf:<name>=<arg>"), its waits (settled,
	 * reload, nvperf) and its exit report with the test runner (src/Testing/Testing.h).
	 */
	void RegisterTestCommands();
}
