#include "ORGSunRays.h"

#include <cstring>

bool ORGSunRays::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_E11_RAYS", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

ORGSunRays& ORGSunRays::Get()
{
	static ORGSunRays s_rays;
	return s_rays;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/CloudShadows.h"
#	include "Features/CloudShadows/ORGCloudShadows.h"
#	include "Features/TerrainShadows.h"
#	include "Features/TerrainShadows/ORGTerrainShadows.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "State.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <atomic>
#	include <cstddef>
#	include <functional>
#	include <map>
#	include <optional>
#	include <string>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.e11-rays";
	constexpr const char* kShaderDir = "Effects11/";
	// Keys at the PostComposite anchor: the volumetric rays first, then the sun rays, as the D3D11 path draws them.
	constexpr std::int32_t kVolumetricOrder = 100;
	constexpr std::int32_t kSunRaysOrder = 200;

	using ShaderBindings::Handle;

	// The blurs' groups cover this many pixels along the blurred axis (BlurVolumetricRaysCS: TG_DIM - WINDOW * 2).
	constexpr std::uint32_t kBlurGroupPixels = 256 - 12 * 2;

	/** @brief Mirrors VLData in Effects11/VolumetricRaysData.hlsli. */
	struct VLData
	{
		std::int32_t screenX, screenY, screenXMin1, screenYMin1;
	};
	static_assert(sizeof(VLData) == 16);

	// The frame's values, in its latch region: the half-res size, then each sun ray pass's constants.
	struct LatchData
	{
		VLData vl;
		std::array<Effects11::SunRaysData, ORGSunRays::kSunRaysPasses> sun;
	};
	static_assert(offsetof(LatchData, sun) % 16 == 0 && sizeof(Effects11::SunRaysData) % 16 == 0);

	constexpr std::uint32_t SunOffset(std::uint32_t a_pass)
	{
		return static_cast<std::uint32_t>(offsetof(LatchData, sun) + a_pass * sizeof(Effects11::SunRaysData));
	}

	// The shaders' push blocks, as their resource lists give them.
	struct RaymarchBlock  // RaymarchVolumetricRaysPS
	{
		SharedConstants::Handles shared;
		Handle sampler, depth, shadowHeights, cloudShadows;
	};
	static_assert(sizeof(RaymarchBlock) == 32);

	struct BlurBlock  // BlurVolumetricRaysCS
	{
		Handle input, linearDepth, output;
		ShaderBindings::Latch vl;
	};
	static_assert(sizeof(BlurBlock) == 20);

	struct ApplyBlock  // ApplyVolumetricRaysPS
	{
		SharedConstants::Handles shared;
		Handle scattering, raymarchDepth;
		ShaderBindings::Latch vl;
		Handle depth;
	};
	static_assert(sizeof(ApplyBlock) == 36);

	struct SunRaysBlock  // SunRaysPS
	{
		SharedConstants::Handles shared;
		Handle sampler, rays;
		ShaderBindings::Latch data;
		Handle depth, cloudShadows;
	};
	static_assert(sizeof(SunRaysBlock) == 40);

	/** @brief A graph resource a pass reads: one imported, or one another client registers by name. */
	struct Source
	{
		std::shared_ptr<org::Resource> resource;
		std::string name;

		explicit operator bool() const { return resource || !name.empty(); }
		bool operator==(const Source&) const = default;
	};

	org::DeclaredViewToken Read(org::PassBuilder& a_builder, const Source& a_source)
	{
		if (a_source.resource)
			return a_builder.ShaderResource(a_source.resource).View();
		return a_builder.ShaderResource(org::ResourceIdentifier(a_source.name)).View();
	}

	/** @brief What the graph is built from: a change makes a new client. */
	struct Shape
	{
		std::uint32_t halfTexWidth = 0, halfTexHeight = 0;
		rhi::Format mainFormat = rhi::Format::Unknown;
		std::shared_ptr<org::Resource> main;
		Source depth, shadowHeights, cloudShadows;

		bool operator==(const Shape&) const = default;
	};

	/** @brief The sizes the passes cover this frame, packed for an atomic: half-res width and height, then full. */
	std::uint64_t Pack(const ORGSunRays::Sizes& a_sizes)
	{
		return static_cast<std::uint64_t>(a_sizes.halfWidth & 0xFFFF) | static_cast<std::uint64_t>(a_sizes.halfHeight & 0xFFFF) << 16 |
		       static_cast<std::uint64_t>(a_sizes.width & 0xFFFF) << 32 | static_cast<std::uint64_t>(a_sizes.height & 0xFFFF) << 48;
	}

	struct Area
	{
		std::uint32_t halfWidth = 0, halfHeight = 0, width = 0, height = 0;
	};

	Area Unpack(std::uint64_t a_packed)
	{
		return { static_cast<std::uint32_t>(a_packed & 0xFFFF), static_cast<std::uint32_t>(a_packed >> 16 & 0xFFFF),
			static_cast<std::uint32_t>(a_packed >> 32 & 0xFFFF), static_cast<std::uint32_t>(a_packed >> 48 & 0xFFFF) };
	}

	/** @brief Everything a build's passes are made from: immutable once registered, but the per-frame flags. */
	struct RaysState
	{
		Shape shape;
		std::shared_ptr<const RasterProgram> raymarch, apply, sunMask, sunBlur, sunComposite;
		std::shared_ptr<const ComputeProgram> blurH, blurV;
		// The volumetric rays' scattering (raymarched, then blurred back into it), the blur's other half and the raymarch's depth;
		// the sun rays' ping-pong pair.
		std::unique_ptr<GraphResources::Texture2D> vlScattering, vlBlur, vlDepth, sunA, sunB;
		std::shared_ptr<org::LatchBlock> latch;
		org::OwnedDescriptorBinding linearSampler;
		// Set at the hook: whether this run has each effect, and the areas they cover. A run another client keeps going records nothing.
		std::shared_ptr<std::atomic<bool>> volumetricLive = std::make_shared<std::atomic<bool>>(false);
		std::shared_ptr<std::atomic<bool>> sunLive = std::make_shared<std::atomic<bool>>(false);
		std::shared_ptr<std::atomic<std::uint64_t>> area = std::make_shared<std::atomic<std::uint64_t>>(0);

		bool Live(bool a_sun) const { return (a_sun ? sunLive : volumetricLive)->load(std::memory_order_acquire); }
		std::uint64_t PackedArea() const { return area->load(std::memory_order_acquire); }
	};

	/** @brief What every pass records from: the state, the frame's area and the pass's index; null state when the effect is off this run. */
	struct RaysFrame
	{
		std::shared_ptr<const RaysState> state;
		Area area;
		std::uint32_t index = 0;
	};

	/** @brief What the client's passes share: the revision and the frame follow the effect's flag and the area. */
	struct PassCommon
	{
		std::shared_ptr<const RaysState> state;
		bool sun = false;
		// The blur's direction (1: horizontal) or the sun ray pass.
		std::uint32_t index = 0;

		void AddRevision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get()));
			a_out.push_back(state->Live(sun));
			a_out.push_back(state->PackedArea());
			a_out.push_back(index);
		}

		RaysFrame Frame() const
		{
			if (!state->Live(sun))
				return {};
			return { state, Unpack(state->PackedArea()), index };
		}
	};

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// Volumetric rays
	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	struct RaymarchBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken scattering, depthOut, depth, shadowHeights, cloudShadows;
		bool hasShadowHeights = false, hasCloudShadows = false;
	};

	/** @brief The raymarch: the scattering and its linear depth into the half-res pair (two targets). */
	class RaymarchPass final : public org::TypedRenderGraphPass<RaymarchPass, RaysFrame, RaymarchBindings>
	{
	public:
		explicit RaymarchPass(std::shared_ptr<const RaysState> a_state) :
			common{ std::move(a_state), false, 0 } {}

		RaymarchBindings Declare(org::PassBuilder& a_builder)
		{
			const auto& s = *common.state;
			RaymarchBindings b{};
			b.shared = SharedConstants::Declare(a_builder);
			b.scattering = a_builder.RenderTarget(s.vlScattering->Graph()).View();
			b.depthOut = a_builder.RenderTarget(s.vlDepth->Graph()).View();
			b.depth = Read(a_builder, s.shape.depth);
			if (s.shape.shadowHeights) {
				b.shadowHeights = Read(a_builder, s.shape.shadowHeights);
				b.hasShadowHeights = true;
			}
			if (s.shape.cloudShadows) {
				b.cloudShadows = Read(a_builder, s.shape.cloudShadows);
				b.hasCloudShadows = true;
			}
			return b;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { common.AddRevision(a_out); }

		RaysFrame Prepare(const RaymarchBindings&, const org::PassPrepareContext&) const { return common.Frame(); }

		static void Record(const RaymarchBindings& a_bindings, const RaysFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.state)
				return;
			const auto& s = *a_frame.state;
			RaymarchBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.sampler = s.linearSampler.Index();
			block.depth = a_recording.Index(a_bindings.depth);
			block.shadowHeights = a_bindings.hasShadowHeights ? a_recording.Index(a_bindings.shadowHeights) : 0;
			block.cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : 0;
			// The raymarch writes every texel of the area: nothing to keep.
			const std::array<RasterProgram::Colour, 2> colours{ { { .view = &a_bindings.scattering, .load = rhi::LoadOp::DontCare },
				{ .view = &a_bindings.depthOut, .load = rhi::LoadOp::DontCare } } };
			s.raymarch->Draw(a_recording, { .colours = colours,
											  .viewport = { .width = a_frame.area.halfWidth, .height = a_frame.area.halfHeight },
											  .constants = ShaderBindings::Data(block),
											  .debugName = "Effects11::VolumetricRays Raymarch" });
		}

	private:
		PassCommon common;
	};

	struct BlurBindings
	{
		org::DeclaredViewToken input, linearDepth, output;
	};

	/** @brief One depth-aware blur: horizontal (the scattering into the other half), then vertical (back into the scattering). */
	class BlurPass final : public org::TypedRenderGraphPass<BlurPass, RaysFrame, BlurBindings>
	{
	public:
		BlurPass(std::shared_ptr<const RaysState> a_state, bool a_horizontal) :
			common{ std::move(a_state), false, a_horizontal ? 1u : 0u } {}

		BlurBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& s = *common.state;
			const bool horizontal = common.index != 0;
			const auto& from = horizontal ? s.vlScattering : s.vlBlur;
			const auto& to = horizontal ? s.vlBlur : s.vlScattering;
			BlurBindings b{};
			b.input = a_builder.ShaderResource(from->Graph()).View();
			b.linearDepth = a_builder.ShaderResource(s.vlDepth->Graph()).View();
			b.output = a_builder.UnorderedAccess(to->Graph()).View();
			return b;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { common.AddRevision(a_out); }

		RaysFrame Prepare(const BlurBindings&, const org::PassPrepareContext&) const { return common.Frame(); }

		static void Record(const BlurBindings& a_bindings, const RaysFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.state)
				return;
			const auto& s = *a_frame.state;
			const bool horizontal = a_frame.index != 0;
			BlurBlock block{};
			block.input = a_recording.Index(a_bindings.input);
			block.linearDepth = a_recording.Index(a_bindings.linearDepth);
			block.output = a_recording.Index(a_bindings.output);
			block.vl = ShaderBindings::LatchOf(*s.latch, a_recording.FrameSlot(), static_cast<std::uint32_t>(offsetof(LatchData, vl)));
			const auto& area = a_frame.area;
			// As the D3D11 dispatches: each group covers kBlurGroupPixels along the blurred axis, one row or column across it.
			if (horizontal)
				s.blurH->Dispatch(a_recording, ShaderBindings::Data(block), (area.halfWidth + kBlurGroupPixels - 1) / kBlurGroupPixels, area.halfHeight, 1);
			else
				s.blurV->Dispatch(a_recording, ShaderBindings::Data(block), area.halfWidth, (area.halfHeight + kBlurGroupPixels - 1) / kBlurGroupPixels, 1);
		}

	private:
		PassCommon common;
	};

	struct ApplyBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken main, scattering, raymarchDepth, depth;
	};

	/** @brief The apply pass: the blurred scattering, upsampled and lit, added to kMAIN. */
	class ApplyPass final : public org::TypedRenderGraphPass<ApplyPass, RaysFrame, ApplyBindings>
	{
	public:
		explicit ApplyPass(std::shared_ptr<const RaysState> a_state) :
			common{ std::move(a_state), false, 0 } {}

		ApplyBindings Declare(org::PassBuilder& a_builder)
		{
			const auto& s = *common.state;
			ApplyBindings b{};
			b.shared = SharedConstants::Declare(a_builder);
			b.main = a_builder.RenderTarget(s.shape.main).View();
			b.scattering = a_builder.ShaderResource(s.vlScattering->Graph()).View();
			b.raymarchDepth = a_builder.ShaderResource(s.vlDepth->Graph()).View();
			b.depth = Read(a_builder, s.shape.depth);
			return b;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { common.AddRevision(a_out); }

		RaysFrame Prepare(const ApplyBindings&, const org::PassPrepareContext&) const { return common.Frame(); }

		static void Record(const ApplyBindings& a_bindings, const RaysFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.state)
				return;
			const auto& s = *a_frame.state;
			ApplyBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.scattering = a_recording.Index(a_bindings.scattering);
			block.raymarchDepth = a_recording.Index(a_bindings.raymarchDepth);
			block.vl = ShaderBindings::LatchOf(*s.latch, a_recording.FrameSlot(), static_cast<std::uint32_t>(offsetof(LatchData, vl)));
			block.depth = a_recording.Index(a_bindings.depth);
			const RasterProgram::Colour colour{ .view = &a_bindings.main, .load = rhi::LoadOp::Load };
			s.apply->Draw(a_recording, { .colours = { &colour, 1 },
										   .viewport = { .width = a_frame.area.width, .height = a_frame.area.height },
										   .constants = ShaderBindings::Data(block),
										   .debugName = "Effects11::VolumetricRays Apply" });
		}

	private:
		PassCommon common;
	};

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// Sun rays
	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	struct SunRaysBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken target, rays, depth, cloudShadows;
		bool hasRays = false, hasCloudShadows = false;
	};

	/**
	 * @brief One sun ray pass: the mask (into A), a blur (A into B, B into A, A into B) or the composite (B added to kMAIN). Each
	 * reads its own SunRaysData from the latch region.
	 */
	class SunRaysPass final : public org::TypedRenderGraphPass<SunRaysPass, RaysFrame, SunRaysBindings>
	{
	public:
		SunRaysPass(std::shared_ptr<const RaysState> a_state, std::uint32_t a_pass) :
			common{ std::move(a_state), true, a_pass } {}

		SunRaysBindings Declare(org::PassBuilder& a_builder)
		{
			const auto& s = *common.state;
			const std::uint32_t pass = common.index;
			const bool composite = pass == ORGSunRays::kSunRaysPasses - 1;
			// The mask and the second blur write A, the other blurs write B; each blur and the composite read the last target.
			const auto& written = pass % 2 == 0 ? s.sunA : s.sunB;
			const auto& read = pass % 2 == 0 ? s.sunB : s.sunA;
			SunRaysBindings b{};
			b.shared = SharedConstants::Declare(a_builder);
			b.target = composite ? a_builder.RenderTarget(s.shape.main).View() : a_builder.RenderTarget(written->Graph()).View();
			if (pass != 0) {
				b.rays = a_builder.ShaderResource(read->Graph()).View();
				b.hasRays = true;
			}
			b.depth = Read(a_builder, s.shape.depth);
			if (pass == 0 && s.shape.cloudShadows) {
				b.cloudShadows = Read(a_builder, s.shape.cloudShadows);
				b.hasCloudShadows = true;
			}
			return b;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { common.AddRevision(a_out); }

		RaysFrame Prepare(const SunRaysBindings&, const org::PassPrepareContext&) const { return common.Frame(); }

		static void Record(const SunRaysBindings& a_bindings, const RaysFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.state)
				return;
			const auto& s = *a_frame.state;
			const std::uint32_t pass = a_frame.index;
			const bool composite = pass == ORGSunRays::kSunRaysPasses - 1;
			SunRaysBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.sampler = s.linearSampler.Index();
			block.rays = a_bindings.hasRays ? a_recording.Index(a_bindings.rays) : 0;
			block.data = ShaderBindings::LatchOf(*s.latch, a_recording.FrameSlot(), SunOffset(pass));
			block.depth = a_recording.Index(a_bindings.depth);
			block.cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : 0;
			// The half-res passes write every texel of the area; the composite adds to kMAIN.
			const RasterProgram::Colour colour{ .view = &a_bindings.target, .load = composite ? rhi::LoadOp::Load : rhi::LoadOp::DontCare };
			const auto& program = pass == 0 ? s.sunMask : (composite ? s.sunComposite : s.sunBlur);
			static constexpr std::array<const char*, ORGSunRays::kSunRaysPasses> kNames{ "Effects11::SunRays Mask", "Effects11::SunRays Blur 1",
				"Effects11::SunRays Blur 2", "Effects11::SunRays Blur 3", "Effects11::SunRays Composite" };
			RasterProgram::Viewport viewport{};
			viewport.width = composite ? a_frame.area.width : a_frame.area.halfWidth;
			viewport.height = composite ? a_frame.area.height : a_frame.area.halfHeight;
			program->Draw(a_recording, { .colours = { &colour, 1 }, .viewport = viewport, .constants = ShaderBindings::Data(block), .debugName = kNames[pass] });
		}

	private:
		PassCommon common;
	};

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// Inputs and programs
	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	/** @brief A D3D11 feature's 2D texture behind a_view, imported (FrameAnchors::D3D11Texture). Empty without one. */
	Source ImportView(std::string_view a_name, ID3D11ShaderResourceView* a_view)
	{
		if (!a_view)
			return {};
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return {};
		return { FrameAnchors::D3D11Texture(a_name, texture.get(), a_view, nullptr), {} };
	}

	/** @brief The scene depth the D3D11 passes read at PS t17: the engine's depth-stencil import when it is one of the engine's. */
	Source SceneDepth(ID3D11ShaderResourceView* a_view)
	{
		if (!a_view || !globals::game::renderer)
			return {};
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
			if (depthStencils[i].texture && depthStencils[i].texture == resource.get())
				return { FrameAnchors::EngineDepth(i), {} };
		// Terrain Blending's blended depth.
		return ImportView("Effects11::SceneDepth", a_view);
	}

	/** @brief A PS slot's bound view, which the D3D11 passes read. */
	winrt::com_ptr<ID3D11ShaderResourceView> BoundPixelView(UINT a_slot)
	{
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		globals::d3d::context->PSGetShaderResources(a_slot, 1, view.put());
		return view;
	}

	/** @brief The defines the D3D11 build gets (Util::CompileShader), as the programs take them. */
	std::vector<std::pair<std::wstring, std::wstring>> Defines(std::vector<std::string> a_names, const char* a_stage)
	{
		std::vector<std::pair<std::string, std::string>> defines;
		for (auto& name : a_names)
			defines.emplace_back(std::move(name), "");
		std::vector<std::string_view> explicitNames;
		for (const auto& define : defines)
			explicitNames.push_back(define.first);
		for (auto& define : Util::GetCommonShaderDefines(explicitNames))
			defines.push_back(std::move(define));
		for (const char* name : { a_stage, "WINPC", "DX11" })
			defines.emplace_back(name, "");
		// Defines are ASCII.
		std::vector<std::pair<std::wstring, std::wstring>> wide;
		for (const auto& [name, value] : defines)
			wide.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
		return wide;
	}

	D3D11_TEXTURE2D_DESC HalfDesc(const Shape& a_shape, DXGI_FORMAT a_format, UINT a_bindFlags)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_shape.halfTexWidth;
		desc.Height = a_shape.halfTexHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = a_bindFlags;
		return desc;
	}

	/** @brief The D3D11 path's additive blend (Effects11::EnsureAdditiveBlendState): colour added, the target's alpha kept. */
	rhi::BlendAttachment AdditiveBlend()
	{
		rhi::BlendAttachment blend{};
		blend.enable = true;
		blend.srcColor = rhi::BlendFactor::One;
		blend.dstColor = rhi::BlendFactor::One;
		blend.colorOp = rhi::BlendOp::Add;
		blend.srcAlpha = rhi::BlendFactor::Zero;
		blend.dstAlpha = rhi::BlendFactor::One;
		blend.alphaOp = rhi::BlendOp::Add;
		blend.writeMask = static_cast<rhi::ColorWriteEnable>(rhi::ColorWriteEnable::R | rhi::ColorWriteEnable::G | rhi::ColorWriteEnable::B);
		return blend;
	}
}

