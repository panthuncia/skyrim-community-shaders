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
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "State.h"
#	include "Utils/D3D.h"
#	include "Utils/Game.h"
#	include "VanillaBinding.h"
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#		include "RenderGraph/ProgramBuild.h"
#	endif

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <cstddef>
#	include <functional>
#	include <optional>
#	include <string>
#	include <utility>
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
	namespace RF = RenderGraph::Frame;
	namespace AP = RenderGraph::AnchorPasses;

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

	/** @brief The seven programs for one kMAIN format. Built at setup for the first key, then on the programs lane. */
	struct RaysPrograms
	{
		std::shared_ptr<const RasterProgram> raymarch, apply, sunMask, sunBlur, sunComposite;
		std::shared_ptr<const ComputeProgram> blurH, blurV;
		rhi::Format mainFormat = rhi::Format::Unknown;
	};

	/** @brief What the programs are built from: kMAIN's format, and the shader cache clears so far. */
	struct ProgramKey
	{
		std::uint32_t format = 0;
		std::uint32_t clears = 0;

		bool operator==(const ProgramKey&) const = default;
	};

	/** @brief Made at setup and kept: the latch, the gate, the sampler and the five declared textures. */
	struct Fixed
	{
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		org::OwnedDescriptorBinding linearSampler;
		// Volumetric scattering, the blur's other half and the raymarch depth, then the sun rays' ping-pong pair.
		RF::DeclaredTexture2D vlScattering, vlBlur, vlDepth, sunA, sunB;
	};

	/**
	* @brief What the client is made for. The areas are part of it, since a kept recording fixes its viewports. The sources
	* are observed by Effects11's Draw* calls, not variables yet.
	*/
	struct Shape
	{
		ORGSunRays::Sizes sizes;
		FrameAnchors::Texture main;
		Source depth, shadowHeights, cloudShadows;
		std::shared_ptr<const RaysPrograms> programs;
		bool volumetric = false;  // the volumetric rays' passes are in the client
		bool sun = false;         // the sun rays' passes are

		bool operator==(const Shape&) const = default;
	};

	/** @brief Immutable once registered. */
	struct RaysState
	{
		Shape shape;
		std::shared_ptr<const Fixed> fixed;
		std::uint64_t generation = 0;  // its registration's, which HeldNow names
	};

	/** @brief A declared texture as passes declare it: its slot, so a new version needs no build. */
	FrameAnchors::Texture Slot(const RF::DeclaredTexture2D& a_texture)
	{
		return AP::TextureOf(*a_texture);
	}

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
		a_desc.indirect = &a_state.fixed->gate->Latch();
		a_desc.indirectOffset = a_state.fixed->gate->Offset(a_entry);
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
			b.scattering = a_builder.RenderTarget(Slot(s.fixed->vlScattering)).View();
			b.depthOut = a_builder.RenderTarget(Slot(s.fixed->vlDepth)).View();
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
			block.sampler = s.fixed->linearSampler.Index();
			block.depth = a_recording.Index(a_bindings.depth);
			block.shadowHeights = a_bindings.hasShadowHeights ? a_recording.Index(a_bindings.shadowHeights) : 0;
			block.cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : 0;
			// The raymarch writes every texel of the area, so nothing to keep.
			const std::array<RasterProgram::Colour, 2> colours{ { { .view = &a_bindings.scattering, .load = rhi::LoadOp::DontCare },
				{ .view = &a_bindings.depthOut, .load = rhi::LoadOp::DontCare } } };
			const auto& sizes = s.shape.sizes;
			GatedDraw(s, kVolumetricDraws, *s.shape.programs->raymarch,
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
			b.scattering = a_builder.ShaderResource(Slot(s.fixed->vlScattering)).View();
			b.raymarchDepth = a_builder.ShaderResource(Slot(s.fixed->vlDepth)).View();
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
			block.vl = ShaderBindings::LatchOf(*s.fixed->latch, a_recording.Run(), kVLOffset);
			block.depth = a_recording.Index(a_bindings.depth);
			const RasterProgram::Colour colour{ .view = &a_bindings.main, .load = rhi::LoadOp::Load };
			const auto& sizes = s.shape.sizes;
			GatedDraw(s, kVolumetricDraws, *s.shape.programs->apply,
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
			const auto& written = pass % 2 == 0 ? s.fixed->sunA : s.fixed->sunB;
			const auto& read = pass % 2 == 0 ? s.fixed->sunB : s.fixed->sunA;
			SunRaysBindings b{};
			b.shared = SharedConstants::Declare(a_builder);
			b.target = composite ? a_builder.RenderTarget(s.shape.main).View() : a_builder.RenderTarget(Slot(written)).View();
			if (pass != 0) {
				b.rays = a_builder.ShaderResource(Slot(read)).View();
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
			const auto& programs = *s.shape.programs;
			const std::uint32_t pass = a_frame.index;
			const bool composite = pass == ORGSunRays::kSunRaysPasses - 1;
			SunRaysBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.sampler = s.fixed->linearSampler.Index();
			block.rays = a_bindings.hasRays ? a_recording.Index(a_bindings.rays) : 0;
			block.data = ShaderBindings::LatchOf(*s.fixed->latch, a_recording.Run(), SunOffset(pass));
			block.depth = a_recording.Index(a_bindings.depth);
			block.cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : 0;
			// The half-res passes write every texel. The composite adds to kMAIN.
			const RasterProgram::Colour colour{ .view = &a_bindings.target, .load = composite ? rhi::LoadOp::Load : rhi::LoadOp::DontCare };
			const auto& program = pass == 0 ? programs.sunMask : (composite ? programs.sunComposite : programs.sunBlur);
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

	using DefineList = std::vector<std::pair<std::wstring, std::wstring>>;

	/** @brief The programs add the D3D11 build's common defines themselves. */
	DefineList Defines(const std::vector<std::string>& a_names)
	{
		std::vector<std::pair<std::string, std::string>> defines;
		for (const auto& name : a_names)
			defines.emplace_back(name, "");
		return RenderGraph::AnchorPasses::Defines(defines);
	}

	/**
	* @brief The programs' descriptions, their defines resolved at setup: CS's common defines read State, which the programs lane
	* must not. The two additive programs get kMAIN's format at each build.
	*/
	struct ProgramDescs
	{
		RasterProgram::Desc raymarch, apply, sunMask, sunBlur, sunComposite;
		ComputeProgram::Desc blurH, blurV;
	};

	RasterProgram::Desc RasterDesc(const char* a_source, const std::vector<std::string>& a_defines, std::vector<RasterProgram::Target> a_targets, std::uint32_t a_words)
	{
		RasterProgram::Desc desc{};
		desc.pixelSource = std::string(kShaderDir) + a_source;
		desc.constantWords = a_words;
		desc.defines = Defines(a_defines);
		desc.targets = std::move(a_targets);
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
		desc.vertexStageDefines = RenderGraph::ProgramBuild::WithCommonDefines(desc.defines, L"vs_6_6");
		desc.pixelStageDefines = RenderGraph::ProgramBuild::WithCommonDefines(desc.defines, L"ps_6_6");
		desc.commonDefines = false;
#	endif
		return desc;
	}

	ComputeProgram::Desc ComputeDesc(const char* a_source, const std::vector<std::string>& a_defines, std::uint32_t a_words)
	{
		ComputeProgram::Desc desc{ .source = std::string(kShaderDir) + a_source, .constantWords = a_words, .defines = Defines(a_defines) };
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
		desc.defines = RenderGraph::ProgramBuild::WithCommonDefines(desc.defines, L"cs_6_6");
		desc.commonDefines = false;
#	endif
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

	/** @brief Render thread, at setup. As Effects11::DrawVolumetricRays and EnsureSunRaysResources define them. */
	ProgramDescs ResolveDescs()
	{
		std::vector<std::string> raymarchDefines;
		if (globals::features::cloudShadows.loaded)
			raymarchDefines.emplace_back("CLOUD_SHADOWS");
		if (globals::features::terrainShadows.loaded)
			raymarchDefines.emplace_back("TERRAIN_SHADOWS");
		std::vector<std::string> maskDefines{ "MASK" };
		if (globals::features::cloudShadows.loaded)
			maskDefines.emplace_back("CLOUD_SHADOWS");
		const RasterProgram::Target half{ .format = rhi::Format::R16_Float };
		const RasterProgram::Target halfDepth{ .format = rhi::Format::R32_Float };
		// The additive targets' format is set at each build.
		const RasterProgram::Target additive{ .blend = AdditiveBlend() };
		ProgramDescs descs;
		descs.raymarch = RasterDesc("RaymarchVolumetricRaysPS.hlsl", raymarchDefines, { half, halfDepth }, ShaderBindings::Words<RaymarchBlock>());
		descs.apply = RasterDesc("ApplyVolumetricRaysPS.hlsl", {}, { additive }, ShaderBindings::Words<ApplyBlock>());
		descs.blurH = ComputeDesc("BlurVolumetricRaysCS.hlsl", { "HORIZONTAL" }, ShaderBindings::Words<BlurBlock>());
		descs.blurV = ComputeDesc("BlurVolumetricRaysCS.hlsl", {}, ShaderBindings::Words<BlurBlock>());
		descs.sunMask = RasterDesc("SunRaysPS.hlsl", maskDefines, { half }, ShaderBindings::Words<SunRaysBlock>());
		descs.sunBlur = RasterDesc("SunRaysPS.hlsl", { "BLUR" }, { half }, ShaderBindings::Words<SunRaysBlock>());
		descs.sunComposite = RasterDesc("SunRaysPS.hlsl", { "COMPOSITE" }, { additive }, ShaderBindings::Words<SunRaysBlock>());
		return descs;
	}

	/** @brief Any thread: setup or the programs lane. Null, logged, when one fails. */
	std::shared_ptr<const RaysPrograms> BuildPrograms(rhi::Device a_device, const ProgramDescs& a_descs, const ProgramKey& a_key)
	{
		auto programs = std::make_shared<RaysPrograms>();
		programs->mainFormat = static_cast<rhi::Format>(a_key.format);
		auto apply = a_descs.apply;
		auto composite = a_descs.sunComposite;
		apply.targets[0].format = programs->mainFormat;
		composite.targets[0].format = programs->mainFormat;
		programs->raymarch = RasterProgram::Load(a_device, a_descs.raymarch);
		programs->apply = RasterProgram::Load(a_device, apply);
		programs->blurH = ComputeProgram::Load(a_device, a_descs.blurH);
		programs->blurV = ComputeProgram::Load(a_device, a_descs.blurV);
		programs->sunMask = RasterProgram::Load(a_device, a_descs.sunMask);
		programs->sunBlur = RasterProgram::Load(a_device, a_descs.sunBlur);
		programs->sunComposite = RasterProgram::Load(a_device, composite);
		if (!programs->raymarch || !programs->apply || !programs->blurH || !programs->blurV || !programs->sunMask || !programs->sunBlur || !programs->sunComposite) {
			logger::error("[ORG] Effects11 rays: a program could not be built for kMAIN format {}", a_key.format);
			return nullptr;
		}
		return programs;
	}

	/** @brief The display size of a revision's values. Before the first display size, the engine's screen. */
	RF::Vars::Extent2D DisplayOf(const RF::Inputs& a_in)
	{
		auto display = a_in.Get(RF::Vars::DisplaySize());
		if (!display.width || !display.height) {
			const auto* graphics = globals::game::graphicsState;
			display = { static_cast<std::uint32_t>(graphics->screenWidth), static_cast<std::uint32_t>(graphics->screenHeight) };
		}
		return display;
	}

	/**
	* @brief The areas a revision's values give, as Effects11's Draw* calls compute them from kMAIN. The dynamic area reads the
	* engine's ratio and lock at the revision's start, as Util::ConvertToDynamic does on D3D11 (Upscaling sets the lock).
	*/
	ORGSunRays::Sizes SizesFor(const RF::Inputs& a_in)
	{
		const auto display = DisplayOf(a_in);
		const float2 dynamic = Util::ConvertToDynamic(float2{ static_cast<float>(display.width), static_cast<float>(display.height) });
		ORGSunRays::Sizes sizes;
		sizes.halfTexWidth = (display.width + 1) / 2;
		sizes.halfTexHeight = (display.height + 1) / 2;
		sizes.width = static_cast<std::uint32_t>(dynamic.x);
		sizes.height = static_cast<std::uint32_t>(dynamic.y);
		sizes.halfWidth = (sizes.width + 1) / 2;
		sizes.halfHeight = (sizes.height + 1) / 2;
		return sizes;
	}

	/** @brief A half-size texture's description: half the display while its effect is on, one texel while it is off. */
	D3D11_TEXTURE2D_DESC HalfDesc(const RF::Inputs& a_in, std::uint32_t a_toggleBits, DXGI_FORMAT a_format, UINT a_bindFlags)
	{
		const bool on = (a_in.Get(globals::features::effects11.raysToggles) & a_toggleBits) != 0;
		const auto display = DisplayOf(a_in);
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = on ? (display.width + 1) / 2 : 1;
		desc.Height = on ? (display.height + 1) / 2 : 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = a_bindFlags;
		return desc;
	}

	/** @brief As the D3D11 dispatches give them. */
	std::array<std::uint32_t, 3> BlurGroups(const ORGSunRays::Sizes& a_sizes, bool a_horizontal)
	{
		if (a_horizontal)
			return { (a_sizes.halfWidth + kBlurGroupPixels - 1) / kBlurGroupPixels, a_sizes.halfHeight, 1 };
		return { a_sizes.halfWidth, (a_sizes.halfHeight + kBlurGroupPixels - 1) / kBlurGroupPixels, 1 };
	}

	/** @brief What the last frame that wanted rays found. Not variables yet: a change invalidates the client's key. */
	struct Observed
	{
		FrameAnchors::Texture main;
		rhi::Format mainFormat = rhi::Format::Unknown;
		Source depth, shadowHeights, cloudShadows;
		const char* missing = nullptr;  // why the graph cannot draw this frame, null when it can

		bool operator==(const Observed& a_other) const
		{
			return main == a_other.main && mainFormat == a_other.mainFormat && depth == a_other.depth && shadowHeights == a_other.shadowHeights &&
			       cloudShadows == a_other.cloudShadows && (missing == nullptr) == (a_other.missing == nullptr);
		}
	};
}

struct ORGSunRays::Impl
{
	std::shared_ptr<Fixed> fixed;
	// Declared at setup and kept. A new kMAIN format or shader cache clear is a program built off the render thread.
	RF::DeclaredProgram<RaysPrograms> programs;
	RF::DeclaredClient client;
	Observed observed;
	// Registrations a schedule may still hold, newest last. The frame gives the graph only the effects its schedule's holds.
	std::vector<std::shared_ptr<const RaysState>> builds;
	std::uint64_t generation = 0;
	// What this frame's schedule holds, found once per frame: its registration, or none while the rays' client lags.
	std::shared_ptr<const RaysState> frameState;
	bool noStage = false;  // this frame kind has no stage for the rays, so D3D11 draws them
	bool lostLogged = false;
	// Gathered by VolumetricRays and SunRays, latched by the commit of the registration the run holds.
	LatchData pending{};
	bool pendingVolumetric = false, pendingSun = false;
	std::uint32_t preparedFrame = UINT32_MAX;
	bool preparedOk = false;
	std::string fallbackReason;  // why the last frame ran on D3D11, logged when it changes

	/**
	* @brief The registration this frame's schedule holds, as the post chain finds its build. With none, the frame draws no rays
	* while the client lags behind a revision, and D3D11 draws them only in a frame kind with no stage for them.
	*/
	void FindHeld()
	{
		frameState.reset();
		noStage = false;
		const auto hold = FrameAnchors::HeldNow(kClientId, FrameAnchors::Anchor::PostComposite);
		if (!hold.held) {
			// Registered: no stage of this frame kind holds the rays. Not registered: the client comes with the next revision.
			noStage = client.Registered();
			return;
		}
		if (!hold.scheduled)
			return;
		const auto found = std::ranges::find_if(builds, [&](const auto& a_build) { return a_build->generation == hold.generation; });
		if (found == builds.end()) {
			if (!std::exchange(lostLogged, true))
				logger::error("[ORG] Effects11 rays: the stage holds rays registration {}, which this driver no longer has; no rays are drawn", hold.generation);
			return;
		}
		builds.erase(builds.begin(), found);
		frameState = builds.front();
	}

	/** @brief The run's values, at the areas of the registration the run holds. */
	void Commit(const std::shared_ptr<const RaysState>& a_state)
	{
		const auto& shape = a_state->shape;
		const bool volumetric = pendingVolumetric && shape.volumetric;
		const bool sun = pendingSun && shape.sun;
		pendingVolumetric = pendingSun = false;
		if (!volumetric && !sun)
			return;
		const auto& sizes = shape.sizes;
		LatchData data = pending;
		data.vl = { static_cast<std::int32_t>(sizes.halfWidth), static_cast<std::int32_t>(sizes.halfHeight), static_cast<std::int32_t>(sizes.halfWidth) - 1,
			static_cast<std::int32_t>(sizes.halfHeight) - 1 };
		// The passes draw these areas, which can be a revision behind the frame's.
		for (auto& pass : data.sun) {
			pass.uvScale = { static_cast<float>(sizes.halfWidth) / static_cast<float>(sizes.halfTexWidth),
				static_cast<float>(sizes.halfHeight) / static_cast<float>(sizes.halfTexHeight) };
			pass.uvMax = { (static_cast<float>(sizes.halfWidth) - 0.5f) / static_cast<float>(sizes.halfTexWidth),
				(static_cast<float>(sizes.halfHeight) - 0.5f) / static_cast<float>(sizes.halfTexHeight) };
			pass.invScreenWidth = 1.0f / (std::max)(static_cast<float>(sizes.width), 1.0f);
		}
		const auto& gate = *a_state->fixed->gate;
		a_state->fixed->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, data);
		if (volumetric) {
			const auto h = BlurGroups(sizes, true);
			const auto v = BlurGroups(sizes, false);
			gate.OpenDraw(kVolumetricDraws);
			gate.OpenDispatch(kBlurH, h[0], h[1], h[2]);
			gate.OpenDispatch(kBlurV, v[0], v[1], v[2]);
		}
		if (sun)
			gate.OpenDraw(kSunDraws);
	}

	/** @brief The registration for a key, when a revision starts. False, logged, when a builder refuses a pass. */
	bool MakeClient(const Shape& a_shape, FrameAnchors::ClientDesc& a_client)
	{
		auto state = std::make_shared<RaysState>();
		state->shape = a_shape;
		state->fixed = fixed;
		state->generation = ++generation;
		const std::shared_ptr<const RaysState> shared = state;
		const auto& f = *fixed;
		const auto& programs = *a_shape.programs;
		// The raster passes stay their own passes, since they read other clients' textures by name and the raymarch has two targets.
		AP::Builder volumetric(FrameAnchors::Anchor::PostComposite, std::string(kClientId) + ".volumetric", kVolumetricOrder, f.latch, f.gate);
		if (a_shape.volumetric) {
			volumetric.Add("raymarch", FrameAnchors::PassKind::Render, [shared] { return std::make_shared<RaymarchPass>(shared); });
			volumetric.Add(AP::Dispatch{ .name = "blur-h",
				.program = programs.blurH,
				.bindings = { Bind::Srv(Slot(f.vlScattering)), Bind::Srv(Slot(f.vlDepth)), Bind::Uav(Slot(f.vlBlur)), Bind::Latch(kVLOffset) },
				.gate = kBlurH });
			volumetric.Add(AP::Dispatch{ .name = "blur-v",
				.program = programs.blurV,
				.bindings = { Bind::Srv(Slot(f.vlBlur)), Bind::Srv(Slot(f.vlDepth)), Bind::Uav(Slot(f.vlScattering)), Bind::Latch(kVLOffset) },
				.gate = kBlurV });
			volumetric.Add("apply", FrameAnchors::PassKind::Render, [shared] { return std::make_shared<ApplyPass>(shared); });
		}
		AP::Builder sun(FrameAnchors::Anchor::PostComposite, std::string(kClientId) + ".sun", kSunRaysOrder, f.latch, f.gate);
		if (a_shape.sun) {
			static constexpr std::array<const char*, kSunRaysPasses> kSunSteps{ "mask", "blur-1", "blur-2", "blur-3", "composite" };
			for (std::uint32_t pass = 0; pass < kSunRaysPasses; ++pass)
				sun.Add(kSunSteps[pass], FrameAnchors::PassKind::Render, [shared, pass] { return std::make_shared<SunRaysPass>(shared, pass); });
		}
		for (const auto* builder : { &volumetric, &sun }) {
			if (builder->Failed()) {
				logger::error("[ORG] Effects11 rays: {}", builder->Failure());
				return false;
			}
		}
		a_client.id = kClientId;
		a_client.passes = volumetric.Take();
		for (auto& pass : sun.Take())
			a_client.passes.push_back(std::move(pass));
		a_client.registerResources = [shared](org::RenderGraph& a_graph) {
			const auto& fixedState = *shared->fixed;
			if (shared->shape.volumetric)
				for (const auto* texture : { &fixedState.vlScattering, &fixedState.vlBlur, &fixedState.vlDepth })
					(*texture)->Register(a_graph);
			if (shared->shape.sun)
				for (const auto* texture : { &fixedState.sunA, &fixedState.sunB })
					(*texture)->Register(a_graph);
		};
		// The commit of the registration the schedule holds runs, also while a newer one waits for its revision (matchesById).
		// Effects the frame did not give stay closed.
		a_client.commit = [this, shared](const FrameAnchors::Point&) { Commit(shared); };
		a_client.sharedConstants = true;
		a_client.gates = { f.gate };
		a_client.matchesById = true;
		a_client.generation = state->generation;
		builds.push_back(shared);
		const auto& sizes = a_shape.sizes;
		logger::info("[ORG] Effects11 rays: client {} for the next revision (volumetric {}, sun {}, half-res {}x{}, kMAIN format {}, terrain shadows {}, cloud "
					 "shadows {})",
			state->generation, a_shape.volumetric ? "on" : "off", a_shape.sun ? "on" : "off", sizes.halfTexWidth, sizes.halfTexHeight, static_cast<std::uint32_t>(programs.mainFormat),
			a_shape.shadowHeights ? "on" : "off", a_shape.cloudShadows ? "on" : "off");
		return true;
	}

	/** @brief The client's key for a revision's values. No client while both effects are off or the inputs are missing. */
	std::optional<Shape> KeyFor(const RF::Inputs& a_in) const
	{
		const auto toggles = a_in.Get(globals::features::effects11.raysToggles);
		Shape shape;
		shape.volumetric = (toggles & Effects11::kVolumetricRaysBit) != 0;
		shape.sun = (toggles & (Effects11::kSunRaysBit | Effects11::kMoonRaysBit)) != 0;
		if (!shape.volumetric && !shape.sun)
			return std::nullopt;
		// The inputs come from the last frame that wanted rays. Before one could read them, no rays are drawn.
		if (observed.missing || !observed.main || !observed.depth)
			return std::nullopt;
		shape.programs = programs.For(a_in);
		// The programs follow raysTargetFormat, which the observation writes: the next revision has both.
		if (!shape.programs || shape.programs->mainFormat != observed.mainFormat)
			return std::nullopt;
		shape.sizes = SizesFor(a_in);
		shape.main = observed.main;
		shape.depth = observed.depth;
		shape.shadowHeights = observed.shadowHeights;
		shape.cloudShadows = observed.cloudShadows;
		return shape;
	}

	/** @brief This frame's inputs, as Effects11 binds them for its D3D11 passes. */
	static Observed Observe()
	{
		Observed now;
		const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		if (!main.texture || !main.RTV) {
			now.missing = "kMAIN is missing";
			return now;
		}
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		main.RTV->GetDesc(&rtvDesc);
		now.mainFormat = rhi::helpers::ToRHI(rtvDesc.Format);
		now.main = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
		if (!now.main || now.mainFormat == rhi::Format::Unknown) {
			now.missing = "kMAIN cannot be imported";
			return now;
		}
		// What CS bound at t17, resolved here rather than read back from the context.
		now.depth = SceneDepth(VanillaBinding::LastSceneDepth());
		if (!now.depth) {
			now.missing = "the scene depth at t17 is missing or cannot be imported";
			return now;
		}
		auto& terrainShadows = globals::features::terrainShadows;
		if (terrainShadows.loaded) {
			auto* view = terrainShadows.GetShadowHeightSRV();
			const auto& graphTerrain = ORGTerrainShadows::Get();
			if (view && graphTerrain.IsActive() && view == graphTerrain.ShadowHeightSRV())
				now.shadowHeights = { nullptr, std::string(ORGTerrainShadows::kShadowHeightId) };
			// The raymarch reads the heights whenever terrain shadows are on.
			if (!now.shadowHeights) {
				now.missing = "Terrain Shadows' heights are missing";
				return now;
			}
		}
		if (globals::features::cloudShadows.loaded) {
			// The cubemap Cloud Shadows binds at t25 for the frame. Use the graph's own when it is that one.
			auto* view = globals::features::cloudShadows.OcclusionSRV();
			const auto& graphClouds = ORGCloudShadows::Get();
			if (view && graphClouds.IsActive() && view == graphClouds.OcclusionSRV())
				now.cloudShadows = { nullptr, std::string(ORGCloudShadows::kOcclusionId) };
			if (!now.cloudShadows) {
				now.missing = "Cloud Shadows' texture is missing";
				return now;
			}
		}
		return now;
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
	auto* host = runtime.Host();
	if (!runtime.IsActive() || !host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::PostComposite)) {
		logger::warn("[ORG] Effects11 rays: CS_ORG_E11_RAYS=1 but the render graph or its PostComposite anchor is not available; the rays stay on D3D11");
		return false;
	}
	auto& effects11 = globals::features::effects11;
	auto state = std::make_unique<Impl>();
	auto fixed = std::make_shared<Fixed>();
	fixed->latch = std::make_shared<org::LatchBlock>("cs.e11-rays.latch", static_cast<std::uint32_t>(sizeof(LatchData)));
	fixed->gate = RenderGraph::RunGate::Create("cs.e11-rays.gate", kGateEntries);
	// As Deferred::linearSampler.
	fixed->linearSampler = ShaderBindings::CreateSampler({});
	if (!fixed->gate || !fixed->linearSampler) {
		logger::error("[ORG] Effects11 rays: the run gate or the sampler could not be made; the rays stay on D3D11");
		failed = true;
		return false;
	}

	// Written and read within the frame, so transient. Declared on the display size and the toggles (decision 6a): a new size,
	// or an effect turned on or off, is a new version of the same slot, with no build.
	{
		const GraphResources::Options transient{ .lifetime = GraphResources::Lifetime::Transient };
		constexpr UINT kTarget = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		constexpr UINT kStorage = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		constexpr std::uint32_t kVolumetric = Effects11::kVolumetricRaysBit;
		constexpr std::uint32_t kSun = Effects11::kSunRaysBit | Effects11::kMoonRaysBit;
		const auto dependencies = RF::DependsOn(RF::Vars::DisplaySize(), effects11.raysToggles);
		const auto declare = [&](const char* a_name, std::uint32_t a_bits, DXGI_FORMAT a_format, UINT a_bind) {
			return RF::DeclareTexture2D(
				a_name, dependencies, [a_bits, a_format, a_bind](const RF::Inputs& a_in) { return HalfDesc(a_in, a_bits, a_format, a_bind); }, transient);
		};
		fixed->vlScattering = declare("Effects11::VLTexA", kVolumetric, DXGI_FORMAT_R16_FLOAT, kTarget | kStorage);
		fixed->vlBlur = declare("Effects11::VLTexB", kVolumetric, DXGI_FORMAT_R16_FLOAT, kStorage);
		fixed->vlDepth = declare("Effects11::VLDepthHalf", kVolumetric, DXGI_FORMAT_R32_FLOAT, kTarget);
		fixed->sunA = declare("Effects11::SunRaysTexA", kSun, DXGI_FORMAT_R16_FLOAT, kTarget);
		fixed->sunB = declare("Effects11::SunRaysTexB", kSun, DXGI_FORMAT_R16_FLOAT, kTarget);
		for (const auto* texture : { &fixed->vlScattering, &fixed->vlBlur, &fixed->vlDepth, &fixed->sunA, &fixed->sunB }) {
			if (!*texture || !**texture) {
				logger::error("[ORG] Effects11 rays: the half-res textures could not be made; the rays stay on D3D11");
				failed = true;
				return false;
			}
		}
	}

	// kMAIN's format as setup finds it, so the first programs, built here, are the ones the first frame needs.
	if (const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN]; main.RTV) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		main.RTV->GetDesc(&rtvDesc);
		if (const auto format = rhi::helpers::ToRHI(rtvDesc.Format); format != rhi::Format::Unknown)
			effects11.raysTargetFormat = static_cast<std::uint32_t>(format);
	} else
		logger::warn("[ORG] Effects11 rays: kMAIN does not exist at setup; the programs are built for R16G16B16A16_FLOAT first");

	// A new kMAIN format or a shader cache clear builds the programs on the programs lane. The revision that needs them starts
	// once they are built.
	state->programs = RF::DeclareProgram("Effects11::Rays", RF::DependsOn(effects11.raysTargetFormat, effects11.shaderCacheClears),
		[](const RF::Inputs& a_in) {
			const auto& e11 = globals::features::effects11;
			const auto format = a_in.Get(e11.raysTargetFormat);
			return ProgramKey{ format ? format : static_cast<std::uint32_t>(rhi::Format::R16G16B16A16_Float), a_in.Get(e11.shaderCacheClears) };
		},
		[device = host->Device(), descs = ResolveDescs()](const ProgramKey& a_key) { return BuildPrograms(device, descs, a_key); });
	if (!state->programs) {
		logger::error("[ORG] Effects11 rays: the programs could not be built; the rays stay on D3D11");
		failed = true;
		return false;
	}
	state->fixed = fixed;

	// The client follows the size, the ratios (its areas), the toggles and the programs' inputs. Its sources are observed.
	auto* self = state.get();
	state->client = RF::DeclareClient<Shape>(kClientId,
		RF::DependsOn(RF::Vars::DisplaySize(), RF::Vars::DynamicRatios(), effects11.raysToggles, effects11.raysTargetFormat, effects11.shaderCacheClears),
		[self](const RF::Inputs& a_in) { return self->KeyFor(a_in); },
		[self](const Shape& a_shape, FrameAnchors::ClientDesc& a_client) { return self->MakeClient(a_shape, a_client); });
	if (!state->client) {
		logger::error("[ORG] Effects11 rays: the client could not be declared; the rays stay on D3D11");
		failed = true;
		return false;
	}
	impl = std::move(state);
	logger::info("[ORG] Effects11 rays run on the render graph; their textures, programs and client follow the frame revisions");
	return true;
}

