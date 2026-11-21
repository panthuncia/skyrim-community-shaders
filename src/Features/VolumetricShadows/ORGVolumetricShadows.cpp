// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "ORGVolumetricShadows.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "Globals.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/Switches.h"
#include "State.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/OwnedDescriptorBinding.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/GloballyIndexedResource.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace
{
	constexpr const char* kClientId = "cs.volumetric-shadows";
	constexpr const char* kDownsampleShader = "VolumetricShadows/DownsampleShadowCS.hlsl";
	constexpr const char* kBlurShader = "VolumetricShadows/BlurShadowCS.hlsl";
	constexpr std::int32_t kOrder = 100;

	// 512x512 with two mips, mip 0 for cascade 1 and mip 1 for cascade 0, as the D3D11 path makes it.
	constexpr UINT kCopySize = 512;
	constexpr UINT kMips = 2;
	// BlurShadowCS's GROUP_SIZE.
	constexpr UINT kBlurGroup = 128;

	// Per mip: downsample, horizontal blur, vertical blur. The commit opens them all.
	constexpr std::uint32_t DownsampleEntry(std::uint32_t a_mip) { return a_mip * 3; }
	constexpr std::uint32_t BlurEntry(std::uint32_t a_mip, bool a_horizontal) { return a_mip * 3 + (a_horizontal ? 1 : 2); }
	constexpr std::uint32_t kGateEntries = kMips * 3;

	/** @brief A group spans 16 source texels. A 32x reduction gathers twice per thread. */
	std::uint32_t DownsampleGroups(UINT a_sourceWidth, std::uint32_t a_mip)
	{
		const UINT outputSize = kCopySize >> a_mip;
		return a_sourceWidth / 16 / (std::max)(a_sourceWidth / outputSize / 16, 1u);
	}

	/** @brief A line of kBlurGroup texels per group along the blur. */
	std::pair<std::uint32_t, std::uint32_t> BlurGroups(std::uint32_t a_mip, bool a_horizontal)
	{
		const std::uint32_t size = kCopySize >> a_mip;
		const std::uint32_t lines = (size + kBlurGroup - 1) / kBlurGroup;
		return { a_horizontal ? lines : size, a_horizontal ? size : lines };
	}

	constexpr auto kCascades = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM);
	constexpr auto kVolumetric = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM);

	// In DOWNSAMPLE_SHADOW_RESOURCES and BLUR_SHADOW_RESOURCES order.
	struct DownsampleBlock
	{
		ShaderBindings::Handle inputTexture, esramShadow, outputTexture, linearSampler;
	};
	struct BlurBlock
	{
		ShaderBindings::Handle inputTexture, outputTexture;
	};

	/**
	* @brief What the client's passes read: the engine's cascades, its volumetric copy and the cascades' width. The engine makes
	* them once a session (Q4) and raises ShadowCascades then, so the key imports them when a revision carries that change.
	*/
	struct CascadeInputs
	{
		FrameAnchors::Texture cascades;
		FrameAnchors::Texture volumetric;  // null means the cascades are read twice
		UINT sourceWidth = 0;

		bool Complete() const { return cascades && sourceWidth; }
		bool operator==(const CascadeInputs&) const = default;
	};

	/** @brief Made at setup and kept. */
	struct Resources
	{
		std::array<RenderGraph::Programs::Fixed<ComputeProgram>, kMips> downsample;  // by mip
		RenderGraph::Programs::Fixed<ComputeProgram> blurHorizontal;
		RenderGraph::Programs::Fixed<ComputeProgram> blurVertical;
		org::OwnedDescriptorBinding sampler;
		std::unique_ptr<GraphResources::Texture2D> shadowCopy;  // exported, engine draws and DCLF read it at t18
		std::unique_ptr<GraphResources::Texture2D> blurTemp;    // transient
		std::shared_ptr<RenderGraph::RunGate> gate;               // opened by the commit
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
		ShaderBindings::Handle sampler = 0;
		std::shared_ptr<const RenderGraph::RunGate> gate;  // its entry, which the commit opens
		std::uint32_t entry = 0;
	};

	// One cascade into its mip, with the volumetric copy's depth folded in.
	class DownsamplePass final : public org::TypedRenderGraphPass<DownsamplePass, DispatchFrame, DownsampleBindings>
	{
	public:
		// At the build, on the render thread, so it sees the resources as they are now.
		DownsamplePass(const std::shared_ptr<Resources>& a_resources, const CascadeInputs& a_inputs, std::uint32_t a_mip) :
			resources(a_resources), program(a_resources->downsample[a_mip].Get()), cascades(a_inputs.cascades),
			volumetric(a_inputs.volumetric), output(a_resources->shadowCopy->Graph()), mip(a_mip) {}

		DownsampleBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// Both cascades, as the D3D11 path's Texture2DArray view.
			const org::SrvView wholeArray{ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) };
			DownsampleBindings bindings{};
			bindings.cascades = a_builder.ShaderResource(cascades, wholeArray).View();
			bindings.volumetric = volumetric ? a_builder.ShaderResource(volumetric, wholeArray).View() : bindings.cascades;
			bindings.output = a_builder.UnorderedAccess(MipOf(output, mip), org::UavView{ .mip = mip }).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(cascades.Key()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(volumetric.Key()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(output.get()));
		}

		DispatchFrame Prepare(const DownsampleBindings&, const org::PassPrepareContext&) const
		{
			return { program.get(), resources->sampler.Index(), resources->gate, DownsampleEntry(mip) };
		}

		static void Record(const DownsampleBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program)
				return;
			const DownsampleBlock block{ a_recording.Index(a_bindings.cascades), a_recording.Index(a_bindings.volumetric),
				a_recording.Index(a_bindings.output), a_frame.sampler };
			if (a_frame.gate)
				a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), a_frame.entry);
		}

	private:
		std::shared_ptr<Resources> resources;  // keeps the program and sampler alive while the pass is
		std::shared_ptr<const ComputeProgram> program;
		FrameAnchors::Texture cascades;
		FrameAnchors::Texture volumetric;
		std::shared_ptr<org::Resource> output;
		std::uint32_t mip;
	};

	struct BlurBindings
	{
		org::DeclaredViewToken input, output;
	};

	// One direction of the 11x11 blur on one mip, through the temporary.
	class BlurPass final : public org::TypedRenderGraphPass<BlurPass, DispatchFrame, BlurBindings>
	{
	public:
		BlurPass(const std::shared_ptr<Resources>& a_resources, std::uint32_t a_mip, bool a_horizontal) :
			resources(a_resources), program((a_horizontal ? a_resources->blurHorizontal : a_resources->blurVertical).Get()),
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
			return { program.get(), 0, resources->gate, BlurEntry(mip, horizontal) };
		}

		static void Record(const BlurBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program)
				return;
			const BlurBlock block{ a_recording.Index(a_bindings.input), a_recording.Index(a_bindings.output) };
			if (a_frame.gate)
				a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), a_frame.entry);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<org::Resource> input;
		std::shared_ptr<org::Resource> output;
		std::uint32_t mip;
		bool horizontal;
	};

	/** @brief Null unless it exists with a depth SRV. */
	FrameAnchors::Texture ImportShadowMap(std::uint32_t a_depthStencil)
	{
		if (!globals::game::renderer->GetDepthStencilData().depthStencils[a_depthStencil].depthSRV)
			return nullptr;
		return FrameAnchors::EngineTexture(FrameAnchors::Depth(a_depthStencil));
	}

	/**
	* @brief The key at a revision's values: the cascades and the volumetric copy, imported now, and the width ShadowCascades
	* carries. Empty inputs (no passes) before the engine made its shadow maps. Fatal when the engine has them and they cannot
	* be imported, since no later event would register the client.
	*/
	CascadeInputs CascadesAt(const RenderGraph::Frame::Inputs& a_values)
	{
		const auto shape = a_values.Get(RenderGraph::Frame::Vars::ShadowCascades());
		CascadeInputs inputs;
		if (!shape.size.width)
			return inputs;
		inputs.cascades = ImportShadowMap(kCascades);
		if (!inputs.cascades)
			RenderGraph::Fatal(ORGVolumetricShadows::kName, "the engine made its cascade shadow map, but it could not be imported");
		inputs.volumetric = ImportShadowMap(kVolumetric);
		inputs.sourceWidth = shape.size.width;
		return inputs;
	}

	/**
	* @brief The registration for a_inputs, when a revision starts. Without the cascades it has no passes, so an epoch never
	* reads a missing map. The group counts come from the width the registration is made for, so the commit and the passes agree.
	*/
	bool MakeClient(const std::shared_ptr<Resources>& a_resources, const CascadeInputs& a_inputs, FrameAnchors::ClientDesc& a_client)
	{
		auto& client = a_client;
		client.id = kClientId;
		const bool complete = a_inputs.Complete();
		auto add = [&](std::string a_name, std::int32_t a_order, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
			client.passes.push_back({ .name = std::string(kClientId) + "." + std::move(a_name),
				.anchor = FrameAnchors::Anchor::EarlyPrepass,
				.order = kOrder + a_order,
				.kind = FrameAnchors::PassKind::Compute,
				.make = std::move(a_make) });
		};
		for (std::uint32_t mip = 0; mip < kMips; ++mip) {
			const std::int32_t order = static_cast<std::int32_t>(mip) * 3;
			add(fmt::format("downsample-mip{}", mip), order, [a_resources, a_inputs, complete, mip]() -> std::shared_ptr<org::RenderPass> {
				return complete ? std::make_shared<DownsamplePass>(a_resources, a_inputs, mip) : nullptr;
			});
			add(fmt::format("blur-h-mip{}", mip), order + 1, [a_resources, complete, mip]() -> std::shared_ptr<org::RenderPass> {
				return complete ? std::make_shared<BlurPass>(a_resources, mip, true) : nullptr;
			});
			add(fmt::format("blur-v-mip{}", mip), order + 2, [a_resources, complete, mip]() -> std::shared_ptr<org::RenderPass> {
				return complete ? std::make_shared<BlurPass>(a_resources, mip, false) : nullptr;
			});
		}
		client.registerResources = [a_resources](org::RenderGraph& a_graph) {
			a_resources->shadowCopy->Register(a_graph);
			a_resources->blurTemp->Register(a_graph);
		};
		// Only with directional shadows, as the D3D11 path.
		client.active = [complete] { return complete && globals::state->HasDirectionalShadows(); };
		// Directional shadows follow the cell and are read at frame start. The cascades need no bit of their own, since builds only make passes with them.
		client.declared = [] { return globals::state->HasDirectionalShadows(); };
		client.gates = { a_resources->gate };
		const UINT sourceWidth = a_inputs.sourceWidth;
		client.commit = [a_resources, sourceWidth](const FrameAnchors::Point&) {
			const auto& gate = *a_resources->gate;
			for (std::uint32_t mip = 0; mip < kMips; ++mip) {
				const std::uint32_t groups = DownsampleGroups(sourceWidth, mip);
				gate.OpenDispatch(DownsampleEntry(mip), groups, groups, 1);
				for (const bool horizontal : { true, false }) {
					const auto [x, y] = BlurGroups(mip, horizontal);
					gate.OpenDispatch(BlurEntry(mip, horizontal), x, y, 1);
				}
			}
		};
		logger::info("[ORG] Volumetric Shadows: a client for the next revision (cascades {} ({} wide), volumetric copy {})",
			a_inputs.cascades ? "imported" : "missing", a_inputs.sourceWidth,
			a_inputs.volumetric ? "imported" : "missing (the cascades are read twice)");
		return true;
	}
}

