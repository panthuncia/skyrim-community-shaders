#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <rhi.h>

#	include <cstdint>
#	include <memory>
#	include <span>
#	include <string>
#	include <utility>
#	include <vector>

namespace org
{
	class RecordingContext;
}

namespace org::services
{
	class LivePipelineSlot;
}

/**
 * @brief A render-graph compute shader: the pipeline and the layout its push constants are bound through.
 *
 * Every feature's compute passes create theirs here, from the HLSL under Data/Shaders, compiled to SPIR-V at runtime by
 * RenderGraphRuntime's ORGModuleServices compiler (saved in the shared shader store), with
 * BasicRHI's descriptor-heap ABI. The shaders fetch everything else from the descriptor heap by index.
 *
 * Hot reload (docs/development/dclf-hot-reload.md): Load registers the pipeline with RenderGraphRuntime::PipelineRegistry, its source
 * as its dependency, and binds through the slot it gets. A reload compiles it again from the files as they are now, and a new SPIR-V
 * gets a new pipeline under the same layout, swapped into the slot at the graph's next build (every recording after it binds it). The
 * program object and its layout stay, so no owner, pass, dispatch signature or revision input changes. A program that fails to
 * compile keeps its pipeline.
 */
struct ComputeProgram
{
	rhi::PipelineLayoutPtr layout;
	// The registry's (payload: rhi::PipelinePtr); bind Pipeline() at record time, never a handle kept across recordings.
	std::shared_ptr<org::services::LivePipelineSlot> slot;
	std::uint32_t constantWords = 0;

	/** @brief Record time: the pipeline to bind, the slot's current one (it changes only at a graph build, where nothing records). */
	rhi::PipelineHandle Pipeline() const;

	struct Desc
	{
		std::string source;  // relative to Data/Shaders, e.g. "DrawcallLimitFix/BuildDrawsCS.hlsl"
		std::wstring entry = L"main";
		std::uint32_t constantWords = 0;  // the push-constant block, in 32-bit words
		std::vector<std::pair<std::wstring, std::wstring>> defines;
	};

	/**
	 * @brief Compiles (or finds in the cache) and creates the program; null, logged, when the runtime has no compiler or
	 * the build fails. Blocks on a compile the cache does not have, so call it at setup, not per frame.
	 */
	static std::shared_ptr<const ComputeProgram> Load(rhi::Device a_device, const Desc& a_desc);

	/**
	 * @brief Record time: binds the program, pushes a_constants (a dual-target shader's push block: ShaderBindings::Data), and
	 * dispatches. Records nothing when the constants are longer than constantWords or a group count is zero.
	 */
	void Dispatch(org::RecordingContext& a_recording, std::span<const std::uint32_t> a_constants, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) const;
};

#endif
