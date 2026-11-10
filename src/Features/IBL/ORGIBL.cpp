#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "ORGIBL.h"

#include "Features/DynamicCubemaps.h"
#include "Features/DynamicCubemaps/ORGDynamicCubemaps.h"
#include "Features/IBL.h"
#include "Globals.h"
#include "RenderGraph/Switches.h"

ORGIBL& ORGIBL::Get()
{
	static ORGIBL s_instance;
	return s_instance;
}

ORGIBL::~ORGIBL() = default;

bool ORGIBL::Requested()
{
	static const bool requested = Switches::On("CS_ORG_IBL");
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/OwnedDescriptorBinding.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

namespace
{
	constexpr const char* kClientId = "cs.ibl";
	constexpr const char* kShader = "IBL/DiffuseIBLCS.hlsl";

	// The SH textures: three texels (R, G, B), each an sh2 (float4) in RGBA16F.
	constexpr UINT kShTexels = 3;

	D3D11_TEXTURE2D_DESC ShTextureDesc()
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kShTexels;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}

	// DiffuseIBLCS's push block (DIFFUSE_IBL_RESOURCES, in list order).
	struct DiffuseIBLBlock
	{
		ShaderBindings::Handle envTexture, iblTexture, linearSampler;
	};

	// One projection: a cubemap into a 3x1 SH texture, one 16x16 group.
	struct ProjectBindings
	{
		org::DeclaredViewToken source, target;
	};

	struct ProjectFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		ShaderBindings::Handle sampler = 0;
	};

	/** @brief A projection's source cubemap: an imported texture, or another feature's published graph texture by its id. */
	struct Source
	{
		std::shared_ptr<org::Resource> resource;
		std::string_view published;

		explicit operator bool() const { return resource || !published.empty(); }
		bool operator==(const Source&) const = default;
	};

	class ProjectPass final : public org::TypedRenderGraphPass<ProjectPass, ProjectFrame, ProjectBindings>
	{
	public:
		ProjectPass(std::shared_ptr<const ComputeProgram> a_program, ShaderBindings::Handle a_sampler, Source a_source, std::shared_ptr<org::Resource> a_target) :
			program(std::move(a_program)), sampler(a_sampler), source(std::move(a_source)), target(std::move(a_target)) {}

		ProjectBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// An import's default view: a cube view of mip 0, as the D3D11 path samples it. A published cubemap by name: its cube view.
			if (source.resource)
				return { a_builder.ShaderResource(source.resource).View(), a_builder.UnorderedAccess(target).View() };
			const org::SrvView cube{ .variant = static_cast<std::uint32_t>(org::SRVViewType::TextureCube) };
			return { a_builder.ShaderResource(org::ResourceIdentifier(source.published), cube).View(), a_builder.UnorderedAccess(target).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(source.resource.get()));
			a_out.push_back(std::hash<std::string_view>{}(source.published));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(target.get()));
		}

		ProjectFrame Prepare(const ProjectBindings&, const org::PassPrepareContext&) const { return { program, sampler }; }

		static void Record(const ProjectBindings& a_bindings, const ProjectFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program)
				return;
			const DiffuseIBLBlock block{ a_recording.Index(a_bindings.source), a_recording.Index(a_bindings.target), a_frame.sampler };
			a_frame.program->Dispatch(a_recording, ShaderBindings::Data(block), 1, 1, 1);
		}

	private:
		std::shared_ptr<const ComputeProgram> program;
		ShaderBindings::Handle sampler;
		Source source;
		std::shared_ptr<org::Resource> target;
	};
}

struct ORGIBL::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	org::OwnedDescriptorBinding sampler;
	std::unique_ptr<GraphResources::Texture2D> env;
	std::unique_ptr<GraphResources::Texture2D> sky;
	// This frame's sources (render thread): the passes read them when they are made.
	Source envSource;
	Source skySource;
	bool useEnvironment = false;
	std::function<void()> onFailed;
};

void ORGIBL::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Prepass))
		logger::warn("[ORG] IBL: the Prepass anchor could not be reserved; IBL stays on D3D11");
}

