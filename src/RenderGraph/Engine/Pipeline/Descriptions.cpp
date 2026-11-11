#include "RenderGraph/Engine/Pipeline/Descriptions.h"

#include "RenderGraph/Switches.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include "Globals.h"
#	include "RenderGraph/Engine/Pipeline/StateConversion.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "State.h"

#	include <ORGModuleServices/ShaderStore.h>

#	include <d3dcompiler.h>

#	include <algorithm>
#	include <cctype>
#	include <cstring>
#	include <shared_mutex>
#	include <string_view>
#	include <type_traits>
#	include <unordered_map>
#endif

namespace RenderGraph::Engine::Pipeline
{
	bool Enabled()
	{
#if defined(CS_HAS_RENDER_GRAPH)
		static const bool enabled = Switches::On("CS_ORG_ENGINE_SHADERS");
		return enabled;
#else
		return false;
#endif
	}

#if defined(CS_HAS_RENDER_GRAPH)

	namespace
	{
		constexpr std::uint32_t kDepthModes = 6;
		constexpr std::uint32_t kStencilModes = 40;

		// The private-data key of the token that ties a description to its object.
		constexpr GUID kTokenGuid = { 0x5c1f3e2a, 0x7b4d, 0x4e61, { 0x9a, 0x52, 0x0d, 0x3f, 0x6e, 0x81, 0x24, 0xb7 } };

		enum class EntryKind : std::uint8_t
		{
			Shader,
			InputLayout,
			Blend,
			DepthStencil,
			Raster,
			Sampler,
			Query
		};

		struct Entry
		{
			EntryKind kind;
			std::shared_ptr<const void> description;
		};

		struct Registry
		{
			std::shared_mutex lock;
			std::unordered_map<const void*, Entry> entries;
			// BSGraphics::Renderer::CreateInputLayout's layouts by key. The engine never releases them.
			std::unordered_map<std::uint64_t, std::shared_ptr<const InputLayoutDescription>> layoutsByKey;
		};

		// Never destroyed: tokens are released when their objects go, which can be after static destruction.
		Registry& GetRegistry()
		{
			static auto* registry = new Registry;
			return *registry;
		}

		/** @brief Kept in an object's private data. Its last release (the object's destruction) removes the object's entry. */
		class Token final : public IUnknown
		{
		public:
			Token(const void* a_key, const void* a_description) :
				key(a_key), description(a_description) {}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
			{
				if (!a_out)
					return E_POINTER;
				if (a_riid == __uuidof(IUnknown)) {
					AddRef();
					*a_out = static_cast<IUnknown*>(this);
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = --refs;
				if (left == 0) {
					auto& registry = GetRegistry();
					{
						std::unique_lock lock(registry.lock);
						// A later description of the same object replaced this one: leave it.
						if (const auto found = registry.entries.find(key); found != registry.entries.end() && found->second.description.get() == description)
							registry.entries.erase(found);
					}
					delete this;
				}
				return left;
			}

		private:
			std::atomic<ULONG> refs{ 1 };
			const void* key;
			const void* description;
		};

		// The object a lookup keys on: a guest handle's wrapped object, else the object itself (a native handle included).
		template <class T>
		T* RealOf(T* a_object)
		{
			if (!a_object || !Tripwire::IsHandle(a_object))
				return a_object;
			const auto info = Tripwire::Describe(a_object);
			return info.wrapped ? static_cast<T*>(info.wrapped) : a_object;
		}

		// a_key is the pointer lookups pass (the interface the object was created as); a_object the same object as a device child.
		void Insert(const void* a_key, ID3D11DeviceChild* a_object, EntryKind a_kind, std::shared_ptr<const void> a_description)
		{
			const void* key = a_key;
			const void* raw = a_description.get();
			{
				auto& registry = GetRegistry();
				std::unique_lock lock(registry.lock);
				registry.entries.insert_or_assign(key, Entry{ a_kind, std::move(a_description) });
			}
			auto* token = new Token(key, raw);
			if (FAILED(a_object->SetPrivateDataInterface(kTokenGuid, token))) {
				static std::atomic<bool> warned{ false };
				if (!warned.exchange(true))
					logger::warn("[ORG] engine pipeline descriptions: an object refused private data, so its description outlives it");
			}
			token->Release();
		}

