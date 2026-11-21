#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/StateConversion.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/ShaderBindings.h"
#include "State.h"

#include <shared_mutex>
#include <string_view>
#include <unordered_map>

namespace RenderGraph::Engine::Pipeline
{

	namespace
	{
		constexpr std::uint32_t kDepthModes = 6;
		constexpr std::uint32_t kStencilModes = 40;

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
		};

		// Never destroyed, since tokens are released when their objects go, which can be after static destruction.
		Registry& GetRegistry()
		{
			static auto* registry = new Registry;
			return *registry;
		}

		/** @brief Its last release, at the object's destruction, removes the object's entry. */
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
						// A later description of the same object replaced this one, so leave it.
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

		// A guest handle's wrapped object, else the object itself, native handles included.
		template <class T>
		T* RealOf(T* a_object)
		{
			if (!a_object || !Tripwire::IsHandle(a_object))
				return a_object;
			IUnknown* const wrapped = Tripwire::WrappedOf(a_object);
			return wrapped ? static_cast<T*>(wrapped) : a_object;
		}

		// a_key is the interface pointer lookups pass. a_object is the same object as a device child.
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

		// Described from GetDesc on first sight, even without CS_ORG_ENGINE_SHADERS, because the engine draw families need it.
		template <class D, class Object, class Make>
		std::shared_ptr<const D> FindOrDescribe(Object* a_object, EntryKind a_kind, Make&& a_make)
		{
			if (!a_object)
				return nullptr;
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

		template <class T>
		T* TableEntry(REL::RelocationID a_id, std::size_t a_index)
		{
			const REL::Relocation<T**> table{ a_id };
			return table.get()[a_index];
		}
	}

	ShaderDescription::ShaderDescription(Stage a_stage, std::span<const std::uint8_t> a_bytecode) :
		stage(a_stage),
		bytecode(a_bytecode.begin(), a_bytecode.end())
	{
	}

	std::shared_ptr<const ShaderDescription> RecordShader(Stage a_stage, IUnknown* a_object, std::span<const std::uint8_t> a_bytecode)
	{
		winrt::com_ptr<ID3D11DeviceChild> child;
		if (!GraphMode() || !a_object || a_bytecode.empty() || FAILED(a_object->QueryInterface(IID_PPV_ARGS(child.put()))))
			return nullptr;
		auto description = std::make_shared<ShaderDescription>(a_stage, a_bytecode);
		Insert(a_object, child.get(), EntryKind::Shader, description);
		return description;
	}

	std::shared_ptr<const InputLayoutDescription> RecordInputLayout(ID3D11InputLayout* a_object, std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements)
	{
		if (!GraphMode() || !a_object)
			return nullptr;
		auto description = std::make_shared<InputLayoutDescription>();
		description->semantics.reserve(a_elements.size());
		for (const auto& element : a_elements)
			description->semantics.emplace_back(element.SemanticName ? element.SemanticName : "");
		description->elements.assign(a_elements.begin(), a_elements.end());
		for (std::size_t i = 0; i < a_elements.size(); ++i)
			description->elements[i].SemanticName = description->semantics[i].c_str();
		Insert(a_object, a_object, EntryKind::InputLayout, description);
		return description;
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
		// The engine keys layouts on its own current vertex shader, even while CS's replacement is bound.
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
}