bool ORGIBL::Setup(std::function<void()> a_onFailed)
{
	impl.reset();
	if (!Requested())
		return false;
	auto* host = RenderGraphRuntime::Get().Host();
	if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass)) {
		logger::warn("[ORG] IBL: the render graph or its Prepass anchor is not available; IBL stays on D3D11");
		return false;
	}
	auto state = std::make_unique<Impl>();
	ComputeProgram::Desc desc{ .source = kShader, .constantWords = ShaderBindings::Words<DiffuseIBLBlock>() };
	// The D3D11 path's own defines (IBL::GetDiffuseIBLCS); ComputeProgram adds the common ones.
	if (globals::features::dynamicCubemaps.loaded)
		desc.defines.push_back({ L"DYNAMIC_CUBEMAPS", L"" });
	state->program = ComputeProgram::Load(host->Device(), desc);
	// Linear, clamped, every mip: Deferred::linearSampler.
	state->sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
	// Read in the next frame (the reflection prepass binds the last frame's SH), and by D3D11 until its readers are ported.
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV };
	state->env = std::make_unique<GraphResources::Texture2D>(ShTextureDesc(), std::string(kEnvId), options);
	state->sky = std::make_unique<GraphResources::Texture2D>(ShTextureDesc(), std::string(kSkyId), options);
	if (!state->program || !state->sampler || !*state->env || !*state->sky || !state->env->SRV() || !state->sky->SRV()) {
		logger::error("[ORG] IBL: its program, sampler or SH textures could not be made; IBL stays on D3D11");
		return false;
	}
	state->onFailed = std::move(a_onFailed);

	auto* self = this;
	auto makePass = [self](bool a_env) -> std::shared_ptr<org::RenderPass> {
		const auto& current = self->impl;
		if (!current)
			return nullptr;
		const auto& source = a_env ? current->envSource : current->skySource;
		const auto& target = a_env ? current->env : current->sky;
		// The environment only while the DALC mode reads it (the D3D11 path skips it too); a missing source leaves the last values.
		if (!source || (a_env && !current->useEnvironment))
			return nullptr;
		return std::make_shared<ProjectPass>(current->program, current->sampler.Index(), source, target->Graph());
	};
	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes.push_back({ .name = "cs.ibl.env-sh", .anchor = FrameAnchors::Anchor::Prepass, .kind = FrameAnchors::PassKind::Compute, .make = [makePass] { return makePass(true); } });
	client.passes.push_back({ .name = "cs.ibl.sky-sh", .anchor = FrameAnchors::Anchor::Prepass, .order = 1, .kind = FrameAnchors::PassKind::Compute, .make = [makePass] { return makePass(false); } });
	client.registerResources = [self](org::RenderGraph& a_graph) {
		if (self->impl) {
			self->impl->env->Register(a_graph);
			self->impl->sky->Register(a_graph);
		}
	};
	// IBL::Prepass returns before computing anything while the scene disables IBL.
	client.active = [] { return !globals::features::ibl.IsDisabledForCurrentScene(); };
	client.onFailed = [self] { self->Fail("the render graph failed", false); };
	impl = std::move(state);
	if (!FrameAnchors::Register(std::move(client))) {
		impl.reset();
		return false;
	}
	// Drawcall Limit Fix's colour passes read them by name, ordered after the projection (Integrations/IBL).
	if (!PublishedResources::Publish(*impl->env) || !PublishedResources::Publish(*impl->sky)) {
		logger::error("[ORG] IBL: the SH textures could not be published; IBL stays on D3D11");
		FrameAnchors::Unregister(kClientId);
		PublishedResources::Withdraw(kEnvId);
		PublishedResources::Withdraw(kSkyId);
		impl.reset();
		return false;
	}
	logger::info("[ORG] IBL: SH projection runs on the render graph (Prepass anchor)");
	return true;
}

void ORGIBL::Fail(const char* a_reason, bool a_unregister)
{
	logger::error("[ORG] IBL returns to D3D11: {}", a_reason);
	if (a_unregister)
		FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kEnvId);
	PublishedResources::Withdraw(kSkyId);
	auto failed = std::move(impl);
	if (failed && failed->onFailed)
		failed->onFailed();
}

bool ORGIBL::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

bool ORGIBL::Prepass(bool a_useEnvironment)
{
	if (!impl)
		return false;
	// The runtime stopped without a failed epoch of this anchor (another epoch faulted): leave as onFailed does.
	if (!RenderGraphRuntime::Get().IsActive()) {
		Fail("the render graph is no longer active", true);
		return false;
	}
	auto& state = *impl;
	// Imported once; imported again (and every anchor rebuilt) when the engine or Dynamic Cubemaps replace the texture.
	const Source sky{ FrameAnchors::EngineCubemap(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS), {} };
	Source env;
	const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
	if (a_useEnvironment && dynamicCubemaps.loaded && dynamicCubemaps.envTexture) {
		const auto* texture = dynamicCubemaps.envTexture;
		// On Dynamic Cubemaps' ORG path its texture is the graph twin's export: read the published twin, ordered after its writes.
		// Importing the export would make a second graph resource of the same image, which nothing orders after them.
		if (PublishedResources::IsExportOf(texture->srv.get(), ORGDynamicCubemaps::kEnvId))
			env.published = ORGDynamicCubemaps::kEnvId;
		else
			env.resource = FrameAnchors::D3D11Texture(ORGDynamicCubemaps::kEnvId, texture->resource.get(), texture->srv.get(), texture->uav.get());
	}
	if (sky != state.skySource || env != state.envSource || a_useEnvironment != state.useEnvironment) {
		if (env.published != state.envSource.published)
			logger::info("[ORG] IBL: the environment SH reads {}", env.published.empty() ? "Dynamic Cubemaps' D3D11 texture" : "Dynamic Cubemaps' published cubemap");
		state.skySource = sky;
		state.envSource = std::move(env);
		state.useEnvironment = a_useEnvironment;
		FrameAnchors::Refresh(kClientId);
	}
	return true;
}


ID3D11ShaderResourceView* ORGIBL::EnvSRV() const
{
	return impl ? impl->env->SRV() : nullptr;
}

ID3D11ShaderResourceView* ORGIBL::SkySRV() const
{
	return impl ? impl->sky->SRV() : nullptr;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: IBL always projects with D3D11.
struct ORGIBL::Impl
{};

void ORGIBL::Reserve() {}
bool ORGIBL::Setup(std::function<void()>) { return false; }
bool ORGIBL::IsActive() const { return false; }
bool ORGIBL::Prepass(bool) { return false; }
void ORGIBL::Fail(const char*, bool) {}
ID3D11ShaderResourceView* ORGIBL::EnvSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGIBL::SkySRV() const { return nullptr; }

#endif