		template <class D>
		std::shared_ptr<const D> Lookup(const void* a_key, EntryKind a_kind)
		{
			if (!a_key)
				return nullptr;
			auto& registry = GetRegistry();
			std::shared_lock lock(registry.lock);
			const auto found = registry.entries.find(a_key);
			if (found == registry.entries.end() || found->second.kind != a_kind)
				return nullptr;
			return std::static_pointer_cast<const D>(found->second.description);
		}

		// A state object's description, made from GetDesc on first sight. It needs no recording (CS_ORG_ENGINE_SHADERS): the engine
		// draw families build their pipelines from these with the switch off too.
		template <class D, class Object, class Make>
		std::shared_ptr<const D> FindOrDescribe(Object* a_object, EntryKind a_kind, Make&& a_make)
		{
			if (!a_object)
				return nullptr;
			// The wrapped object, or the object itself; a native handle answers GetDesc too.
			auto* real = RealOf(a_object);
			if (auto found = Lookup<D>(real, a_kind))
				return found;
			auto description = std::make_shared<D>();
			real->GetDesc(&description->d3d);
			a_make(*description);
			// A native handle has no private data to tie the description to, so it is described on every lookup.
			if (!Tripwire::IsHandle(real))
				Insert(real, real, a_kind, description);
			return description;
		}

		const wchar_t* TargetOf(Stage a_stage)
		{
			switch (a_stage) {
			case Stage::Vertex:
				return L"vs_6_6";
			case Stage::Pixel:
				return L"ps_6_6";
			default:
				return L"cs_6_6";
			}
		}

		org::services::ShaderStoreHash Hash(std::span<const std::uint8_t> a_bytes)
		{
			return org::services::HashShaderBytes(std::as_bytes(a_bytes));
		}

		/** @brief FNV-1a over the values a pipeline key covers, field by field (D3D11 descriptions have padding). */
		class KeyHasher
		{
		public:
			template <class T>
			void Add(const T& a_value)
			{
				static_assert(std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>);
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
				for (std::size_t i = 0; i < sizeof(T); ++i) {
					value ^= bytes[i];
					value *= 0x100000001b3ull;
				}
			}
			void Add(std::string_view a_text)
			{
				Add(a_text.size());
				for (const char c : a_text)
					Add(c);
			}
			void Add(float a_value)
			{
				std::uint32_t bits;
				std::memcpy(&bits, &a_value, sizeof(bits));
				Add(bits);
			}

			std::uint64_t value = 0xcbf29ce484222325ull;
		};

		void AddShader(KeyHasher& a_hasher, const std::shared_ptr<const ShaderDescription>& a_shader)
		{
			a_hasher.Add(a_shader ? a_shader->hash.high : 0ull);
			a_hasher.Add(a_shader ? a_shader->hash.low : 0ull);
		}

		void AddBlend(KeyHasher& a_hasher, const D3D11_BLEND_DESC& a_desc)
		{
			a_hasher.Add(a_desc.AlphaToCoverageEnable);
			a_hasher.Add(a_desc.IndependentBlendEnable);
			for (const auto& target : a_desc.RenderTarget) {
				a_hasher.Add(target.BlendEnable);
				a_hasher.Add(static_cast<std::uint32_t>(target.SrcBlend));
				a_hasher.Add(static_cast<std::uint32_t>(target.DestBlend));
				a_hasher.Add(static_cast<std::uint32_t>(target.BlendOp));
				a_hasher.Add(static_cast<std::uint32_t>(target.SrcBlendAlpha));
				a_hasher.Add(static_cast<std::uint32_t>(target.DestBlendAlpha));
				a_hasher.Add(static_cast<std::uint32_t>(target.BlendOpAlpha));
				a_hasher.Add(target.RenderTargetWriteMask);
			}
		}

		void AddFace(KeyHasher& a_hasher, const D3D11_DEPTH_STENCILOP_DESC& a_face)
		{
			a_hasher.Add(static_cast<std::uint32_t>(a_face.StencilFailOp));
			a_hasher.Add(static_cast<std::uint32_t>(a_face.StencilDepthFailOp));
			a_hasher.Add(static_cast<std::uint32_t>(a_face.StencilPassOp));
			a_hasher.Add(static_cast<std::uint32_t>(a_face.StencilFunc));
		}

