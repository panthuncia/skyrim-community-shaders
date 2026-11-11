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
 * @brief Descriptions of the engine's shaders, input layouts, state objects and queries (org-full-port.md P4.6), for building ORG
 * pipelines from latched engine state in Phase 5.
 *
 * CS_ORG_ENGINE_SHADERS=1 turns the recording on (EngineHooks.h installs it). Every shader and input layout the device creates is
 * described when it is created. State objects and queries are described from GetDesc the first time they are looked up, with or
 * without the switch (the engine draw families build their pipelines from them). A description lives as long as its D3D11 object: a
 * token kept in the object's private data removes it when the object goes.
 *
 * Every Find takes a tripwire handle or a D3D11 object, and returns null for an object nothing described. Callable from any
 * thread. RE: docs/development/p4/p4.6-shaders-states.md.
 */
namespace RenderGraph::Engine::Pipeline
{
	/** @brief Whether CS_ORG_ENGINE_SHADERS=1 (and the render graph is built in). Read once. */
	bool Enabled();

#if defined(CS_HAS_RENDER_GRAPH)

	enum class Stage : std::uint8_t
	{
		Vertex,
		Pixel,
		Compute
	};

	/** @brief Where a shader came from, when CS knows: its HLSL and how ShaderCache compiled it. */
	struct ShaderSource
	{
		std::uint32_t shaderType = 0;  ///< RE::BSShader::Type
		std::uint32_t descriptor = 0;
		std::wstring path;  ///< The HLSL file under Data/Shaders.
		std::vector<std::pair<std::string, std::string>> defines;  ///< ShaderCache::GetCompileDefines at creation.
	};

	/** @brief A shader as it was created: its bytecode, the SPIR-V of a DXC build, and its source when CS compiled it. */
	class ShaderDescription
	{
	public:
		ShaderDescription(Stage a_stage, std::span<const std::uint8_t> a_bytecode);
		ShaderDescription(const ShaderDescription&) = delete;
		ShaderDescription& operator=(const ShaderDescription&) = delete;

		const Stage stage;
		const std::vector<std::uint8_t> bytecode;
		/** @brief org::services::HashShaderBytes of the bytecode. */
		const org::services::ShaderStoreHash hash;
		/** @brief The module of a DXC container (CS_SHADER_DXC builds); empty for FXC's DXBC. Its bindings follow DXVK's ABI. */
		const std::vector<std::uint32_t> spirv;

		/** @brief Constant buffers and input semantics (Util::ReflectShader), read on first use; null when it cannot be read. */
		const Util::ShaderReflection* Reflection() const;

		/** @brief The source, set by ShaderCache for CS's shaders; null for the engine's own (vanilla DXBC). */
		std::shared_ptr<const ShaderSource> Source() const { return source.load(std::memory_order_acquire); }

		/** @brief Records the source. Called once, after creation. */
		void SetSource(std::shared_ptr<const ShaderSource> a_source) const { source.store(std::move(a_source), std::memory_order_release); }

		/** @brief The stage as an ORG pipeline shader (path, "main", the stage's 6.6 target); nothing without a source. */
		std::optional<org::PipelineShaderSource> PipelineSource() const;

	private:
		mutable std::once_flag reflected;
		mutable std::optional<Util::ShaderReflection> reflection;
		mutable std::atomic<std::shared_ptr<const ShaderSource>> source;
	};

	/** @brief An input layout: the D3D11 elements, the ORG layout they make, and the engine's key when the engine made it. */
	struct InputLayoutDescription
	{
		std::vector<std::string> semantics;
		std::vector<D3D11_INPUT_ELEMENT_DESC> elements;  ///< SemanticName points into semantics.
		org::services::ShaderStoreHash vertexShaderHash;  ///< The bytecode the layout was validated against.
		org::PipelineInputLayout layout;  ///< Strides 0: the geometry's VertexDesc gives them (WithStrides).
		std::string error;  ///< An element BasicRHI cannot express (left out of layout).
		/** @brief The VertexDesc key (vertex shader mask & geometry desc) BSGraphics::Renderer::CreateInputLayout made it for; ~0 otherwise. */
		mutable std::atomic<std::uint64_t> engineKey{ ~0ull };
	};

