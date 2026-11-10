#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGVolumetricShadows.h"

#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/Switches.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/OwnedDescriptorBinding.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/GloballyIndexedResource.h>

#	include <algorithm>
#	include <array>

namespace
{
	constexpr const char* kClientId = "cs.volumetric-shadows";
	constexpr const char* kDownsampleShader = "VolumetricShadows/DownsampleShadowCS.hlsl";
	constexpr const char* kBlurShader = "VolumetricShadows/BlurShadowCS.hlsl";
	// EarlyPrepass order key (org-extension-api-notes.md, "Keys used so far").
	constexpr std::int32_t kOrder = 100;

	// The shared shadow map: 512x512, two mips (mip 0 cascade 1, mip 1 cascade 0), as the D3D11 path makes it.
	constexpr UINT kCopySize = 512;
	constexpr UINT kMips = 2;
	// BlurShadowCS's GROUP_SIZE.
	constexpr UINT kBlurGroup = 128;

	constexpr auto kCascades = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM);
	constexpr auto kVolumetric = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM);

	// The push blocks: the shaders' resource lists (DOWNSAMPLE_SHADOW_RESOURCES, BLUR_SHADOW_RESOURCES), in list order.
	struct DownsampleBlock
	{
		ShaderBindings::Handle inputTexture, esramShadow, outputTexture, linearSampler;
	};
	struct BlurBlock
	{
		ShaderBindings::Handle inputTexture, outputTexture;
	};

	struct Resources
	{
		std::array<std::shared_ptr<const ComputeProgram>, kMips> downsample;  // by mip: DOWNSAMPLE_SHADOW_MIP0, _MIP1
		std::shared_ptr<const ComputeProgram> blurHorizontal;
		std::shared_ptr<const ComputeProgram> blurVertical;
		org::OwnedDescriptorBinding sampler;
		std::unique_ptr<GraphResources::Texture2D> shadowCopy;  // exported: engine draws and DCLF read it at t18
		std::unique_ptr<GraphResources::Texture2D> blurTemp;    // transient, aliasable
		// Render thread: the engine's maps as imported now, and the cascades' width. Read when a pass is made.
		std::shared_ptr<org::ExternalTextureResource> cascades;
		std::shared_ptr<org::ExternalTextureResource> volumetric;  // null: the cascades are read twice
		UINT sourceWidth = 0;
	};

	D3D11_TEXTURE2D_DESC CopyDesc()
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kCopySize;
		desc.Height = kCopySize;
		desc.MipLevels = kMips;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}

	org::ResourcePtrAndRange MipOf(const std::shared_ptr<org::Resource>& a_texture, std::uint32_t a_mip)
	{
		return org::Subresources(a_texture, org::Mip(a_mip, 1));
	}

	struct DownsampleBindings
	{
		org::DeclaredViewToken cascades, volumetric, output;
	};

	struct DispatchFrame
	{
		const ComputeProgram* program = nullptr;
		std::uint32_t groupsX = 0, groupsY = 0;
		ShaderBindings::Handle sampler = 0;
	};

	// One cascade into its mip of the shared map, with the volumetric copy's depth folded in (DownsampleShadowCS).
	class DownsamplePass final : public org::TypedRenderGraphPass<DownsamplePass, DispatchFrame, DownsampleBindings>
	{
	public:
		// At the build, on the render thread: the resources as they are now.
		DownsamplePass(const std::shared_ptr<Resources>& a_resources, std::uint32_t a_mip) :
			resources(a_resources), program(a_resources->downsample[a_mip]), cascades(a_resources->cascades),
			volumetric(a_resources->volumetric), output(a_resources->shadowCopy->Graph()), mip(a_mip)
		{
			// A group spans 16 source texels; a 32x reduction gathers twice per thread (the D3D11 path's sizing).
			const UINT outputSize = kCopySize >> a_mip;
			const UINT width = a_resources->sourceWidth;
			groups = width / 16 / (std::max)(width / outputSize / 16, 1u);
		}

		DownsampleBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// Both cascades, as the D3D11 path's Texture2DArray view of the engine's map.
			const org::SrvView wholeArray{ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) };
			DownsampleBindings bindings{};
			bindings.cascades = a_builder.ShaderResource(std::static_pointer_cast<org::Resource>(cascades), wholeArray).View();
			bindings.volumetric = volumetric ? a_builder.ShaderResource(std::static_pointer_cast<org::Resource>(volumetric), wholeArray).View() : bindings.cascades;
			bindings.output = a_builder.UnorderedAccess(MipOf(output, mip), org::UavView{ .mip = mip }).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(cascades.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(volumetric.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(output.get()));
			a_out.push_back(groups);
		}

		DispatchFrame Prepare(const DownsampleBindings&, const org::PassPrepareContext&) const
		{
			return { program.get(), groups, groups, resources->sampler.Index() };
		}

		static void Record(const DownsampleBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program)
				return;
			const DownsampleBlock block{ a_recording.Index(a_bindings.cascades), a_recording.Index(a_bindings.volumetric),
				a_recording.Index(a_bindings.output), a_frame.sampler };
			a_frame.program->Dispatch(a_recording, ShaderBindings::Data(block), a_frame.groupsX, a_frame.groupsY, 1);
		}

	private:
		std::shared_ptr<Resources> resources;  // keeps the program and the sampler alive while the pass is
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<org::ExternalTextureResource> cascades;
		std::shared_ptr<org::ExternalTextureResource> volumetric;
		std::shared_ptr<org::Resource> output;
		std::uint32_t mip;
		std::uint32_t groups = 0;
	};

	struct BlurBindings
	{
		org::DeclaredViewToken input, output;
	};

	// One direction of the 11x11 blur on one mip (BlurShadowCS): horizontal from the map into the temporary, vertical back.
	class BlurPass final : public org::TypedRenderGraphPass<BlurPass, DispatchFrame, BlurBindings>
	{
	public:
		BlurPass(const std::shared_ptr<Resources>& a_resources, std::uint32_t a_mip, bool a_horizontal) :
			resources(a_resources), program(a_horizontal ? a_resources->blurHorizontal : a_resources->blurVertical),
			input(a_horizontal ? a_resources->shadowCopy->Graph() : a_resources->blurTemp->Graph()),
			output(a_horizontal ? a_resources->blurTemp->Graph() : a_resources->shadowCopy->Graph()), mip(a_mip), horizontal(a_horizontal) {}

		BlurBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.ShaderResource(MipOf(input, mip), org::SrvView{ .mip = mip }).View(),
				a_builder.UnorderedAccess(MipOf(output, mip), org::UavView{ .mip = mip }).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(input.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(output.get()));
		}

		DispatchFrame Prepare(const BlurBindings&, const org::PassPrepareContext&) const
		{
			const std::uint32_t size = kCopySize >> mip;
			const std::uint32_t lines = (size + kBlurGroup - 1) / kBlurGroup;
			return { program.get(), horizontal ? lines : size, horizontal ? size : lines, 0 };
		}

		static void Record(const BlurBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program)
				return;
			const BlurBlock block{ a_recording.Index(a_bindings.input), a_recording.Index(a_bindings.output) };
			a_frame.program->Dispatch(a_recording, ShaderBindings::Data(block), a_frame.groupsX, a_frame.groupsY, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<org::Resource> input;
		std::shared_ptr<org::Resource> output;
		std::uint32_t mip;
		bool horizontal;
	};

	/** @brief The engine's depth map as a graph texture, when it exists with a depth SRV (what the passes read); else null. */
	std::shared_ptr<org::ExternalTextureResource> ImportShadowMap(std::uint32_t a_depthStencil)
	{
		if (!globals::game::renderer->GetDepthStencilData().depthStencils[a_depthStencil].depthSRV)
			return nullptr;
		return FrameAnchors::EngineDepth(a_depthStencil);
	}

	/** @brief The engine's cascade width; 0 when the map does not exist. */
	UINT CascadeWidth()
	{
		auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[kCascades].texture;
		if (!texture)
			return 0;
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		return desc.Width;
	}

	FrameAnchors::ClientDesc MakeClient(const std::shared_ptr<Resources>& a_resources, std::function<void()> a_onFailed)
	{
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		// Each pass is left out of a build without the engine's cascades: an epoch never reads a missing map.
		auto add = [&](std::string a_name, std::int32_t a_order, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
			client.passes.push_back({ .name = std::string(kClientId) + "." + std::move(a_name), .anchor = FrameAnchors::Anchor::EarlyPrepass, .order = kOrder + a_order, .kind = FrameAnchors::PassKind::Compute, .make = std::move(a_make) });
		};
		for (std::uint32_t mip = 0; mip < kMips; ++mip) {
			const std::int32_t order = static_cast<std::int32_t>(mip) * 3;
			add(fmt::format("downsample-mip{}", mip), order, [a_resources, mip]() -> std::shared_ptr<org::RenderPass> {
				return a_resources->cascades && a_resources->sourceWidth ? std::make_shared<DownsamplePass>(a_resources, mip) : nullptr;
			});
			add(fmt::format("blur-h-mip{}", mip), order + 1, [a_resources, mip]() -> std::shared_ptr<org::RenderPass> {
				return a_resources->cascades && a_resources->sourceWidth ? std::make_shared<BlurPass>(a_resources, mip, true) : nullptr;
			});
			add(fmt::format("blur-v-mip{}", mip), order + 2, [a_resources, mip]() -> std::shared_ptr<org::RenderPass> {
				return a_resources->cascades && a_resources->sourceWidth ? std::make_shared<BlurPass>(a_resources, mip, false) : nullptr;
			});
		}
		client.registerResources = [a_resources](org::RenderGraph& a_graph) {
			a_resources->shadowCopy->Register(a_graph);
			a_resources->blurTemp->Register(a_graph);
		};
		// The D3D11 path ran at the shadow mask draw only with directional shadows; so do the passes.
		client.active = [a_resources] { return a_resources->cascades && a_resources->sourceWidth && globals::state->HasDirectionalShadows(); };
		client.onFailed = std::move(a_onFailed);
		return client;
	}
}

