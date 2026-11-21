#pragma once

// CS's side of the programs' builds: the defines its D3D11 builds get. The rest is OMS's
// (ORGModuleServices/Programs/ProgramBuild.h), with CS's environment (RenderGraph::Programs::InstallEnvironment).

#if defined(ORG_MODULE_SERVICES_HAS_DXC)

#	include <ORGModuleServices/Programs/ProgramBuild.h>

namespace RenderGraph::ProgramBuild
{
	using Defines = org::services::ProgramDefines;

	using org::services::program_build::ReadSource;

	/**
	 * @brief a_own plus the defines every D3D11 build of the stage gets, so both paths build one permutation.
	 * Reads State's defines, so call it at setup, not from a compile thread. It is the environment's common defines.
	 */
	Defines WithCommonDefines(const Defines& a_own, const wchar_t* a_target);
}

#endif
