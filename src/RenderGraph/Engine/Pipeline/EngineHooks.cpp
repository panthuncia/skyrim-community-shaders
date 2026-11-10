#include "RenderGraph/Engine/Pipeline/EngineHooks.h"

#include "RenderGraph/Engine/Pipeline/Descriptions.h"

#include <algorithm>

#if defined(CS_HAS_RENDER_GRAPH)
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
// P4.1's guest context. A branch without it never installs handles.
#	if __has_include("RenderGraph/Engine/Tripwire/GuestDevice.h")
#		include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#		define CS_ORG_ENGINE_HANDLES_GUEST_CONTEXT 1
#	endif

#	include <format>
#	include <span>
#	include <unordered_map>
#endif

namespace RenderGraph::Engine::Pipeline
{
	namespace
	{
		constexpr UINT kMaxSamplerAnisotropy = 8;

		struct ID3D11Device_CreateSamplerState
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const D3D11_SAMPLER_DESC* a_desc, ID3D11SamplerState** a_out)
			{
				if (!a_desc)
					return func(a_this, a_desc, a_out);
				// A copy: the caller's description is immutable.
				const auto capped = CapSamplerAnisotropy(*a_desc);
				return func(a_this, &capped, a_out);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

#if defined(CS_HAS_RENDER_GRAPH)
		std::span<const std::uint8_t> Bytes(const void* a_data, SIZE_T a_size)
		{
			return a_data ? std::span(static_cast<const std::uint8_t*>(a_data), a_size) : std::span<const std::uint8_t>();
		}

		struct ID3D11Device_CreateVertexShader
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const void* a_bytecode, SIZE_T a_size, ID3D11ClassLinkage* a_linkage, ID3D11VertexShader** a_out)
			{
				const HRESULT result = func(a_this, a_bytecode, a_size, a_linkage, a_out);
				if (SUCCEEDED(result) && a_out && *a_out)
					RecordShader(Stage::Vertex, *a_out, Bytes(a_bytecode, a_size));
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ID3D11Device_CreatePixelShader
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const void* a_bytecode, SIZE_T a_size, ID3D11ClassLinkage* a_linkage, ID3D11PixelShader** a_out)
			{
				const HRESULT result = func(a_this, a_bytecode, a_size, a_linkage, a_out);
				if (SUCCEEDED(result) && a_out && *a_out)
					RecordShader(Stage::Pixel, *a_out, Bytes(a_bytecode, a_size));
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ID3D11Device_CreateComputeShader
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const void* a_bytecode, SIZE_T a_size, ID3D11ClassLinkage* a_linkage, ID3D11ComputeShader** a_out)
			{
				const HRESULT result = func(a_this, a_bytecode, a_size, a_linkage, a_out);
				if (SUCCEEDED(result) && a_out && *a_out)
					RecordShader(Stage::Compute, *a_out, Bytes(a_bytecode, a_size));
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ID3D11Device_CreateInputLayout
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const D3D11_INPUT_ELEMENT_DESC* a_elements, UINT a_count, const void* a_bytecode, SIZE_T a_size,
				ID3D11InputLayout** a_out)
			{
				const HRESULT result = func(a_this, a_elements, a_count, a_bytecode, a_size, a_out);
				if (SUCCEEDED(result) && a_out && *a_out && a_elements)
					RecordInputLayout(*a_out, std::span(a_elements, a_count), Bytes(a_bytecode, a_size));
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Whether handles go into engine fields now: only through P4.1's guest context, which unwraps them for DXVK.
		bool HandlesWanted()
		{
#	if defined(CS_ORG_ENGINE_HANDLES_GUEST_CONTEXT)
			return Tripwire::GuestContextInstalled();
#	else
			return false;
#	endif
		}

		/**
		 * @brief Puts a handle wrapping *a_field into a_field. The handle takes over the field's reference: the engine's later Release
		 * of the field frees the handle, which releases the object.
		 */
		template <class Interface, class Make>
		void WrapField(Interface*& a_field, Make&& a_make)
		{
			Interface* real = a_field;
			if (!real || Tripwire::IsHandle(real))
				return;
			winrt::com_ptr<Interface> handle = a_make(real);
			if (!handle)
				return;
			a_field = handle.detach();
			real->Release();
		}

		Tripwire::Options OptionsFor(std::string a_name, std::shared_ptr<const void> a_owner)
		{
			Tripwire::Options options;
			options.name = std::move(a_name);
			options.owner = std::move(a_owner);
			return options;
		}

		template <class Field>
		void WrapShader(Field& a_field, Stage a_stage, std::uint32_t a_id)
		{
			if (!HandlesWanted())
				return;
			auto& field = reinterpret_cast<IUnknown*&>(a_field);
			const auto description = FindShader(field);
			const auto bytecode = description ? std::span<const std::uint8_t>(description->bytecode) : std::span<const std::uint8_t>();
			switch (a_stage) {
			case Stage::Vertex:
				WrapField(reinterpret_cast<ID3D11VertexShader*&>(field), [&](ID3D11VertexShader* a_real) {
					return Tripwire::CreateVertexShader(bytecode, OptionsFor(std::format("Engine::VertexShader {:X}", a_id), description), a_real);
				});
				break;
			case Stage::Pixel:
				WrapField(reinterpret_cast<ID3D11PixelShader*&>(field), [&](ID3D11PixelShader* a_real) {
					return Tripwire::CreatePixelShader(bytecode, OptionsFor(std::format("Engine::PixelShader {:X}", a_id), description), a_real);
				});
				break;
			case Stage::Compute:
				WrapField(reinterpret_cast<ID3D11ComputeShader*&>(field), [&](ID3D11ComputeShader* a_real) {
					return Tripwire::CreateComputeShader(bytecode, OptionsFor(std::format("Engine::ComputeShader {:X}", a_id), description), a_real);
				});
				break;
			}
		}

		struct LoadVertexShader
		{
			static RE::BSGraphics::VertexShader* thunk(void* a_renderer, void* a_stream)
			{
				auto* shader = func(a_renderer, a_stream);
				if (shader)
					WrapShader(shader->shader, Stage::Vertex, shader->id);
				return shader;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Bink's tile shaders (BSMovie::CreateTileTextures).
		struct CreateVertexShader
		{
			static RE::BSGraphics::VertexShader* thunk(void* a_renderer, const void* a_bytecode, std::uint64_t a_size, void* a_unused, std::uint64_t a_vertexDesc)
			{
				auto* shader = func(a_renderer, a_bytecode, a_size, a_unused, a_vertexDesc);
				if (shader)
					WrapShader(shader->shader, Stage::Vertex, shader->id);
				return shader;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CreatePixelShader
		{
			static RE::BSGraphics::PixelShader* thunk(void* a_renderer, const void* a_bytecode, std::uint64_t a_size)
			{
				auto* shader = func(a_renderer, a_bytecode, a_size);
				if (shader)
					WrapShader(shader->shader, Stage::Pixel, shader->id);
				return shader;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LoadPixelShader
		{
			static RE::BSGraphics::PixelShader* thunk(void* a_renderer, void* a_stream)
			{
				auto* shader = func(a_renderer, a_stream);
				if (shader)
					WrapShader(shader->shader, Stage::Pixel, shader->id);
				return shader;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LoadComputeShader
		{
			static RE::BSGraphics::ComputeShader* thunk(void* a_renderer, void* a_stream)
			{
				auto* shader = func(a_renderer, a_stream);
				if (shader)
					WrapShader(shader->shader, Stage::Compute, shader->id);
				return shader;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The layout SetDirtyStates caches for a VertexDesc key.
		struct EngineCreateInputLayout
		{
			static ID3D11InputLayout* thunk(std::uint64_t a_key)
			{
				ID3D11InputLayout* layout = func(a_key);
				if (!layout)
					return layout;
				RecordEngineInputLayoutKey(layout, a_key);
				if (HandlesWanted()) {
					const auto description = FindInputLayout(layout);
					const auto elements = description ? std::span<const D3D11_INPUT_ELEMENT_DESC>(description->elements) : std::span<const D3D11_INPUT_ELEMENT_DESC>();
					WrapField(layout, [&](ID3D11InputLayout* a_real) {
						return Tripwire::CreateInputLayout(elements, {}, OptionsFor(std::format("Engine::InputLayout {:X}", a_key), description), a_real);
					});
				}
				return layout;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Wraps every entry of a table of state objects or queries. DXVK hands out one object per description, so entries
		 * repeat: each distinct object gets one handle, and each entry's reference moves to it.
		 */
		template <class Interface, class Make>
		std::size_t WrapTable(Interface** a_table, std::size_t a_count, std::size_t a_stride, Make&& a_make)
		{
			std::unordered_map<Interface*, Interface*> handles;
			for (std::size_t i = 0; i < a_count; ++i) {
				auto*& entry = *reinterpret_cast<Interface**>(reinterpret_cast<std::uint8_t*>(a_table) + i * a_stride);
				Interface* real = entry;
				if (!real || Tripwire::IsHandle(real))
					continue;
				auto& handle = handles[real];
				if (!handle) {
					handle = a_make(real, i).detach();
					if (!handle)
						continue;
				} else {
					handle->AddRef();
				}
				entry = handle;
				real->Release();
			}
			return handles.size();
		}

		template <class Interface>
		Interface** Table(REL::RelocationID a_id)
		{
			return REL::Relocation<Interface**>{ a_id }.get();
		}

		void InstallEngineHandles()
		{
			if (!HandlesWanted()) {
				logger::info("[ORG] engine pipeline descriptions: no guest context, so the engine keeps its D3D11 state objects and queries");
				return;
			}
			const auto depthStencil = WrapTable(Table<ID3D11DepthStencilState>(RELOCATION_ID(524747, 411362)), 6 * 40, sizeof(void*),
				[](ID3D11DepthStencilState* a_real, std::size_t a_index) {
					const auto description = FindDepthStencil(a_real);
					return Tripwire::CreateDepthStencilState(description->d3d,
						OptionsFor(std::format("Engine::DepthStencil {}/{}", a_index / 40, a_index % 40), description), a_real);
				});
			const auto raster = WrapTable(Table<ID3D11RasterizerState>(RELOCATION_ID(524748, 411363)), 2 * 3 * 12 * 2, sizeof(void*),
				[](ID3D11RasterizerState* a_real, std::size_t a_index) {
					const auto description = FindRaster(a_real);
					return Tripwire::CreateRasterizerState(description->d3d, OptionsFor(std::format("Engine::Raster {}", a_index), description), a_real);
				});
			const auto blend = WrapTable(Table<ID3D11BlendState>(RELOCATION_ID(524749, 411364)), 7 * 2 * 13 * 2, sizeof(void*),
				[](ID3D11BlendState* a_real, std::size_t a_index) {
					const auto description = FindBlend(a_real);
					return Tripwire::CreateBlendState(description->d3d, OptionsFor(std::format("Engine::Blend {}", a_index), description), a_real);
				});
			const auto samplers = WrapTable(Table<ID3D11SamplerState>(RELOCATION_ID(524751, 411366)), 6 * 5, sizeof(void*),
				[](ID3D11SamplerState* a_real, std::size_t a_index) {
					const auto description = FindSampler(a_real);
					return Tripwire::CreateSamplerState(description->d3d,
						OptionsFor(std::format("Engine::Sampler {}/{}", a_index / 5, a_index % 5), description), a_real);
				});
			const auto makeQuery = [](const char* a_pool) {
				return [a_pool](ID3D11Query* a_real, std::size_t a_index) {
					const auto description = FindQuery(a_real);
					return Tripwire::CreateQuery(description->d3d, OptionsFor(std::format("Engine::{} {}", a_pool, a_index), description), a_real);
				};
			};
			// 64 occlusion queries, then the 6 profiling frames' disjoint queries, 0x50 bytes apart.
			const auto occlusion = WrapTable(Table<ID3D11Query>(RELOCATION_ID(524740, 411344)), 64, sizeof(void*), makeQuery("OcclusionQuery"));
			const auto disjoint = WrapTable(Table<ID3D11Query>(RELOCATION_ID(524952, 411440)), 6, 0x50, makeQuery("DisjointQuery"));
			logger::info("[ORG] engine pipeline descriptions: handles for {} depth-stencil, {} rasterizer, {} blend, {} sampler states and {} queries",
				depthStencil, raster, blend, samplers, occlusion + disjoint);
		}

		struct RendererInit
		{
			static void thunk(void* a_renderer, void* a_osData, void* a_windowProperties, int a_flags)
			{
				func(a_renderer, a_osData, a_windowProperties, a_flags);
				InstallEngineHandles();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
#endif
	}

	D3D11_SAMPLER_DESC CapSamplerAnisotropy(const D3D11_SAMPLER_DESC& a_desc)
	{
		D3D11_SAMPLER_DESC capped = a_desc;
		capped.MaxAnisotropy = (std::min)(capped.MaxAnisotropy, kMaxSamplerAnisotropy);
		return capped;
	}

	void InstallDeviceHooks(ID3D11Device* a_device)
	{
		if (!a_device)
			return;
		stl::detour_vfunc<23, ID3D11Device_CreateSamplerState>(a_device);
#if defined(CS_HAS_RENDER_GRAPH)
		if (!Enabled())
			return;
		logger::info("[ORG] engine pipeline descriptions: recording shader and input layout creation (CS_ORG_ENGINE_SHADERS=1)");
		stl::detour_vfunc<11, ID3D11Device_CreateInputLayout>(a_device);
		stl::detour_vfunc<12, ID3D11Device_CreateVertexShader>(a_device);
		stl::detour_vfunc<15, ID3D11Device_CreatePixelShader>(a_device);
		stl::detour_vfunc<18, ID3D11Device_CreateComputeShader>(a_device);
#endif
	}

	void InstallGameHooks()
	{
#if defined(CS_HAS_RENDER_GRAPH)
		if (!Enabled())
			return;
		logger::info("[ORG] engine pipeline descriptions: hooking BSGraphics::Renderer's shader, input layout and state creation");
		stl::detour_thunk<LoadVertexShader>(REL::RelocationID(75547, 77348));
		stl::detour_thunk<CreateVertexShader>(REL::RelocationID(75548, 77349));
		stl::detour_thunk<CreatePixelShader>(REL::RelocationID(75551, 77352));
		stl::detour_thunk<LoadPixelShader>(REL::RelocationID(75552, 77353));
		stl::detour_thunk<LoadComputeShader>(REL::RelocationID(75556, 77357));
		stl::detour_thunk<EngineCreateInputLayout>(REL::RelocationID(75583, 77389));
		stl::detour_thunk<RendererInit>(REL::RelocationID(75445, 77226));
#endif
	}
}