struct ORGVolumetricShadows::Impl
{
	std::shared_ptr<Resources> resources;
	bool inputChecked = false;
	std::function<void()> onFailed;
};

bool ORGVolumetricShadows::Requested()
{
	static const bool requested = Switches::On("CS_ORG_VOLUMETRIC_SHADOWS");
	return requested;
}

ORGVolumetricShadows& ORGVolumetricShadows::Get()
{
	static ORGVolumetricShadows s_shadows;
	return s_shadows;
}

ORGVolumetricShadows::~ORGVolumetricShadows() = default;

void ORGVolumetricShadows::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] Volumetric Shadows stays on D3D11: the EarlyPrepass anchor could not be reserved");
}

bool ORGVolumetricShadows::Setup(std::function<void()> a_onFailed)
{
	impl.reset();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.IsActive() || !runtime.Host() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass)) {
		logger::warn("[ORG] Volumetric Shadows stays on D3D11: the render graph or its EarlyPrepass anchor is not available");
		return false;
	}
	auto resources = std::make_shared<Resources>();
	auto device = runtime.Host()->Device();
	for (std::uint32_t mip = 0; mip < kMips; ++mip)
		resources->downsample[mip] = ComputeProgram::Load(device, { .source = kDownsampleShader,
																	  .constantWords = ShaderBindings::Words<DownsampleBlock>(),
																	  .defines = { { mip ? L"DOWNSAMPLE_SHADOW_MIP1" : L"DOWNSAMPLE_SHADOW_MIP0", L"1" } } });
	resources->blurHorizontal = ComputeProgram::Load(device, { .source = kBlurShader, .constantWords = ShaderBindings::Words<BlurBlock>(), .defines = { { L"BLUR_HORIZONTAL", L"1" } } });
	resources->blurVertical = ComputeProgram::Load(device, { .source = kBlurShader, .constantWords = ShaderBindings::Words<BlurBlock>(), .defines = { { L"BLUR_VERTICAL", L"1" } } });
	if (!resources->downsample[0] || !resources->downsample[1] || !resources->blurHorizontal || !resources->blurVertical) {
		logger::warn("[ORG] Volumetric Shadows stays on D3D11: its programs could not be created");
		return false;
	}
	resources->sampler = ShaderBindings::CreateSampler({});
	// Read by the engine's draws and DCLF in later frames too (the reflections draw before this frame's copy): exported, so never aliased.
	resources->shadowCopy = std::make_unique<GraphResources::Texture2D>(CopyDesc(), std::string(kShadowCopyId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV });
	resources->blurTemp = std::make_unique<GraphResources::Texture2D>(CopyDesc(), "VolumetricShadows::ShadowBlurTemp");
	if (!resources->sampler || !*resources->shadowCopy || !resources->shadowCopy->SRV() || !*resources->blurTemp) {
		logger::warn("[ORG] Volumetric Shadows stays on D3D11: its sampler or textures could not be created");
		return false;
	}
	auto state = std::make_unique<Impl>();
	state->resources = resources;
	state->onFailed = std::move(a_onFailed);
	impl = std::move(state);
	const bool registered = FrameAnchors::Register(MakeClient(resources, [this] {
		logger::error("[ORG] Volumetric Shadows: the render graph failed; back to the D3D11 dispatches");
		PublishedResources::Withdraw(kShadowCopyId);
		auto onFailed = impl ? std::move(impl->onFailed) : nullptr;
		impl.reset();
		if (onFailed)
			onFailed();
	}));
	if (!registered) {
		logger::warn("[ORG] Volumetric Shadows stays on D3D11: its anchor client could not be registered");
		impl.reset();
		return false;
	}
	// Drawcall Limit Fix's colour passes read it by name, ordered after the blurs (Integrations/VolumetricShadows).
	if (!PublishedResources::Publish(*resources->shadowCopy)) {
		logger::warn("[ORG] Volumetric Shadows stays on D3D11: the shared shadow map could not be published");
		Stop();
		return false;
	}
	logger::info("[ORG] Volumetric Shadows runs on the render graph at EarlyPrepass (shared map aliasable {}, temporary aliasable {})",
		resources->shadowCopy->Aliasable(), resources->blurTemp->Aliasable());
	return true;
}