		void AddDepthStencil(KeyHasher& a_hasher, const D3D11_DEPTH_STENCIL_DESC& a_desc)
		{
			a_hasher.Add(a_desc.DepthEnable);
			a_hasher.Add(static_cast<std::uint32_t>(a_desc.DepthWriteMask));
			a_hasher.Add(static_cast<std::uint32_t>(a_desc.DepthFunc));
			a_hasher.Add(a_desc.StencilEnable);
			a_hasher.Add(a_desc.StencilReadMask);
			a_hasher.Add(a_desc.StencilWriteMask);
			AddFace(a_hasher, a_desc.FrontFace);
			AddFace(a_hasher, a_desc.BackFace);
		}

		// The scissor flag is left out: it is not pipeline state.
		void AddRaster(KeyHasher& a_hasher, const D3D11_RASTERIZER_DESC& a_desc)
		{
			a_hasher.Add(static_cast<std::uint32_t>(a_desc.FillMode));
			a_hasher.Add(static_cast<std::uint32_t>(a_desc.CullMode));
			a_hasher.Add(a_desc.FrontCounterClockwise);
			a_hasher.Add(a_desc.DepthBias);
			a_hasher.Add(a_desc.DepthBiasClamp);
			a_hasher.Add(a_desc.SlopeScaledDepthBias);
			a_hasher.Add(a_desc.DepthClipEnable);
		}

		void AddLayout(KeyHasher& a_hasher, const InputLayoutDescription* a_layout)
		{
			if (!a_layout) {
				a_hasher.Add(0u);
				return;
			}
			a_hasher.Add(static_cast<std::uint32_t>(a_layout->elements.size()));
			for (const auto& element : a_layout->elements) {
				a_hasher.Add(std::string_view(element.SemanticName ? element.SemanticName : ""));
				a_hasher.Add(element.SemanticIndex);
				a_hasher.Add(static_cast<std::uint32_t>(element.Format));
				a_hasher.Add(element.InputSlot);
				a_hasher.Add(element.AlignedByteOffset);
				a_hasher.Add(static_cast<std::uint32_t>(element.InputSlotClass));
				a_hasher.Add(element.InstanceDataStepRate);
			}
		}

		template <class T>
		T* TableEntry(REL::RelocationID a_id, std::size_t a_index)
		{
			const REL::Relocation<T**> table{ a_id };
			return table.get()[a_index];
		}
	}

	ShaderDescription::ShaderDescription(Stage a_stage, std::span<const std::uint8_t> a_bytecode) :
		stage(a_stage),
		bytecode(a_bytecode.begin(), a_bytecode.end()),
		hash(Hash(a_bytecode)),
		spirv(Util::DxcSpirvModule(std::as_bytes(a_bytecode)))
	{
	}

	const Util::ShaderReflection* ShaderDescription::Reflection() const
	{
		std::call_once(reflected, [this] {
			winrt::com_ptr<ID3DBlob> blob;
			if (bytecode.empty() || FAILED(D3DCreateBlob(bytecode.size(), blob.put())))
				return;
			std::memcpy(blob->GetBufferPointer(), bytecode.data(), bytecode.size());
			Util::ShaderReflection read;
			if (Util::ReflectShader(*blob, read))
				reflection = std::move(read);
		});
		return reflection ? &*reflection : nullptr;
	}

	std::optional<org::PipelineShaderSource> ShaderDescription::PipelineSource() const
	{
		const auto from = Source();
		if (!from)
			return std::nullopt;
		return org::PipelineShaderSource{ .path = from->path, .entry = L"main", .target = TargetOf(stage) };
	}

	std::shared_ptr<const ShaderDescription> RecordShader(Stage a_stage, IUnknown* a_object, std::span<const std::uint8_t> a_bytecode)
	{
		winrt::com_ptr<ID3D11DeviceChild> child;
		if (!Enabled() || !a_object || a_bytecode.empty() || FAILED(a_object->QueryInterface(IID_PPV_ARGS(child.put()))))
			return nullptr;
		auto description = std::make_shared<ShaderDescription>(a_stage, a_bytecode);
		Insert(a_object, child.get(), EntryKind::Shader, description);
		return description;
	}