struct ORGSunRays::Impl
{
	std::shared_ptr<const RaysState> state;
	// Programs by source, defines and target formats, kept across shapes: a resize does not compile again.
	std::map<std::string, std::shared_ptr<const RasterProgram>> rasterPrograms;
	std::map<std::string, std::shared_ptr<const ComputeProgram>> computePrograms;
	// This frame's values, gathered by VolumetricRays and SunRays, handed to the run at the hook and latched by the commit.
	LatchData pending{};
	Sizes pendingSizes{};
	bool pendingVolumetric = false, pendingSun = false;
	std::optional<LatchData> committed;
	// The frame PrepareFrame last ran for, and its answer.
	std::uint32_t preparedFrame = UINT32_MAX;
	bool preparedOk = false;
	std::string fallbackReason;  // why the last frame ran on D3D11, logged when it changes

	std::shared_ptr<const RasterProgram> Raster(const char* a_source, std::vector<std::string> a_defines, std::vector<RasterProgram::Target> a_targets,
		std::uint32_t a_words)
	{
		std::string key = a_source;
		for (const auto& define : a_defines)
			key += ";" + define;
		for (const auto& target : a_targets)
			key += ";" + std::to_string(static_cast<std::uint32_t>(target.format)) + (target.blend.enable ? "+" : "");
		auto& program = rasterPrograms[key];
		if (!program) {
			RasterProgram::Desc desc{};
			desc.pixelSource = std::string(kShaderDir) + a_source;
			desc.constantWords = a_words;
			desc.defines = Defines(std::move(a_defines), "PSHADER");
			desc.targets = std::move(a_targets);
			program = RasterProgram::Load(RenderGraphRuntime::Get().Host()->Device(), desc);
		}
		return program;
	}

