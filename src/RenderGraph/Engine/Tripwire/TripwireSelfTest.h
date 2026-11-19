#pragma once

/**
 * @brief The tripwire handles' self-test, run at startup with CS_ORG_TRIPWIRE_SELFTEST=1. It needs no device.
 * Logs "[ORG] tripwire self-test: PASS", or each failed check and "FAIL". The sites it records are cleared afterwards.
 */
namespace RenderGraph::Engine::Tripwire::SelfTest
{
	bool Requested();

	void Run();
}