	std::shared_ptr<const InputLayoutDescription> RecordInputLayout(ID3D11InputLayout* a_object, std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements,
		std::span<const std::uint8_t> a_vertexShaderBytecode)
	{
		if (!Enabled() || !a_object)
			return nullptr;
		auto description = std::make_shared<InputLayoutDescription>();
		description->semantics.reserve(a_elements.size());
		for (const auto& element : a_elements)
			description->semantics.emplace_back(element.SemanticName ? element.SemanticName : "");
		description->elements.assign(a_elements.begin(), a_elements.end());
		for (std::size_t i = 0; i < a_elements.size(); ++i)
			description->elements[i].SemanticName = description->semantics[i].c_str();
		description->vertexShaderHash = Hash(a_vertexShaderBytecode);
		description->layout = InputLayoutFromD3D11(description->elements, description->error);
		Insert(a_object, a_object, EntryKind::InputLayout, description);
		return description;
	}

	void RecordEngineInputLayoutKey(ID3D11InputLayout* a_object, std::uint64_t a_key)
	{
		const auto description = FindInputLayout(a_object);
		if (!description)
			return;
		description->engineKey.store(a_key, std::memory_order_relaxed);
		auto& registry = GetRegistry();
		std::unique_lock lock(registry.lock);
		registry.layoutsByKey.insert_or_assign(a_key, description);
	}

	std::shared_ptr<const InputLayoutDescription> FindEngineInputLayout(std::uint64_t a_key)
	{
		auto& registry = GetRegistry();
		std::shared_lock lock(registry.lock);
		const auto found = registry.layoutsByKey.find(a_key);
		return found != registry.layoutsByKey.end() ? found->second : nullptr;
	}

	void RecordShaderSource(IUnknown* a_object, ShaderSource a_source)
	{
		if (const auto description = FindShader(a_object))
			description->SetSource(std::make_shared<const ShaderSource>(std::move(a_source)));
	}

	std::shared_ptr<const ShaderDescription> FindShader(IUnknown* a_object)
	{
		return Lookup<ShaderDescription>(RealOf(a_object), EntryKind::Shader);
	}

	std::shared_ptr<const InputLayoutDescription> FindInputLayout(ID3D11InputLayout* a_object)
	{
		return Lookup<InputLayoutDescription>(RealOf(a_object), EntryKind::InputLayout);
	}

	std::shared_ptr<const BlendDescription> FindBlend(ID3D11BlendState* a_object)
	{
		return FindOrDescribe<BlendDescription>(a_object, EntryKind::Blend, [](BlendDescription& a_description) {
			BlendFromD3D11(a_description.d3d, a_description.rhi, a_description.error);
			a_description.readsBlendFactor = ReadsBlendFactor(a_description.d3d);
			a_description.dualSource = UsesDualSource(a_description.d3d);
		});
	}

	std::shared_ptr<const DepthStencilDescription> FindDepthStencil(ID3D11DepthStencilState* a_object)
	{
		return FindOrDescribe<DepthStencilDescription>(a_object, EntryKind::DepthStencil, [](DepthStencilDescription& a_description) {
			a_description.rhi = DepthStencilFromD3D11(a_description.d3d);
		});
	}

	std::shared_ptr<const RasterDescription> FindRaster(ID3D11RasterizerState* a_object)
	{
		return FindOrDescribe<RasterDescription>(a_object, EntryKind::Raster, [](RasterDescription& a_description) {
			a_description.rhi = RasterFromD3D11(a_description.d3d);
			a_description.scissorEnable = a_description.d3d.ScissorEnable != FALSE;
		});
	}

	std::shared_ptr<const SamplerDescription> FindSampler(ID3D11SamplerState* a_object)
	{
		return FindOrDescribe<SamplerDescription>(a_object, EntryKind::Sampler, [](SamplerDescription& a_description) {
			a_description.rhi = ShaderBindings::SamplerFromD3D11(a_description.d3d);
		});
	}

	std::shared_ptr<const QueryDescription> FindQuery(ID3D11Query* a_object)
	{
		return FindOrDescribe<QueryDescription>(a_object, EntryKind::Query, [](QueryDescription& a_description) {
			switch (a_description.d3d.Query) {
			case D3D11_QUERY_OCCLUSION:
				a_description.mapping = QueryMapping::Occlusion;
				break;
			case D3D11_QUERY_TIMESTAMP:
				a_description.mapping = QueryMapping::Timestamp;
				break;
			case D3D11_QUERY_TIMESTAMP_DISJOINT:
				a_description.mapping = QueryMapping::Disjoint;
				break;
			case D3D11_QUERY_EVENT:
				a_description.mapping = QueryMapping::Event;
				break;
			default:
				a_description.mapping = QueryMapping::Unsupported;
				break;
			}
		});
	}