	struct BlendDescription
	{
		D3D11_BLEND_DESC d3d{};
		rhi::BlendState rhi{};
		bool readsBlendFactor = false;  ///< The engine's factor is always 1 (kEngineBlendFactor).
		bool dualSource = false;
		std::string error;  ///< Non-empty: rhi is unusable.
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
		bool scissorEnable = false;  ///< No rhi field: with false the pass sets a scissor that covers the target.
	};

	struct SamplerDescription
	{
		D3D11_SAMPLER_DESC d3d{};  ///< As created, with the anisotropy cap (CapSamplerAnisotropy).
		rhi::SamplerDesc rhi{};  ///< ShaderBindings::SamplerFromD3D11.
	};

	/** @brief What a query becomes on ORG (p4.6-shaders-states.md, "What Phase 5 does with them"). */
	enum class QueryMapping : std::uint8_t
	{
		Occlusion,  ///< An rhi QueryType::Occlusion slot, resolved to a readback.
		Timestamp,  ///< An rhi QueryType::Timestamp slot.
		Disjoint,  ///< No object: the queue's timestamp frequency, never disjoint.
		Event,  ///< A timeline value the CPU waits on.
		Unsupported  ///< Predicates, statistics, stream-out: the engine creates none.
	};

	struct QueryDescription
	{
		D3D11_QUERY_DESC d3d{};
		QueryMapping mapping = QueryMapping::Unsupported;
	};

	/** @brief The blend factor BSGraphics::SetDirtyStates passes (SE 512153 / AE 388963): never written. */
	inline constexpr float kEngineBlendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	/** @name Recording (EngineHooks and ShaderCache). Each replaces an earlier description of the same object.
	 * @{ */
	std::shared_ptr<const ShaderDescription> RecordShader(Stage a_stage, IUnknown* a_object, std::span<const std::uint8_t> a_bytecode);
	std::shared_ptr<const InputLayoutDescription> RecordInputLayout(ID3D11InputLayout* a_object, std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements,
		std::span<const std::uint8_t> a_vertexShaderBytecode);
	/** @brief Adds the source of a shader ShaderCache built. Nothing when the shader was not recorded. */
	void RecordShaderSource(IUnknown* a_object, ShaderSource a_source);
	/** @brief Marks a layout BSGraphics::Renderer::CreateInputLayout made for a_key (FindEngineInputLayout). */
	void RecordEngineInputLayoutKey(ID3D11InputLayout* a_object, std::uint64_t a_key);
	/** @} */

	/** @name Lookups. a_object may be a tripwire handle (its wrapped object is looked up) or a D3D11 object.
	 * @{ */
	std::shared_ptr<const ShaderDescription> FindShader(IUnknown* a_object);
	std::shared_ptr<const InputLayoutDescription> FindInputLayout(ID3D11InputLayout* a_object);
	/** @brief The layout the engine made for a VertexDesc key (EngineDrawState::inputLayoutKey); null before its first draw. */
	std::shared_ptr<const InputLayoutDescription> FindEngineInputLayout(std::uint64_t a_key);
	/** @brief The state lookups describe an object on first sight (GetDesc), so they answer for any live state object. */
	std::shared_ptr<const BlendDescription> FindBlend(ID3D11BlendState* a_object);
	std::shared_ptr<const DepthStencilDescription> FindDepthStencil(ID3D11DepthStencilState* a_object);
	std::shared_ptr<const RasterDescription> FindRaster(ID3D11RasterizerState* a_object);
	std::shared_ptr<const SamplerDescription> FindSampler(ID3D11SamplerState* a_object);
	std::shared_ptr<const QueryDescription> FindQuery(ID3D11Query* a_object);
	/** @} */

