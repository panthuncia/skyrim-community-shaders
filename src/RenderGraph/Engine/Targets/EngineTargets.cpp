#include "RenderGraph/Engine/Targets/EngineTargets.h"

#include "RenderGraph/Switches.h"

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Globals.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/D3D.h"

#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <atomic>
#	include <mutex>
#	include <string>
#	include <vector>

namespace RenderGraph::Engine::Targets
{
	namespace
	{
		using RE::BSGraphics::CubeMapRenderTargetProperties;
		using RE::BSGraphics::DepthStencilTargetProperties;
		using RE::BSGraphics::RenderTargetProperties;

		// Slots per table: room above the engine's counts (render targets SE 114 / AE 116, depth-stencils 12, one cubemap, three 3D).
		constexpr std::uint32_t kColourSlots = 128;
		constexpr std::uint32_t kDepthSlots = 32;
		constexpr std::uint32_t kCubeSlots = 4;
		constexpr std::uint32_t kVolumeSlots = 8;
		// DepthStencilData has eight DSVs and eight read-only DSVs: one per slice.
		constexpr std::uint32_t kMaxDepthSlices = 8;

		// The private-data key of the token each ORG-made D3D11 texture carries.
		constexpr GUID kRetireTokenKey{ 0x6c2f7a41, 0x93d5, 0x4c0e, { 0x8b, 0x1a, 0x52, 0x7e, 0x0d, 0x3f, 0x91, 0xc4 } };

		std::mutex retiredLock;
		std::vector<std::shared_ptr<const void>> retired;