	std::shared_ptr<const ComputeProgram> Compute(const char* a_source, std::vector<std::string> a_defines, std::uint32_t a_words)
	{
		std::string key = a_source;
		for (const auto& define : a_defines)
			key += ";" + define;
		auto& program = computePrograms[key];
		if (!program)
			program = ComputeProgram::Load(RenderGraphRuntime::Get().Host()->Device(),
				{ .source = std::string(kShaderDir) + a_source, .constantWords = a_words, .defines = Defines(std::move(a_defines), "COMPUTESHADER") });
		return program;
	}

	/** @brief A new state for a_shape, registered as the client. False, logged, when anything could not be made. */
	bool Build(const Shape& a_shape)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto built = std::make_shared<RaysState>();
		built->shape = a_shape;

		// The D3D11 path's defines (Effects11::DrawVolumetricRays, EnsureSunRaysResources).
		std::vector<std::string> raymarchDefines;
		if (globals::features::cloudShadows.loaded)
			raymarchDefines.emplace_back("CLOUD_SHADOWS");
		if (globals::features::terrainShadows.loaded)
			raymarchDefines.emplace_back("TERRAIN_SHADOWS");
		std::vector<std::string> maskDefines{ "MASK" };
		if (globals::features::cloudShadows.loaded)
			maskDefines.emplace_back("CLOUD_SHADOWS");
		const RasterProgram::Target additive{ .format = a_shape.mainFormat, .blend = AdditiveBlend() };
		const RasterProgram::Target half{ .format = rhi::Format::R16_Float };
		const RasterProgram::Target halfDepth{ .format = rhi::Format::R32_Float };
		built->raymarch = Raster("RaymarchVolumetricRaysPS.hlsl", raymarchDefines, { half, halfDepth }, ShaderBindings::Words<RaymarchBlock>());
		built->apply = Raster("ApplyVolumetricRaysPS.hlsl", {}, { additive }, ShaderBindings::Words<ApplyBlock>());
		built->blurH = Compute("BlurVolumetricRaysCS.hlsl", { "HORIZONTAL" }, ShaderBindings::Words<BlurBlock>());
		built->blurV = Compute("BlurVolumetricRaysCS.hlsl", {}, ShaderBindings::Words<BlurBlock>());
		built->sunMask = Raster("SunRaysPS.hlsl", maskDefines, { half }, ShaderBindings::Words<SunRaysBlock>());
		built->sunBlur = Raster("SunRaysPS.hlsl", { "BLUR" }, { half }, ShaderBindings::Words<SunRaysBlock>());
		built->sunComposite = Raster("SunRaysPS.hlsl", { "COMPOSITE" }, { additive }, ShaderBindings::Words<SunRaysBlock>());
		if (!built->raymarch || !built->apply || !built->blurH || !built->blurV || !built->sunMask || !built->sunBlur || !built->sunComposite) {
			logger::error("[ORG] Effects11 rays: a program could not be built");
			return false;
		}

