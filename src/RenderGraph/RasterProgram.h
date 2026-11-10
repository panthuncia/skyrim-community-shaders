#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <rhi.h>

#	include <array>
#	include <cstdint>
#	include <memory>
#	include <span>
#	include <string>
#	include <utility>
#	include <vector>

namespace org
{
	class LatchBlock;
	class RecordingContext;
	struct DeclaredViewToken;
}

namespace org::services
{
	class LivePipelineSlot;
}

/**
 * @brief A render-graph raster program: a vertex and a pixel shader with the pipeline state for one set of targets.
 *
 * The raster twin of ComputeProgram. Both stages are HLSL under Data/Shaders, compiled to SPIR-V at runtime through the same
 * ORGModuleServices compiler and shader store, with BasicRHI's descriptor-heap ABI (OrgBindless.hlsli): the shaders fetch their
 * resources from the descriptor heap by index, and their constants are the push block (a cbuffer at b0, both stages).
 *
 * The default vertex stage is the fullscreen triangle (kFullscreenVertexShader): three vertices, no vertex buffer, TEXCOORD0 from
 * (0,0) at the top left to (1,1) at the bottom right, as PostProcessing's fullscreen.hlsli.
 *
 * A pass declares its targets with PassBuilder::RenderTarget (a mip or slice through RtvView) and its depth with DepthRead or
 * DepthReadWrite, then records with Draw. The viewport is the pass's render area, which may be a rectangle of the targets (a mip's
 * size, say).
 *
 * Hot reload works as for ComputeProgram: Load registers the pipeline with RenderGraphRuntime::PipelineRegistry with both sources as
 * its dependencies, and a reload swaps a new pipeline into the same slot at the graph's next build.
 */
struct RasterProgram
{
	/** @brief The fullscreen-triangle vertex shader, relative to Data/Shaders. */
	static constexpr const char* kFullscreenVertexShader = "RenderGraph/FullscreenVS.hlsl";

	rhi::PipelineLayoutPtr layout;
	// The registry's (payload: rhi::PipelinePtr); bind Pipeline() at record time, never a handle kept across recordings.
	std::shared_ptr<org::services::LivePipelineSlot> slot;
	std::uint32_t constantWords = 0;
	std::uint32_t targetCount = 0;
	bool hasDepth = false;
	// The draw command signature under the program's layout (vertex count, instance count, first vertex, first instance), for
	// DrawDesc::indirect.
	rhi::CommandSignaturePtr drawSignature;

	/** @brief Record time: the pipeline to bind, the slot's current one (it changes only at a graph build, where nothing records). */
	rhi::PipelineHandle Pipeline() const;

	/** @brief A colour target's format and blending. */
	struct Target
	{
		rhi::Format format = rhi::Format::Unknown;
		rhi::BlendAttachment blend{};  // off by default: the pixel stage's output replaces the target
	};

	struct Desc
	{
		std::string pixelSource;  // relative to Data/Shaders, e.g. "PostProcessing/Vignette/VignettePS.hlsl"
		std::wstring pixelEntry = L"main";
		std::string vertexSource = kFullscreenVertexShader;
		std::wstring vertexEntry = L"main";
		// Generated stages (the ORG FX runtime's lowered passes): when set, a stage compiles this text instead of reading its file,
		// and the stage's source above is only the name diagnostics and the shader store give it. Such a program has no files to
		// hot reload; a reload of every shader compiles the same text again.
		std::string pixelText;
		std::string vertexText;
		std::wstring languageVersion = L"2021";                      // DXC -HV: "2018" for FXC-era sources (vector ?:, && and || on vectors)
		std::uint32_t constantWords = 0;                             // the push block, in 32-bit words, read by both stages
		std::vector<std::pair<std::wstring, std::wstring>> defines;  // both stages
		std::vector<Target> targets;                                 // 1 to 8 colour targets, in SV_Target order
		rhi::Format depthFormat = rhi::Format::Unknown;              // Unknown: no depth attachment
		rhi::DepthStencilState depth{ .depthEnable = false, .depthWrite = false, .depthFunc = rhi::CompareOp::Always };
		rhi::RasterState raster{ .cull = rhi::CullMode::None };
		bool alphaToCoverage = false;
		// Also the defines the D3D11 build of each stage gets (ProgramBuild::WithCommonDefines: VSHADER, PSHADER, CS's common
		// defines), so a dual-target shader compiles the same permutation on both paths. Off for generated stages (the FX
		// runtime's), which have no D3D11 build with CS's defines.
		bool commonDefines = true;
	};

	/**
	 * @brief Compiles both stages (or finds them in the cache) and creates the pipeline; null, logged, when the runtime has no
	 * compiler, a stage fails to compile, or the pipeline cannot be made. Blocks on a compile the cache does not have, so call it at
	 * setup, not per frame.
	 */
	static std::shared_ptr<const RasterProgram> Load(rhi::Device a_device, const Desc& a_desc);

	/** @brief A colour attachment: a declared RenderTarget view and what the pass does with its contents first. */
	struct Colour
	{
		const org::DeclaredViewToken* view = nullptr;
		rhi::LoadOp load = rhi::LoadOp::Load;
		std::array<float, 4> clear{ 0.0f, 0.0f, 0.0f, 0.0f };  // with LoadOp::Clear
	};

	/** @brief The depth attachment: a declared DepthRead (readOnly) or DepthReadWrite view. */
	struct Depth
	{
		const org::DeclaredViewToken* view = nullptr;
		bool readOnly = true;
		rhi::LoadOp load = rhi::LoadOp::Load;
		float clear = 1.0f;  // with LoadOp::Clear
	};

	/** @brief The render area, which is also the viewport and the scissor (BasicRHI has no separate dynamic viewport yet). */
	struct Viewport
	{
		std::uint32_t x = 0, y = 0, width = 0, height = 0;
		float minDepth = 0.0f, maxDepth = 1.0f;
	};

	struct DrawDesc
	{
		std::span<const Colour> colours;  // as many as the program's targets
		const Depth* depth = nullptr;     // when the program has a depth format
		Viewport viewport;
		std::span<const std::uint32_t> constants;  // at most constantWords
		std::uint32_t vertexCount = 3;             // the fullscreen triangle
		std::uint32_t instanceCount = 1;
		const char* debugName = nullptr;
		// When set, the draw's four uints (vertex count, instance count, first vertex, first instance) are read from this latch
		// block, indirectOffset bytes into the region of the recording's frame slot, instead of vertexCount and instanceCount. A
		// vertex count of zero there draws nothing, so a kept recording can do no work on a frame by its data alone.
		const org::LatchBlock* indirect = nullptr;
		std::uint32_t indirectOffset = 0;
	};

	/**
	 * @brief Record time: begins a render pass on the draw's targets, binds the program, pushes the constants, draws, and ends the
	 * pass. The descriptor heaps are the frame's, which the graph binds before the first pass. Records nothing when the draw does
	 * not match the program (target count, depth, constants) or its viewport is empty.
	 */
	void Draw(org::RecordingContext& a_recording, const DrawDesc& a_draw) const;
};

#endif
