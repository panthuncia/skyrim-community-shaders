#pragma once

/**
 * @brief The tripwire handles' self-test (CS_ORG_TRIPWIRE_SELFTEST=1), run by Tripwire::Start at startup. Without a device: every
 * handle type is made standalone and checked for its refcount, QueryInterface, GetDesc, debug name and a forbidden method (it fails,
 * zeroes its outputs and is counted); guest handles are checked to forward and to unwrap handle arguments; Release from several
 * threads retires the owner once; a game address resolves to its Address Library ID. Logs "[ORG] tripwire self-test: PASS" or
 * one line per failed check and "FAIL". The sites it records are cleared afterwards.
 */
namespace RenderGraph::Engine::Tripwire::SelfTest
{
	/** @brief Whether CS_ORG_TRIPWIRE_SELFTEST=1 is set. */
	bool Requested();

	/** @brief Runs the self-test and logs the result. */
	void Run();
}
