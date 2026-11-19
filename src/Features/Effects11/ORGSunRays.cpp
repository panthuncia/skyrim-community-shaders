#include "ORGSunRays.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

bool ORGSunRays::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
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
#	include "Features/TerrainBlending/ORGTerrainBlending.h"
#	include "Features/TerrainShadows.h"
#	include "Features/TerrainShadows/ORGTerrainShadows.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "State.h"
#	include "Utils/D3D.h"
#	include "VanillaBinding.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

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
	// Volumetric rays first, then sun rays, as the D3D11 path draws them.
	constexpr std::int32_t kVolumetricOrder = 100;
	constexpr std::int32_t kSunRaysOrder = 200;

	// The commit opens the entries of the effects the frame gave the graph.
	constexpr std::uint32_t kVolumetricDraws = 0;  // the raymarch and the apply pass
	constexpr std::uint32_t kBlurH = 1;
	constexpr std::uint32_t kBlurV = 2;
	constexpr std::uint32_t kSunDraws = 3;  // the mask, the blurs and the composite
	constexpr std::uint32_t kGateEntries = 4;

	using ShaderBindings::Handle;
	using RenderGraph::AnchorPasses::Bind;

	// BlurVolumetricRaysCS's TG_DIM - WINDOW * 2.
	constexpr std::uint32_t kBlurGroupPixels = 256 - 12 * 2;

	/** @brief Mirrors VLData in Effects11/VolumetricRaysData.hlsli. */
	struct VLData
	{
		std::int32_t screenX, screenY, screenXMin1, screenYMin1;
	};
	static_assert(sizeof(VLData) == 16);

	// The half-res size, then each sun ray pass's constants.
	struct LatchData
	{
		VLData vl;
		std::array<Effects11::SunRaysData, ORGSunRays::kSunRaysPasses> sun;
	};
	static_assert(offsetof(LatchData, sun) % 16 == 0 && sizeof(Effects11::SunRaysData) % 16 == 0);

	constexpr std::uint32_t kVLOffset = static_cast<std::uint32_t>(offsetof(LatchData, vl));

	constexpr std::uint32_t SunOffset(std::uint32_t a_pass)
	{
		return static_cast<std::uint32_t>(offsetof(LatchData, sun) + a_pass * sizeof(Effects11::SunRaysData));
	}

	// Push blocks in each resource list's order.
	struct RaymarchBlock
	{
		SharedConstants::Handles shared;
		Handle sampler, depth, shadowHeights, cloudShadows;
	};
	static_assert(sizeof(RaymarchBlock) == 32);

	struct BlurBlock
	{
		Handle input, linearDepth, output;
		ShaderBindings::Latch vl;
	};
	static_assert(sizeof(BlurBlock) == 20);

	struct ApplyBlock
	{
		SharedConstants::Handles shared;
		Handle scattering, raymarchDepth;
		ShaderBindings::Latch vl;
		Handle depth;
	};
	static_assert(sizeof(ApplyBlock) == 36);

	struct SunRaysBlock
	{
		SharedConstants::Handles shared;
		Handle sampler, rays;
		ShaderBindings::Latch data;
		Handle depth, cloudShadows;
	};
	static_assert(sizeof(SunRaysBlock) == 40);

	struct Source
	{
		FrameAnchors::Texture resource;
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

	/** @brief A change makes a new client. The areas are part of it, since a kept recording fixes its viewports. */
	struct Shape
	{
		ORGSunRays::Sizes sizes;
		rhi::Format mainFormat = rhi::Format::Unknown;
		FrameAnchors::Texture main;
		Source depth, shadowHeights, cloudShadows;

		bool operator==(const Shape& a_other) const
		{
			const auto& a = sizes;
			const auto& b = a_other.sizes;
			return a.halfTexWidth == b.halfTexWidth && a.halfTexHeight == b.halfTexHeight && a.halfWidth == b.halfWidth && a.halfHeight == b.halfHeight &&
			       a.width == b.width && a.height == b.height && mainFormat == a_other.mainFormat && main == a_other.main && depth == a_other.depth &&
			       shadowHeights == a_other.shadowHeights && cloudShadows == a_other.cloudShadows;
		}
	};

	/** @brief Immutable once registered. */
	struct RaysState
	{
		Shape shape;
		std::shared_ptr<const RasterProgram> raymarch, apply, sunMask, sunBlur, sunComposite;
		std::shared_ptr<const ComputeProgram> blurH, blurV;
		// Volumetric scattering, the blur's other half and the raymarch depth, then the sun rays' ping-pong pair.
		std::unique_ptr<GraphResources::Texture2D> vlScattering, vlBlur, vlDepth, sunA, sunB;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t linearSampler = 0;
	};

	/** @brief Its revision never changes, so its recording is kept. */
	struct RaysFrame
	{
		std::shared_ptr<const RaysState> state;
		std::uint32_t index = 0;
	};

	/** @brief Does no work on runs whose commit did not open the entry. */
	void GatedDraw(const RaysState& a_state, std::uint32_t a_entry, const RasterProgram& a_program, RasterProgram::DrawDesc a_desc,
		org::PassRecordContext& a_recording)
	{
		a_desc.indirect = &a_state.gate->Latch();
		a_desc.indirectOffset = a_state.gate->Offset(a_entry);
		a_program.Draw(a_recording, a_desc);
	}

	// ---- Volumetric rays ----

	struct RaymarchBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken scattering, depthOut, depth, shadowHeights, cloudShadows;
		bool hasShadowHeights = false, hasCloudShadows = false;
	};

	/** @brief Its own pass, since it has two targets and reads other clients' textures by name, which AnchorPasses::Draw does not cover. */
	class RaymarchPass final : public org::TypedRenderGraphPass<RaymarchPass, RaysFrame, RaymarchBindings>
	{
	public:
		explicit RaymarchPass(std::shared_ptr<const RaysState> a_state) :
			state(std::move(a_state)) {}

		RaymarchBindings Declare(org::PassBuilder& a_builder)
		{
			const auto& s = *state;
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

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get())); }

		RaysFrame Prepare(const RaymarchBindings&, const org::PassPrepareContext&) const { return { state }; }

		static void Record(const RaymarchBindings& a_bindings, const RaysFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& s = *a_frame.state;
			RaymarchBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.sampler = s.linearSampler;
			block.depth = a_recording.Index(a_bindings.depth);
			block.shadowHeights = a_bindings.hasShadowHeights ? a_recording.Index(a_bindings.shadowHeights) : 0;
			block.cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : 0;
			// The raymarch writes every texel of the area, so nothing to keep.
			const std::array<RasterProgram::Colour, 2> colours{ { { .view = &a_bindings.scattering, .load = rhi::LoadOp::DontCare },
				{ .view = &a_bindings.depthOut, .load = rhi::LoadOp::DontCare } } };
			const auto& sizes = s.shape.sizes;
			GatedDraw(s, kVolumetricDraws, *s.raymarch,
				{ .colours = colours,
					.viewport = { .width = sizes.halfWidth, .height = sizes.halfHeight },
					.constants = ShaderBindings::Data(block),
					.debugName = "Effects11::VolumetricRays Raymarch" },
				a_recording);
		}

	private:
		std::shared_ptr<const RaysState> state;
	};

	struct ApplyBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken main, scattering, raymarchDepth, depth;
	};

	/** @brief Upsamples and lights the blurred scattering, added to kMAIN. */
	class ApplyPass final : public org::TypedRenderGraphPass<ApplyPass, RaysFrame, ApplyBindings>
	{
	public:
		explicit ApplyPass(std::shared_ptr<const RaysState> a_state) :
			state(std::move(a_state)) {}

		ApplyBindings Declare(org::PassBuilder& a_builder)
		{
			const auto& s = *state;
			ApplyBindings b{};
			b.shared = SharedConstants::Declare(a_builder);
			b.main = a_builder.RenderTarget(s.shape.main).View();
			b.scattering = a_builder.ShaderResource(s.vlScattering->Graph()).View();
			b.raymarchDepth = a_builder.ShaderResource(s.vlDepth->Graph()).View();
			b.depth = Read(a_builder, s.shape.depth);
			return b;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get())); }

		RaysFrame Prepare(const ApplyBindings&, const org::PassPrepareContext&) const { return { state }; }

		static void Record(const ApplyBindings& a_bindings, const RaysFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& s = *a_frame.state;
			ApplyBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.scattering = a_recording.Index(a_bindings.scattering);
			block.raymarchDepth = a_recording.Index(a_bindings.raymarchDepth);
			block.vl = ShaderBindings::LatchOf(*s.latch, a_recording.Run(), kVLOffset);
			block.depth = a_recording.Index(a_bindings.depth);
			const RasterProgram::Colour colour{ .view = &a_bindings.main, .load = rhi::LoadOp::Load };
			const auto& sizes = s.shape.sizes;
			GatedDraw(s, kVolumetricDraws, *s.apply,
				{ .colours = { &colour, 1 },
					.viewport = { .width = sizes.width, .height = sizes.height },
					.constants = ShaderBindings::Data(block),
					.debugName = "Effects11::VolumetricRays Apply" },
				a_recording);
		}

	private:
		std::shared_ptr<const RaysState> state;
	};

	// ---- Sun rays ----

	struct SunRaysBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken target, rays, depth, cloudShadows;
		bool hasRays = false, hasCloudShadows = false;
	};

	/** @brief The mask into A, the blurs A to B to A to B, or the composite adding B to kMAIN. Each reads its own SunRaysData. */
	class SunRaysPass final : public org::TypedRenderGraphPass<SunRaysPass, RaysFrame, SunRaysBindings>
	{
	public:
		SunRaysPass(std::shared_ptr<const RaysState> a_state, std::uint32_t a_pass) :
			state(std::move(a_state)), pass(a_pass) {}

		SunRaysBindings Declare(org::PassBuilder& a_builder)
		{
			const auto& s = *state;
			const bool composite = pass == ORGSunRays::kSunRaysPasses - 1;
			// The mask and the second blur write A, the other blurs write B.
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

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get()));
			a_out.push_back(pass);
		}

		RaysFrame Prepare(const SunRaysBindings&, const org::PassPrepareContext&) const { return { state, pass }; }

		static void Record(const SunRaysBindings& a_bindings, const RaysFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& s = *a_frame.state;
			const std::uint32_t pass = a_frame.index;
			const bool composite = pass == ORGSunRays::kSunRaysPasses - 1;
			SunRaysBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.sampler = s.linearSampler;
			block.rays = a_bindings.hasRays ? a_recording.Index(a_bindings.rays) : 0;
			block.data = ShaderBindings::LatchOf(*s.latch, a_recording.Run(), SunOffset(pass));
			block.depth = a_recording.Index(a_bindings.depth);
			block.cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : 0;
			// The half-res passes write every texel. The composite adds to kMAIN.
			const RasterProgram::Colour colour{ .view = &a_bindings.target, .load = composite ? rhi::LoadOp::Load : rhi::LoadOp::DontCare };
			const auto& program = pass == 0 ? s.sunMask : (composite ? s.sunComposite : s.sunBlur);
			static constexpr std::array<const char*, ORGSunRays::kSunRaysPasses> kNames{ "Effects11::SunRays Mask", "Effects11::SunRays Blur 1",
				"Effects11::SunRays Blur 2", "Effects11::SunRays Blur 3", "Effects11::SunRays Composite" };
			const auto& sizes = s.shape.sizes;
			RasterProgram::Viewport viewport{};
			viewport.width = composite ? sizes.width : sizes.halfWidth;
			viewport.height = composite ? sizes.height : sizes.halfHeight;
			GatedDraw(s, kSunDraws, *program, { .colours = { &colour, 1 }, .viewport = viewport, .constants = ShaderBindings::Data(block), .debugName = kNames[pass] },
				a_recording);
		}

	private:
		std::shared_ptr<const RaysState> state;
		std::uint32_t pass = 0;
	};

	// ---- Inputs and programs ----

	/** @brief The engine's depth import when it is one of the engine's, else Terrain Blending's published blended depth. */
	Source SceneDepth(ID3D11ShaderResourceView* a_view)
	{
		if (!a_view || !globals::game::renderer)
			return {};
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
			// The view is DXVK's from the raw context, while a field may be a tripwire handle wrapping it.
			if (depthStencils[i].texture && RenderGraph::Engine::Tripwire::Unwrap(depthStencils[i].texture) == resource.get())
				return { FrameAnchors::EngineDepth(i), {} };
		// By name on Terrain Blending's ORG path, since importing its export would make a second graph resource of the same image.
		const auto& graphBlending = ORGTerrainBlending::Get();
		if (a_view == graphBlending.BlendedDepthSRV(false))
			return { nullptr, std::string(ORGTerrainBlending::kBlendedDepthId) };
		if (a_view == graphBlending.BlendedDepthSRV(true))
			return { nullptr, std::string(ORGTerrainBlending::kBlendedDepth16Id) };
		return {};
	}

	/** @brief The programs add the D3D11 build's common defines themselves. */
	std::vector<std::pair<std::wstring, std::wstring>> Defines(const std::vector<std::string>& a_names)
	{
		std::vector<std::pair<std::string, std::string>> defines;
		for (const auto& name : a_names)
			defines.emplace_back(name, "");
		return RenderGraph::AnchorPasses::Defines(defines);
	}

	D3D11_TEXTURE2D_DESC HalfDesc(const Shape& a_shape, DXGI_FORMAT a_format, UINT a_bindFlags)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_shape.sizes.halfTexWidth;
		desc.Height = a_shape.sizes.halfTexHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = a_bindFlags;
		return desc;
	}

	/** @brief As Effects11::EnsureAdditiveBlendState: colour added, the target's alpha kept. */
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

	/** @brief As the D3D11 dispatches give them. */
	std::array<std::uint32_t, 3> BlurGroups(const ORGSunRays::Sizes& a_sizes, bool a_horizontal)
	{
		if (a_horizontal)
			return { (a_sizes.halfWidth + kBlurGroupPixels - 1) / kBlurGroupPixels, a_sizes.halfHeight, 1 };
		return { a_sizes.halfWidth, (a_sizes.halfHeight + kBlurGroupPixels - 1) / kBlurGroupPixels, 1 };
	}
}