void ORGVolumetricShadows::Stop()
{
	FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kShadowCopyId);
	impl.reset();
}

void ORGVolumetricShadows::UpdateImports()
{
	if (!IsActive())
		return;
	// The engine's maps as they are now: a new import (the engine replaced its texture) builds the graphs again.
	auto& resources = *impl->resources;
	auto cascades = ImportShadowMap(kCascades);
	auto volumetric = ImportShadowMap(kVolumetric);
	const UINT width = cascades ? CascadeWidth() : 0u;
	if (cascades != resources.cascades || volumetric != resources.volumetric || width != resources.sourceWidth) {
		const bool first = !resources.cascades;
		resources.cascades = std::move(cascades);
		resources.volumetric = std::move(volumetric);
		resources.sourceWidth = width;
		FrameAnchors::Refresh(kClientId);
		if (first || !resources.cascades)
			logger::info("[ORG] Volumetric Shadows: cascades {} ({} wide), volumetric copy {}", resources.cascades ? "imported" : "missing",
				resources.sourceWidth, resources.volumetric ? "imported" : "missing (the cascades are read twice)");
	}
}

bool ORGVolumetricShadows::IsActive() const
{
	return impl && impl->resources && RenderGraphRuntime::Get().IsActive();
}

void ORGVolumetricShadows::CheckShadowMaskInput(ID3D11ShaderResourceView* a_shadowMap)
{
	if (!IsActive() || impl->inputChecked || !a_shadowMap)
		return;
	impl->inputChecked = true;
	// The field can be a tripwire handle; a_shadowMap comes from DXVK's context. Compare what they wrap.
	using RenderGraph::Engine::Tripwire::Unwrap;
	auto* expected = globals::game::renderer->GetDepthStencilData().depthStencils[kCascades].depthSRV;
	if (Unwrap(a_shadowMap) == Unwrap(expected)) {
		logger::info("[ORG] Volumetric Shadows: the shadow mask's t4 is the engine's cascade map, as the passes read it");
		return;
	}
	logger::error("[ORG] Volumetric Shadows: the shadow mask's t4 is not the cascade map the passes read; back to the D3D11 dispatches");
	Stop();
}

