#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "ORGIBL.h"
#include "RenderGraph/Engine/Native/Native.h"

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
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/UavClearPass.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/OwnedDescriptorBinding.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

namespace
{
	constexpr const char* kClientId = "cs.ibl";
	constexpr const char* kShader = "IBL/DiffuseIBLCS.hlsl";

	// R, G, B texels, each an sh2 in RGBA16F.
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

	// In DIFFUSE_IBL_RESOURCES order.
	struct DiffuseIBLBlock
	{
		ShaderBindings::Handle envTexture, iblTexture, linearSampler;
	};

	// A cubemap into a 3x1 SH texture, one 16x16 group.
	struct ProjectBindings
	{
		org::DeclaredViewToken source, target;
	};

	// The environment's projection or clear, then the sky's. Every commit opens both.
	constexpr std::uint32_t kEnvEntry = 0;
	constexpr std::uint32_t kSkyEntry = 1;

	struct ProjectFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		ShaderBindings::Handle sampler = 0;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t entry = 0;
	};

	/** @brief An import, or another feature's published texture by id. */
	struct Source
	{
		FrameAnchors::Texture resource;
		std::string_view published;

		explicit operator bool() const { return resource || !published.empty(); }
		bool operator==(const Source&) const = default;
	};

	/** @brief The client's key. A change registers the client again with the next revision. */
	struct ClientKey
	{
		bool sky = false;           // the engine made its reflection cubemap
		bool environment = false;   // the DALC mode reads the environment SH (modes 0 and 1)
		bool envPublished = false;  // Dynamic Cubemaps' cubemap is a published graph texture

		bool operator==(const ClientKey&) const = default;
	};

	class ProjectPass final : public org::TypedRenderGraphPass<ProjectPass, ProjectFrame, ProjectBindings>
	{
	public:
		ProjectPass(std::shared_ptr<const ComputeProgram> a_program, ShaderBindings::Handle a_sampler, Source a_source, std::shared_ptr<org::Resource> a_target,
			std::shared_ptr<const RenderGraph::RunGate> a_gate, std::uint32_t a_entry) :
			program(std::move(a_program)), sampler(a_sampler), source(std::move(a_source)), target(std::move(a_target)), gate(std::move(a_gate)), entry(a_entry) {}

		ProjectBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// A cube view of mip 0, as the D3D11 path samples it.
			if (source.resource)
				return { a_builder.ShaderResource(source.resource).View(), a_builder.UnorderedAccess(target).View() };
			const org::SrvView cube{ .variant = static_cast<std::uint32_t>(org::SRVViewType::TextureCube) };
			return { a_builder.ShaderResource(org::ResourceIdentifier(source.published), cube).View(), a_builder.UnorderedAccess(target).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(source.resource.Key()));
			a_out.push_back(std::hash<std::string_view>{}(source.published));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(target.get()));
		}

		ProjectFrame Prepare(const ProjectBindings&, const org::PassPrepareContext&) const { return { program, sampler, gate, entry }; }

		static void Record(const ProjectBindings& a_bindings, const ProjectFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.gate)
				return;
			const DiffuseIBLBlock block{ a_recording.Index(a_bindings.source), a_recording.Index(a_bindings.target), a_frame.sampler };
			a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), a_frame.entry);
		}

	private:
		std::shared_ptr<const ComputeProgram> program;
		ShaderBindings::Handle sampler;
		Source source;
		std::shared_ptr<org::Resource> target;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t entry = 0;
	};
}