		// Written and read within the frame, as the D3D11 path's own textures: transient and aliasable.
		const GraphResources::Options transient{ .lifetime = GraphResources::Lifetime::Transient };
		constexpr UINT kTarget = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		constexpr UINT kStorage = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		built->vlScattering = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R16_FLOAT, kTarget | kStorage), "Effects11::VLTexA", transient);
		built->vlBlur = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R16_FLOAT, kStorage), "Effects11::VLTexB", transient);
		built->vlDepth = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R32_FLOAT, kTarget), "Effects11::VLDepthHalf", transient);
		built->sunA = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R16_FLOAT, kTarget), "Effects11::SunRaysTexA", transient);
		built->sunB = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R16_FLOAT, kTarget), "Effects11::SunRaysTexB", transient);
		built->latch = std::make_shared<org::LatchBlock>("cs.e11-rays.latch", static_cast<std::uint32_t>(sizeof(LatchData)), host->FrameSlots());
		// As Deferred::linearSampler: linear, clamped.
		built->linearSampler = ShaderBindings::CreateSampler({});
		if (!*built->vlScattering || !*built->vlBlur || !*built->vlDepth || !*built->sunA || !*built->sunB || !built->linearSampler) {
			logger::error("[ORG] Effects11 rays: the half-res textures or the sampler could not be made");
			return false;
		}

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		const std::shared_ptr<const RaysState> shared = built;
		const auto add = [&](std::string a_name, std::int32_t a_order, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
			client.passes.push_back({ .name = "cs.e11-rays." + std::move(a_name),
				.anchor = FrameAnchors::Anchor::PostComposite,
				.order = a_order,
				.kind = a_kind,
				.make = std::move(a_make) });
		};
		add("volumetric.raymarch", kVolumetricOrder, FrameAnchors::PassKind::Render, [shared] { return std::make_shared<RaymarchPass>(shared); });
		add("volumetric.blur-h", kVolumetricOrder + 1, FrameAnchors::PassKind::Compute, [shared] { return std::make_shared<BlurPass>(shared, true); });
		add("volumetric.blur-v", kVolumetricOrder + 2, FrameAnchors::PassKind::Compute, [shared] { return std::make_shared<BlurPass>(shared, false); });
		add("volumetric.apply", kVolumetricOrder + 3, FrameAnchors::PassKind::Render, [shared] { return std::make_shared<ApplyPass>(shared); });
		static constexpr std::array<const char*, kSunRaysPasses> kSunNames{ "sun.mask", "sun.blur-1", "sun.blur-2", "sun.blur-3", "sun.composite" };
		for (std::uint32_t pass = 0; pass < kSunRaysPasses; ++pass)
			add(kSunNames[pass], kSunRaysOrder + static_cast<std::int32_t>(pass), FrameAnchors::PassKind::Render,
				[shared, pass] { return std::make_shared<SunRaysPass>(shared, pass); });
		client.registerResources = [shared](org::RenderGraph& a_graph) {
			for (const auto* texture : { shared->vlScattering.get(), shared->vlBlur.get(), shared->vlDepth.get(), shared->sunA.get(), shared->sunB.get() })
				texture->Register(a_graph);
		};
		client.commit = [this, shared](const FrameAnchors::Point&) {
			if (committed)
				shared->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, *committed);
			committed.reset();
		};
		// At the hook, before the epoch: the passes record only for the effects the frame gave the graph.
		client.active = [this, shared] {
			shared->volumetricLive->store(pendingVolumetric, std::memory_order_release);
			shared->sunLive->store(pendingSun, std::memory_order_release);
			shared->area->store(Pack(pendingSizes), std::memory_order_release);
			const bool live = pendingVolumetric || pendingSun;
			if (live)
				committed = pending;
			pendingVolumetric = pendingSun = false;
			return live;
		};
		client.onFailed = [] {
			logger::error("[ORG] Effects11 rays return to D3D11: the render graph failed");
			auto& self = Get();
			self.failed = true;
			self.impl.reset();
		};
		client.sharedConstants = true;
		if (!FrameAnchors::Register(std::move(client)))
			return false;
		state = std::move(built);
		logger::info("[ORG] Effects11 rays run on the render graph (half-res {}x{}, kMAIN format {}, terrain shadows {}, cloud shadows {})", a_shape.halfTexWidth,
			a_shape.halfTexHeight, static_cast<std::uint32_t>(a_shape.mainFormat), a_shape.shadowHeights ? "on" : "off", a_shape.cloudShadows ? "on" : "off");
		return true;
	}
};

