#pragma once

#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(CS_HAS_RENDER_GRAPH)
#	include "Utils/DxcShaderStore.h"

#	include <Render/PipelineDesc.h>
#	include <rhi.h>
#endif

namespace RE::BSGraphics
{
	class VertexShader;
	class PixelShader;
	class ComputeShader;
}

/**
* @brief Descriptions of the engine's shaders, input layouts, state objects and queries, for building ORG pipelines.
*
* With CS_ORG_ENGINE_SHADERS=1 every shader and input layout is described at creation. State objects and queries are described
* from GetDesc on first lookup, with or without the switch. A token in the object's private data removes the description
* when the object goes. Every Find accepts a tripwire handle or a D3D11 object and returns null if nothing was described.
* Any thread.
*/
namespace RenderGraph::Engine::Pipeline
{
	/** @brief Read once. */
	bool Enabled();

#if defined(CS_HAS_RENDER_GRAPH)

	enum class Stage : std::uint8_t
	{
		Vertex,
		Pixel,
		Compute
	};

	struct ShaderSource
	{
		std::uint32_t shaderType = 0;
		std::uint32_t descriptor = 0;
		std::wstring path;  ///< under Data/Shaders
		std::vector<std::pair<std::string, std::string>> defines;  ///< ShaderCache::GetCompileDefines at creation
	};

	class ShaderDescription
	{
	public:
		ShaderDescription(Stage a_stage, std::span<const std::uint8_t> a_bytecode);
		ShaderDescription(const ShaderDescription&) = delete;
		ShaderDescription& operator=(const ShaderDescription&) = delete;

		const Stage stage;
		const std::vector<std::uint8_t> bytecode;
		const org::services::ShaderStoreHash hash;
		/** @brief Empty for FXC's DXBC. Bindings follow DXVK's ABI. */
		const std::vector<std::uint32_t> spirv;

		/** @brief Read on first use. Null when it cannot be read. */
		const Util::ShaderReflection* Reflection() const;

		/** @brief Null for the engine's own vanilla shaders. */
		std::shared_ptr<const ShaderSource> Source() const { return source.load(std::memory_order_acquire); }

		/** @brief Called once, after creation. */
		void SetSource(std::shared_ptr<const ShaderSource> a_source) const { source.store(std::move(a_source), std::memory_order_release); }

		/** @brief Nothing without a source. */
		std::optional<org::PipelineShaderSource> PipelineSource() const;

	private:
		mutable std::once_flag reflected;
		mutable std::optional<Util::ShaderReflection> reflection;
		mutable std::atomic<std::shared_ptr<const ShaderSource>> source;
	};

	struct InputLayoutDescription
	{
		std::vector<std::string> semantics;
		std::vector<D3D11_INPUT_ELEMENT_DESC> elements;  ///< SemanticName points into semantics
		org::services::ShaderStoreHash vertexShaderHash;
		org::PipelineInputLayout layout;  ///< strides 0, the geometry's VertexDesc gives them
		std::string error;  ///< an element BasicRHI cannot express, left out of layout
		/** @brief The VertexDesc key CreateInputLayout made it for, or ~0. */
		mutable std::atomic<std::uint64_t> engineKey{ ~0ull };
	};

	struct BlendDescription
	{
		D3D11_BLEND_DESC d3d{};
		rhi::BlendState rhi{};
		bool readsBlendFactor = false;  ///< the engine's factor is always 1
		bool dualSource = false;
		std::string error;
	};

	struct DepthStencilDescription
	{
		D3D11_DEPTH_STENCIL_DESC d3d{};
		rhi::DepthStencilState rhi{};
	};

	struct RasterDescription
	{
		D3D11_RASTERIZER_DESC d3d{};
		rhi::RasterState rhi{};
		bool scissorEnable = false;  ///< no rhi field. False means the pass sets a scissor covering the target.
	};

	struct SamplerDescription
	{
		D3D11_SAMPLER_DESC d3d{};  ///< as created, with the anisotropy cap
		rhi::SamplerDesc rhi{};
	};

	enum class QueryMapping : std::uint8_t
	{
		Occlusion,  ///< resolved to a readback
		Timestamp,
		Disjoint,  ///< no object. Uses the queue's timestamp frequency, never disjoint.
		Event,  ///< a timeline value the CPU waits on
		Unsupported  ///< the engine creates none
	};

	struct QueryDescription
	{
		D3D11_QUERY_DESC d3d{};
		QueryMapping mapping = QueryMapping::Unsupported;
	};

	/** @brief The engine never writes the blend factor SetDirtyStates passes (SE 512153 / AE 388963). */
	inline constexpr float kEngineBlendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	/** @name Recording. Each replaces an earlier description of the same object.
	* @{ */
	std::shared_ptr<const ShaderDescription> RecordShader(Stage a_stage, IUnknown* a_object, std::span<const std::uint8_t> a_bytecode);
	std::shared_ptr<const InputLayoutDescription> RecordInputLayout(ID3D11InputLayout* a_object, std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements,
		std::span<const std::uint8_t> a_vertexShaderBytecode);
	/** @brief Nothing when the shader was not recorded. */
	void RecordShaderSource(IUnknown* a_object, ShaderSource a_source);
	void RecordEngineInputLayoutKey(ID3D11InputLayout* a_object, std::uint64_t a_key);
	/** @} */

