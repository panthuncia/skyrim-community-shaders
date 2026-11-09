#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "ORGIBL.h"

#include "Features/DynamicCubemaps.h"
#include "Features/IBL.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <DirectXPackedVector.h>

#include <array>
#include <cstring>

namespace
{
	bool EnvironmentFlag(const char* a_name)
	{
		char value[8]{};
		return GetEnvironmentVariableA(a_name, value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}

	// The SH textures: three texels (R, G, B), each an sh2 (float4) in RGBA16F.
	constexpr UINT kShTexels = 3;

	D3D11_TEXTURE2D_DESC ShTextureDesc(bool a_staging)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kShTexels;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = a_staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
		desc.BindFlags = a_staging ? 0u : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		desc.CPUAccessFlags = a_staging ? D3D11_CPU_ACCESS_READ : 0u;
		return desc;
	}
}

// The debug readback: plain D3D11 on both paths.
struct ORGIBL::Readback
{
	static constexpr std::uint32_t kInterval = 250;

	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> staging;  // env, sky
	std::uint32_t copiedFrame = 0;
	bool pending = false;
	bool path = false;  // the copy came from the render graph's textures

	bool Create()
	{
		const auto desc = ShTextureDesc(true);
		for (auto& texture : staging) {
			if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, texture.put())))
				return false;
			Util::SetResourceName(texture.get(), "IBL::ReadbackStaging");
		}
		return true;
	}

	/** @brief The texture's three texels as text, or why they could not be read; false while the copy is not done. */
	bool Read(ID3D11Texture2D* a_texture, std::string& a_out)
	{
		auto* context = globals::d3d::context;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT hr = context->Map(a_texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
			return false;
		if (FAILED(hr)) {
			a_out = fmt::format("map failed 0x{:08X}", static_cast<std::uint32_t>(hr));
			return true;
		}
		const auto* halves = static_cast<const DirectX::PackedVector::HALF*>(mapped.pData);
		for (UINT i = 0; i < kShTexels * 4; ++i)
			a_out += fmt::format("{}{:.6f}", i == 0 ? "" : (i % 4 == 0 ? " | " : " "), DirectX::PackedVector::XMConvertHalfToFloat(halves[i]));
		context->Unmap(a_texture, 0);
		return true;
	}
};

ORGIBL& ORGIBL::Get()
{
	static ORGIBL s_instance;
	return s_instance;
}

ORGIBL::~ORGIBL() = default;

bool ORGIBL::Requested()
{
	static const bool requested = EnvironmentFlag("CS_ORG_IBL");
	return requested;
}