struct ORGSunRays::Impl
{
	// Made at Setup and shared by every build.
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> gate;
	org::OwnedDescriptorBinding linearSampler;

	// Null until the first frame the graph takes.
	std::shared_ptr<const RaysState> state;
	// Kept across shapes, so a resize does not recompile.
	std::map<std::string, std::shared_ptr<const RasterProgram>> rasterPrograms;
	std::map<std::string, std::shared_ptr<const ComputeProgram>> computePrograms;
	// After this, a newly built program is logged as a format setup did not know.
	bool prebuilt = false;
	// Gathered by VolumetricRays and SunRays, handed to the run at the hook, latched by the commit.
	LatchData pending{};
	bool pendingVolumetric = false, pendingSun = false;
	std::optional<LatchData> committed;
	bool committedVolumetric = false, committedSun = false;
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
			if (prebuilt)
				logger::info("[ORG] Effects11 rays: {} for this target was not built at setup; building it now", a_source);
			RasterProgram::Desc desc{};
			desc.pixelSource = std::string(kShaderDir) + a_source;
			desc.constantWords = a_words;
			desc.defines = Defines(a_defines);
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
		if (!program && prebuilt)
			logger::info("[ORG] Effects11 rays: {} was not built at setup; building it now", a_source);
		if (!program)
			program = ComputeProgram::Load(RenderGraphRuntime::Get().Host()->Device(),
				{ .source = std::string(kShaderDir) + a_source, .constantWords = a_words, .defines = Defines(a_defines) });
		return program;
	}

	/** @brief False, logged, when one fails. */
	bool LoadPrograms(RaysState& a_state, rhi::Format a_mainFormat)
	{
		// As Effects11::DrawVolumetricRays and EnsureSunRaysResources define them.
		std::vector<std::string> raymarchDefines;
		if (globals::features::cloudShadows.loaded)
			raymarchDefines.emplace_back("CLOUD_SHADOWS");
		if (globals::features::terrainShadows.loaded)
			raymarchDefines.emplace_back("TERRAIN_SHADOWS");
		std::vector<std::string> maskDefines{ "MASK" };
		if (globals::features::cloudShadows.loaded)
			maskDefines.emplace_back("CLOUD_SHADOWS");
		const RasterProgram::Target additive{ .format = a_mainFormat, .blend = AdditiveBlend() };
		const RasterProgram::Target half{ .format = rhi::Format::R16_Float };
		const RasterProgram::Target halfDepth{ .format = rhi::Format::R32_Float };
		a_state.raymarch = Raster("RaymarchVolumetricRaysPS.hlsl", raymarchDefines, { half, halfDepth }, ShaderBindings::Words<RaymarchBlock>());
		a_state.apply = Raster("ApplyVolumetricRaysPS.hlsl", {}, { additive }, ShaderBindings::Words<ApplyBlock>());
		a_state.blurH = Compute("BlurVolumetricRaysCS.hlsl", { "HORIZONTAL" }, ShaderBindings::Words<BlurBlock>());
		a_state.blurV = Compute("BlurVolumetricRaysCS.hlsl", {}, ShaderBindings::Words<BlurBlock>());
		a_state.sunMask = Raster("SunRaysPS.hlsl", maskDefines, { half }, ShaderBindings::Words<SunRaysBlock>());
		a_state.sunBlur = Raster("SunRaysPS.hlsl", { "BLUR" }, { half }, ShaderBindings::Words<SunRaysBlock>());
		a_state.sunComposite = Raster("SunRaysPS.hlsl", { "COMPOSITE" }, { additive }, ShaderBindings::Words<SunRaysBlock>());
		if (!a_state.raymarch || !a_state.apply || !a_state.blurH || !a_state.blurV || !a_state.sunMask || !a_state.sunBlur || !a_state.sunComposite) {
			logger::error("[ORG] Effects11 rays: a program could not be built");
			return false;
		}
		return true;
	}

	/** @brief False, logged, on failure. */
	bool Build(const Shape& a_shape)
	{
		auto built = std::make_shared<RaysState>();
		built->shape = a_shape;
		built->latch = latch;
		built->gate = gate;
		built->linearSampler = linearSampler.Index();
		if (!LoadPrograms(*built, a_shape.mainFormat))
			return false;

		// Written and read within the frame, so transient.
		const GraphResources::Options transient{ .lifetime = GraphResources::Lifetime::Transient };
		constexpr UINT kTarget = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		constexpr UINT kStorage = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		built->vlScattering = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R16_FLOAT, kTarget | kStorage), "Effects11::VLTexA", transient);
		built->vlBlur = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R16_FLOAT, kStorage), "Effects11::VLTexB", transient);
		built->vlDepth = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R32_FLOAT, kTarget), "Effects11::VLDepthHalf", transient);
		built->sunA = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R16_FLOAT, kTarget), "Effects11::SunRaysTexA", transient);
		built->sunB = std::make_unique<GraphResources::Texture2D>(HalfDesc(a_shape, DXGI_FORMAT_R16_FLOAT, kTarget), "Effects11::SunRaysTexB", transient);
		if (!*built->vlScattering || !*built->vlBlur || !*built->vlDepth || !*built->sunA || !*built->sunB) {
			logger::error("[ORG] Effects11 rays: the half-res textures could not be made");
			return false;
		}

		// The raster passes stay their own passes, since they read other clients' textures by name and the raymarch has two targets.
		const std::shared_ptr<const RaysState> shared = built;
		namespace AP = RenderGraph::AnchorPasses;
		AP::Builder volumetric(FrameAnchors::Anchor::PostComposite, std::string(kClientId) + ".volumetric", kVolumetricOrder, latch, gate);
		volumetric.Add("raymarch", FrameAnchors::PassKind::Render, [shared] { return std::make_shared<RaymarchPass>(shared); });
		volumetric.Add(AP::Dispatch{ .name = "blur-h",
			.program = built->blurH,
			.bindings = { Bind::Srv(built->vlScattering->Graph()), Bind::Srv(built->vlDepth->Graph()), Bind::Uav(built->vlBlur->Graph()), Bind::Latch(kVLOffset) },
			.gate = kBlurH });
		volumetric.Add(AP::Dispatch{ .name = "blur-v",
			.program = built->blurV,
			.bindings = { Bind::Srv(built->vlBlur->Graph()), Bind::Srv(built->vlDepth->Graph()), Bind::Uav(built->vlScattering->Graph()), Bind::Latch(kVLOffset) },
			.gate = kBlurV });
		volumetric.Add("apply", FrameAnchors::PassKind::Render, [shared] { return std::make_shared<ApplyPass>(shared); });

		AP::Builder sun(FrameAnchors::Anchor::PostComposite, std::string(kClientId) + ".sun", kSunRaysOrder, latch, gate);
		static constexpr std::array<const char*, kSunRaysPasses> kSunSteps{ "mask", "blur-1", "blur-2", "blur-3", "composite" };
		for (std::uint32_t pass = 0; pass < kSunRaysPasses; ++pass)
			sun.Add(kSunSteps[pass], FrameAnchors::PassKind::Render, [shared, pass] { return std::make_shared<SunRaysPass>(shared, pass); });

		for (const auto* builder : { &volumetric, &sun }) {
			if (builder->Failed()) {
				logger::error("[ORG] Effects11 rays: {}", builder->Failure());
				return false;
			}
		}

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.passes = volumetric.Take();
		for (auto& pass : sun.Take())
			client.passes.push_back(std::move(pass));
		client.registerResources = [shared](org::RenderGraph& a_graph) {
			for (const auto* texture : { shared->vlScattering.get(), shared->vlBlur.get(), shared->vlDepth.get(), shared->sunA.get(), shared->sunB.get() })
				texture->Register(a_graph);
		};
		client.commit = [this, shared](const FrameAnchors::Point&) {
			if (committed) {
				latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, *committed);
				if (committedVolumetric) {
					const auto h = BlurGroups(shared->shape.sizes, true);
					const auto v = BlurGroups(shared->shape.sizes, false);
					gate->OpenDraw(kVolumetricDraws);
					gate->OpenDispatch(kBlurH, h[0], h[1], h[2]);
					gate->OpenDispatch(kBlurV, v[0], v[1], v[2]);
				}
				if (committedSun)
					gate->OpenDraw(kSunDraws);
			}
			committed.reset();
		};
		// The run has work only for the effects the frame gave the graph.
		client.active = [this] {
			committedVolumetric = pendingVolumetric;
			committedSun = pendingSun;
			const bool live = pendingVolumetric || pendingSun;
			if (live)
				committed = pending;
			pendingVolumetric = pendingSun = false;
			return live;
		};
		client.sharedConstants = true;
		client.gates = { gate };
		if (!FrameAnchors::Register(std::move(client)))
			return false;
		state = std::move(built);
		const auto& sizes = a_shape.sizes;
		logger::info("[ORG] Effects11 rays run on the render graph (half-res {}x{}, kMAIN format {}, terrain shadows {}, cloud shadows {})", sizes.halfTexWidth,
			sizes.halfTexHeight, static_cast<std::uint32_t>(a_shape.mainFormat), a_shape.shadowHeights ? "on" : "off", a_shape.cloudShadows ? "on" : "off");
		return true;
	}
};