	/** @name Lookups. a_object may be a tripwire handle or a D3D11 object.
	* @{ */
	std::shared_ptr<const ShaderDescription> FindShader(IUnknown* a_object);
	std::shared_ptr<const InputLayoutDescription> FindInputLayout(ID3D11InputLayout* a_object);
	/** @brief Null before its first draw. */
	std::shared_ptr<const InputLayoutDescription> FindEngineInputLayout(std::uint64_t a_key);
	/** @brief Described on first sight, so these answer for any live state object. */
	std::shared_ptr<const BlendDescription> FindBlend(ID3D11BlendState* a_object);
	std::shared_ptr<const DepthStencilDescription> FindDepthStencil(ID3D11DepthStencilState* a_object);
	std::shared_ptr<const RasterDescription> FindRaster(ID3D11RasterizerState* a_object);
	std::shared_ptr<const SamplerDescription> FindSampler(ID3D11SamplerState* a_object);
	std::shared_ptr<const QueryDescription> FindQuery(ID3D11Query* a_object);
	/** @} */

	/** @name The engine's state tables. Read live, since Deferred and ShadowmapRasterizerFix swap entries at runtime. Null when out of range.
	* @{ */
	ID3D11DepthStencilState* EngineDepthStencilState(std::uint32_t a_depthMode, std::uint32_t a_stencilMode);
	ID3D11RasterizerState* EngineRasterState(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_depthBias, std::uint32_t a_scissor);
	ID3D11BlendState* EngineBlendState(std::uint32_t a_mode, std::uint32_t a_alphaToCoverage, std::uint32_t a_writeMode, std::uint32_t a_extra);
	ID3D11SamplerState* EngineSamplerState(std::uint32_t a_addressMode, std::uint32_t a_filterMode);
	/** @} */

	/** @brief Latched from RendererShadowState after SetDirtyStates. */
	struct EngineDrawState
	{
		/** @brief CS's replacement shader when one is bound, else the engine's. */
		const RE::BSGraphics::VertexShader* vertexShader = nullptr;
		const RE::BSGraphics::PixelShader* pixelShader = nullptr;
		/** @brief The key the engine's input layout cache uses. */
		std::uint64_t inputLayoutKey = 0;
		std::uint32_t depthMode = 0;
		std::uint32_t stencilMode = 0;
		std::uint32_t stencilRef = 0;
		std::uint32_t fill = 0;
		std::uint32_t cull = 0;
		std::uint32_t depthBias = 0;
		std::uint32_t scissor = 0;
		std::uint32_t blendMode = 0;
		std::uint32_t alphaToCoverage = 0;
		std::uint32_t writeMode = 0;
		std::uint32_t blendExtra = 0;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

		/** @brief Render thread, or under Renderer::Lock on the loading-screen thread. */
		static EngineDrawState Latch();
	};

	/** @brief Null members were not found, and missing names them. */
	struct DrawPipelinePieces
	{
		std::shared_ptr<const ShaderDescription> vertexShader;
		std::shared_ptr<const ShaderDescription> pixelShader;
		std::shared_ptr<const InputLayoutDescription> inputLayout;
		std::shared_ptr<const BlendDescription> blend;
		std::shared_ptr<const DepthStencilDescription> depthStencil;
		std::shared_ptr<const RasterDescription> raster;
		rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
		std::uint32_t stencilRef = 0;  ///< command list state, not part of the pipeline
		std::string missing;

		bool Complete() const { return missing.empty(); }
	};

	/** @brief Render thread. a_state's shaders and table entries must be alive. */
	DrawPipelinePieces Resolve(const EngineDrawState& a_state);

	/** @brief Equal keys describe the same pipeline. */
	std::uint64_t PipelineKey(const DrawPipelinePieces& a_pieces, std::span<const std::uint32_t> a_strides, const rhi::RenderTargets& a_targets, rhi::Format a_depthFormat);

	/**
	* @brief The state half of a draw's pipeline. The caller adds the shaders, compiled per stage, because GraphicsPipelineDesc
	* has one define list and the engine's vertex and pixel shaders have different defines.
	*/
	org::GraphicsPipelineDesc PipelineStates(const DrawPipelinePieces& a_pieces, std::span<const std::uint32_t> a_strides, const rhi::RenderTargets& a_targets,
		rhi::Format a_depthFormat);

	/** @brief Sets each attribute's location to its semantic's position in the shader's input signature. Unnamed ones stay automatic. */
	void AssignLocations(org::PipelineInputLayout& a_layout, const Util::ShaderReflection& a_vertexShader);

#endif
}