void ORGIBL::DebugReadback(ID3D11Texture2D* a_env, ID3D11Texture2D* a_sky)
{
	static const bool requested = EnvironmentFlag("CS_ORG_IBL_READBACK");
	if (!requested)
		return;
	if (!readback) {
		readback = std::make_unique<Readback>();
		if (!readback->Create()) {
			logger::error("[IBL] SH readback: the staging textures could not be created");
			return;
		}
	}
	auto& state = *readback;
	if (!state.staging[0])
		return;
	if (state.pending) {
		std::string env, sky;
		if (!state.Read(state.staging[0].get(), env) || !state.Read(state.staging[1].get(), sky))
			return;
		state.pending = false;
		logger::info("[IBL] SH readback ({}) frame {}: env [{}] sky [{}]", state.path ? "ORG" : "D3D11", state.copiedFrame, env, sky);
		return;
	}
	const std::uint32_t frame = globals::state->frameCount;
	if (frame % Readback::kInterval != 0)
		return;
	state.path = IsActive();
	auto* env = state.path ? Exported(true) : a_env;
	auto* sky = state.path ? Exported(false) : a_sky;
	if (!env || !sky)
		return;
	// The last frame's result: the D3D11 path has not dispatched yet, the graph's Prepass anchor has not run yet.
	globals::d3d::context->CopyResource(state.staging[0].get(), env);
	globals::d3d::context->CopyResource(state.staging[1].get(), sky);
	state.copiedFrame = frame;
	state.pending = true;
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
	// The SH textures' names, which they are published under: Drawcall Limit Fix's t76 and t77.
	constexpr const char* kEnvName = "IBL::EnvIBL";
	constexpr const char* kSkyName = "IBL::SkyIBL";

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

	class ProjectPass final : public org::TypedRenderGraphPass<ProjectPass, ProjectFrame, ProjectBindings>
	{
	public:
		ProjectPass(std::shared_ptr<const ComputeProgram> a_program, ShaderBindings::Handle a_sampler, std::shared_ptr<org::Resource> a_source,
			std::shared_ptr<org::Resource> a_target) :
			program(std::move(a_program)), sampler(a_sampler), source(std::move(a_source)), target(std::move(a_target)) {}

		ProjectBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// The source's default view: a cube view of mip 0, as the D3D11 path samples it.
			return { a_builder.ShaderResource(source).View(), a_builder.UnorderedAccess(target).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(source.get()));
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
		std::shared_ptr<org::Resource> source;
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
	std::shared_ptr<org::ExternalTextureResource> envSource;
	std::shared_ptr<org::ExternalTextureResource> skySource;
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
	// The D3D11 path's defines (IBL::GetDiffuseIBLCS).
	if (globals::features::dynamicCubemaps.loaded)
		desc.defines.push_back({ L"DYNAMIC_CUBEMAPS", L"" });
	state->program = ComputeProgram::Load(host->Device(), desc);
	// Linear, clamped, every mip: Deferred::linearSampler.
	state->sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
	// Read in the next frame (the reflection prepass binds the last frame's SH), and by D3D11 until its readers are ported.
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV };
	state->env = std::make_unique<GraphResources::Texture2D>(ShTextureDesc(false), kEnvName, options);
	state->sky = std::make_unique<GraphResources::Texture2D>(ShTextureDesc(false), kSkyName, options);
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
	client.onFailed = [self] {
		logger::error("[ORG] IBL: the render graph failed; IBL returns to D3D11");
		PublishedResources::Withdraw(kEnvName);
		PublishedResources::Withdraw(kSkyName);
		auto failed = std::move(self->impl);
		if (failed && failed->onFailed)
			failed->onFailed();
	};
	impl = std::move(state);
	if (!FrameAnchors::Register(std::move(client))) {
		impl.reset();
		return false;
	}
	// Drawcall Limit Fix's colour passes read them by name, ordered after the projection (Integrations/IBL).
	if (!PublishedResources::Publish(*impl->env) || !PublishedResources::Publish(*impl->sky)) {
		logger::error("[ORG] IBL: the SH textures could not be published; IBL stays on D3D11");
		FrameAnchors::Unregister(kClientId);
		PublishedResources::Withdraw(kEnvName);
		PublishedResources::Withdraw(kSkyName);
		impl.reset();
		return false;
	}
	logger::info("[ORG] IBL: SH projection runs on the render graph (Prepass anchor)");
	return true;
}

bool ORGIBL::IsActive() const
{
	return impl != nullptr;
}

bool ORGIBL::Prepass(bool a_useEnvironment)
{
	if (!impl)
		return false;
	auto& state = *impl;
	// Imported once; imported again (and every anchor rebuilt) when the engine or Dynamic Cubemaps replace the texture.
	auto sky = FrameAnchors::EngineCubemap(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS);
	std::shared_ptr<org::ExternalTextureResource> env;
	const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
	if (a_useEnvironment && dynamicCubemaps.loaded && dynamicCubemaps.envTexture) {
		const auto* texture = dynamicCubemaps.envTexture;
		env = FrameAnchors::D3D11Texture("DynamicCubemaps::EnvTexture", texture->resource.get(), texture->srv.get(), texture->uav.get());
	}
	if (sky != state.skySource || env != state.envSource || a_useEnvironment != state.useEnvironment) {
		state.skySource = std::move(sky);
		state.envSource = std::move(env);
		state.useEnvironment = a_useEnvironment;
		FrameAnchors::Refresh(kClientId);
	}
	return true;
}

ID3D11Texture2D* ORGIBL::Exported(bool a_env) const
{
	return impl ? (a_env ? impl->env : impl->sky)->D3D11() : nullptr;
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
ID3D11Texture2D* ORGIBL::Exported(bool) const { return nullptr; }
ID3D11ShaderResourceView* ORGIBL::EnvSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGIBL::SkySRV() const { return nullptr; }

#endif