ORGSunRays::~ORGSunRays() = default;

void ORGSunRays::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::PostComposite))
		logger::warn("[ORG] Effects11 rays: the PostComposite anchor could not be reserved; the rays stay on D3D11");
}

bool ORGSunRays::Setup()
{
	if (!Requested() || failed)
		return false;
	if (impl)
		return true;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.IsActive() || !runtime.Host() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::PostComposite)) {
		logger::warn("[ORG] Effects11 rays: CS_ORG_E11_RAYS=1 but the render graph or its PostComposite anchor is not available; the rays stay on D3D11");
		return false;
	}
	auto state = std::make_unique<Impl>();
	state->latch = std::make_shared<org::LatchBlock>("cs.e11-rays.latch", static_cast<std::uint32_t>(sizeof(LatchData)));
	state->gate = RenderGraph::RunGate::Create("cs.e11-rays.gate", kGateEntries);
	// As Deferred::linearSampler.
	state->linearSampler = ShaderBindings::CreateSampler({});
	if (!state->gate || !state->linearSampler) {
		logger::error("[ORG] Effects11 rays: the run gate or the sampler could not be made; the rays stay on D3D11");
		failed = true;
		return false;
	}
	// Build kMAIN's programs now so the first frame compiles nothing.
	const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (main.RTV) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		main.RTV->GetDesc(&rtvDesc);
		RaysState programs;
		if (const auto format = rhi::helpers::ToRHI(rtvDesc.Format); format == rhi::Format::Unknown || !state->LoadPrograms(programs, format)) {
			logger::error("[ORG] Effects11 rays: the programs for kMAIN could not be built; the rays stay on D3D11");
			failed = true;
			return false;
		}
		state->prebuilt = true;
	} else
		logger::warn("[ORG] Effects11 rays: kMAIN does not exist at setup; the programs are built at the first frame");
	impl = std::move(state);
	return true;
}