bool ORGSunRays::IsActive() const
{
	return impl && !failed && RenderGraphRuntime::Get().IsActive();
}

bool ORGSunRays::PrepareFrame()
{
	if (!IsActive())
		return false;
	const std::uint32_t frame = globals::state->frameCount;
	if (impl->preparedFrame == frame)
		return impl->preparedOk;
	impl->preparedFrame = frame;
	impl->pendingVolumetric = impl->pendingSun = false;

	// Not variables yet: a change of the inputs registers the client again at the next revision.
	auto now = Impl::Observe();
	if (now.mainFormat != rhi::Format::Unknown)
		globals::features::effects11.raysTargetFormat = static_cast<std::uint32_t>(now.mainFormat);
	if (!(now == impl->observed))
		impl->client.Invalidate();
	impl->observed = std::move(now);
	impl->preparedOk = !impl->observed.missing;
	if (impl->observed.missing) {
		if (impl->fallbackReason != impl->observed.missing) {
			logger::info("[ORG] Effects11 rays: this frame runs on D3D11 ({})", impl->observed.missing);
			impl->fallbackReason = impl->observed.missing;
		}
		return false;
	}
	impl->FindHeld();
	const char* lag = impl->noStage ? "this frame kind has no stage for the rays" : (impl->frameState ? nullptr : "the rays' client lags behind a revision; no rays");
	if (lag) {
		if (impl->fallbackReason != lag) {
			logger::info("[ORG] Effects11 rays: this frame {}", impl->noStage ? "runs on D3D11 (no stage holds the rays)" : "draws no rays (the client lags behind a revision)");
			impl->fallbackReason = lag;
		}
	} else
		impl->fallbackReason.clear();
	// No stage: D3D11 draws them, as the post chain in a frame kind without a post stage.
	impl->preparedOk = !impl->noStage;
	return impl->preparedOk;
}

