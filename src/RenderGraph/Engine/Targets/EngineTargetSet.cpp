// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Engine/Targets/EngineTargetSet.h"

#include "Globals.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Targets/EngineTargets.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ImageRegistry.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "Utils/D3D.h"

#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace RenderGraph::Engine::Targets
{
	namespace
	{
		using RE::BSGraphics::CubeMapRenderTargetProperties;
		using RE::BSGraphics::DepthStencilTargetProperties;
		using RE::BSGraphics::RenderTargetProperties;
		using RT = RE::RENDER_TARGET;
		using Frame::Vars::Extent2D;

		constexpr std::string_view kWriter = "engine target set";
		constexpr std::uint32_t kFramebuffer = RT::kFRAMEBUFFER;
		constexpr std::uint32_t kMain = RT::kMAIN;
		constexpr std::uint32_t kNoiseVolume = RE::RENDER_TARGETS_3D::kVOLUMETRIC_LIGHTING_NOISE;

		// --- The targets nothing uses (design 3.6). They have no memory in both modes. ---

		/** @brief A render target kept without memory, and why nothing uses it. */
		struct UnbackedColour
		{
			std::uint32_t target;
			const char* users;  // who would use it, and what they use instead
		};

		// Every target not listed is backed.
		constexpr std::array kUnbackedColours{
			UnbackedColour{ RT::kSAO, "none: the image-space port draws SAO into ImageSpace::SAO (Frame::Resources)" },
			UnbackedColour{ RT::kSAO_RAWAO, "none: the raw AO history is ImageSpace::SAORawAO0/1 (Frame::Resources)" },
			UnbackedColour{ RT::kSAO_RAWAO_PREVIOUS, "none: the raw AO history is ImageSpace::SAORawAO0/1 (Frame::Resources)" },
			UnbackedColour{ RT::kSAO_TEMP_BLUR, "none: the SAO blur draws into ImageSpace::SAOBlurTemp" },
			UnbackedColour{ RT::kTEMPORAL_AA_ACCUMULATION_1, "the engine's clear at a TAA reset only (dropped); TAA's history is ImageSpace::TAAHistory0/1" },
			UnbackedColour{ RT::kTEMPORAL_AA_ACCUMULATION_2, "the engine's clear at a TAA reset only (dropped); TAA's history is ImageSpace::TAAHistory0/1" },
			UnbackedColour{ RT::kTEMPORAL_AA_UI_ACCUMULATION_1, "the engine's clear at a TAA reset only (dropped); TAA's UI form has no CS pass" },
			UnbackedColour{ RT::kTEMPORAL_AA_UI_ACCUMULATION_2, "the engine's clear at a TAA reset only (dropped); TAA's UI form has no CS pass" },
			UnbackedColour{ RT::kIMAGESPACE_TEMP_COPY, "none: the post chain works in place, with ImageSpace::HdrScratch for its copies" },
			UnbackedColour{ RT::kIMAGESPACE_TEMP_COPY2, "none: the post chain works in place, with ImageSpace::LdrScratch for its copies" },
			UnbackedColour{ RT::kGETHIT_BUFFER, "none: the get-hit step draws into ImageSpace::GetHitA/B" },
			UnbackedColour{ RT::kGETHIT_BLURSWAP, "none: the get-hit step draws into ImageSpace::GetHitSwap" },
			UnbackedColour{ RT::kLDR_BLURSWAP, "none: depth of field draws into ImageSpace::DofBlurSwap" },
			UnbackedColour{ RT::kLDR_DOWNSAMPLE0, "none: depth of field draws into ImageSpace::DofDownsample" },
			UnbackedColour{ RT::kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED, "the engine's frame clear only (dropped); SSR reads ImageSpace::SSRMaskDownsampled" },
		};

		// The volumetric lighting accumulation volumes (RENDER_TARGETS_3D 0 and 1).
		constexpr const char* kUnbackedVolume = "none: the volumetric lighting port draws VolumetricLighting::Volume";

		const UnbackedColour* FindUnbacked(std::uint32_t a_target)
		{
			for (const auto& entry : kUnbackedColours)
				if (entry.target == a_target)
					return &entry;
			return nullptr;
		}

		bool UnbackedVolume(std::uint32_t a_index) { return a_index == RE::RENDER_TARGETS_3D::kVOLUMETRIC_LIGHTING_ACCUMULATION || a_index == RE::RENDER_TARGETS_3D::kVOLUMETRIC_LIGHTING_ACCUMULATION_COPY; }

		// --- Fields ---

		/** @brief One engine field: the current version's object behind it, the handle the field holds, and what the handle reports. */
		template <class T, class D>
		struct FieldObject
		{
			winrt::com_ptr<T> object;  // null for an unbacked handle
			winrt::com_ptr<T> handle;  // made once; the field holds it from InstallHandles on
			D desc{};
			bool used = false;  // the target has this field
		};

		using TextureField = FieldObject<ID3D11Texture2D, D3D11_TEXTURE2D_DESC>;
		using VolumeField = FieldObject<ID3D11Texture3D, D3D11_TEXTURE3D_DESC>;
		using RtvField = FieldObject<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>;
		using SrvField = FieldObject<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>;
		using UavField = FieldObject<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>;
		using DsvField = FieldObject<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>;

		enum class Mode : std::uint8_t
		{
			Unknown,      // the engine never made it
			Backed,       // a declared texture
			Unbacked,     // a handle over nothing (kUnbackedColours)
			EngineOwned,  // the engine's own D3D11 objects
			MipView,      // views of a backed source's mip
		};

		struct ColourRecord
		{
			RenderTargetProperties props{};
			bool known = false;
			Mode mode = Mode::Unknown;
			const UnbackedColour* unbacked = nullptr;
			DXGI_FORMAT copyFormat = DXGI_FORMAT_UNKNOWN;
			Frame::DeclaredTexture2D main;
			Frame::DeclaredTexture2D copy;
			TextureField texture;
			TextureField textureCopy;
			RtvField rtv;
			SrvField srv;
			SrvField srvCopy;
			UavField uav;
		};

		struct DepthRecord
		{
			DepthStencilTargetProperties props{};
			bool known = false;
			Frame::DeclaredTexture2D main;
			TextureField texture;
			std::array<DsvField, kMaxDepthSlices> views;
			std::array<DsvField, kMaxDepthSlices> readOnlyViews;
			SrvField depthSrv;
			SrvField stencilSrv;
		};

		struct CubeRecord
		{
			CubeMapRenderTargetProperties props{};
			bool known = false;
			Frame::DeclaredTexture2D main;
			TextureField texture;
			std::array<RtvField, 6> faces;
			SrvField srv;
		};

		struct VolumeRecord
		{
			bool known = false;  // its handles exist
			Shape shape{};
			Frame::DeclaredTexture3D main;
			VolumeField texture;
			SrvField srv;
			UavField uav;
		};

		/** @brief What the engine keeps of its size in BSGraphics::State. */
		struct EngineSize
		{
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			std::uint32_t viewport[2]{};
		};

		struct SetState
		{
			std::array<ColourRecord, kColourSlots> colours;
			std::array<DepthRecord, kDepthSlots> depths;
			std::array<CubeRecord, kCubeSlots> cubes;
			std::array<VolumeRecord, kVolumeSlots> volumes;
			std::array<std::optional<Plan>, kColourSlots> plans;
			bool inSet = false;
			ShapeTable adopted{};  // what the field handles point at
			EngineSize adoptedSize{};
			EngineSize wanted{};  // what the engine asked for at the last set
			std::unordered_map<const void*, TargetRef> identify;
		};

		SetState& Set()
		{
			// Never destroyed: the engine may release fields during process teardown.
			static auto* s_state = new SetState;
			return *s_state;
		}

		std::atomic<bool> g_declared{ false };
		std::atomic<bool> g_handles{ false };
		std::atomic<std::uint32_t> g_framebufferFormat{ DXGI_FORMAT_UNKNOWN };

		RE::BSGraphics::Renderer* Renderer() { return RE::BSGraphics::Renderer::GetSingleton(); }

		// Indexed past CommonLib's array sizes, since AE has 116 render targets where CommonLib declares 114.
		RE::BSGraphics::RenderTargetData& ColourData(std::uint32_t a_index) { return (&Renderer()->GetRuntimeData().renderTargets[0])[a_index]; }
		RE::BSGraphics::DepthStencilData& DepthData(std::uint32_t a_index) { return (&Renderer()->GetDepthStencilData().depthStencils[0])[a_index]; }

		/**
		* @brief The engine's window keeps its own pointers to the swap chain target's RTV and SRV, and copies them back into
		* kFRAMEBUFFER's field (a movie frame does). They alias the field: after the set's writes they name the field's objects.
		*/
		void AliasWindowViews()
		{
			// The window holds a reference of its own, so the engine's release of it at a resize leaves the field's.
			static ID3D11RenderTargetView* aliasedRtv = nullptr;
			static ID3D11ShaderResourceView* aliasedSrv = nullptr;
			auto& data = ColourData(kFramebuffer);
			// A window whose views the engine changed (a resize) took our references with it.
			for (const auto& window : Renderer()->GetRuntimeData().renderWindows) {
				if (!window.swapChain || window.swapChainRenderTarget != RE::RENDER_TARGET::kFRAMEBUFFER)
					continue;
				if (reinterpret_cast<void*>(window.renderView) != aliasedRtv)
					aliasedRtv = nullptr;
				if (reinterpret_cast<void*>(window.resourceView) != aliasedSrv)
					aliasedSrv = nullptr;
			}
			if (data.RTV != aliasedRtv) {
				if (data.RTV)
					data.RTV->AddRef();
				if (aliasedRtv)
					aliasedRtv->Release();
				aliasedRtv = data.RTV;
			}
			if (data.SRV != aliasedSrv) {
				if (data.SRV)
					data.SRV->AddRef();
				if (aliasedSrv)
					aliasedSrv->Release();
				aliasedSrv = data.SRV;
			}
			for (auto& window : Renderer()->GetRuntimeData().renderWindows) {
				if (!window.swapChain || window.swapChainRenderTarget != RE::RENDER_TARGET::kFRAMEBUFFER)
					continue;
				window.renderView = reinterpret_cast<REX::W32::ID3D11RenderTargetView1*>(data.RTV);
				window.resourceView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView1*>(data.SRV);
			}
		}

		RE::BSGraphics::CubemapRenderTargetData& CubeData(std::uint32_t a_index) { return (&Renderer()->GetRendererData().cubemapRenderTargets[0])[a_index]; }
		RE::BSGraphics::Texture3DTargetData& VolumeData(std::uint32_t a_index) { return (&Renderer()->GetRendererData().texture3DRenderTargets[0])[a_index]; }

		/** @brief Texture3DTargetData as Create3DRenderTarget fills it. CommonLib names these fields unk*. */
		struct VolumeFields
		{
			ID3D11Texture3D* texture;        // 00
			ID3D11UnorderedAccessView* uav;  // 08, every depth slice of mip 0
			ID3D11ShaderResourceView* srv;   // 10
			std::uint32_t created;           // 18, set to 1 once the texture exists
			std::uint32_t pad1C;             // 1C
		};
		static_assert(sizeof(VolumeFields) == sizeof(RE::BSGraphics::Texture3DTargetData));

		VolumeFields& VolumeFieldsOf(std::uint32_t a_index) { return *reinterpret_cast<VolumeFields*>(&VolumeData(a_index)); }

		template <class E>
		std::string EnumName(std::uint32_t a_index, std::uint32_t a_count, const char* a_fallback)
		{
			if (a_index < a_count)
				if (const auto name = magic_enum::enum_name(static_cast<E>(a_index)); !name.empty())
					return std::string(name);
			return fmt::format("{}{}", a_fallback, a_index);
		}

		std::string ColourName(std::uint32_t a_index) { return "Engine::" + EnumName<RE::RENDER_TARGET>(a_index, RE::RENDER_TARGET::kTOTAL, "Target"); }
		std::string DepthName(std::uint32_t a_index)
		{
			return "Engine::Depth::" + EnumName<RE::RENDER_TARGET_DEPTHSTENCIL>(a_index, RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL, "DepthStencil");
		}
		std::string CubeName(std::uint32_t a_index)
		{
			return "Engine::Cubemap::" + EnumName<RE::RENDER_TARGET_CUBEMAP>(a_index, RE::RENDER_TARGETS_CUBEMAP::kTOTAL, "Cubemap");
		}
		std::string VolumeName(std::uint32_t a_index)
		{
			// The enum's names carry no k prefix after "Engine::3D::", as the 3D targets were named before.
			auto name = EnumName<RE::RENDER_TARGET_3D>(a_index, RE::RENDER_TARGETS_3D::kTOTAL, "Volume");
			if (name.size() > 1 && name[0] == 'k')
				name.erase(0, 1);
			return "Engine::3D::" + name;
		}

		template <class T>
		void ReleaseField(T*& a_field)
		{
			if (a_field) {
				a_field->Release();
				a_field = nullptr;
			}
		}

		/** @brief The field takes its own reference to a_object. */
		template <class T>
		void PutField(T*& a_field, T* a_object)
		{
			if (a_object)
				a_object->AddRef();
			a_field = a_object;
		}

		bool GuestsInstalled()
		{
			const auto& data = Renderer()->GetRuntimeData();
			return Tripwire::IsHandle(static_cast<const void*>(data.context)) && Tripwire::IsHandle(static_cast<const void*>(data.forwarder));
		}

		// --- Formats and descriptions ---

		struct DepthFormats
		{
			DXGI_FORMAT texture, dsv, srv;
		};

		/** @brief 32-bit float depth, 16-bit with use16BitsDepth, D24S8 with a stencil, which wins. */
		DepthFormats FormatsOf(const DepthStencilTargetProperties& a_props)
		{
			if (a_props.stencil)
				return { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS };
			if (a_props.use16BitsDepth)
				return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM };
			return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT };
		}

		/** @brief The views' formats of a typeless depth texture format. */
		DepthFormats FormatsOfTypeless(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R24G8_TYPELESS:
				return { a_format, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS };
			case DXGI_FORMAT_R16_TYPELESS:
				return { a_format, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM };
			case DXGI_FORMAT_R32_TYPELESS:
				return { a_format, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT };
			default:
				return { a_format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
			}
		}

		DXGI_FORMAT WithoutSrgb(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
				return DXGI_FORMAT_B8G8R8X8_UNORM;
			default:
				return a_format;
			}
		}

		/** @brief The swap chain's format without sRGB: native's declared one, or the guest swap chain's. */
		DXGI_FORMAT SwapChainFormat()
		{
			if (Native::Active())
				return Native::DeclaredBackBufferFormat();
			DXGI_SWAP_CHAIN_DESC desc{};
			if (!globals::d3d::swapChain || FAILED(globals::d3d::swapChain->GetDesc(&desc)))
				RenderGraph::Fatal("engine targets", "the swap chain's format cannot be read, so kFRAMEBUFFER cannot be declared");
			return WithoutSrgb(desc.BufferDesc.Format);
		}

		D3D11_TEXTURE2D_DESC Desc2D(const Shape& a_shape)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_shape.width;
			desc.Height = a_shape.height;
			desc.MipLevels = a_shape.mips;
			desc.ArraySize = a_shape.depthOrSlices;
			desc.Format = a_shape.format;
			desc.SampleDesc = { 1, 0 };
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = a_shape.bindFlags;
			desc.MiscFlags = a_shape.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;
			return desc;
		}

		D3D11_TEXTURE3D_DESC Desc3D(const Shape& a_shape)
		{
			D3D11_TEXTURE3D_DESC desc{};
			desc.Width = a_shape.width;
			desc.Height = a_shape.height;
			desc.Depth = a_shape.depthOrSlices;
			desc.MipLevels = a_shape.mips;
			desc.Format = a_shape.format;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = a_shape.bindFlags;
			return desc;
		}

		Shape CopyShape(const Shape& a_colour, DXGI_FORMAT a_copyFormat)
		{
			auto shape = a_colour;
			shape.format = a_copyFormat;
			shape.bindFlags = D3D11_BIND_SHADER_RESOURCE;
			shape.copy = false;
			return shape;
		}

		/** @brief The engine's properties with the plan on top. Only the extent may change between sets. */
		Shape ColourShape(std::uint32_t a_index, const SetState& a_state, const Extent2D& a_display)
		{
			const auto& record = a_state.colours[a_index];
			if (!record.known)
				return {};
			const auto& props = record.props;
			Shape shape{ props.width, props.height, 1, 1, static_cast<DXGI_FORMAT>(props.format.underlying()),
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | (props.supportUnorderedAccess ? static_cast<std::uint32_t>(D3D11_BIND_UNORDERED_ACCESS) : 0u),
				props.copyable, false, false };
			if (a_index == kFramebuffer) {
				// The scene back buffer, which present copies into the swap chain.
				shape.width = a_display.width;
				shape.height = a_display.height;
				shape.format = static_cast<DXGI_FORMAT>(g_framebufferFormat.load(std::memory_order_acquire));
				shape.bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				shape.copy = false;
				return shape;
			}
			const auto& plan = a_state.plans[a_index];
			if (!plan)
				return shape;
			if (plan->format != DXGI_FORMAT_UNKNOWN)
				shape.format = plan->format;
			if (plan->bindFlags)
				shape.bindFlags = plan->bindFlags;
			if (const auto& main = a_state.colours[kMain]; plan->mainSize && main.known) {
				shape.width = main.props.width;
				shape.height = main.props.height;
			}
			if (plan->dropCopy)
				shape.copy = false;
			return shape;
		}

		Shape DepthShape(const DepthRecord& a_record)
		{
			if (!a_record.known)
				return {};
			const auto& props = a_record.props;
			return { props.width, props.height, props.arraySize, 1, FormatsOf(props).texture, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, false, false, false };
		}

		Shape CubeShape(const CubeRecord& a_record)
		{
			if (!a_record.known)
				return {};
			const auto& props = a_record.props;
			return { props.width, props.height, 6, 1, static_cast<DXGI_FORMAT>(props.format.underlying()), D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
				false, true, false };
		}

		/** @brief Every target's shape from the recorded properties and the plans. */
		ShapeTable BuildTable(const Extent2D& a_display)
		{
			auto& state = Set();
			ShapeTable table;
			for (std::uint32_t i = 0; i < kColourSlots; ++i)
				table.colours[i] = ColourShape(i, state, a_display);
			for (std::uint32_t i = 0; i < kDepthSlots; ++i)
				table.depths[i] = DepthShape(state.depths[i]);
			for (std::uint32_t i = 0; i < kCubeSlots; ++i)
				table.cubes[i] = CubeShape(state.cubes[i]);
			for (std::uint32_t i = 0; i < kVolumeSlots; ++i)
				table.volumes[i] = state.volumes[i].shape;
			return table;
		}

		Extent2D ScreenSize()
		{
			const auto* state = RE::BSGraphics::State::GetSingleton();
			return state ? Extent2D{ state->screenWidth, state->screenHeight } : Extent2D{};
		}

		EngineSize ReadEngineSize()
		{
			EngineSize size;
			if (const auto* state = RE::BSGraphics::State::GetSingleton()) {
				size.width = state->screenWidth;
				size.height = state->screenHeight;
				size.viewport[0] = state->frameBufferViewport[0];
				size.viewport[1] = state->frameBufferViewport[1];
			}
			return size;
		}

		void WriteEngineSize(const EngineSize& a_size)
		{
			if (auto* state = RE::BSGraphics::State::GetSingleton()) {
				state->screenWidth = a_size.width;
				state->screenHeight = a_size.height;
				state->frameBufferViewport[0] = a_size.viewport[0];
				state->frameBufferViewport[1] = a_size.viewport[1];
			}
		}

		/** @brief The shape variables the features read, from the table. */
		void RaiseShapeVariables(const ShapeTable& a_table, const Extent2D& a_display)
		{
			namespace Vars = Frame::Vars;
			Vars::Set(Vars::EngineTargetShapes(), a_table, kWriter);
			Vars::Set(Vars::DisplaySize(), a_display, kWriter);
			const auto shadow = [](const Shape& a_shape) {
				return Vars::ShadowMapShape{ { a_shape.width, a_shape.height }, static_cast<std::uint32_t>(a_shape.format), a_shape.depthOrSlices };
			};
			if (const auto& shape = a_table.depths[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS]; shape.width)
				Vars::Set(Vars::EngineShadowMap(), shadow(shape), kWriter);
			if (const auto& shape = a_table.depths[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM]; shape.width)
				Vars::Set(Vars::ShadowCascades(), shadow(shape), kWriter);
			if (const auto& shape = a_table.cubes[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS]; shape.width)
				Vars::Set(Vars::ReflectionCubemap(), { shape.width }, kWriter);
		}

		/** @brief Without the set (D3D11 mode, or the first set before the graph): the variables from the engine's fields. */
		void RaiseFromFields()
		{
			namespace Vars = Frame::Vars;
			Vars::Set(Vars::DisplaySize(), ScreenSize(), kWriter);
			auto* renderer = Renderer();
			if (!renderer)
				return;
			D3D11_TEXTURE2D_DESC desc{};
			if (auto* shadows = DepthData(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS)).texture) {
				shadows->GetDesc(&desc);
				Vars::Set(Vars::EngineShadowMap(), { { desc.Width, desc.Height }, static_cast<std::uint32_t>(desc.Format), desc.ArraySize }, kWriter);
			}
			if (auto* cascades = DepthData(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM)).texture) {
				cascades->GetDesc(&desc);
				Vars::Set(Vars::ShadowCascades(), { { desc.Width, desc.Height }, static_cast<std::uint32_t>(desc.Format), desc.ArraySize }, kWriter);
			}
			if (auto* cube = CubeData(static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS)).texture) {
				cube->GetDesc(&desc);
				Vars::Set(Vars::ReflectionCubemap(), { desc.Width }, kWriter);
			}
		}

		// --- Objects: the current version's D3D11 objects behind each field ---

		ID3D11Device* Device()
		{
			auto* device = globals::d3d::device;
			if (!device)
				RenderGraph::Fatal("engine targets", "there is no D3D11 device to make the targets' views on");
			return device;
		}

		HRESULT DeviceView(ID3D11Resource* a_resource, const D3D11_RENDER_TARGET_VIEW_DESC& a_desc, winrt::com_ptr<ID3D11RenderTargetView>& a_out)
		{
			return Device()->CreateRenderTargetView(a_resource, &a_desc, a_out.put());
		}
		HRESULT DeviceView(ID3D11Resource* a_resource, const D3D11_SHADER_RESOURCE_VIEW_DESC& a_desc, winrt::com_ptr<ID3D11ShaderResourceView>& a_out)
		{
			return Device()->CreateShaderResourceView(a_resource, &a_desc, a_out.put());
		}
		HRESULT DeviceView(ID3D11Resource* a_resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC& a_desc, winrt::com_ptr<ID3D11UnorderedAccessView>& a_out)
		{
			return Device()->CreateUnorderedAccessView(a_resource, &a_desc, a_out.put());
		}
		HRESULT DeviceView(ID3D11Resource* a_resource, const D3D11_DEPTH_STENCIL_VIEW_DESC& a_desc, winrt::com_ptr<ID3D11DepthStencilView>& a_out)
		{
			return Device()->CreateDepthStencilView(a_resource, &a_desc, a_out.put());
		}

		/** @brief A view of a_resource, or none over nothing (an unbacked handle). Fatal when D3D11 refuses it. */
		template <class T, class D>
		void MakeView(FieldObject<T, D>& a_field, ID3D11Resource* a_resource, const D& a_desc, const std::string& a_name)
		{
			a_field.used = true;
			a_field.desc = a_desc;
			a_field.object = nullptr;
			if (!a_resource)
				return;
			if (FAILED(DeviceView(a_resource, a_desc, a_field.object)) || !a_field.object)
				RenderGraph::Fatal("engine targets", fmt::format("{} could not be made", a_name));
			Util::SetResourceName(a_field.object.get(), "%s", a_name.c_str());
		}

		/** @brief A declared twin's own object for the field. Its description is the object's. */
		template <class T, class D>
		void TakeObject(FieldObject<T, D>& a_field, T* a_object, const std::string& a_name)
		{
			if (!a_object)
				RenderGraph::Fatal("engine targets", fmt::format("{} has no D3D11 export", a_name));
			if (Tripwire::IsHandle(a_object) && !Native::Active())
				RenderGraph::Fatal("engine targets", fmt::format("{}'s D3D11 export is a tripwire handle, not DXVK's object", a_name));
			a_field.used = true;
			a_field.object = nullptr;
			a_field.object.copy_from(a_object);
			a_object->GetDesc(&a_field.desc);
		}

		D3D11_RENDER_TARGET_VIEW_DESC RtvDesc(DXGI_FORMAT a_format, UINT a_mip = 0)
		{
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			desc.Format = a_format;
			desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MipSlice = a_mip;
			return desc;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC SrvDesc(DXGI_FORMAT a_format, UINT a_mip = 0)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = a_format;
			desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MostDetailedMip = a_mip;
			desc.Texture2D.MipLevels = 1;
			return desc;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC UavDesc(DXGI_FORMAT a_format, UINT a_mip = 0)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = a_format;
			desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MipSlice = a_mip;
			return desc;
		}

		/** @brief An unbacked colour texture has no object. Its field gets the tripwire's unbacked handle (MakeHandle), in both modes. */
		void MakeUnbackedTexture(TextureField& a_field, const D3D11_TEXTURE2D_DESC& a_desc)
		{
			a_field.used = true;
			a_field.desc = a_desc;
			a_field.object = nullptr;
		}

		/** @brief A colour target's objects for its current version, from its twins or as unbacked. */
		void BuildColour(std::uint32_t a_index, const Shape& a_shape)
		{
			auto& state = Set();
			auto& record = state.colours[a_index];
			const auto name = ColourName(a_index);
			const bool rtv = (a_shape.bindFlags & D3D11_BIND_RENDER_TARGET) != 0;
			const bool uav = (a_shape.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
			if (record.mode == Mode::Backed) {
				TakeObject(record.texture, record.main->D3D11(), name);
				TakeObject(record.srv, record.main->SRV(), name + " SRV");
				if (rtv)
					TakeObject(record.rtv, record.main->RTV(), name + " RTV");
				if (uav)
					TakeObject(record.uav, record.main->UAV(), name + " UAV");
				if (record.copy) {
					TakeObject(record.textureCopy, record.copy->D3D11(), name + " Copy");
					TakeObject(record.srvCopy, record.copy->SRV(), name + " Copy SRV");
				}
				return;
			}
			if (record.mode == Mode::Unbacked) {
				MakeUnbackedTexture(record.texture, Desc2D(a_shape));
				auto* texture = record.texture.object.get();
				MakeView(record.srv, texture, SrvDesc(a_shape.format), name + " SRV");
				if (rtv)
					MakeView(record.rtv, texture, RtvDesc(a_shape.format), name + " RTV");
				if (uav)
					MakeView(record.uav, texture, UavDesc(a_shape.format), name + " UAV");
				if (a_shape.copy) {
					const auto copyShape = CopyShape(a_shape, record.copyFormat);
					MakeUnbackedTexture(record.textureCopy, Desc2D(copyShape));
					MakeView(record.srvCopy, record.textureCopy.object.get(), SrvDesc(copyShape.format), name + " Copy SRV");
				}
				return;
			}
			if (record.mode == Mode::MipView) {
				// Views of the source's mip, over its current version's object.
				const auto source = record.props.textureTarget;
				const auto& from = state.colours[source];
				const auto format = from.texture.desc.Format;
				const auto mip = static_cast<UINT>(record.props.mipLevel);
				auto* texture = from.texture.object.get();
				MakeView(record.rtv, texture, RtvDesc(format, mip), fmt::format("{} RTV (mip {} of {})", name, mip, ColourName(source)));
				MakeView(record.srv, texture, SrvDesc(format, mip), fmt::format("{} SRV (mip {} of {})", name, mip, ColourName(source)));
				if (record.props.supportUnorderedAccess && (from.texture.desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS))
					MakeView(record.uav, texture, UavDesc(format, mip), fmt::format("{} UAV (mip {} of {})", name, mip, ColourName(source)));
			}
		}

		void BuildDepth(std::uint32_t a_index)
		{
			auto& record = Set().depths[a_index];
			const auto name = DepthName(a_index);
			const auto formats = FormatsOf(record.props);
			TakeObject(record.texture, record.main->D3D11(), name);
			TakeObject(record.depthSrv, record.main->SRV(), name + " SRV");
			auto* texture = record.texture.object.get();
			const UINT slices = record.texture.desc.ArraySize;
			const bool array = slices > 1;
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
				MakeView(record.views[slice], texture, dsv, fmt::format("{} DSV {}", name, slice));
				dsv.Flags = D3D11_DSV_READ_ONLY_DEPTH | (record.props.stencil ? static_cast<UINT>(D3D11_DSV_READ_ONLY_STENCIL) : 0u);
				MakeView(record.readOnlyViews[slice], texture, dsv, fmt::format("{} read-only DSV {}", name, slice));
			}
			if (record.props.stencil) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
				if (array) {
					srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
					srv.Texture2DArray.MipLevels = 1;
					srv.Texture2DArray.ArraySize = slices;
				} else {
					srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					srv.Texture2D.MipLevels = 1;
				}
				MakeView(record.stencilSrv, texture, srv, name + " stencil SRV");
			}
		}

		void BuildCube(std::uint32_t a_index)
		{
			auto& record = Set().cubes[a_index];
			const auto name = CubeName(a_index);
			TakeObject(record.texture, record.main->D3D11(), name);
			TakeObject(record.srv, record.main->SRV(), name + " SRV");
			for (UINT face = 0; face < 6; ++face) {
				D3D11_RENDER_TARGET_VIEW_DESC rtv{};
				rtv.Format = record.texture.desc.Format;
				rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				rtv.Texture2DArray.FirstArraySlice = face;
				rtv.Texture2DArray.ArraySize = 1;
				MakeView(record.faces[face], record.texture.object.get(), rtv, fmt::format("{} RTV {}", name, face));
			}
		}

		// --- Handles ---

		Tripwire::Options HandleOptions(std::string a_name, std::shared_ptr<org::Resource> a_graph)
		{
			Tripwire::Options options;
			options.name = std::move(a_name);
			options.graph = std::move(a_graph);
			return options;
		}

		winrt::com_ptr<ID3D11RenderTargetView> TripwireView(ID3D11Resource* a_resource, const D3D11_RENDER_TARGET_VIEW_DESC& a_desc, Tripwire::Options a_options,
			ID3D11RenderTargetView* a_wrap)
		{
			return Tripwire::CreateRenderTargetView(a_resource, a_desc, std::move(a_options), a_wrap);
		}
		winrt::com_ptr<ID3D11ShaderResourceView> TripwireView(ID3D11Resource* a_resource, const D3D11_SHADER_RESOURCE_VIEW_DESC& a_desc, Tripwire::Options a_options,
			ID3D11ShaderResourceView* a_wrap)
		{
			return Tripwire::CreateShaderResourceView(a_resource, a_desc, std::move(a_options), a_wrap);
		}
		winrt::com_ptr<ID3D11UnorderedAccessView> TripwireView(ID3D11Resource* a_resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC& a_desc, Tripwire::Options a_options,
			ID3D11UnorderedAccessView* a_wrap)
		{
			return Tripwire::CreateUnorderedAccessView(a_resource, a_desc, std::move(a_options), a_wrap);
		}
		winrt::com_ptr<ID3D11DepthStencilView> TripwireView(ID3D11Resource* a_resource, const D3D11_DEPTH_STENCIL_VIEW_DESC& a_desc, Tripwire::Options a_options,
			ID3D11DepthStencilView* a_wrap)
		{
			return Tripwire::CreateDepthStencilView(a_resource, a_desc, std::move(a_options), a_wrap);
		}

		void MakeHandle(TextureField& a_field, const std::string& a_name, const std::shared_ptr<org::Resource>& a_graph)
		{
			if (!a_field.used || a_field.handle)
				return;
			a_field.handle = a_field.object ? Tripwire::CreateTexture2D(a_field.desc, HandleOptions(a_name, a_graph), a_field.object.get()) :
			                                  Tripwire::CreateUnbackedTexture2D(a_field.desc, HandleOptions(a_name, a_graph));
		}

		void MakeHandle(VolumeField& a_field, const std::string& a_name, const std::shared_ptr<org::Resource>& a_graph)
		{
			if (!a_field.used || a_field.handle)
				return;
			a_field.handle = a_field.object ? Tripwire::CreateTexture3D(a_field.desc, HandleOptions(a_name, a_graph), a_field.object.get()) :
			                                  Tripwire::CreateUnbackedTexture3D(a_field.desc, HandleOptions(a_name, a_graph));
		}

		/** @brief A view handle answers GetResource with a_texture, the field's texture handle. */
		template <class T, class D>
		void MakeHandle(FieldObject<T, D>& a_field, ID3D11Resource* a_texture, const std::string& a_name, const std::shared_ptr<org::Resource>& a_graph)
		{
			if (!a_field.used || a_field.handle)
				return;
			a_field.handle = TripwireView(a_texture, a_field.desc, HandleOptions(a_name, a_graph), a_field.object.get());
		}

		template <class T, class D>
		void Retarget(FieldObject<T, D>& a_field, const std::shared_ptr<org::Resource>& a_graph)
		{
			if (!a_field.handle)
				return;
			Tripwire::Retarget(a_field.handle.get(), Tripwire::Target<D>{ a_field.object.get(), a_field.desc, a_graph, nullptr });
		}

		/**
		* @brief Before the handles exist, the field holds the version's own object (or the unbacked handle, which has none). Native
		* mode installs the handles at once, so this is only the guest window until the state setup hook.
		*/
		template <class T, class D>
		void PutObject(T*& a_field, FieldObject<T, D>& a_object, const std::string& a_name, const std::shared_ptr<org::Resource>& a_graph)
		{
			if (!a_object.used)
				return;
			if (!a_object.object) {
				if constexpr (std::is_same_v<T, ID3D11Texture2D> || std::is_same_v<T, ID3D11Texture3D>)
					MakeHandle(a_object, a_name, a_graph);
				PutField(a_field, a_object.handle.get());
				return;
			}
			PutField(a_field, a_object.object.get());
		}

		/** @brief The field moves from the version's object to the handle. */
		template <class T, class D>
		void InstallField(T*& a_field, FieldObject<T, D>& a_object, std::string_view a_name)
		{
			if (!a_object.handle || a_field == a_object.handle.get())
				return;
			if (a_field && a_field != a_object.object.get())
				RenderGraph::Fatal("engine targets", fmt::format("{}'s field holds an object the set did not put there", a_name));
			ReleaseField(a_field);
			PutField(a_field, a_object.handle.get());
		}

		/** @brief At a later set: a field the engine released gets its handle back. Anything else in it is fatal. */
		template <class T, class D>
		void RefillField(T*& a_field, const FieldObject<T, D>& a_object, std::string_view a_name)
		{
			if (!a_object.handle || a_field == a_object.handle.get())
				return;
			if (a_field)
				RenderGraph::Fatal("engine targets", fmt::format("{}'s field holds another object at the engine's new set (a feature changed it)", a_name));
			PutField(a_field, a_object.handle.get());
		}

		void Note(const void* a_handle, TargetRef a_ref)
		{
			if (a_handle)
				Set().identify.emplace(a_handle, a_ref);
		}

		std::shared_ptr<org::Resource> GraphOf(const Frame::DeclaredTexture2D& a_twin) { return a_twin ? a_twin->Graph() : nullptr; }
		std::shared_ptr<org::Resource> GraphOf(const Frame::DeclaredTexture3D& a_twin) { return a_twin ? a_twin->Graph() : nullptr; }

		// --- Per kind: handles, fields, retarget ---

		void ColourHandles(std::uint32_t a_index)
		{
			auto& state = Set();
			auto& record = state.colours[a_index];
			const auto name = ColourName(a_index);
			const auto graph = GraphOf(record.main);
			auto& data = ColourData(a_index);
			if (record.mode == Mode::MipView) {
				const auto source = record.props.textureTarget;
				auto* texture = state.colours[source].texture.handle.get();
				const auto mip = static_cast<std::uint8_t>(record.props.mipLevel);
				const auto sourceGraph = GraphOf(state.colours[source].main);
				MakeHandle(record.rtv, texture, name + " RTV", sourceGraph);
				MakeHandle(record.srv, texture, name + " SRV", sourceGraph);
				MakeHandle(record.uav, texture, name + " UAV", sourceGraph);
				InstallField(data.RTV, record.rtv, name);
				InstallField(data.SRV, record.srv, name);
				InstallField(data.UAV, record.uav, name);
				Note(record.rtv.handle.get(), { Kind::Colour, static_cast<std::uint16_t>(source), View::Rtv, 0, mip });
				Note(record.srv.handle.get(), { Kind::Colour, static_cast<std::uint16_t>(source), View::Srv, 0, mip });
				Note(record.uav.handle.get(), { Kind::Colour, static_cast<std::uint16_t>(source), View::Uav, 0, mip });
				return;
			}
			MakeHandle(record.texture, name, graph);
			auto* texture = record.texture.handle.get();
			MakeHandle(record.rtv, texture, name + " RTV", graph);
			MakeHandle(record.srv, texture, name + " SRV", graph);
			MakeHandle(record.uav, texture, name + " UAV", graph);
			const auto copyGraph = GraphOf(record.copy);
			MakeHandle(record.textureCopy, name + " Copy", copyGraph);
			MakeHandle(record.srvCopy, record.textureCopy.handle.get(), name + " Copy SRV", copyGraph);
			InstallField(data.texture, record.texture, name);
			InstallField(data.RTV, record.rtv, name);
			InstallField(data.SRV, record.srv, name);
			InstallField(data.UAV, record.uav, name);
			InstallField(data.textureCopy, record.textureCopy, name);
			InstallField(data.SRVCopy, record.srvCopy, name);
			const auto index = static_cast<std::uint16_t>(a_index);
			Note(record.texture.handle.get(), { Kind::Colour, index, View::Texture });
			Note(record.rtv.handle.get(), { Kind::Colour, index, View::Rtv });
			Note(record.srv.handle.get(), { Kind::Colour, index, View::Srv });
			Note(record.uav.handle.get(), { Kind::Colour, index, View::Uav });
			Note(record.textureCopy.handle.get(), { Kind::ColourCopy, index, View::Texture });
			Note(record.srvCopy.handle.get(), { Kind::ColourCopy, index, View::Srv });
		}

		void DepthHandles(std::uint32_t a_index)
		{
			auto& record = Set().depths[a_index];
			const auto name = DepthName(a_index);
			const auto graph = GraphOf(record.main);
			auto& data = DepthData(a_index);
			MakeHandle(record.texture, name, graph);
			auto* texture = record.texture.handle.get();
			const auto index = static_cast<std::uint16_t>(a_index);
			for (std::uint32_t slice = 0; slice < kMaxDepthSlices; ++slice) {
				MakeHandle(record.views[slice], texture, fmt::format("{} DSV {}", name, slice), graph);
				MakeHandle(record.readOnlyViews[slice], texture, fmt::format("{} read-only DSV {}", name, slice), graph);
				InstallField(data.views[slice], record.views[slice], name);
				InstallField(data.readOnlyViews[slice], record.readOnlyViews[slice], name);
				Note(record.views[slice].handle.get(), { Kind::Depth, index, View::Dsv, static_cast<std::uint8_t>(slice) });
				Note(record.readOnlyViews[slice].handle.get(), { Kind::Depth, index, View::ReadOnlyDsv, static_cast<std::uint8_t>(slice) });
			}
			MakeHandle(record.depthSrv, texture, name + " SRV", graph);
			MakeHandle(record.stencilSrv, texture, name + " stencil SRV", graph);
			InstallField(data.texture, record.texture, name);
			InstallField(data.depthSRV, record.depthSrv, name);
			InstallField(data.stencilSRV, record.stencilSrv, name);
			Note(texture, { Kind::Depth, index, View::Texture });
			Note(record.depthSrv.handle.get(), { Kind::Depth, index, View::Srv });
			Note(record.stencilSrv.handle.get(), { Kind::Depth, index, View::StencilSrv });
		}

		void CubeHandles(std::uint32_t a_index)
		{
			auto& record = Set().cubes[a_index];
			const auto name = CubeName(a_index);
			const auto graph = GraphOf(record.main);
			auto& data = CubeData(a_index);
			MakeHandle(record.texture, name, graph);
			auto* texture = record.texture.handle.get();
			const auto index = static_cast<std::uint16_t>(a_index);
			for (std::uint32_t face = 0; face < 6; ++face) {
				MakeHandle(record.faces[face], texture, fmt::format("{} RTV {}", name, face), graph);
				InstallField(data.cubeSideRTV[face], record.faces[face], name);
				Note(record.faces[face].handle.get(), { Kind::Cubemap, index, View::CubeFace, static_cast<std::uint8_t>(face) });
			}
			MakeHandle(record.srv, texture, name + " SRV", graph);
			InstallField(data.texture, record.texture, name);
			InstallField(data.SRV, record.srv, name);
			Note(texture, { Kind::Cubemap, index, View::Texture });
			Note(record.srv.handle.get(), { Kind::Cubemap, index, View::Srv });
		}

		void RetargetColour(std::uint32_t a_index)
		{
			auto& state = Set();
			auto& record = state.colours[a_index];
			if (record.mode == Mode::MipView) {
				const auto graph = GraphOf(state.colours[record.props.textureTarget].main);
				Retarget(record.rtv, graph);
				Retarget(record.srv, graph);
				Retarget(record.uav, graph);
				return;
			}
			const auto graph = GraphOf(record.main);
			const auto copyGraph = GraphOf(record.copy);
			Retarget(record.texture, graph);
			Retarget(record.rtv, graph);
			Retarget(record.srv, graph);
			Retarget(record.uav, graph);
			Retarget(record.textureCopy, copyGraph);
			Retarget(record.srvCopy, copyGraph);
		}

		void RetargetDepth(std::uint32_t a_index)
		{
			auto& record = Set().depths[a_index];
			const auto graph = GraphOf(record.main);
			Retarget(record.texture, graph);
			for (std::uint32_t slice = 0; slice < kMaxDepthSlices; ++slice) {
				Retarget(record.views[slice], graph);
				Retarget(record.readOnlyViews[slice], graph);
			}
			Retarget(record.depthSrv, graph);
			Retarget(record.stencilSrv, graph);
		}

		void RetargetCube(std::uint32_t a_index)
		{
			auto& record = Set().cubes[a_index];
			const auto graph = GraphOf(record.main);
			Retarget(record.texture, graph);
			for (auto& face : record.faces)
				Retarget(face, graph);
			Retarget(record.srv, graph);
		}

		// --- Volumes ---

		std::uint32_t IniInteger(const char* a_name)
		{
			RE::Setting* found = globals::game::iniSettingCollection ? globals::game::iniSettingCollection->GetSetting(a_name) : nullptr;
			if (!found && globals::game::iniPrefSettingCollection)
				found = globals::game::iniPrefSettingCollection->GetSetting(a_name);
			return found ? static_cast<std::uint32_t>((std::max)(found->GetInteger(), 0)) : 0u;
		}

		/** @brief The noise volume, as the engine makes it when volumetric lighting first renders (R8_UNORM at the INI's size). */
		Shape NoiseShape()
		{
			return { IniInteger("iVolumetricLightingNoiseTextureWidth:Display"), IniInteger("iVolumetricLightingNoiseTextureHeight:Display"),
				IniInteger("iVolumetricLightingNoiseTextureDepth:Display"), 1, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
				false, false, true };
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC VolumeSrvDesc(DXGI_FORMAT a_format)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = a_format;
			desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			desc.Texture3D.MipLevels = 1;
			return desc;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC VolumeUavDesc(DXGI_FORMAT a_format, UINT a_depth)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = a_format;
			desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
			desc.Texture3D.WSize = a_depth;
			return desc;
		}

		/** @brief An accumulation volume: a handle over nothing, made when the engine first asks for it. CPU only. */
		void MakeUnbackedVolume(std::uint32_t a_index, const Shape& a_shape)
		{
			auto& record = Set().volumes[a_index];
			const auto name = VolumeName(a_index);
			record.shape = a_shape;
			record.texture.used = true;
			record.texture.desc = Desc3D(a_shape);
			record.texture.object = nullptr;
			MakeView(record.srv, record.texture.object.get(), VolumeSrvDesc(a_shape.format), name + " SRV");
			MakeView(record.uav, record.texture.object.get(), VolumeUavDesc(a_shape.format, a_shape.depthOrSlices), name + " UAV");
			MakeHandle(record.texture, name, nullptr);
			MakeHandle(record.srv, record.texture.handle.get(), name + " SRV", nullptr);
			MakeHandle(record.uav, record.texture.handle.get(), name + " UAV", nullptr);
			record.known = true;
		}

		/** @brief The fields of a volume the set made: its handles, and created = 1. */
		void FillVolume(std::uint32_t a_index)
		{
			auto& record = Set().volumes[a_index];
			auto& data = VolumeFieldsOf(a_index);
			const auto name = VolumeName(a_index);
			RefillField(data.texture, record.texture, name);
			RefillField(data.srv, record.srv, name);
			RefillField(data.uav, record.uav, name);
			data.created = 1;
		}

		// --- Adoption ---

		/** @brief After each revision's versions are adopted: points the handles of every changed target at its new version. */
		void OnVersionsAdopted()
		{
			auto& state = Set();
			if (!g_handles.load(std::memory_order_acquire))
				return;
			const auto table = Frame::Vars::Adopted(Frame::Vars::EngineTargetShapes());
			if (table == state.adopted)
				return;
			std::uint32_t retargeted = 0;
			for (std::uint32_t i = 0; i < kColourSlots; ++i) {
				auto& record = state.colours[i];
				if (table.colours[i] != state.adopted.colours[i] && (record.mode == Mode::Backed || record.mode == Mode::Unbacked)) {
					BuildColour(i, table.colours[i]);
					++retargeted;
				}
			}
			// Mip views after their sources, since they read the source's new object.
			for (std::uint32_t i = 0; i < kColourSlots; ++i) {
				auto& record = state.colours[i];
				const bool changed = table.colours[i] != state.adopted.colours[i];
				const bool sourceChanged = record.mode == Mode::MipView && table.colours[record.props.textureTarget] != state.adopted.colours[record.props.textureTarget];
				if ((changed || sourceChanged) && record.mode == Mode::MipView)
					BuildColour(i, table.colours[i]);
				if ((changed || sourceChanged) && (record.mode == Mode::Backed || record.mode == Mode::Unbacked || record.mode == Mode::MipView))
					RetargetColour(i);
			}
			for (std::uint32_t i = 0; i < kDepthSlots; ++i) {
				if (table.depths[i] == state.adopted.depths[i] || !state.depths[i].main)
					continue;
				BuildDepth(i);
				RetargetDepth(i);
				++retargeted;
			}
			for (std::uint32_t i = 0; i < kCubeSlots; ++i) {
				if (table.cubes[i] == state.adopted.cubes[i] || !state.cubes[i].main)
					continue;
				BuildCube(i);
				RetargetCube(i);
				++retargeted;
			}
			state.adopted = table;
			// The engine draws at the size of the targets it has from now on.
			const auto display = Frame::Vars::Adopted(Frame::Vars::DisplaySize());
			EngineSize size = state.wanted;
			if (size.width != display.width || size.height != display.height)
				size = { display.width, display.height, { display.width, display.height } };
			WriteEngineSize(size);
			state.adoptedSize = size;
			Tripwire::MarkEngineBindingsDirty();
			logger::info("[ORG] engine target set: adopted {}x{} ({} targets pointed at new versions)", display.width, display.height, retargeted);
		}

		// --- The create detours ---

		/** @brief Why a later set's target the first set never made cannot be declared now. */
		std::string LaterSetTarget(const std::string& a_name)
		{
			return fmt::format("the engine made {} at a later set, which its first set did not make, so it has no declaration", a_name);
		}

		/** @brief A create outside a set after DeclareSet raises the shapes at once. */
		void RaiseOutsideSet()
		{
			auto& state = Set();
			if (state.inSet)
				return;
			const auto display = Frame::Vars::Written(Frame::Vars::DisplaySize());
			RaiseShapeVariables(BuildTable(display), display);
		}

		struct CreateRenderTarget
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_target, const char* a_name, RenderTargetProperties* a_properties)
			{
				if (!a_properties || a_target >= kColourSlots) {
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				auto& record = Set().colours[a_target];
				if (!g_declared.load(std::memory_order_acquire)) {
					// The first set, made with D3D11 before the graph exists. DeclareSet takes it over.
					record.props = *a_properties;
					record.known = true;
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				if (record.mode == Mode::Unknown)
					RenderGraph::Fatal("engine targets", LaterSetTarget(ColourName(a_target)));
				record.props = *a_properties;
				if (record.mode == Mode::EngineOwned) {
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				auto& data = ColourData(a_target);
				const auto name = ColourName(a_target);
				if (record.mode != Mode::MipView) {
					RefillField(data.texture, record.texture, name);
					RefillField(data.textureCopy, record.textureCopy, name);
					RefillField(data.SRVCopy, record.srvCopy, name);
				}
				RefillField(data.RTV, record.rtv, name);
				RefillField(data.SRV, record.srv, name);
				RefillField(data.UAV, record.uav, name);
				RaiseOutsideSet();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CreateDepthStencilTarget
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_target, const char* a_name, DepthStencilTargetProperties* a_properties)
			{
				if (!a_properties || a_target >= kDepthSlots) {
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				auto& record = Set().depths[a_target];
				if (!g_declared.load(std::memory_order_acquire)) {
					record.props = *a_properties;
					record.known = true;
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				if (!record.main)
					RenderGraph::Fatal("engine targets", LaterSetTarget(DepthName(a_target)));
				record.props = *a_properties;
				auto& data = DepthData(a_target);
				const auto name = DepthName(a_target);
				RefillField(data.texture, record.texture, name);
				for (std::uint32_t slice = 0; slice < kMaxDepthSlices; ++slice) {
					RefillField(data.views[slice], record.views[slice], name);
					RefillField(data.readOnlyViews[slice], record.readOnlyViews[slice], name);
				}
				RefillField(data.depthSRV, record.depthSrv, name);
				RefillField(data.stencilSRV, record.stencilSrv, name);
				RaiseOutsideSet();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CreateCubeMapRenderTarget
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_target, const char* a_name, CubeMapRenderTargetProperties* a_properties)
			{
				if (!a_properties || a_target >= kCubeSlots) {
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				auto& record = Set().cubes[a_target];
				if (!g_declared.load(std::memory_order_acquire)) {
					record.props = *a_properties;
					record.known = true;
					func(a_renderer, a_target, a_name, a_properties);
					return;
				}
				if (!record.main)
					RenderGraph::Fatal("engine targets", LaterSetTarget(CubeName(a_target)));
				record.props = *a_properties;
				auto& data = CubeData(a_target);
				const auto name = CubeName(a_target);
				RefillField(data.texture, record.texture, name);
				for (std::uint32_t face = 0; face < 6; ++face)
					RefillField(data.cubeSideRTV[face], record.faces[face], name);
				RefillField(data.SRV, record.srv, name);
				RaiseOutsideSet();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// 3D targets are made when volumetric lighting first renders, inside a frame. The set declared them at setup, so this
		// only checks the engine's arguments and fills the fields.
		struct Create3DRenderTarget
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_target, const char* a_name, std::uint32_t a_width, std::uint32_t a_height,
				std::uint32_t a_depth, std::int32_t a_usage, std::uint32_t a_format)
			{
				// Dynamic and staging variants are CPU-visible, which an exported image is not.
				if (a_target >= kVolumeSlots || a_usage != D3D11_USAGE_DEFAULT || !g_declared.load(std::memory_order_acquire)) {
					func(a_renderer, a_target, a_name, a_width, a_height, a_depth, a_usage, a_format);
					return;
				}
				auto& record = Set().volumes[a_target];
				const auto name = VolumeName(a_target);
				const Shape asked{ a_width, a_height, a_depth, 1, static_cast<DXGI_FORMAT>(a_format), D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, false,
					false, true };
				if (!record.known) {
					if (!UnbackedVolume(a_target))
						RenderGraph::Fatal("engine targets", fmt::format("the engine made the 3D target {} ({}x{}x{}, format {}), which the set did not declare{}", name, a_width, a_height, a_depth,
							a_format, a_target == kNoiseVolume ? " (the noise volume's INI size iVolumetricLightingNoiseTexture*:Display was not readable at setup)" : ""));
					MakeUnbackedVolume(a_target, asked);
					logger::info("[ORG] engine target set: {} has no memory ({}x{}x{}): {}", name, a_width, a_height, a_depth, kUnbackedVolume);
				} else if (!(record.shape == asked) && UnbackedVolume(a_target)) {
					// A new volumetric lighting quality makes the volume again at another size. It has no memory and nothing draws
					// through it (the port's VolumetricLighting::Volume follows the setting), so its handles stay.
					logger::info("[ORG] engine target set: {} made again at {}x{}x{}; it has no memory, so its handles stay", name, a_width, a_height, a_depth);
				} else if (!(record.shape == asked)) {
					RenderGraph::Fatal("engine targets", fmt::format("the engine made the 3D target {} as {}x{}x{} format {}, but it is declared as {}x{}x{} format {}", name, a_width, a_height, a_depth,
						a_format, record.shape.width, record.shape.height, record.shape.depthOrSlices, static_cast<std::uint32_t>(record.shape.format)));
				}
				FillVolume(a_target);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// --- DeclareSet ---

		GraphResources::Export ExportsOf(std::uint32_t a_bindFlags)
		{
			auto exports = GraphResources::Export::SRV;
			if (a_bindFlags & D3D11_BIND_RENDER_TARGET)
				exports = exports | GraphResources::Export::RTV;
			if (a_bindFlags & D3D11_BIND_UNORDERED_ACCESS)
				exports = exports | GraphResources::Export::UAV;
			if (a_bindFlags & D3D11_BIND_DEPTH_STENCIL)
				exports = exports | GraphResources::Export::DSV;
			return exports;
		}

		GraphResources::Options TwinOptions(std::uint32_t a_bindFlags)
		{
			return { .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = ExportsOf(a_bindFlags), .versioned = true };
		}

		template <class Twin>
		void Publish(const Frame::DeclaredTexture<Twin>& a_twin, const std::string& a_name)
		{
			if (!a_twin)
				RenderGraph::Fatal("engine targets", fmt::format("{} could not be declared", a_name));
			// Published at setup, so no reader's first ask publishes it inside a frame.
			if (!PublishedResources::Publish(a_name, *a_twin))
				RenderGraph::Fatal("engine targets", fmt::format("{} could not be published", a_name));
		}

		/** @brief A colour target's twin. a_join is a slot its versions join (native's kFRAMEBUFFER), or null for its own. */
		Frame::DeclaredTexture2D DeclareColour(std::uint32_t a_index, bool a_copy, DXGI_FORMAT a_copyFormat, std::uint32_t a_bindFlags,
			std::shared_ptr<org::VersionedTexture> a_join = nullptr)
		{
			using namespace Frame;
			auto name = ColourName(a_index) + (a_copy ? " Copy" : "");
			auto options = TwinOptions(a_bindFlags);
			options.joinSlot = std::move(a_join);
			auto twin = DeclareTexture2D(
				name, DependsOn(Vars::EngineTargetShapes()),
				[a_index, a_copy, a_copyFormat](const Inputs& a_in) {
					const auto shape = a_in.Get(Vars::EngineTargetShapes()).colours[a_index];
					return Desc2D(a_copy ? CopyShape(shape, a_copyFormat) : shape);
				},
				options);
			Publish(twin, name);
			return twin;
		}

		/** @brief Which colour targets are declared, unbacked, the engine's own, or views of another. */
		void SortColours()
		{
			auto& state = Set();
			for (std::uint32_t i = 0; i < kColourSlots; ++i) {
				auto& record = state.colours[i];
				if (!record.known)
					continue;
				if (i != kFramebuffer && record.props.allowMipGeneration)
					record.mode = Mode::EngineOwned;
				else if (i != kFramebuffer && record.props.mipLevel != -1)
					record.mode = Mode::MipView;  // settled below, once every source is sorted
				else if (const auto* unbacked = FindUnbacked(i)) {
					record.mode = Mode::Unbacked;
					record.unbacked = unbacked;
				} else
					record.mode = Mode::Backed;
			}
			std::string engineOwned, mipViews;
			for (std::uint32_t i = 0; i < kColourSlots; ++i) {
				auto& record = state.colours[i];
				if (record.mode == Mode::EngineOwned)
					engineOwned += fmt::format("{}{}", engineOwned.empty() ? "" : ", ", ColourName(i));
				if (record.mode != Mode::MipView)
					continue;
				const auto source = record.props.textureTarget;
				const auto sourceMode = source < kColourSlots ? state.colours[source].mode : Mode::Unknown;
				if (sourceMode == Mode::EngineOwned) {
					record.mode = Mode::EngineOwned;
					engineOwned += fmt::format("{}{} (mip {} of {})", engineOwned.empty() ? "" : ", ", ColourName(i), record.props.mipLevel, ColourName(source));
					continue;
				}
				if (sourceMode != Mode::Backed && sourceMode != Mode::Unbacked)
					RenderGraph::Fatal("engine targets", fmt::format("{} views mip {} of {}, which is neither declared nor the engine's", ColourName(i), record.props.mipLevel, ColourName(source)));
				if (record.props.mipLevel != 0)
					RenderGraph::Fatal("engine targets", fmt::format("{} views mip {} of {}, which the set declares with one mip", ColourName(i), record.props.mipLevel, ColourName(source)));
				mipViews += fmt::format("{}{} (of {})", mipViews.empty() ? "" : ", ", ColourName(i), ColourName(source));
			}
			logger::info("[ORG] engine target set: the engine's own (mip-generating and their mip views): {}; mip views of declared targets: {}",
				engineOwned.empty() ? "none" : engineOwned, mipViews.empty() ? "none" : mipViews);
		}

		void ReleaseEngineColour(RE::BSGraphics::RenderTargetData& a_data, bool a_viewsOnly)
		{
			ReleaseField(a_data.UAV);
			ReleaseField(a_data.RTV);
			ReleaseField(a_data.SRV);
			if (a_viewsOnly)
				return;
			ReleaseField(a_data.SRVCopy);
			ReleaseField(a_data.textureCopy);
			ReleaseField(a_data.texture);
		}

		void ReleaseEngineDepth(RE::BSGraphics::DepthStencilData& a_data)
		{
			for (auto& view : a_data.views)
				ReleaseField(view);
			for (auto& view : a_data.readOnlyViews)
				ReleaseField(view);
			ReleaseField(a_data.depthSRV);
			ReleaseField(a_data.stencilSRV);
			ReleaseField(a_data.texture);
		}

		void ReleaseEngineCube(RE::BSGraphics::CubemapRenderTargetData& a_data)
		{
			for (auto& view : a_data.cubeSideRTV)
				ReleaseField(view);
			ReleaseField(a_data.SRV);
			ReleaseField(a_data.texture);
		}
	}

	void Install()
	{
		if (!Engine::GraphMode())
			return;
		logger::info("[ORG] engine target set: hooking the render target creation functions");
		stl::detour_thunk<CreateRenderTarget>(REL::RelocationID(75467, 77253));
		stl::detour_thunk<CreateDepthStencilTarget>(REL::RelocationID(75469, 77255));
		stl::detour_thunk<Create3DRenderTarget>(REL::RelocationID(75470, 77256));
		stl::detour_thunk<CreateCubeMapRenderTarget>(REL::RelocationID(75471, 77257));
	}

	void BeginSet()
	{
		Set().inSet = true;
		// Native mode, a later set: the engine took its size from GetBuffer's description (CreateSwapChainViews), which is
		// kFRAMEBUFFER's adopted one. The swap chain's new size goes in its place, so the engine asks for its targets at that size.
		if (!g_declared.load(std::memory_order_acquire) || !Native::Active())
			return;
		std::uint32_t width = 0, height = 0;
		if (!Native::EngineSwapChainExtent(width, height))
			return;
		EngineSize size;
		size.width = width;
		size.height = height;
		size.viewport[0] = width;
		size.viewport[1] = height;
		WriteEngineSize(size);
	}

	void EndSet()
	{
		auto& state = Set();
		state.inSet = false;
		if (!g_declared.load(std::memory_order_acquire)) {
			// The first set, or D3D11 mode: the variables from the engine's own fields. DeclareSet writes the table.
			RaiseFromFields();
			return;
		}
		// The engine released the fields of targets it never makes again (Deferred's planned ones), and made kFRAMEBUFFER's from
		// the swap chain. They get the set's handles back.
		auto& framebuffer = ColourData(kFramebuffer);
		const auto& framebufferRecord = state.colours[kFramebuffer];
		if (framebuffer.texture != framebufferRecord.texture.handle.get())
			ReleaseField(framebuffer.texture);
		if (framebuffer.RTV != framebufferRecord.rtv.handle.get())
			ReleaseField(framebuffer.RTV);
		if (framebuffer.SRV != framebufferRecord.srv.handle.get())
			ReleaseField(framebuffer.SRV);
		for (std::uint32_t i = 0; i < kColourSlots; ++i) {
			const auto& record = state.colours[i];
			if (record.mode != Mode::Backed && record.mode != Mode::Unbacked && record.mode != Mode::MipView)
				continue;
			auto& data = ColourData(i);
			const auto name = ColourName(i);
			if (record.mode != Mode::MipView) {
				RefillField(data.texture, record.texture, name);
				RefillField(data.textureCopy, record.textureCopy, name);
				RefillField(data.SRVCopy, record.srvCopy, name);
			}
			RefillField(data.RTV, record.rtv, name);
			RefillField(data.SRV, record.srv, name);
			RefillField(data.UAV, record.uav, name);
		}
		AliasWindowViews();
		// The engine wrote its new size. It keeps drawing at the size of the targets it has until the revision is adopted.
		state.wanted = ReadEngineSize();
		WriteEngineSize(state.adoptedSize);
		const Extent2D display{ state.wanted.width, state.wanted.height };
		RaiseShapeVariables(BuildTable(display), display);
		logger::info("[ORG] engine target set: the engine made its targets again at {}x{}; the next revision brings them", display.width, display.height);
	}

	bool OwnsSets()
	{
		return g_declared.load(std::memory_order_acquire);
	}

	void PlanTarget(std::uint32_t a_target, const Plan& a_plan)
	{
		if (!Engine::GraphMode() || a_target >= kColourSlots)
			return;
		if (g_declared.load(std::memory_order_acquire))
			RenderGraph::Fatal("engine targets", fmt::format("{} plans {} after the targets were declared", a_plan.owner, ColourName(a_target)));
		Set().plans[a_target] = a_plan;
	}

	void DeclareSet()
	{
		if (!Engine::GraphMode())
			return;
		if (!RenderGraphRuntime::Get().IsActive())
			RenderGraph::Fatal("engine targets", fmt::format("the render graph is not active ({})", RenderGraphRuntime::Get().GetDisabledReason()));
		if (g_declared.load(std::memory_order_acquire))
			return;
		auto& state = Set();
		if (!state.colours[kMain].known)
			RenderGraph::Fatal("engine targets", "the engine's first set did not reach the create detours (kMAIN is unknown)");
		g_framebufferFormat.store(static_cast<std::uint32_t>(SwapChainFormat()), std::memory_order_release);
		// The engine makes kFRAMEBUFFER from the swap chain's buffer, not through the create detour, so its record is never seen
		// there. The set declares it anyway: its shape comes from the display size and the swap chain's format (ColourShape).
		if (auto& framebuffer = state.colours[kFramebuffer]; !framebuffer.known) {
			framebuffer.props = {};
			framebuffer.props.mipLevel = -1;
			framebuffer.known = true;
		}
		// Deferred reuses slots the PC engine never makes (the indirect targets). Their plan gives the whole shape. A plan that
		// only changes the engine's shape (HDR's) leaves a target the engine never made unknown.
		for (std::uint32_t i = 0; i < kColourSlots; ++i) {
			auto& record = state.colours[i];
			const auto& plan = state.plans[i];
			if (record.known || !plan || !plan->mainSize || plan->format == DXGI_FORMAT_UNKNOWN || !plan->bindFlags)
				continue;
			record.props = {};
			record.props.width = state.colours[kMain].props.width;
			record.props.height = state.colours[kMain].props.height;
			record.props.format = static_cast<RE::BSGraphics::Format>(plan->format);
			record.props.supportUnorderedAccess = (plan->bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
			record.props.mipLevel = -1;
			record.known = true;
			logger::info("[ORG] engine target set: {} is not made by the engine; declared from {}'s plan", ColourName(i), plan->owner);
		}
		SortColours();

		// The variables first: the declarations read the newest written values.
		const auto display = ScreenSize();
		auto& noise = state.volumes[kNoiseVolume];
		noise.shape = NoiseShape();
		if (!noise.shape.width || !noise.shape.height || !noise.shape.depthOrSlices)
			noise.shape = {};
		const auto table = BuildTable(display);
		RaiseShapeVariables(table, display);

		std::uint32_t declared = 0, unbacked = 0;
		for (std::uint32_t i = 0; i < kColourSlots; ++i) {
			auto& record = state.colours[i];
			const auto& shape = table.colours[i];
			if (record.mode == Mode::Backed) {
				record.copyFormat = static_cast<DXGI_FORMAT>(record.props.format.underlying());
				// Native made kFRAMEBUFFER's slot with the swap chain, and the present copies it. The twin's versions join it.
				std::shared_ptr<org::VersionedTexture> join;
				if (i == kFramebuffer && Native::Active()) {
					join = Native::BackBufferSlot();
					if (!join)
						RenderGraph::Fatal("engine targets", "native mode made no kFRAMEBUFFER slot with its swap chain");
				}
				record.main = DeclareColour(i, false, DXGI_FORMAT_UNKNOWN, shape.bindFlags, std::move(join));
				if (shape.copy)
					record.copy = DeclareColour(i, true, record.copyFormat, D3D11_BIND_SHADER_RESOURCE);
				++declared;
				if (const auto& plan = state.plans[i]; plan && shape.format != record.copyFormat)
					logger::info("[ORG] {}: declared in format {}, as {} plans it (the engine asks for {})", ColourName(i), static_cast<int>(shape.format), plan->owner,
						static_cast<int>(record.copyFormat));
			} else if (record.mode == Mode::Unbacked) {
				record.copyFormat = static_cast<DXGI_FORMAT>(record.props.format.underlying());
				++unbacked;
			}
		}
		for (std::uint32_t i = 0; i < kDepthSlots; ++i) {
			auto& record = state.depths[i];
			if (!record.known)
				continue;
			if (record.props.arraySize == 0 || record.props.arraySize > kMaxDepthSlices)
				RenderGraph::Fatal("engine targets", fmt::format("{} has {} slices; the set takes 1 to {}", DepthName(i), record.props.arraySize, kMaxDepthSlices));
			const auto name = DepthName(i);
			record.main = Frame::DeclareTexture2D(
				name, Frame::DependsOn(Frame::Vars::EngineTargetShapes()),
				[i](const Frame::Inputs& a_in) { return Desc2D(a_in.Get(Frame::Vars::EngineTargetShapes()).depths[i]); }, TwinOptions(table.depths[i].bindFlags));
			Publish(record.main, name);
			++declared;
		}
		for (std::uint32_t i = 0; i < kCubeSlots; ++i) {
			auto& record = state.cubes[i];
			if (!record.known)
				continue;
			const auto name = CubeName(i);
			record.main = Frame::DeclareTexture2D(
				name, Frame::DependsOn(Frame::Vars::EngineTargetShapes()),
				[i](const Frame::Inputs& a_in) { return Desc2D(a_in.Get(Frame::Vars::EngineTargetShapes()).cubes[i]); }, TwinOptions(table.cubes[i].bindFlags));
			Publish(record.main, name);
			++declared;
		}
		if (noise.shape.width) {
			// The 3D targets are made inside a frame, when volumetric lighting first renders. Declared now, so nothing is declared then.
			const auto name = VolumeName(kNoiseVolume);
			const auto desc = Desc3D(noise.shape);
			noise.main = Frame::DeclareTexture3D(
				name, Frame::Dependencies{}, [desc](const Frame::Inputs&) { return desc; },
				GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV, .versioned = true });
			Publish(noise.main, name);
			TakeObject(noise.texture, noise.main->D3D11(), name);
			TakeObject(noise.srv, noise.main->SRV(), name + " SRV");
			TakeObject(noise.uav, noise.main->UAV(), name + " UAV");
			noise.known = true;
			++declared;
		} else {
			logger::warn("[ORG] engine target set: the noise volume's INI size (iVolumetricLightingNoiseTexture*:Display) is not readable; it is not declared");
		}

		// Version 0's objects, in place of the engine's first D3D11 objects.
		for (std::uint32_t i = 0; i < kColourSlots; ++i)
			if (state.colours[i].mode == Mode::Backed || state.colours[i].mode == Mode::Unbacked)
				BuildColour(i, table.colours[i]);
		for (std::uint32_t i = 0; i < kColourSlots; ++i)
			if (state.colours[i].mode == Mode::MipView)
				BuildColour(i, table.colours[i]);
		for (std::uint32_t i = 0; i < kDepthSlots; ++i)
			if (state.depths[i].main)
				BuildDepth(i);
		for (std::uint32_t i = 0; i < kCubeSlots; ++i)
			if (state.cubes[i].main)
				BuildCube(i);

		for (std::uint32_t i = 0; i < kColourSlots; ++i) {
			auto& record = state.colours[i];
			if (record.mode != Mode::Backed && record.mode != Mode::Unbacked && record.mode != Mode::MipView)
				continue;
			auto& data = ColourData(i);
			const auto name = ColourName(i);
			const auto graph = GraphOf(record.main);
			ReleaseEngineColour(data, record.mode == Mode::MipView);
			if (record.mode != Mode::MipView) {
				PutObject(data.texture, record.texture, name, graph);
				PutObject(data.textureCopy, record.textureCopy, name + " Copy", GraphOf(record.copy));
				PutObject(data.SRVCopy, record.srvCopy, name + " Copy SRV", GraphOf(record.copy));
			}
			PutObject(data.RTV, record.rtv, name + " RTV", graph);
			PutObject(data.SRV, record.srv, name + " SRV", graph);
			PutObject(data.UAV, record.uav, name + " UAV", graph);
		}
		AliasWindowViews();
		for (std::uint32_t i = 0; i < kDepthSlots; ++i) {
			auto& record = state.depths[i];
			if (!record.main)
				continue;
			auto& data = DepthData(i);
			const auto name = DepthName(i);
			const auto graph = GraphOf(record.main);
			ReleaseEngineDepth(data);
			PutObject(data.texture, record.texture, name, graph);
			for (std::uint32_t slice = 0; slice < kMaxDepthSlices; ++slice) {
				PutObject(data.views[slice], record.views[slice], name, graph);
				PutObject(data.readOnlyViews[slice], record.readOnlyViews[slice], name, graph);
			}
			PutObject(data.depthSRV, record.depthSrv, name, graph);
			PutObject(data.stencilSRV, record.stencilSrv, name, graph);
		}
		for (std::uint32_t i = 0; i < kCubeSlots; ++i) {
			auto& record = state.cubes[i];
			if (!record.main)
				continue;
			auto& data = CubeData(i);
			const auto name = CubeName(i);
			const auto graph = GraphOf(record.main);
			ReleaseEngineCube(data);
			PutObject(data.texture, record.texture, name, graph);
			for (std::uint32_t face = 0; face < 6; ++face)
				PutObject(data.cubeSideRTV[face], record.faces[face], name, graph);
			PutObject(data.SRV, record.srv, name, graph);
		}

		state.adopted = table;
		state.adoptedSize = ReadEngineSize();
		state.wanted = state.adoptedSize;
		Frame::Revision::OnVersionsAdopted([] { OnVersionsAdopted(); });
		g_declared.store(true, std::memory_order_release);
		logger::info("[ORG] engine target set: {} targets declared, {} without memory, at {}x{}; kFRAMEBUFFER in format {}", declared, unbacked, display.width,
			display.height, g_framebufferFormat.load(std::memory_order_relaxed));
		for (const auto& entry : kUnbackedColours)
			if (state.colours[entry.target].mode == Mode::Unbacked)
				logger::info("[ORG] engine target set: {} has no memory: {}", ColourName(entry.target), entry.users);
		// Native mode's device and context are handles from the start. The guest ones come with the state setup hook.
		if (GuestsInstalled())
			InstallHandles();
	}

	void InstallHandles()
	{
		if (!Engine::GraphMode() || g_handles.load(std::memory_order_acquire))
			return;
		if (!g_declared.load(std::memory_order_acquire)) {
			logger::warn("[ORG] engine target set: the handles were asked for before the set was declared; they come with DeclareSet");
			return;
		}
		if (!GuestsInstalled())
			RenderGraph::Fatal("engine targets", "the engine's device and context are not tripwire handles, so the target fields cannot hold handles");
		auto& state = Set();
		for (std::uint32_t i = 0; i < kColourSlots; ++i)
			if (state.colours[i].mode == Mode::Backed || state.colours[i].mode == Mode::Unbacked)
				ColourHandles(i);
		for (std::uint32_t i = 0; i < kColourSlots; ++i)
			if (state.colours[i].mode == Mode::MipView)
				ColourHandles(i);
		for (std::uint32_t i = 0; i < kDepthSlots; ++i)
			if (state.depths[i].main)
				DepthHandles(i);
		for (std::uint32_t i = 0; i < kCubeSlots; ++i)
			if (state.cubes[i].main)
				CubeHandles(i);
		if (auto& noise = state.volumes[kNoiseVolume]; noise.known) {
			const auto name = VolumeName(kNoiseVolume);
			const auto graph = GraphOf(noise.main);
			MakeHandle(noise.texture, name, graph);
			MakeHandle(noise.srv, noise.texture.handle.get(), name + " SRV", graph);
			MakeHandle(noise.uav, noise.texture.handle.get(), name + " UAV", graph);
			const auto index = static_cast<std::uint16_t>(kNoiseVolume);
			Note(noise.texture.handle.get(), { Kind::Volume, index, View::Texture });
			Note(noise.srv.handle.get(), { Kind::Volume, index, View::Srv });
			Note(noise.uav.handle.get(), { Kind::Volume, index, View::Uav });
		}
		// Native mode: the set adopted native's kFRAMEBUFFER slot, so GetBuffer answers the field from now on.
		if (Native::Active()) {
			const auto& framebuffer = state.colours[kFramebuffer];
			if (framebuffer.mode != Mode::Backed || !framebuffer.main)
				RenderGraph::Fatal("engine targets", "kFRAMEBUFFER is not declared, so native present has no back buffer slot");
			Native::AdoptBackBufferSlot(framebuffer.main->Versioned(), framebuffer.texture.handle);
		}
		AliasWindowViews();
		g_handles.store(true, std::memory_order_release);
		logger::info("[ORG] engine target set: tripwire handles in the fields ({} handles known to Identify)", state.identify.size());
	}

	void CheckFields(std::string_view a_where)
	{
		if (!g_handles.load(std::memory_order_acquire))
			return;
		auto& state = Set();
		const auto check = [&](const void* a_field, const void* a_handle, std::uint32_t a_index, const char* a_what) {
			if (a_handle && a_field != a_handle)
				RenderGraph::Fatal("engine targets", fmt::format("{}'s {} field holds {} instead of its handle {} ({})", ColourName(a_index), a_what, a_field, a_handle, a_where));
		};
		for (std::uint32_t i = 0; i < kColourSlots; ++i) {
			const auto& record = state.colours[i];
			if (record.mode != Mode::Backed && record.mode != Mode::Unbacked && record.mode != Mode::MipView)
				continue;
			const auto& data = ColourData(i);
			check(data.texture, record.texture.handle.get(), i, "texture");
			check(data.RTV, record.rtv.handle.get(), i, "RTV");
			check(data.SRV, record.srv.handle.get(), i, "SRV");
			check(data.UAV, record.uav.handle.get(), i, "UAV");
		}
	}

	bool SetDeclared()
	{
		return g_declared.load(std::memory_order_acquire);
	}

	std::optional<TargetRef> Identify(const ID3D11View* a_view)
	{
		if (!a_view || !g_handles.load(std::memory_order_acquire))
			return std::nullopt;
		const auto& map = Set().identify;
		const auto found = map.find(static_cast<const void*>(a_view));
		return found != map.end() ? std::optional<TargetRef>(found->second) : std::nullopt;
	}

	std::optional<TargetRef> Identify(const ID3D11Resource* a_resource)
	{
		if (!a_resource || !g_handles.load(std::memory_order_acquire))
			return std::nullopt;
		const auto& map = Set().identify;
		const auto found = map.find(static_cast<const void*>(a_resource));
		return found != map.end() ? std::optional<TargetRef>(found->second) : std::nullopt;
	}

	namespace
	{
		FrameAnchors::Texture SlotOf(const std::shared_ptr<org::VersionedTexture>& a_slot, std::string a_name)
		{
			if (!a_slot)
				return {};
			return FrameAnchors::Texture(std::move(a_name), a_slot);
		}

		FrameAnchors::Texture UnbackedUse(TargetRef a_target)
		{
			const auto name = Name(a_target);
			Frame::Driver::NoteUnbackedUse(name, fmt::format("a graph import of {}, an engine target with no memory ({}); the reader gets no texture", name,
													 UnbackedReason(a_target)));
			return {};
		}
	}

	FrameAnchors::Texture Slot(TargetRef a_target)
	{
		if (!g_declared.load(std::memory_order_acquire))
			return {};
		auto& state = Set();
		switch (a_target.kind) {
		case Kind::Colour:
		case Kind::ColourCopy:
			{
				if (a_target.index >= kColourSlots)
					return {};
				std::uint32_t index = a_target.index;
				if (state.colours[index].mode == Mode::MipView)
					index = state.colours[index].props.textureTarget;
				const auto& record = state.colours[index];
				if (record.mode == Mode::Unbacked)
					return UnbackedUse(a_target);
				if (record.mode != Mode::Backed)
					return {};
				const auto& twin = a_target.kind == Kind::Colour ? record.main : record.copy;
				return twin ? SlotOf(twin->Versioned(), twin->Name()) : FrameAnchors::Texture();
			}
		case Kind::Depth:
			if (a_target.index >= kDepthSlots || !state.depths[a_target.index].main)
				return {};
			return SlotOf(state.depths[a_target.index].main->Versioned(), DepthName(a_target.index));
		case Kind::Cubemap:
			if (a_target.index >= kCubeSlots || !state.cubes[a_target.index].main)
				return {};
			return SlotOf(state.cubes[a_target.index].main->Versioned(), CubeName(a_target.index));
		case Kind::Volume:
			if (a_target.index >= kVolumeSlots)
				return {};
			if (UnbackedVolume(a_target.index))
				return UnbackedUse(a_target);
			if (!state.volumes[a_target.index].main)
				return {};
			return SlotOf(state.volumes[a_target.index].main->Versioned(), VolumeName(a_target.index));
		}
		return {};
	}

	FrameAnchors::Texture FramebufferSlot()
	{
		return Slot(Colour(kFramebuffer));
	}

	ID3D11Texture2D* FramebufferTexture()
	{
		if (!g_handles.load(std::memory_order_acquire))
			return nullptr;
		return Set().colours[kFramebuffer].texture.handle.get();
	}

	DXGI_FORMAT FramebufferFormat()
	{
		return static_cast<DXGI_FORMAT>(g_framebufferFormat.load(std::memory_order_acquire));
	}

	Shape AdoptedShape(TargetRef a_target)
	{
		if (!g_declared.load(std::memory_order_acquire))
			return {};
		const auto& state = Set();
		switch (a_target.kind) {
		case Kind::Colour:
			return a_target.index < kColourSlots ? state.adopted.colours[a_target.index] : Shape{};
		case Kind::ColourCopy:
			if (a_target.index >= kColourSlots || !state.adopted.colours[a_target.index].copy)
				return {};
			return CopyShape(state.adopted.colours[a_target.index], state.colours[a_target.index].copyFormat);
		case Kind::Depth:
			return a_target.index < kDepthSlots ? state.adopted.depths[a_target.index] : Shape{};
		case Kind::Cubemap:
			return a_target.index < kCubeSlots ? state.adopted.cubes[a_target.index] : Shape{};
		case Kind::Volume:
			return a_target.index < kVolumeSlots ? state.volumes[a_target.index].shape : Shape{};
		}
		return {};
	}

	D3D11_TEXTURE2D_DESC Desc(TargetRef a_target)
	{
		if (a_target.kind == Kind::Volume)
			return {};
		const auto shape = AdoptedShape(a_target);
		return shape.width ? Desc2D(shape) : D3D11_TEXTURE2D_DESC{};
	}

	DXGI_FORMAT ViewFormat(TargetRef a_target)
	{
		const auto shape = AdoptedShape(a_target);
		if (!shape.width)
			return DXGI_FORMAT_UNKNOWN;
		if (a_target.kind != Kind::Depth)
			return shape.format;
		const auto formats = FormatsOfTypeless(shape.format);
		switch (a_target.view) {
		case View::Dsv:
		case View::ReadOnlyDsv:
			return formats.dsv;
		case View::Srv:
			return formats.srv;
		case View::StencilSrv:
			return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
		default:
			return shape.format;
		}
	}

	bool Unbacked(TargetRef a_target)
	{
		if (!g_declared.load(std::memory_order_acquire))
			return false;
		if (a_target.kind == Kind::Volume)
			return UnbackedVolume(a_target.index);
		if ((a_target.kind == Kind::Colour || a_target.kind == Kind::ColourCopy) && a_target.index < kColourSlots)
			return Set().colours[a_target.index].mode == Mode::Unbacked;
		return false;
	}

	std::string_view UnbackedReason(TargetRef a_target)
	{
		if (!Unbacked(a_target))
			return "";
		if (a_target.kind == Kind::Volume)
			return kUnbackedVolume;
		const auto* entry = Set().colours[a_target.index].unbacked;
		return entry ? entry->users : "";
	}

	bool EngineOwned(TargetRef a_target)
	{
		if (!g_declared.load(std::memory_order_acquire))
			return false;
		return (a_target.kind == Kind::Colour || a_target.kind == Kind::ColourCopy) && a_target.index < kColourSlots &&
		       Set().colours[a_target.index].mode == Mode::EngineOwned;
	}

	std::string Name(TargetRef a_target)
	{
		switch (a_target.kind) {
		case Kind::Colour:
			return ColourName(a_target.index);
		case Kind::ColourCopy:
			return ColourName(a_target.index) + " Copy";
		case Kind::Depth:
			return DepthName(a_target.index);
		case Kind::Cubemap:
			return CubeName(a_target.index);
		case Kind::Volume:
			return VolumeName(a_target.index);
		}
		return {};
	}

}