	ID3D11DepthStencilState* EngineDepthStencilState(std::uint32_t a_depthMode, std::uint32_t a_stencilMode)
	{
		if (a_depthMode >= kDepthModes || a_stencilMode >= kStencilModes)
			return nullptr;
		return TableEntry<ID3D11DepthStencilState>(RELOCATION_ID(524747, 411362), a_depthMode * kStencilModes + a_stencilMode);
	}

	ID3D11RasterizerState* EngineRasterState(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_depthBias, std::uint32_t a_scissor)
	{
		if (a_fill >= 2 || a_cull >= 3 || a_depthBias >= 12 || a_scissor >= 2)
			return nullptr;
		return TableEntry<ID3D11RasterizerState>(RELOCATION_ID(524748, 411363), ((a_fill * 3 + a_cull) * 12 + a_depthBias) * 2 + a_scissor);
	}

	ID3D11BlendState* EngineBlendState(std::uint32_t a_mode, std::uint32_t a_alphaToCoverage, std::uint32_t a_writeMode, std::uint32_t a_extra)
	{
		if (a_mode >= 7 || a_alphaToCoverage >= 2 || a_writeMode >= 13 || a_extra >= 2)
			return nullptr;
		return TableEntry<ID3D11BlendState>(RELOCATION_ID(524749, 411364), ((a_mode * 2 + a_alphaToCoverage) * 13 + a_writeMode) * 2 + a_extra);
	}

	ID3D11SamplerState* EngineSamplerState(std::uint32_t a_addressMode, std::uint32_t a_filterMode)
	{
		if (a_addressMode >= 6 || a_filterMode >= 5)
			return nullptr;
		return TableEntry<ID3D11SamplerState>(RELOCATION_ID(524751, 411366), a_addressMode * 5 + a_filterMode);
	}

	EngineDrawState EngineDrawState::Latch()
	{
		EngineDrawState out;
		auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		const auto* state = globals::state;
		out.vertexShader = state && state->customVertexShader ? state->customVertexShader : shadow.currentVertexShader;
		out.pixelShader = state && state->customPixelShader ? state->customPixelShader : shadow.currentPixelShader;
		// The engine keys its layouts on its own current vertex shader (SetDirtyStates), even while CS's replacement is bound.
		out.inputLayoutKey = shadow.currentVertexShader ? (shadow.currentVertexShader->vertexDesc & shadow.vertexDesc) : 0;
		out.depthMode = static_cast<std::uint32_t>(shadow.depthStencilDepthMode);
		out.stencilMode = shadow.depthStencilStencilMode;
		out.stencilRef = shadow.stencilRef;
		out.fill = shadow.rasterStateFillMode;
		out.cull = shadow.rasterStateCullMode;
		out.depthBias = shadow.rasterStateDepthBiasMode;
		out.scissor = shadow.rasterStateScissorMode;
		out.blendMode = shadow.alphaBlendMode;
		out.alphaToCoverage = shadow.alphaBlendAlphaToCoverage;
		out.writeMode = shadow.alphaBlendWriteMode;
		out.blendExtra = shadow.alphaBlendModeExtra;
		out.topology = shadow.topology;
		return out;
	}

	DrawPipelinePieces Resolve(const EngineDrawState& a_state)
	{
		DrawPipelinePieces out;
		const auto note = [&](std::string_view a_what) {
			if (!out.missing.empty())
				out.missing += ", ";
			out.missing += a_what;
		};
		if (a_state.vertexShader)
			out.vertexShader = FindShader(reinterpret_cast<IUnknown*>(a_state.vertexShader->shader));
		if (!out.vertexShader)
			note("vertex shader");
		// No pixel shader is a depth-only draw, not a gap.
		if (a_state.pixelShader) {
			out.pixelShader = FindShader(reinterpret_cast<IUnknown*>(a_state.pixelShader->shader));
			if (!out.pixelShader)
				note("pixel shader");
		}
		out.inputLayout = FindEngineInputLayout(a_state.inputLayoutKey);
		if (!out.inputLayout)
			note("input layout");
		out.blend = FindBlend(EngineBlendState(a_state.blendMode, a_state.alphaToCoverage, a_state.writeMode, a_state.blendExtra));
		if (!out.blend || !out.blend->error.empty())
			note("blend state");
		out.depthStencil = FindDepthStencil(EngineDepthStencilState(a_state.depthMode, a_state.stencilMode));
		if (!out.depthStencil)
			note("depth-stencil state");
		out.raster = FindRaster(EngineRasterState(a_state.fill, a_state.cull, a_state.depthBias, a_state.scissor));
		if (!out.raster)
			note("rasterizer state");
		out.topology = TopologyFromD3D11(a_state.topology);
		if (out.topology == rhi::PrimitiveTopology::Unknown)
			note("topology");
		out.stencilRef = a_state.stencilRef;
		return out;
	}