std::optional<std::uint32_t> ORGSunRays::AdoptedToggles() const
{
	if (!IsActive())
		return std::nullopt;
	return RenderGraph::Frame::Revision::Adopted().Get(globals::features::effects11.raysToggles);
}

bool ORGSunRays::VolumetricRays(const Sizes&)
{
	if (!PrepareFrame())
		return false;
	// The schedule's registration decides. One without the effect, or none while the client lags, draws nothing this frame.
	if (impl->frameState && impl->frameState->shape.volumetric)
		impl->pendingVolumetric = true;
	return true;
}

bool ORGSunRays::SunRays(const Sizes&, const std::array<Effects11::SunRaysData, kSunRaysPasses>& a_passes)
{
	if (!PrepareFrame())
		return false;
	if (impl->frameState && impl->frameState->shape.sun) {
		impl->pending.sun = a_passes;
		impl->pendingSun = true;
	}
	return true;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph the rays always run on D3D11.
struct ORGSunRays::Impl
{};

ORGSunRays::~ORGSunRays() = default;
void ORGSunRays::Reserve() {}
bool ORGSunRays::Setup() { return false; }
bool ORGSunRays::IsActive() const { return false; }
bool ORGSunRays::PrepareFrame() { return false; }
std::optional<std::uint32_t> ORGSunRays::AdoptedToggles() const { return std::nullopt; }
bool ORGSunRays::VolumetricRays(const Sizes&) { return false; }
bool ORGSunRays::SunRays(const Sizes&, const std::array<Effects11::SunRaysData, kSunRaysPasses>&) { return false; }

#endif
