#pragma once

namespace RenderGraph
{
	/**
	 * @brief Whether the render graph's and DCLF's DXC stages carry source-level debug info. Off outside developer mode and in
	 * every release build (ALL, PR, Release presets), since it makes stages several times larger and slower to lower.
	 * CS_DCLF_SHADER_DEBUG=1 turns it on and =0 off whatever the build or mode. Read once, so the store keys stay stable.
	 */
	bool ShaderDebugInfo();
}
