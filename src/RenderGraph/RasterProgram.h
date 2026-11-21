#pragma once

#include <rhi.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

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
* The raster twin of ComputeProgram. Both stages are HLSL under Data/Shaders, compiled to SPIR-V at runtime with BasicRHI's
* descriptor-heap ABI. Shaders fetch resources by heap index, and their constants are the push block at b0.
* The default vertex stage is a fullscreen triangle with TEXCOORD0 from (0,0) top left to (1,1) bottom right.
* The viewport is the pass's render area. Hot reload adds a new version to the same slot, which graphs built after it bind.
*/
struct RasterProgram
{
	/** @brief The fullscreen-triangle vertex shader, relative to Data/Shaders. */
	static constexpr const char* kFullscreenVertexShader = "RenderGraph/FullscreenVS.hlsl";

	rhi::PipelineLayoutPtr layout;
	// Bind Pipeline(recording) at record time. Never keep a handle across recordings.
	std::shared_ptr<org::services::LivePipelineSlot> slot;
	std::uint32_t constantWords = 0;
	std::uint32_t targetCount = 0;
	bool hasDepth = false;
	// The draw command signature under the program's layout, for DrawDesc::indirect.
	rhi::CommandSignaturePtr drawSignature;

	/**
	* @brief The pipeline a_recording binds: the slot's version at its graph's pipeline pin (RenderGraph/PipelineVersions.h). A shader
	* reload never changes it; the pin keeps it alive while the recording lives.
	*/
	rhi::PipelineHandle Pipeline(const org::RecordingContext& a_recording) const;

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
		// Generated stages compile this text instead of a file. The source above is then just a name, and there is nothing to hot reload.
		std::string pixelText;
		std::string vertexText;
		std::wstring languageVersion = L"2021";                      // DXC -HV: "2018" for FXC-era sources (vector ?:, && and || on vectors)
		std::uint32_t constantWords = 0;                             // the push block, in 32-bit words, read by both stages
		std::vector<std::pair<std::wstring, std::wstring>> defines;  // both stages
		std::vector<Target> targets;                                 // 1 to 8 colour targets, in SV_Target order; none for a depth-only program
		rhi::Format depthFormat = rhi::Format::Unknown;              // Unknown: no depth attachment (then targets must not be empty)
		// The stencil reference is the draw's (DrawDesc::stencilReference, 0 by default).
		rhi::DepthStencilState depth{ .depthEnable = false, .depthWrite = false, .depthFunc = rhi::CompareOp::Always };
		rhi::RasterState raster{ .cull = rhi::CullMode::None };
		// The pipeline's primitive type: a draw sets the same one (SetPrimitiveTopology) before it draws.
		rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
		bool alphaToCoverage = false;
		// Also give each stage CS's common defines, as its D3D11 build gets, so dual-target shaders compile the same permutation.
		// Off for generated stages, which have no D3D11 build.
		bool commonDefines = true;
		// Each stage's full define list, used as is when not empty. Set by the program service on the requesting thread,
		// since CS's common defines read State, which a build thread must not.
		std::vector<std::pair<std::wstring, std::wstring>> vertexStageDefines;
		std::vector<std::pair<std::wstring, std::wstring>> pixelStageDefines;
	};

	/** @brief Compiles both stages or finds them cached, and makes the pipeline. Null, logged, on failure.
	*  Blocks on an uncached compile, so call it at setup, not per frame. */
	static std::shared_ptr<const RasterProgram> Load(rhi::Device a_device, const Desc& a_desc);

	/** @brief Compiles both stages into the shader store without making a pipeline, so a later Load finds them. Blocks.
	*  False when there is no compiler yet or a stage failed. */
	static bool CompileStages(const Desc& a_desc);

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
		bool clearStencil = false;  // with LoadOp::Clear: the stencil is cleared to 0 too
	};

	/** @brief The render area. Also the viewport and scissor, unless the draw replaces them. */
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
		// The draw's own viewport instead of the render area. It may reach past the targets, and then TEXCOORD0 spans this viewport.
		std::optional<rhi::Viewport> dynamicViewport;
		// The draw's scissor, clipped to the render area. Unset means the render area.
		std::optional<rhi::ScissorRect> scissor;
		std::span<const std::uint32_t> constants;  // at most constantWords
		std::uint32_t vertexCount = 3;             // the fullscreen triangle
		std::uint32_t instanceCount = 1;
		const char* debugName = nullptr;
		// The stencil test's and REPLACE's reference (OMSetDepthStencilState's), for a program whose depth state enables stencil.
		std::uint32_t stencilReference = 0;
		// Reads the draw's four indirect uints from this latch block in the recording's run. A vertex count of zero draws nothing,
		// so a kept recording can skip work by its data alone.
		const org::LatchBlock* indirect = nullptr;
		std::uint32_t indirectOffset = 0;
		// Or reads them from this buffer at this offset, which a GPU pass wrote and the pass declares as indirect arguments.
		// Non-indexed: vertex count, instances, first vertex, first instance.
		rhi::ResourceHandle indirectBuffer{};
		std::uint64_t indirectBufferOffset = 0;
		// An occlusion query around this draw alone. The caller resets it before and resolves it after.
		rhi::QueryPoolHandle occlusionPool{};
		std::uint32_t occlusionIndex = 0;
	};

	/** @brief Record time. Begins a render pass, binds the program, pushes constants, draws and ends the pass.
	*  Returns false and records nothing when the draw does not match the program or its viewport is empty. */
	bool Draw(org::RecordingContext& a_recording, const DrawDesc& a_draw) const;
};