	std::uint64_t PipelineKey(const DrawPipelinePieces& a_pieces, std::span<const std::uint32_t> a_strides, const rhi::RenderTargets& a_targets, rhi::Format a_depthFormat)
	{
		KeyHasher hasher;
		AddShader(hasher, a_pieces.vertexShader);
		AddShader(hasher, a_pieces.pixelShader);
		AddLayout(hasher, a_pieces.inputLayout.get());
		hasher.Add(static_cast<std::uint32_t>(a_strides.size()));
		for (const auto stride : a_strides)
			hasher.Add(stride);
		hasher.Add(a_pieces.blend ? 1u : 0u);
		if (a_pieces.blend)
			AddBlend(hasher, a_pieces.blend->d3d);
		hasher.Add(a_pieces.depthStencil ? 1u : 0u);
		if (a_pieces.depthStencil)
			AddDepthStencil(hasher, a_pieces.depthStencil->d3d);
		hasher.Add(a_pieces.raster ? 1u : 0u);
		if (a_pieces.raster)
			AddRaster(hasher, a_pieces.raster->d3d);
		hasher.Add(static_cast<std::uint32_t>(a_pieces.topology));
		hasher.Add(a_targets.count);
		for (std::uint32_t i = 0; i < a_targets.count && i < 8; ++i)
			hasher.Add(static_cast<std::uint32_t>(a_targets.formats[i]));
		hasher.Add(static_cast<std::uint32_t>(a_depthFormat));
		return hasher.value;
	}

	org::GraphicsPipelineDesc PipelineStates(const DrawPipelinePieces& a_pieces, std::span<const std::uint32_t> a_strides, const rhi::RenderTargets& a_targets,
		rhi::Format a_depthFormat)
	{
		org::GraphicsPipelineDesc out;
		out.name = "Engine draw";
		if (a_pieces.raster)
			out.raster = a_pieces.raster->rhi;
		if (a_pieces.blend && a_pieces.blend->error.empty())
			out.blend = a_pieces.blend->rhi;
		if (a_pieces.depthStencil)
			out.depth = a_pieces.depthStencil->rhi;
		out.renderTargets = a_targets;
		if (a_depthFormat != rhi::Format::Unknown)
			out.depthFormat = a_depthFormat;
		out.topology = a_pieces.topology;
		if (a_pieces.inputLayout) {
			auto layout = WithStrides(a_pieces.inputLayout->layout, a_strides);
			if (a_pieces.vertexShader) {
				if (const auto* reflection = a_pieces.vertexShader->Reflection())
					AssignLocations(layout, *reflection);
			}
			out.inputLayout = std::move(layout);
		}
		return out;
	}

	void AssignLocations(org::PipelineInputLayout& a_layout, const Util::ShaderReflection& a_vertexShader)
	{
		const auto equal = [](std::string_view a_left, std::string_view a_right) {
			return std::ranges::equal(a_left, a_right, [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); });
		};
		for (auto& attribute : a_layout.attributes) {
			std::uint32_t location = 0;
			for (const auto& input : a_vertexShader.inputs) {
				// System values (SV_VertexID, SV_InstanceID) take no vertex input location.
				if (input.semantic.size() >= 3 && equal(std::string_view(input.semantic).substr(0, 3), "SV_"))
					continue;
				if (input.index == attribute.semanticIndex && equal(input.semantic, attribute.semanticName)) {
					attribute.location = location;
					break;
				}
				++location;
			}
		}
	}

#endif
}