bool ORGSunRays::IsActive() const
{
	return impl && !failed && RenderGraphRuntime::Get().IsActive();
}

bool ORGSunRays::PrepareFrame(const Sizes& a_sizes)
{
	if (!IsActive())
		return false;
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

	Shape shape;
	shape.sizes = a_sizes;
	const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!main.texture || !main.RTV)
		return fallback("kMAIN is missing");
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	main.RTV->GetDesc(&rtvDesc);
	shape.mainFormat = rhi::helpers::ToRHI(rtvDesc.Format);
	shape.main = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
	if (!shape.main || shape.mainFormat == rhi::Format::Unknown)
		return fallback("kMAIN cannot be imported");

	// What CS bound at t17, resolved here rather than read back from the context.
	shape.depth = SceneDepth(VanillaBinding::LastSceneDepth());
	if (!shape.depth)
		return fallback("the scene depth at t17 is missing or cannot be imported");

	auto& terrainShadows = globals::features::terrainShadows;
	if (terrainShadows.loaded) {
		auto* view = terrainShadows.GetShadowHeightSRV();
		const auto& graphTerrain = ORGTerrainShadows::Get();
		if (view && graphTerrain.IsActive() && view == graphTerrain.ShadowHeightSRV())
			shape.shadowHeights = { nullptr, std::string(ORGTerrainShadows::kShadowHeightId) };
		// The raymarch reads the heights whenever terrain shadows are on.
		if (!shape.shadowHeights)
			return fallback("Terrain Shadows' heights are missing");
	}

	if (globals::features::cloudShadows.loaded) {
		// The cubemap Cloud Shadows binds at t25 for the frame. Use the graph's own when it is that one.
		auto* view = globals::features::cloudShadows.OcclusionSRV();
		const auto& graphClouds = ORGCloudShadows::Get();
		if (view && graphClouds.IsActive() && view == graphClouds.OcclusionSRV())
			shape.cloudShadows = { nullptr, std::string(ORGCloudShadows::kOcclusionId) };
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
	// The latch, gate and sampler stay. The next frame builds again.
	FrameAnchors::Unregister(kClientId);
	impl->state.reset();
	impl->rasterPrograms.clear();
	impl->computePrograms.clear();
	impl->preparedFrame = UINT32_MAX;
	impl->pendingVolumetric = impl->pendingSun = false;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph the rays always run on D3D11.
struct ORGSunRays::Impl
{};

ORGSunRays::~ORGSunRays() = default;
void ORGSunRays::Reserve() {}
bool ORGSunRays::Setup() { return false; }
bool ORGSunRays::IsActive() const { return false; }
bool ORGSunRays::PrepareFrame(const Sizes&) { return false; }
bool ORGSunRays::VolumetricRays(const Sizes&) { return false; }
bool ORGSunRays::SunRays(const Sizes&, const std::array<Effects11::SunRaysData, kSunRaysPasses>&) { return false; }
void ORGSunRays::Release() {}

#endif