struct ORGVolumetricShadows::Impl
{
	std::shared_ptr<Resources> resources;
	// Whether the key last evaluated imported the cascades.
	std::shared_ptr<bool> imported = std::make_shared<bool>(false);
	// Declared at Setup and kept. New inputs are a new registration, switched in by a frame revision.
	RenderGraph::Frame::DeclaredClient client;
	bool inputChecked = false;
};

ORGVolumetricShadows::ORGVolumetricShadows() = default;
ORGVolumetricShadows::~ORGVolumetricShadows() = default;

void ORGVolumetricShadows::Setup()
{
	impl.reset();
	if (!On())
		return;
	SetupHost();
	auto resources = std::make_shared<Resources>();
	for (std::uint32_t mip = 0; mip < kMips; ++mip)
		resources->downsample[mip] = RenderGraph::Programs::Queue("VolumetricShadows",
			ComputeProgram::Desc{ .source = kDownsampleShader,
				.constantWords = ShaderBindings::Words<DownsampleBlock>(),
				.defines = { { mip ? L"DOWNSAMPLE_SHADOW_MIP1" : L"DOWNSAMPLE_SHADOW_MIP0", L"1" } } });
	resources->blurHorizontal = RenderGraph::Programs::Queue("VolumetricShadows",
		ComputeProgram::Desc{ .source = kBlurShader, .constantWords = ShaderBindings::Words<BlurBlock>(), .defines = { { L"BLUR_HORIZONTAL", L"1" } } });
	resources->blurVertical = RenderGraph::Programs::Queue("VolumetricShadows",
		ComputeProgram::Desc{ .source = kBlurShader, .constantWords = ShaderBindings::Words<BlurBlock>(), .defines = { { L"BLUR_VERTICAL", L"1" } } });
	resources->sampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearClamp);
	// Read in later frames too, since reflections draw before this frame's copy. Exported, so never aliased.
	resources->shadowCopy = std::make_unique<GraphResources::Texture2D>(CopyDesc(), std::string(kShadowCopyId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV });
	resources->blurTemp = std::make_unique<GraphResources::Texture2D>(CopyDesc(), "VolumetricShadows::ShadowBlurTemp");
	resources->gate = RenderGraph::RunGate::Create(kClientId, kGateEntries);
	if (!resources->sampler || !resources->shadowCopy->SRV())
		Fatal("its sampler or the shadow copy's view could not be created");
	auto state = std::make_unique<Impl>();
	state->resources = resources;
	// The client follows the engine's cascades (one event a session, when the engine's targets are made). Its first evaluation is
	// at the next revision.
	auto imported = state->imported;
	state->client = RenderGraph::Frame::DeclareClient<CascadeInputs>(
		kClientId, RenderGraph::Frame::DependsOn(RenderGraph::Frame::Vars::ShadowCascades()),
		[imported](const RenderGraph::Frame::Inputs& a_values) -> std::optional<CascadeInputs> {
			auto inputs = CascadesAt(a_values);
			*imported = static_cast<bool>(inputs.cascades);
			return inputs;
		},
		[resources](const CascadeInputs& a_inputs, FrameAnchors::ClientDesc& a_client) { return MakeClient(resources, a_inputs, a_client); });
	// DCLF's colour passes read it by name, ordered after the blurs. Published once, for the session.
	if (!PublishedResources::Publish(*resources->shadowCopy))
		Fatal("the shared shadow map could not be published");
	impl = std::move(state);
	logger::info("[ORG] Volumetric Shadows runs on the render graph at EarlyPrepass (shared map aliasable {}, temporary aliasable {}); its client "
				 "follows the frame revisions",
		resources->shadowCopy->Aliasable(), resources->blurTemp->Aliasable());
}