		/**
		 * @brief Kept in a D3D11 texture's private data. DXVK releases it when it destroys the texture, after every view and
		 * reference is gone: then the image it keeps goes to the retire queue.
		 */
		class RetireToken final : public IUnknown
		{
		public:
			explicit RetireToken(std::shared_ptr<const void> a_owner) :
				owner(std::move(a_owner)) {}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_out) override
			{
				if (!a_out)
					return E_POINTER;
				if (a_iid != __uuidof(IUnknown)) {
					*a_out = nullptr;
					return E_NOINTERFACE;
				}
				AddRef();
				*a_out = static_cast<IUnknown*>(this);
				return S_OK;
			}

			ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }

			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
				if (left == 0) {
					if (owner) {
						std::scoped_lock lock(retiredLock);
						retired.push_back(std::move(owner));
					}
					delete this;
				}
				return left;
			}

			/** @brief Takes the image back, so the last Release retires nothing (the texture refused the token). */
			std::shared_ptr<const void> Disarm() { return std::move(owner); }

		private:
			std::atomic<ULONG> refs{ 1 };
			std::shared_ptr<const void> owner;
		};

		/**
		 * @brief An engine target's graph texture: a GraphResources twin whose D3D11 side is made by its owner, so PublishedResources
		 * publishes it by id and the anchors see its replacement through the resize generation.
		 */
		class EngineTexture final : public GraphResources::Resource
		{
		public:
			explicit EngineTexture(std::string a_name) :
				Resource(std::move(a_name), { GraphResources::Lifetime::Persistent, GraphResources::Export::SRV }) {}

			/** @brief Takes a new image and its SRV; the old one goes (its image lives while its D3D11 texture does). */
			void Adopt(const GraphResources::ExportedTexture& a_image, ID3D11ShaderResourceView* a_srv)
			{
				Drop();
				graph = a_image.graph;
				external = a_image.graph;
				d3d11 = a_image.d3d11;
				srv.copy_from(a_srv);
				image = a_image.image;
			}

			/** @brief Lets the image go: nothing of it is kept here any more. */
			void Drop()
			{
				external.reset();
				image = 0;
				Reset();
			}

			const std::shared_ptr<org::ExternalTextureResource>& External() const { return external; }
			const ID3D11Resource* D3D11Texture() const { return d3d11.get(); }
			std::uint64_t Image() const { return image; }

			/** @brief The resize generation moves once: the anchors build again and republish what changed. */
			static void BumpGeneration() { BumpResizeGeneration(); }

		private:
			std::shared_ptr<org::ExternalTextureResource> external;
			std::uint64_t image = 0;
		};

		struct Colour
		{
			RenderTargetProperties props{};  // the engine's last creation of the target
			bool known = false;
			std::unique_ptr<EngineTexture> texture;
			std::unique_ptr<EngineTexture> copy;
			// The UAV given to the engine. WindowSizeChanged releases a target's texture, views and copy but never its UAV.
			ID3D11UnorderedAccessView* uav = nullptr;
		};

		struct Depth
		{
			DepthStencilTargetProperties props{};
			bool known = false;
			std::unique_ptr<EngineTexture> texture;
		};

		struct Cube
		{
			CubeMapRenderTargetProperties props{};
			bool known = false;
			std::unique_ptr<EngineTexture> texture;
		};

		struct Volume
		{
			std::unique_ptr<EngineTexture> texture;
		};

		struct State
		{
			std::array<Colour, kColourSlots> colours;
			std::array<Depth, kDepthSlots> depths;
			std::array<Cube, kCubeSlots> cubes;
			std::array<Volume, kVolumeSlots> volumes;
			bool changed = false;  // a target was made on ORG since the last EndSet
			// Images whose texture refused the retire token: kept for the session rather than freed under DXVK.
			std::vector<std::shared_ptr<const void>> pinned;
		};

		State& Targets()
		{
			static State s_state;
			return s_state;
		}

		RE::BSGraphics::Renderer* Renderer() { return RE::BSGraphics::Renderer::GetSingleton(); }

		// The engine's tables, by index past CommonLib's array sizes (AE has 116 render targets where CommonLib declares 114).
		RE::BSGraphics::RenderTargetData& ColourData(std::uint32_t a_index) { return (&Renderer()->GetRuntimeData().renderTargets[0])[a_index]; }
		RE::BSGraphics::DepthStencilData& DepthData(std::uint32_t a_index) { return (&Renderer()->GetDepthStencilData().depthStencils[0])[a_index]; }
		RE::BSGraphics::CubemapRenderTargetData& CubeData(std::uint32_t a_index) { return (&Renderer()->GetRendererData().cubemapRenderTargets[0])[a_index]; }
		RE::BSGraphics::Texture3DTargetData& VolumeData(std::uint32_t a_index) { return (&Renderer()->GetRendererData().texture3DRenderTargets[0])[a_index]; }

		template <class E>
		std::string EnumName(std::uint32_t a_index, std::uint32_t a_count, const char* a_fallback)
		{
			if (a_index < a_count)
				if (const auto name = magic_enum::enum_name(static_cast<E>(a_index)); !name.empty())
					return std::string(name);
			return fmt::format("{}{}", a_fallback, a_index);
		}

		std::string ColourName(std::uint32_t a_index)
		{
			return "Engine::" + EnumName<RE::RENDER_TARGET>(a_index, RE::RENDER_TARGET::kTOTAL, "Target");
		}

		std::string DepthName(std::uint32_t a_index)
		{
			return "Engine::Depth::" + EnumName<RE::RENDER_TARGET_DEPTHSTENCIL>(a_index, RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL, "DepthStencil");
		}

		std::string CubeName(std::uint32_t a_index)
		{
			return "Engine::Cubemap::" + EnumName<RE::RENDER_TARGET_CUBEMAP>(a_index, RE::RENDER_TARGETS_CUBEMAP::kTOTAL, "Cubemap");
		}

		bool Active() { return RenderGraphRuntime::Get().IsActive(); }

		/**
		 * @brief Whether the engine's fields get tripwire handles: CS_ORG_ENGINE_TARGET_HANDLES=1, and the engine's device and immediate
		 * context are the tripwire's wrapping guests (P4.1). Without them a handle would reach DXVK's own device or context and crash there.
		 */
		bool UseHandles()
		{
			static const bool requested = Switches::On("CS_ORG_ENGINE_TARGET_HANDLES");
			if (!requested)
				return false;
			const auto& data = Renderer()->GetRuntimeData();
			const bool guests = Tripwire::IsHandle(static_cast<const void*>(data.context)) && Tripwire::IsHandle(static_cast<const void*>(data.forwarder));
			static std::atomic_flag logged;
			if (!guests && !logged.test_and_set())
				logger::warn("[ORG] engine targets: CS_ORG_ENGINE_TARGET_HANDLES=1, but the engine's device and context are not tripwire guests; the fields keep DXVK's objects");
			return guests;
		}

		template <class T>
		void ReleaseField(T*& a_field)
		{
			if (a_field) {
				a_field->Release();
				a_field = nullptr;
			}
		}

		/**
		 * @brief An image for an engine target, with the retire token on its D3D11 texture. Empty, logged, on failure. Dropping what
		 * this returns is safe at any point: the token retires the image once DXVK destroys the texture.
		 */
		GraphResources::ExportedTexture Export(const GraphResources::ExportDesc& a_desc, const std::string& a_name)
		{
			auto exported = GraphResources::ExportTexture(a_desc, a_name);
			if (!exported.graph || !exported.d3d11)
				return {};
			auto* token = new RetireToken(exported.graph);
			if (FAILED(exported.d3d11->SetPrivateDataInterface(kRetireTokenKey, token))) {
				Targets().pinned.push_back(token->Disarm());
				logger::warn("[ORG] {}: the D3D11 texture takes no retire token; its image is kept for the session", a_name);
			}
			token->Release();
			Util::SetResourceName(exported.d3d11.get(), "%s", a_name.c_str());
			return exported;
		}

		/** @brief Options for a tripwire handle of an engine target's object. */
		Tripwire::Options HandleOptions(const std::string& a_name, const std::shared_ptr<org::ExternalTextureResource>& a_graph)
		{
			Tripwire::Options options;
			options.name = a_name;
			options.graph = a_graph;
			return options;
		}

		// What goes into the engine's fields: DXVK's object, or a tripwire handle wrapping it (UseHandles). Each takes the reference
		// a_object holds. A view's handle names a_resource, the texture as the engine holds it, for GetResource.

		ID3D11Texture2D* Give(winrt::com_ptr<ID3D11Texture2D> a_object, const std::string& a_name, const std::shared_ptr<org::ExternalTextureResource>& a_graph, bool a_handles)
		{
			if (!a_object || !a_handles)
				return a_object.detach();
			D3D11_TEXTURE2D_DESC desc{};
			a_object->GetDesc(&desc);
			return Tripwire::CreateTexture2D(desc, HandleOptions(a_name, a_graph), a_object.get()).detach();
		}

		ID3D11Texture3D* Give(winrt::com_ptr<ID3D11Texture3D> a_object, const std::string& a_name, const std::shared_ptr<org::ExternalTextureResource>& a_graph, bool a_handles)
		{
			if (!a_object || !a_handles)
				return a_object.detach();
			D3D11_TEXTURE3D_DESC desc{};
			a_object->GetDesc(&desc);
			return Tripwire::CreateTexture3D(desc, HandleOptions(a_name, a_graph), a_object.get()).detach();
		}

		ID3D11ShaderResourceView* Give(winrt::com_ptr<ID3D11ShaderResourceView> a_object, ID3D11Resource* a_resource, const std::string& a_name,
			const std::shared_ptr<org::ExternalTextureResource>& a_graph, bool a_handles)
		{
			if (!a_object || !a_handles)
				return a_object.detach();
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			a_object->GetDesc(&desc);
			return Tripwire::CreateShaderResourceView(a_resource, desc, HandleOptions(a_name, a_graph), a_object.get()).detach();
		}

		ID3D11RenderTargetView* Give(winrt::com_ptr<ID3D11RenderTargetView> a_object, ID3D11Resource* a_resource, const std::string& a_name,
			const std::shared_ptr<org::ExternalTextureResource>& a_graph, bool a_handles)
		{
			if (!a_object || !a_handles)
				return a_object.detach();
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_object->GetDesc(&desc);
			return Tripwire::CreateRenderTargetView(a_resource, desc, HandleOptions(a_name, a_graph), a_object.get()).detach();
		}

		ID3D11DepthStencilView* Give(winrt::com_ptr<ID3D11DepthStencilView> a_object, ID3D11Resource* a_resource, const std::string& a_name,
			const std::shared_ptr<org::ExternalTextureResource>& a_graph, bool a_handles)
		{
			if (!a_object || !a_handles)
				return a_object.detach();
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			a_object->GetDesc(&desc);
			return Tripwire::CreateDepthStencilView(a_resource, desc, HandleOptions(a_name, a_graph), a_object.get()).detach();
		}

		ID3D11UnorderedAccessView* Give(winrt::com_ptr<ID3D11UnorderedAccessView> a_object, ID3D11Resource* a_resource, const std::string& a_name,
			const std::shared_ptr<org::ExternalTextureResource>& a_graph, bool a_handles)
		{
			if (!a_object || !a_handles)
				return a_object.detach();
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			a_object->GetDesc(&desc);
			return Tripwire::CreateUnorderedAccessView(a_resource, desc, HandleOptions(a_name, a_graph), a_object.get()).detach();
		}

		/** @brief Withdraws a target's id and lets its image go, when ORG made it before and D3D11 makes it now. */
		void Forget(std::unique_ptr<EngineTexture>& a_texture)
		{
			if (!a_texture || !a_texture->Graph())
				return;
			PublishedResources::Withdraw(a_texture->Name());
			a_texture->Drop();
		}

		/** @brief Publishes the new image of a target whose id a reader already asked for (Find); the others wait for their first request. */
		void Republish(const std::unique_ptr<EngineTexture>& a_texture)
		{
			if (a_texture && a_texture->Graph() && PublishedResources::IsRegistered(a_texture->Name()))
				PublishedResources::Publish(a_texture->Name(), *a_texture);
		}

		/** @brief The graph texture of a_texture, published on the first request. */
		std::shared_ptr<org::ExternalTextureResource> Serve(const std::unique_ptr<EngineTexture>& a_texture)
		{
			if (!a_texture || !a_texture->External())
				return nullptr;
			if (!PublishedResources::Publish(a_texture->Name(), *a_texture))
				return nullptr;
			return a_texture->External();
		}

		// --- Render targets (75467 / 77253) ---

		/** @brief A render target's shape: the engine's properties, or Recreate's description. */
		struct ColourShape
		{
			UINT width = 0;
			UINT height = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			bool srv = true;
			bool rtv = true;
			bool uav = false;
			bool copy = false;
		};

		ColourShape ShapeOf(const RenderTargetProperties& a_props)
		{
			return { a_props.width, a_props.height, static_cast<DXGI_FORMAT>(a_props.format.underlying()), true, true, a_props.supportUnorderedAccess, a_props.copyable };
		}

		/**
		 * @brief Whether ORG makes the target. The engine's own path stays for the swap chain's kFRAMEBUFFER, mip views of another
		 * target (mipLevel != -1: views of that target's texture) and mip generation, which DXVK cannot wrap.
		 */
		bool CanOwnColour(std::uint32_t a_index, const RenderTargetProperties& a_props)
		{
			return a_index != RE::RENDER_TARGET::kFRAMEBUFFER && a_props.mipLevel == -1 && !a_props.allowMipGeneration;
		}

		/** @brief The release WindowSizeChanged would have made: every field (the D3D11 path's objects, or ORG's when replaced). */
		void ReleaseColourFields(RE::BSGraphics::RenderTargetData& a_data, bool a_copy)
		{
			ReleaseField(a_data.UAV);
			ReleaseField(a_data.RTV);
			ReleaseField(a_data.SRV);
			ReleaseField(a_data.texture);
			if (a_copy) {
				ReleaseField(a_data.SRVCopy);
				ReleaseField(a_data.textureCopy);
			}
		}

		/** @brief How MakeColour treats what the engine's fields hold. */
		enum class Fields : std::uint8_t
		{
			Released,  // the engine released them (WindowSizeChanged) or never filled them: only the UAV it leaves is ours to release
			Replace,   // replace every field (TakeOver, Recreate without the copy kept)
			KeepCopy,  // replace the texture and its views, keep the copy (HDR Display's upgrade and its restore)
		};

		/**
		 * @brief Makes render target a_index on ORG as Renderer::CreateRenderTarget's own-texture branch does: an RGBA texture with an
		 * RTV and SRV of the whole texture (D3D11's default views), a UAV of mip 0 in the target's format when asked, and a copy
		 * texture (SRV bind only) with its SRV when copyable. False, with the fields untouched, when ORG cannot make it.
		 */
		bool MakeColour(std::uint32_t a_index, Colour& a_record, const ColourShape& a_shape, Fields a_fields)
		{
			const auto name = ColourName(a_index);
			auto* device = globals::d3d::device;
			GraphResources::ExportDesc desc{};
			desc.width = a_shape.width;
			desc.height = a_shape.height;
			desc.format = a_shape.format;
			desc.bindFlags = (a_shape.srv ? D3D11_BIND_SHADER_RESOURCE : 0u) | (a_shape.rtv ? D3D11_BIND_RENDER_TARGET : 0u) | (a_shape.uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
			desc.srv = a_shape.srv ? a_shape.format : DXGI_FORMAT_UNKNOWN;
			desc.rtv = a_shape.rtv ? a_shape.format : DXGI_FORMAT_UNKNOWN;
			desc.uav = a_shape.uav ? a_shape.format : DXGI_FORMAT_UNKNOWN;
			const auto main = Export(desc, name);
			auto texture = main.d3d11 ? main.d3d11.try_as<ID3D11Texture2D>() : nullptr;
			if (!texture || !device)
				return false;
			winrt::com_ptr<ID3D11RenderTargetView> rtv;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
			if (a_shape.rtv && FAILED(device->CreateRenderTargetView(texture.get(), nullptr, rtv.put())))
				return false;
			if (a_shape.srv && FAILED(device->CreateShaderResourceView(texture.get(), nullptr, srv.put())))
				return false;
			if (a_shape.uav) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.Format = a_shape.format;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				if (FAILED(device->CreateUnorderedAccessView(texture.get(), &uavDesc, uav.put())))
					return false;
			}
			GraphResources::ExportedTexture copy;
			winrt::com_ptr<ID3D11Texture2D> copyTexture;
			winrt::com_ptr<ID3D11ShaderResourceView> copySrv;
			const bool makeCopy = a_shape.copy && a_fields != Fields::KeepCopy;
			if (makeCopy) {
				auto copyDesc = desc;
				copyDesc.bindFlags = D3D11_BIND_SHADER_RESOURCE;
				copyDesc.srv = a_shape.format;
				copyDesc.rtv = DXGI_FORMAT_UNKNOWN;
				copyDesc.uav = DXGI_FORMAT_UNKNOWN;
				copy = Export(copyDesc, name + " Copy");
				copyTexture = copy.d3d11 ? copy.d3d11.try_as<ID3D11Texture2D>() : nullptr;
				if (!copyTexture || FAILED(device->CreateShaderResourceView(copyTexture.get(), nullptr, copySrv.put())))
					return false;
			}
			Util::SetResourceName(rtv.get(), "%s RTV", name.c_str());
			Util::SetResourceName(srv.get(), "%s SRV", name.c_str());
			Util::SetResourceName(uav.get(), "%s UAV", name.c_str());
			Util::SetResourceName(copySrv.get(), "%s Copy SRV", name.c_str());

			auto& data = ColourData(a_index);
			if (a_fields == Fields::Released) {
				if (a_record.uav && data.UAV == a_record.uav)
					ReleaseField(data.UAV);
			} else {
				ReleaseColourFields(data, a_fields == Fields::Replace);
			}
			// The records first: they keep their own references; the fields then take the ones made here.
			if (!a_record.texture)
				a_record.texture = std::make_unique<EngineTexture>(name);
			a_record.texture->Adopt(main, srv.get());
			if (makeCopy) {
				if (!a_record.copy)
					a_record.copy = std::make_unique<EngineTexture>(name + " Copy");
				a_record.copy->Adopt(copy, copySrv.get());
			} else if (a_fields != Fields::KeepCopy) {
				Forget(a_record.copy);
			}
			const bool handles = UseHandles();
			data.texture = Give(std::move(texture), name, main.graph, handles);
			data.RTV = Give(std::move(rtv), data.texture, name + " RTV", main.graph, handles);
			data.SRV = Give(std::move(srv), data.texture, name + " SRV", main.graph, handles);
			data.UAV = Give(std::move(uav), data.texture, name + " UAV", main.graph, handles);
			if (makeCopy) {
				data.textureCopy = Give(std::move(copyTexture), name + " Copy", copy.graph, handles);
				data.SRVCopy = Give(std::move(copySrv), data.textureCopy, name + " Copy SRV", copy.graph, handles);
			}
			a_record.uav = data.UAV;
			Targets().changed = true;
			return true;
		}

		struct CreateRenderTarget
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_target, const char* a_name, RenderTargetProperties* a_properties)
			{
				if (!a_properties || a_target >= kColourSlots) {
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				auto& record = Targets().colours[a_target];
				record.props = *a_properties;
				record.known = true;
				if (CanOwnColour(a_target, *a_properties) && Active() && MakeColour(a_target, record, ShapeOf(*a_properties), Fields::Released))
					return;
				// The D3D11 path: before the graph exists (TakeOver follows), for the targets ORG does not make, and on a failure.
				if (record.texture && record.texture->Graph())
					logger::info("[ORG] {}: made on D3D11 again", record.texture->Name());
				Forget(record.texture);
				Forget(record.copy);
				// The UAV ORG made last time is the one field the engine's release leaves.
				if (auto& data = ColourData(a_target); record.uav && data.UAV == record.uav)
					ReleaseField(data.UAV);
				record.uav = nullptr;
				func(a_renderer, a_target, a_name, a_properties);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// --- Depth-stencil targets (75469 / 77255) ---

		struct DepthFormats
		{
			DXGI_FORMAT texture, dsv, srv;
		};

		/** @brief CreateDepthStencilTarget's formats: 32-bit float depth, 16-bit with use16BitsDepth, D24S8 with a stencil (which wins). */
		DepthFormats FormatsOf(const DepthStencilTargetProperties& a_props)
		{
			if (a_props.stencil)
				return { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS };
			if (a_props.use16BitsDepth)
				return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM };
			return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT };
		}

		/** @brief The release ZeroDepthStencilData makes, for a replacement the engine did not release itself. */
		void ReleaseDepthFields(RE::BSGraphics::DepthStencilData& a_data)
		{
			for (auto& view : a_data.views)
				ReleaseField(view);
			for (auto& view : a_data.readOnlyViews)
				ReleaseField(view);
			ReleaseField(a_data.depthSRV);
			ReleaseField(a_data.stencilSRV);
			ReleaseField(a_data.texture);
		}

		/**
		 * @brief Makes depth-stencil target a_index on ORG as Renderer::CreateDepthStencilTarget does: a typeless texture of arraySize
		 * slices; per slice a DSV and a read-only DSV (read-only depth, and stencil when it has one); a depth SRV of every slice; a
		 * stencil SRV (X24_TYPELESS_G8_UINT) with a stencil. A 2D view for one slice, 2D-array views for more.
		 */
		bool MakeDepth(std::uint32_t a_index, Depth& a_record, bool a_replace)
		{
			const auto& props = a_record.props;
			const auto name = DepthName(a_index);
			const auto formats = FormatsOf(props);
			const UINT slices = props.arraySize;
			auto* device = globals::d3d::device;
			if (!device || slices == 0 || slices > kMaxDepthSlices)
				return false;
			GraphResources::ExportDesc desc{};
			desc.width = props.width;
			desc.height = props.height;
			desc.depthOrArraySize = slices;
			desc.format = formats.texture;
			desc.bindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			desc.srv = formats.srv;
			desc.dsv = formats.dsv;
			const auto image = Export(desc, name);
			auto texture = image.d3d11 ? image.d3d11.try_as<ID3D11Texture2D>() : nullptr;
			if (!texture)
				return false;
			const bool array = slices > 1;
			std::array<winrt::com_ptr<ID3D11DepthStencilView>, kMaxDepthSlices> views, readOnlyViews;
			for (UINT slice = 0; slice < slices; ++slice) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
				dsv.Format = formats.dsv;
				if (array) {
					dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
					dsv.Texture2DArray.FirstArraySlice = slice;
					dsv.Texture2DArray.ArraySize = 1;
				} else {
					dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
				}
				if (FAILED(device->CreateDepthStencilView(texture.get(), &dsv, views[slice].put())))
					return false;
				dsv.Flags = D3D11_DSV_READ_ONLY_DEPTH | (props.stencil ? D3D11_DSV_READ_ONLY_STENCIL : 0u);
				if (FAILED(device->CreateDepthStencilView(texture.get(), &dsv, readOnlyViews[slice].put())))
					return false;
				Util::SetResourceName(views[slice].get(), "%s DSV %u", name.c_str(), slice);
				Util::SetResourceName(readOnlyViews[slice].get(), "%s read-only DSV %u", name.c_str(), slice);
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = formats.srv;
			if (array) {
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				srvDesc.Texture2DArray.MipLevels = 1;
				srvDesc.Texture2DArray.ArraySize = slices;
			} else {
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
			}
			winrt::com_ptr<ID3D11ShaderResourceView> depthSrv, stencilSrv;
			if (FAILED(device->CreateShaderResourceView(texture.get(), &srvDesc, depthSrv.put())))
				return false;
			if (props.stencil) {
				srvDesc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
				if (FAILED(device->CreateShaderResourceView(texture.get(), &srvDesc, stencilSrv.put())))
					return false;
			}
			Util::SetResourceName(depthSrv.get(), "%s SRV", name.c_str());
			Util::SetResourceName(stencilSrv.get(), "%s stencil SRV", name.c_str());

			auto& data = DepthData(a_index);
			if (a_replace)
				ReleaseDepthFields(data);
			if (!a_record.texture)
				a_record.texture = std::make_unique<EngineTexture>(name);
			a_record.texture->Adopt(image, depthSrv.get());
			const bool handles = UseHandles();
			data.texture = Give(std::move(texture), name, image.graph, handles);
			for (UINT slice = 0; slice < slices; ++slice) {
				data.views[slice] = Give(std::move(views[slice]), data.texture, fmt::format("{} DSV {}", name, slice), image.graph, handles);
				data.readOnlyViews[slice] = Give(std::move(readOnlyViews[slice]), data.texture, fmt::format("{} read-only DSV {}", name, slice), image.graph, handles);
			}
			data.depthSRV = Give(std::move(depthSrv), data.texture, name + " SRV", image.graph, handles);
			data.stencilSRV = Give(std::move(stencilSrv), data.texture, name + " stencil SRV", image.graph, handles);
			Targets().changed = true;
			return true;
		}

		struct CreateDepthStencilTarget
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_target, const char* a_name, DepthStencilTargetProperties* a_properties)
			{
				if (!a_properties || a_target >= kDepthSlots) {
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				auto& record = Targets().depths[a_target];
				record.props = *a_properties;
				record.known = true;
				if (Active() && MakeDepth(a_target, record, false))
					return;
				Forget(record.texture);
				func(a_renderer, a_target, a_name, a_properties);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// --- Cubemap targets (75471 / 77257) ---

		void ReleaseCubeFields(RE::BSGraphics::CubemapRenderTargetData& a_data)
		{
			for (auto& view : a_data.cubeSideRTV)
				ReleaseField(view);
			ReleaseField(a_data.SRV);
			ReleaseField(a_data.texture);
		}

		/** @brief Makes cubemap target a_index on ORG as Renderer::CreateCubeMapRenderTarget does: six slices, an RTV per face, a cube SRV. */
		bool MakeCube(std::uint32_t a_index, Cube& a_record, bool a_replace)
		{
			const auto& props = a_record.props;
			const auto name = CubeName(a_index);
			const auto format = static_cast<DXGI_FORMAT>(props.format.underlying());
			auto* device = globals::d3d::device;
			if (!device)
				return false;
			GraphResources::ExportDesc desc{};
			desc.width = props.width;
			desc.height = props.height;
			desc.depthOrArraySize = 6;
			desc.format = format;
			desc.bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			desc.miscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
			desc.srv = format;
			desc.rtv = format;
			const auto image = Export(desc, name);
			auto texture = image.d3d11 ? image.d3d11.try_as<ID3D11Texture2D>() : nullptr;
			if (!texture)
				return false;
			std::array<winrt::com_ptr<ID3D11RenderTargetView>, 6> faces;
			for (UINT face = 0; face < 6; ++face) {
				D3D11_RENDER_TARGET_VIEW_DESC rtv{};
				rtv.Format = format;
				rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				rtv.Texture2DArray.FirstArraySlice = face;
				rtv.Texture2DArray.ArraySize = 1;
				if (FAILED(device->CreateRenderTargetView(texture.get(), &rtv, faces[face].put())))
					return false;
				Util::SetResourceName(faces[face].get(), "%s RTV %u", name.c_str(), face);
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
			srvDesc.TextureCube.MipLevels = 1;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			if (FAILED(device->CreateShaderResourceView(texture.get(), &srvDesc, srv.put())))
				return false;
			Util::SetResourceName(srv.get(), "%s SRV", name.c_str());

			auto& data = CubeData(a_index);
			if (a_replace)
				ReleaseCubeFields(data);
			if (!a_record.texture)
				a_record.texture = std::make_unique<EngineTexture>(name);
			a_record.texture->Adopt(image, srv.get());
			const bool handles = UseHandles();
			data.texture = Give(std::move(texture), name, image.graph, handles);
			for (UINT face = 0; face < 6; ++face)
				data.cubeSideRTV[face] = Give(std::move(faces[face]), data.texture, fmt::format("{} RTV {}", name, face), image.graph, handles);
			data.SRV = Give(std::move(srv), data.texture, name + " SRV", image.graph, handles);
			Targets().changed = true;
			return true;
		}

		struct CreateCubeMapRenderTarget
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_target, const char* a_name, CubeMapRenderTargetProperties* a_properties)
			{
				if (!a_properties || a_target >= kCubeSlots) {
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				auto& record = Targets().cubes[a_target];
				record.props = *a_properties;
				record.known = true;
				if (Active() && MakeCube(a_target, record, false))
					return;
				Forget(record.texture);
				func(a_renderer, a_target, a_name, a_properties);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// --- 3D targets (75470 / 77256): the volumetric lighting's accumulation volumes and noise, made when it renders ---

		/**
		 * @brief Texture3DTargetData as Renderer::Create3DRenderTarget fills it (CommonLib names the fields unk*): the texture, its UAV
		 * (every depth slice of mip 0), its SRV, and a dword set to 1 once the texture exists.
		 */
		struct VolumeFields
		{
			ID3D11Texture3D* texture;            // 00
			ID3D11UnorderedAccessView* uav;      // 08
			ID3D11ShaderResourceView* srv;       // 10
			std::uint32_t created;               // 18
			std::uint32_t pad1C;                 // 1C
		};
		static_assert(sizeof(VolumeFields) == sizeof(RE::BSGraphics::Texture3DTargetData));

		/**
		 * @brief Makes 3D target a_index on ORG: DEFAULT usage, SRV and UAV bind, one mip. What the fields held is left as the engine
		 * leaves it (Create3DRenderTarget overwrites them too).
		 */
		bool MakeVolume(std::uint32_t a_index, Volume& a_record, const char* a_label, UINT a_width, UINT a_height, UINT a_depth, DXGI_FORMAT a_format)
		{
			const auto name = a_label && *a_label ? fmt::format("Engine::3D::{}", a_label) : fmt::format("Engine::3D::Volume{}", a_index);
			auto* device = globals::d3d::device;
			if (!device)
				return false;
			GraphResources::ExportDesc desc{};
			desc.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
			desc.width = a_width;
			desc.height = a_height;
			desc.depthOrArraySize = a_depth;
			desc.format = a_format;
			desc.bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc.srv = a_format;
			desc.uav = a_format;
			const auto image = Export(desc, name);
			auto texture = image.d3d11 ? image.d3d11.try_as<ID3D11Texture3D>() : nullptr;
			if (!texture)
				return false;
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = a_format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			srvDesc.Texture3D.MipLevels = 1;
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = a_format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
			uavDesc.Texture3D.WSize = a_depth;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
			if (FAILED(device->CreateShaderResourceView(texture.get(), &srvDesc, srv.put())) ||
				FAILED(device->CreateUnorderedAccessView(texture.get(), &uavDesc, uav.put())))
				return false;
			Util::SetResourceName(srv.get(), "%s SRV", name.c_str());
			Util::SetResourceName(uav.get(), "%s UAV", name.c_str());

			auto& data = *reinterpret_cast<VolumeFields*>(&VolumeData(a_index));
			if (!a_record.texture)
				a_record.texture = std::make_unique<EngineTexture>(name);
			a_record.texture->Adopt(image, srv.get());
			const bool handles = UseHandles();
			data.texture = Give(std::move(texture), name, image.graph, handles);
			data.srv = Give(std::move(srv), data.texture, name + " SRV", image.graph, handles);
			data.uav = Give(std::move(uav), data.texture, name + " UAV", image.graph, handles);
			data.created = 1;
			// Made outside a set: the readers see it at the anchors' next build.
			EngineTexture::BumpGeneration();
			Republish(a_record.texture);
			return true;
		}

		struct Create3DRenderTarget
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_target, const char* a_name, std::uint32_t a_width, std::uint32_t a_height,
				std::uint32_t a_depth, std::int32_t a_usage, std::uint32_t a_format)
			{
				// DEFAULT usage only: the dynamic and staging variants are CPU-visible, which an exported image is not.
				if (a_target < kVolumeSlots && a_usage == D3D11_USAGE_DEFAULT && Active() &&
					MakeVolume(a_target, Targets().volumes[a_target], a_name, a_width, a_height, a_depth, static_cast<DXGI_FORMAT>(a_format)))
					return;
				if (a_target < kVolumeSlots)
					Forget(Targets().volumes[a_target].texture);
				func(a_renderer, a_target, a_name, a_width, a_height, a_depth, a_usage, a_format);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Calls f on every target record's graph textures. */
		template <class F>
		void ForEachTexture(F&& a_function)
		{
			auto& state = Targets();
			for (auto& colour : state.colours) {
				a_function(colour.texture);
				a_function(colour.copy);
			}
			for (auto& depth : state.depths)
				a_function(depth.texture);
			for (auto& cube : state.cubes)
				a_function(cube.texture);
			for (auto& volume : state.volumes)
				a_function(volume.texture);
		}
	}

	bool Enabled()
	{
		static const bool enabled = Switches::On("CS_ORG_ENGINE_TARGETS");
		return enabled;
	}

	void Install()
	{
		if (!Enabled())
			return;
		logger::info("[ORG] engine targets: hooking the render target creation functions (CS_ORG_ENGINE_TARGETS=1)");
		stl::detour_thunk<CreateRenderTarget>(REL::RelocationID(75467, 77253));
		stl::detour_thunk<CreateDepthStencilTarget>(REL::RelocationID(75469, 77255));
		stl::detour_thunk<Create3DRenderTarget>(REL::RelocationID(75470, 77256));
		stl::detour_thunk<CreateCubeMapRenderTarget>(REL::RelocationID(75471, 77257));
	}

	void BeginSet()
	{
		Targets().changed = false;
	}

	void EndSet()
	{
		auto& state = Targets();
		if (!state.changed || !Active())
			return;
		state.changed = false;
		std::uint32_t colours = 0, depths = 0;
		for (const auto& colour : state.colours)
			colours += colour.texture && colour.texture->Graph() ? 1u : 0u;
		for (const auto& depth : state.depths)
			depths += depth.texture && depth.texture->Graph() ? 1u : 0u;
		// One move for the whole set: the anchors build again once and PublishedResources publishes every replaced id again.
		ForEachTexture([](const std::unique_ptr<EngineTexture>& a_texture) { Republish(a_texture); });
		EngineTexture::BumpGeneration();
		logger::info("[ORG] engine targets: the set was made again on the render graph ({} render targets, {} depth-stencil targets)", colours, depths);
	}

	void TakeOver()
	{
		if (!Enabled())
			return;
		if (!Active()) {
			logger::warn("[ORG] engine targets: the render graph is not active ({}); the engine's targets stay on D3D11", RenderGraphRuntime::Get().GetDisabledReason());
			return;
		}
		auto& state = Targets();
		std::uint32_t taken = 0, kept = 0, mipViews = 0;
		for (std::uint32_t i = 0; i < kColourSlots; ++i) {
			auto& record = state.colours[i];
			if (!record.known || (record.texture && record.texture->Graph()) || !CanOwnColour(i, record.props))
				continue;
			if (MakeColour(i, record, ShapeOf(record.props), Fields::Replace))
				++taken;
			else
				++kept;
		}
		// Targets that view a mip of another (mipLevel, textureTarget): their views are made again on the owner's new texture by
		// the engine's own function, as its next Create would.
		for (std::uint32_t i = 0; i < kColourSlots; ++i) {
			auto& record = state.colours[i];
			const auto source = record.props.textureTarget;
			if (!record.known || record.props.mipLevel == -1 || source >= kColourSlots || !state.colours[source].texture || !state.colours[source].texture->Graph())
				continue;
			auto& data = ColourData(i);
			ReleaseField(data.UAV);
			ReleaseField(data.RTV);
			ReleaseField(data.SRV);
			auto props = record.props;
			CreateRenderTarget::func(Renderer(), i, "RenderTarget", &props);
			++mipViews;
		}
		for (std::uint32_t i = 0; i < kDepthSlots; ++i) {
			auto& record = state.depths[i];
			if (!record.known || (record.texture && record.texture->Graph()))
				continue;
			if (MakeDepth(i, record, true))
				++taken;
			else
				++kept;
		}
		for (std::uint32_t i = 0; i < kCubeSlots; ++i) {
			auto& record = state.cubes[i];
			if (!record.known || (record.texture && record.texture->Graph()))
				continue;
			if (MakeCube(i, record, true))
				++taken;
			else
				++kept;
		}
		state.changed = false;
		ForEachTexture([](const std::unique_ptr<EngineTexture>& a_texture) { Republish(a_texture); });
		EngineTexture::BumpGeneration();
		logger::info("[ORG] engine targets: {} targets now on the render graph, {} mip views made again on them, {} kept on D3D11 after a failure", taken, mipViews, kept);
	}

	bool Owns(std::uint32_t a_target)
	{
		if (!Enabled() || a_target >= kColourSlots)
			return false;
		const auto& texture = Targets().colours[a_target].texture;
		return texture && texture->Graph();
	}

	bool Recreate(std::uint32_t a_target, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, bool a_keepCopy)
	{
		if (!Owns(a_target) || !Active())
			return false;
		if (a_desc.Usage != D3D11_USAGE_DEFAULT || a_desc.CPUAccessFlags || a_desc.MiscFlags || a_desc.ArraySize != 1 || a_desc.MipLevels > 1 ||
			a_desc.SampleDesc.Count > 1)
			return false;
		auto& record = Targets().colours[a_target];
		ColourShape shape{ a_desc.Width, a_desc.Height, a_desc.Format, (a_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0,
			(a_desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0, (a_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0, false };
		if (!MakeColour(a_target, record, shape, a_keepCopy ? Fields::KeepCopy : Fields::Replace))
			return false;
		Targets().changed = false;
		Republish(record.texture);
		EngineTexture::BumpGeneration();
		logger::info("[ORG] {}: made again as {} ({}x{}, format {})", record.texture->Name(), a_name ? a_name : "?", a_desc.Width, a_desc.Height, static_cast<int>(a_desc.Format));
		return true;
	}

	bool Restore(std::uint32_t a_target)
	{
		if (!Owns(a_target) || !Active())
			return false;
		auto& record = Targets().colours[a_target];
		auto shape = ShapeOf(record.props);
		shape.copy = false;
		if (!MakeColour(a_target, record, shape, Fields::KeepCopy))
			return false;
		Targets().changed = false;
		Republish(record.texture);
		EngineTexture::BumpGeneration();
		return true;
	}

	std::shared_ptr<org::ExternalTextureResource> Find(const ID3D11Resource* a_texture)
	{
		if (!a_texture || !Enabled())
			return nullptr;
		const auto* texture = Tripwire::Unwrap(const_cast<ID3D11Resource*>(a_texture));
		if (!texture)
			return nullptr;
		std::shared_ptr<org::ExternalTextureResource> found;
		ForEachTexture([&](const std::unique_ptr<EngineTexture>& a_record) {
			if (!found && a_record && a_record->D3D11Texture() == texture)
				found = Serve(a_record);
		});
		return found;
	}

	std::shared_ptr<org::ExternalTextureResource> FindImage(std::uint64_t a_image)
	{
		if (!a_image || !Enabled())
			return nullptr;
		std::shared_ptr<org::ExternalTextureResource> found;
		ForEachTexture([&](const std::unique_ptr<EngineTexture>& a_record) {
			if (!found && a_record && a_record->Image() == a_image)
				found = Serve(a_record);
		});
		return found;
	}

	void ServiceRetired()
	{
		std::vector<std::shared_ptr<const void>> owners;
		{
			std::scoped_lock lock(retiredLock);
			owners.swap(retired);
		}
		auto& runtime = RenderGraphRuntime::Get();
		for (auto& owner : owners)
			runtime.ReleaseAfterD3D11(std::move(owner));
	}
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the engine makes every target with D3D11.
namespace RenderGraph::Engine::Targets
{
	bool Enabled() { return false; }
	void Install() {}
	void BeginSet() {}
	void EndSet() {}
	void TakeOver() {}
	bool Owns(std::uint32_t) { return false; }
	bool Recreate(std::uint32_t, const D3D11_TEXTURE2D_DESC&, const char*, bool) { return false; }
	bool Restore(std::uint32_t) { return false; }
	std::shared_ptr<org::ExternalTextureResource> Find(const ID3D11Resource*) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> FindImage(std::uint64_t) { return nullptr; }
	void ServiceRetired() {}
}

#endif
