#pragma once

// The areas' installs, called by Diagnostics::Install in this order. Each subscribes the observers its switches ask for.

namespace DCLF::Diagnostics
{
	void InstallFeature();
	void InstallEngine();
	void InstallScene();
	void InstallDraws();
}