void ORGVolumetricShadows::UpdateImports()
{
	if (!IsActive() || !*impl->imported)
		return;
	// The engine makes its shadow maps once a session, so a lost map is an error, not a change to follow.
	if (!globals::game::renderer->GetDepthStencilData().depthStencils[kCascades].depthSRV)
		Fatal("the engine's cascade shadow map could no longer be imported");
}

void ORGVolumetricShadows::CheckShadowMaskInput(ID3D11ShaderResourceView* a_shadowMap)
{
	if (!IsActive() || impl->inputChecked || !a_shadowMap)
		return;
	impl->inputChecked = true;
	// The field may be a tripwire handle while a_shadowMap is DXVK's, so compare what they wrap.
	using RenderGraph::Engine::Tripwire::Unwrap;
	auto* expected = globals::game::renderer->GetDepthStencilData().depthStencils[kCascades].depthSRV;
	if (Unwrap(a_shadowMap) == Unwrap(expected)) {
		logger::info("[ORG] Volumetric Shadows: the shadow mask's t4 is the engine's cascade map, as the passes read it");
		return;
	}
	// Going back to D3D11 here would withdraw the published map outside a revision, from graphs built on it.
	Fatal("the shadow mask's t4 is not the cascade map the passes read");
}

ID3D11ShaderResourceView* ORGVolumetricShadows::SharedShadowMapSRV() const
{
	return IsActive() ? impl->resources->shadowCopy->SRV() : nullptr;
}

ID3D11Texture2D* ORGVolumetricShadows::SharedShadowMapTexture() const
{
	return IsActive() ? impl->resources->shadowCopy->D3D11() : nullptr;
}