struct ORGIBL::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	org::OwnedDescriptorBinding sampler;
	std::unique_ptr<GraphResources::Texture2D> env;
	std::unique_ptr<GraphResources::Texture2D> sky;
	std::shared_ptr<RenderGraph::RunGate> gate;  // opened by every commit
	RenderGraph::Frame::DeclaredClient client;
	std::function<void()> onFailed;

	/** @brief The registration for a_key. The passes take their sources from it when they are made. */
	bool MakeClient(const ClientKey& a_key, FrameAnchors::ClientDesc& a_client) const
	{
		// The D3D11 path projects a null view, which samples zero, when a source is missing.
		const Source skySource{ a_key.sky ? FrameAnchors::EngineCubemap(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS) : FrameAnchors::Texture{}, {} };
		// Only the published twin is read, since an import is not ordered after its producer. Without it the SH is zero.
		const Source envSource{ {}, a_key.envPublished ? ORGDynamicCubemaps::kEnvId : std::string_view{} };
		if (a_key.sky && !skySource.resource)
			logger::warn("[ORG] IBL: the engine's reflection cubemap cannot be imported; the sky SH is zero");
		auto makePass = [program = program, sampler = sampler.Index(), gate = gate](bool a_env, Source a_source,
							GraphResources::Texture2D* a_target) -> std::shared_ptr<org::RenderPass> {
			const std::uint32_t entry = a_env ? kEnvEntry : kSkyEntry;
			if (!a_source)
				return GraphPasses::MakeUavClear(a_target->Graph(), {}, { gate, entry });
			return std::make_shared<ProjectPass>(program, sampler, a_source, a_target->Graph(), gate, entry);
		};
		a_client.id = kClientId;
		// A projection dispatches through its entry. A clear records at execution and skips a closed run.
		// The D3D11 path also skips the environment unless the DALC mode reads it, and the last values stay.
		if (a_key.environment)
			a_client.passes.push_back({ .name = "cs.ibl.env-sh",
				.anchor = FrameAnchors::Anchor::Prepass,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [makePass, envSource, target = env.get()] { return makePass(true, envSource, target); },
				.atExecution = true });
		a_client.passes.push_back({ .name = "cs.ibl.sky-sh",
			.anchor = FrameAnchors::Anchor::Prepass,
			.order = 1,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [makePass, skySource, target = sky.get()] { return makePass(false, skySource, target); },
			.atExecution = true });
		a_client.gates = { gate };
		a_client.commit = [gate = gate](const FrameAnchors::Point&) {
			gate->OpenDispatch(kEnvEntry, 1);
			gate->OpenDispatch(kSkyEntry, 1);
		};
		a_client.registerResources = [envTarget = env.get(), skyTarget = sky.get()](org::RenderGraph& a_graph) {
			envTarget->Register(a_graph);
			skyTarget->Register(a_graph);
		};
		// IBL::Prepass computes nothing while the scene disables IBL. The passes stay in the graph and their gate stays shut.
		a_client.active = [] { return !globals::features::ibl.IsDisabledForCurrentScene(); };
		logger::info("[ORG] IBL: a client for the next revision (sky SH from {}, environment SH {})", skySource ? "the reflection cubemap" : "nothing",
			!a_key.environment ? "not projected" : (envSource ? "from Dynamic Cubemaps' published cubemap" : "zero (Dynamic Cubemaps' cubemap is not published)"));
		return true;
	}
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
	// IBL::GetDiffuseIBLCS's defines. ComputeProgram adds the common ones.
	if (globals::features::dynamicCubemaps.loaded)
		desc.defines.push_back({ L"DYNAMIC_CUBEMAPS", L"" });
	state->program = ComputeProgram::Load(host->Device(), desc);
	// Deferred::linearSampler.
	state->sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
	// Read in the next frame, and by D3D11 until its readers are ported.
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV };
	state->env = std::make_unique<GraphResources::Texture2D>(ShTextureDesc(), std::string(kEnvId), options);
	state->sky = std::make_unique<GraphResources::Texture2D>(ShTextureDesc(), std::string(kSkyId), options);
	state->gate = RenderGraph::RunGate::Create(kClientId, 2);
	if (!state->program || !state->sampler || !*state->env || !*state->sky || !state->env->SRV() || !state->sky->SRV() || !state->gate) {
		logger::error("[ORG] IBL: its program, sampler, SH textures or run gate could not be made; IBL stays on D3D11");
		return false;
	}
	state->onFailed = std::move(a_onFailed);
	// DCLF's colour passes read them by name, ordered after the projection. Published once, for the session.
	if (!PublishedResources::Publish(*state->env) || !PublishedResources::Publish(*state->sky)) {
		logger::error("[ORG] IBL: the SH textures could not be published; IBL stays on D3D11");
		PublishedResources::Withdraw(kEnvId);
		PublishedResources::Withdraw(kSkyId);
		return false;
	}
	// The client follows the reflection cubemap and the DALC mode from the first revision. Dynamic Cubemaps publishes its
	// cubemap at its own setup, before the first revision starts, and never again.
	using namespace RenderGraph::Frame;
	auto* made = state.get();
	state->client = DeclareClient<ClientKey>(
		kClientId, DependsOn(Vars::ReflectionCubemap(), globals::features::ibl.settings.DALCMode),
		[](const Inputs& a_in) -> std::optional<ClientKey> {
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			return ClientKey{ .sky = a_in.Get(Vars::ReflectionCubemap()).size != 0,
				.environment = a_in.Get(globals::features::ibl.settings.DALCMode) < 2,
				.envPublished = dynamicCubemaps.loaded && PublishedResources::IsPublished(ORGDynamicCubemaps::kEnvId) };
		},
		[made](const ClientKey& a_key, FrameAnchors::ClientDesc& a_client) { return made->MakeClient(a_key, a_client); });
	if (!state->client) {
		logger::error("[ORG] IBL: its client could not be declared; IBL stays on D3D11");
		PublishedResources::Withdraw(kEnvId);
		PublishedResources::Withdraw(kSkyId);
		return false;
	}
	impl = std::move(state);
	logger::info("[ORG] IBL: SH projection runs on the render graph (Prepass anchor); its client follows the frame revisions");
	return true;
}

void ORGIBL::Fail(const char* a_reason)
{
	// The runtime is down, so its registrations and published ids run nothing more. The D3D11 path makes its own textures.
	logger::error("[ORG] IBL returns to D3D11: {}", a_reason);
	// The declaration lives until process exit and its functions hold the state, so the state stays too.
	static std::unique_ptr<Impl> s_failed;
	s_failed = std::move(impl);
	if (s_failed && s_failed->onFailed)
		s_failed->onFailed();
}

bool ORGIBL::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

bool ORGIBL::Prepass()
{
	if (!impl)
		return false;
	if (!RenderGraphRuntime::Get().IsActive()) {
		Fail("the render graph is no longer active");
		return false;
	}
	// The adopted revision's client projects the SH at the Prepass anchor.
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

// Without OpenRenderGraph IBL always projects with D3D11.
struct ORGIBL::Impl
{};

void ORGIBL::Reserve() {}
bool ORGIBL::Setup(std::function<void()>) { return false; }
bool ORGIBL::IsActive() const { return false; }
bool ORGIBL::Prepass() { return false; }
void ORGIBL::Fail(const char*) {}
ID3D11ShaderResourceView* ORGIBL::EnvSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGIBL::SkySRV() const { return nullptr; }

#endif