ID3D11ShaderResourceView* ORGVolumetricShadows::SharedShadowMapSRV() const
{
	return IsActive() ? impl->resources->shadowCopy->SRV() : nullptr;
}

ID3D11Texture2D* ORGVolumetricShadows::SharedShadowMapTexture() const
{
	return IsActive() ? impl->resources->shadowCopy->D3D11() : nullptr;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Volumetric Shadows always runs its D3D11 dispatches.
#	include "ORGVolumetricShadows.h"

struct ORGVolumetricShadows::Impl
{};

bool ORGVolumetricShadows::Requested() { return false; }

ORGVolumetricShadows& ORGVolumetricShadows::Get()
{
	static ORGVolumetricShadows s_shadows;
	return s_shadows;
}

ORGVolumetricShadows::~ORGVolumetricShadows() = default;
void ORGVolumetricShadows::Reserve() {}
bool ORGVolumetricShadows::Setup(std::function<void()>) { return false; }
void ORGVolumetricShadows::UpdateImports() {}
void ORGVolumetricShadows::Stop() {}
bool ORGVolumetricShadows::IsActive() const { return false; }
void ORGVolumetricShadows::CheckShadowMaskInput(ID3D11ShaderResourceView*) {}
ID3D11ShaderResourceView* ORGVolumetricShadows::SharedShadowMapSRV() const { return nullptr; }
ID3D11Texture2D* ORGVolumetricShadows::SharedShadowMapTexture() const { return nullptr; }
#endif
