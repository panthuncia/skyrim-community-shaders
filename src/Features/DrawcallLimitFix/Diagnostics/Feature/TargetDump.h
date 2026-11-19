#pragma once

#include <cstdint>

struct ID3D11Texture2D;

namespace DCLF::TargetDump
{
	/**
	 * @brief CS_DCLF_TARGET_DUMP=f1,f2,... Copies the bound targets and key renderer targets at each named point of those frames,
	 * counted as the test commands count them. Nothing waits. Copies are written once done as half-size PNGs and a stats line each
	 * into SKSE/dclf-dump/<count>/. A point reached twice in a frame keeps its first copy. Render thread.
	 */
	void Point(const char* a_label, ID3D11Texture2D* a_extra = nullptr);
	/** @brief Advances the count and writes the copies whose GPU work is done. Every Present. */
	void Present();
	/** @brief Whether this frame dumps. A caller may skip work to reach Point otherwise. */
	bool Armed();
}