	/** @name The engine's state tables (SetDirtyStates' indexing; p4.6-shaders-states.md "State tables"). Read now: Deferred and
	 * ShadowmapRasterizerFix swap entries at runtime. Null for an index out of range.
	 * @{ */
	ID3D11DepthStencilState* EngineDepthStencilState(std::uint32_t a_depthMode, std::uint32_t a_stencilMode);
	ID3D11RasterizerState* EngineRasterState(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_depthBias, std::uint32_t a_scissor);
	ID3D11BlendState* EngineBlendState(std::uint32_t a_mode, std::uint32_t a_alphaToCoverage, std::uint32_t a_writeMode, std::uint32_t a_extra);
	ID3D11SamplerState* EngineSamplerState(std::uint32_t a_addressMode, std::uint32_t a_filterMode);
	/** @} */

	/** @brief The engine state a draw's pipeline depends on, latched from RendererShadowState after SetDirtyStates. */
	struct EngineDrawState
	{
		/** @brief The shaders bound: CS's replacement (State::customVertexShader/customPixelShader) when one is, else the engine's. */
		const RE::BSGraphics::VertexShader* vertexShader = nullptr;
		const RE::BSGraphics::PixelShader* pixelShader = nullptr;
		/** @brief RSS.currentVertexShader->vertexDesc & RSS.vertexDesc: the key the engine's input layout cache uses. */
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

		/** @brief Reads the shadow state. Render thread (or under Renderer::Lock on the loading-screen thread). */
		static EngineDrawState Latch();
	};

	/** @brief The descriptions a draw's pipeline is made of. Null members were not found; missing names them. */
	struct DrawPipelinePieces
	{
		std::shared_ptr<const ShaderDescription> vertexShader;
		std::shared_ptr<const ShaderDescription> pixelShader;
		std::shared_ptr<const InputLayoutDescription> inputLayout;
		std::shared_ptr<const BlendDescription> blend;
		std::shared_ptr<const DepthStencilDescription> depthStencil;
		std::shared_ptr<const RasterDescription> raster;
		rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
		std::uint32_t stencilRef = 0;  ///< Command list state (SetStencilRef), not part of the pipeline.
		std::string missing;

		bool Complete() const { return missing.empty(); }
	};

	/** @brief The descriptions of a latched draw state. A_state's shaders and table entries must be alive (render thread). */
	DrawPipelinePieces Resolve(const EngineDrawState& a_state);

	/**
	 * @brief A key for org::PipelinePermutations: equal keys describe the same pipeline. It hashes the shaders' bytecode hashes, the
	 * states' D3D11 descriptions, the layout, the strides, the topology and the target formats.
	 */
	std::uint64_t PipelineKey(const DrawPipelinePieces& a_pieces, std::span<const std::uint32_t> a_strides, const rhi::RenderTargets& a_targets, rhi::Format a_depthFormat);

	/**
	 * @brief The state half of a draw's ORG pipeline: raster, blend, depth-stencil, topology, targets and the input layout with
	 * a_strides. The caller adds the shaders: ORG's GraphicsPipelineDesc has one define list for every stage, and an engine vertex
	 * and pixel shader have different defines, so each stage is compiled on its own (as DCLF's ShaderPrograms does).
	 */
	org::GraphicsPipelineDesc PipelineStates(const DrawPipelinePieces& a_pieces, std::span<const std::uint32_t> a_strides, const rhi::RenderTargets& a_targets,
		rhi::Format a_depthFormat);

	/**
	 * @brief Gives a_layout's attributes the Vulkan locations a vertex shader reads them at: the position of their semantic in the
	 * shader's input signature. Attributes the signature does not name keep automatic locations.
	 */
	void AssignLocations(org::PipelineInputLayout& a_layout, const Util::ShaderReflection& a_vertexShader);

#endif
}