ORGSunRays::~ORGSunRays() = default;

void ORGSunRays::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::PostComposite))
		logger::warn("[ORG] Effects11 rays: the PostComposite anchor could not be reserved; the rays stay on D3D11");
}

bool ORGSunRays::PrepareFrame(const Sizes& a_sizes)
{
	if (!Requested() || failed)
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.IsActive() || !runtime.Host() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::PostComposite))
		return false;
	if (!impl)
		impl = std::make_unique<Impl>();
	const std::uint32_t frame = globals::state->frameCount;
	if (impl->preparedFrame == frame)
		return impl->preparedOk;
	impl->preparedFrame = frame;
	impl->preparedOk = false;

	const auto fallback = [&](const char* a_reason) {
		if (impl->fallbackReason != a_reason) {
			logger::info("[ORG] Effects11 rays: this frame runs on D3D11 ({})", a_reason);
			impl->fallbackReason = a_reason;
		}
		return false;
	};

	// This frame's inputs, as the D3D11 passes read them, each as a graph resource.
	Shape shape;
	shape.halfTexWidth = a_sizes.halfTexWidth;
	shape.halfTexHeight = a_sizes.halfTexHeight;
	const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!main.texture || !main.RTV)
		return fallback("kMAIN is missing");
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	main.RTV->GetDesc(&rtvDesc);
	shape.mainFormat = rhi::helpers::ToRHI(rtvDesc.Format);
	shape.main = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
	if (!shape.main || shape.mainFormat == rhi::Format::Unknown)
		return fallback("kMAIN cannot be imported");

	shape.depth = SceneDepth(BoundPixelView(17).get());
	if (!shape.depth)
		return fallback("the scene depth at t17 is missing or cannot be imported");

	auto& terrainShadows = globals::features::terrainShadows;
	if (terrainShadows.loaded) {
		auto* view = terrainShadows.GetShadowHeightSRV();
		const auto& graphTerrain = ORGTerrainShadows::Get();
		if (view && graphTerrain.Active() && view == graphTerrain.ShadowHeightSRV())
			shape.shadowHeights = { nullptr, "TerrainShadows::ShadowHeight" };
		else
			shape.shadowHeights = ImportView("TerrainShadows::ShadowHeight", view);
		// The raymarch reads the heights whenever terrain shadows are on.
		if (!shape.shadowHeights)
			return fallback("Terrain Shadows' heights are missing");
	}

	if (globals::features::cloudShadows.loaded) {
		// The view Cloud Shadows leaves bound at PS t25, which the D3D11 passes read: the graph's own cubemap when it is that one.
		const auto view = BoundPixelView(25);
		const auto& graphClouds = ORGCloudShadows::Get();
		if (view && graphClouds.Active() && view.get() == graphClouds.OcclusionSRV())
			shape.cloudShadows = { nullptr, "CloudShadows::Occlusion" };
		else
			shape.cloudShadows = ImportView("CloudShadows::Occlusion", view.get());
		if (!shape.cloudShadows)
			return fallback("Cloud Shadows' texture is missing");
	}

	if (!impl->state || !(impl->state->shape == shape)) {
		if (!impl->Build(shape)) {
			logger::error("[ORG] Effects11 rays return to D3D11 for good");
			FrameAnchors::Unregister(kClientId);
			failed = true;
			impl.reset();
			return false;
		}
	}

	impl->pendingSizes = a_sizes;
	impl->pending.vl = { static_cast<std::int32_t>(a_sizes.halfWidth), static_cast<std::int32_t>(a_sizes.halfHeight),
		static_cast<std::int32_t>(a_sizes.halfWidth) - 1, static_cast<std::int32_t>(a_sizes.halfHeight) - 1 };
	impl->fallbackReason.clear();
	impl->preparedOk = true;
	return true;
}

bool ORGSunRays::VolumetricRays(const Sizes& a_sizes)
{
	if (!PrepareFrame(a_sizes))
		return false;
	impl->pendingVolumetric = true;
	return true;
}

bool ORGSunRays::SunRays(const Sizes& a_sizes, const std::array<Effects11::SunRaysData, kSunRaysPasses>& a_passes)
{
	if (!PrepareFrame(a_sizes))
		return false;
	impl->pending.sun = a_passes;
	impl->pendingSun = true;
	return true;
}

void ORGSunRays::Release()
{
	if (!impl)
		return;
	FrameAnchors::Unregister(kClientId);
	impl.reset();
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the rays always run on D3D11.
struct ORGSunRays::Impl
{};

ORGSunRays::~ORGSunRays() = default;
void ORGSunRays::Reserve() {}
bool ORGSunRays::PrepareFrame(const Sizes&) { return false; }
bool ORGSunRays::VolumetricRays(const Sizes&) { return false; }
bool ORGSunRays::SunRays(const Sizes&, const std::array<Effects11::SunRaysData, kSunRaysPasses>&) { return false; }
void ORGSunRays::Release() {}

#endif

