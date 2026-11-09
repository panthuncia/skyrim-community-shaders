#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGScreenSpaceShadows.h"

#	include "Features/ScreenSpaceShadows.h"
#	include "Features/TerrainBlending.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>

#	include <cstddef>
#	include <cstring>
#	include <format>

namespace
{
	constexpr const char* kClientId = "cs.sss";
	constexpr const char* kShader = "ScreenSpaceShadows/RaymarchCS.hlsl";
	constexpr const char* kTextureName = "SSS::ShadowTexture";
	constexpr const char* kBlendedDepthName = "terrain-blending.blended-depth";
	constexpr std::uint32_t kMaxDispatches = ScreenSpaceShadows::Dispatches::kMax;

	// RaymarchCS's push block under ORG, in list order: three handles, then PerFrame's latch region.
	struct RaymarchBlock
	{
		ShaderBindings::Handle depthTexture, outputTexture, pointBorderSampler;
		ShaderBindings::Latch perFrame;
	};

	// One execution's values, in its slot of the latch block: each dispatch's constants (RaymarchConstants, read at their
	// offset by the shader) and its thread-group counts, which the dispatch reads indirectly.
	struct RaymarchLatchData
	{
		ScreenSpaceShadows::RaymarchCB constants[kMaxDispatches];
		std::uint32_t groups[kMaxDispatches][4];  // x, y, z, padding
	};
	static_assert(sizeof(ScreenSpaceShadows::RaymarchCB) % 16 == 0);
	static_assert(offsetof(RaymarchLatchData, groups) % 16 == 0);

	/** @brief The raymarch program for one sample count, with the dispatch signature made against its layout. */
	struct Program
	{
		std::shared_ptr<const ComputeProgram> compute;
		rhi::CommandSignaturePtr signature;
		std::uint32_t sampleCount = 0;
	};

	/** @brief What the passes are made from; changed on the render thread only, between builds (then FrameAnchors::Refresh). */
	struct ShadowState
	{
		std::shared_ptr<const Program> program;
		std::unique_ptr<GraphResources::Texture2D> output;
		std::shared_ptr<org::LatchBlock> latch;
		org::OwnedDescriptorBinding sampler;
		std::shared_ptr<org::ExternalTextureResource> depth;
	};

	std::shared_ptr<const Program> LoadProgram(rhi::Device a_device, std::uint32_t a_sampleCount)
	{
		std::vector<std::pair<std::wstring, std::wstring>> defines{ { L"SAMPLE_COUNT", std::to_wstring(a_sampleCount) } };
		// As the D3D11 build: the depth texture's HLSL type follows Terrain Blending.
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ L"TERRAIN_BLENDING", L"" });
		auto program = std::make_shared<Program>();
		program->sampleCount = a_sampleCount;
		program->compute = ComputeProgram::Load(a_device, { .source = kShader, .constantWords = ShaderBindings::Words<RaymarchBlock>(), .defines = defines });
		if (!program->compute)
			return nullptr;
		rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
		if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 },
				program->compute->layout->GetHandle(), program->signature) != rhi::Result::Ok)
			return nullptr;
		return program;
	}

	/** @brief The depth the D3D11 path reads at Prepass (Util::GetCurrentSceneDepthSRV(false)), as a graph texture. */
	std::shared_ptr<org::ExternalTextureResource> CurrentDepth()
	{
		auto& terrainBlending = globals::features::terrainBlending;
		if (terrainBlending.loaded && terrainBlending.settings.Enabled && terrainBlending.blendedDepthTexture)
			return FrameAnchors::D3D11Texture(kBlendedDepthName, terrainBlending.blendedDepthTexture->resource.get(),
				terrainBlending.blendedDepthTexture->srv.get(), nullptr);
		return FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
	}

	const char* DepthLabel()
	{
		auto& terrainBlending = globals::features::terrainBlending;
		return terrainBlending.loaded && terrainBlending.settings.Enabled ? "Terrain Blending" : "the z-prepass copy";
	}

	// The clear to white (no shadow), as the D3D11 path clears before its dispatches.
	struct ClearBindings
	{
		org::ResourceBindingToken resource;
		org::DeclaredViewToken shaderVisible, cpuVisible;
	};

	struct ClearFrame
	{};

	class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
	{
	public:
		explicit ClearPass(std::shared_ptr<org::Resource> a_output) :
			output(std::move(a_output)) {}

		ClearBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto use = a_builder.UnorderedAccessClear(output);
			return { use.resources.front(), use.views[0], use.views[1] };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(output.get())); }

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return {}; }

		static void Record(const ClearBindings& a_bindings, const ClearFrame&, org::PassRecordContext& a_recording)
		{
			rhi::UavClearInfo info{};
			info.shaderVisible = a_recording.View(a_bindings.shaderVisible);
			info.cpuVisible = a_recording.View(a_bindings.cpuVisible);
			info.resource = a_recording.Resolve(a_bindings.resource);
			const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			a_recording.Commands().ClearUavFloat(info, rhi::UavClearFloat(white));
		}

	private:
		std::shared_ptr<org::Resource> output;
	};

	// Every dispatch of the list's capacity, each reading its constants and group counts from the execution's latch region.
	struct RaymarchBindings
	{
		org::DeclaredViewToken depth, output;
	};

	struct RaymarchFrame
	{
		std::shared_ptr<const Program> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t sampler = 0;
	};

	class RaymarchPass final : public org::TypedRenderGraphPass<RaymarchPass, RaymarchFrame, RaymarchBindings>
	{
	public:
		// At the build, on the render thread: the state as it is now.
		explicit RaymarchPass(const ShadowState& a_state) :
			program(a_state.program), latch(a_state.latch), sampler(a_state.sampler.Index()), depth(a_state.depth), output(a_state.output->Graph()) {}

		RaymarchBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.ShaderResource(depth).View(), a_builder.UnorderedAccess(output).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(depth.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(output.get()));
		}

		RaymarchFrame Prepare(const RaymarchBindings&, const org::PassPrepareContext&) const { return { program, latch, sampler }; }

		static void Record(const RaymarchBindings& a_bindings, const RaymarchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			const auto& compute = *a_frame.program->compute;
			auto& commands = a_recording.Commands();
			commands.BindLayout(compute.layout->GetHandle());
			commands.BindPipeline(compute.Pipeline());
			const std::uint32_t slot = a_recording.FrameSlot();
			const std::uint64_t region = a_frame.latch->Offset(slot);
			const auto latchBuffer = a_frame.latch->Resource()->GetAPIResource().GetHandle();
			RaymarchBlock block{ a_recording.Index(a_bindings.depth), a_recording.Index(a_bindings.output), a_frame.sampler, {} };
			// Bend's dispatches write separate pixels, so they need no barrier between them (as on D3D11).
			for (std::uint32_t i = 0; i < kMaxDispatches; ++i) {
				block.perFrame = ShaderBindings::LatchOf(*a_frame.latch, slot,
					static_cast<std::uint32_t>(offsetof(RaymarchLatchData, constants) + i * sizeof(ScreenSpaceShadows::RaymarchCB)));
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, ShaderBindings::Words<RaymarchBlock>(), reinterpret_cast<const std::uint32_t*>(&block));
				commands.ExecuteIndirect(a_frame.program->signature->GetHandle(), latchBuffer,
					region + offsetof(RaymarchLatchData, groups) + i * sizeof(RaymarchLatchData::groups[0]), {}, 0, 1);
			}
		}

	private:
		std::shared_ptr<const Program> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t sampler;
		std::shared_ptr<org::Resource> depth;
		std::shared_ptr<org::Resource> output;
	};
}

struct ORGScreenSpaceShadows::Impl
{
	std::shared_ptr<ShadowState> state;
};

bool ORGScreenSpaceShadows::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_SCREEN_SPACE_SHADOWS", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

ORGScreenSpaceShadows& ORGScreenSpaceShadows::Get()
{
	static ORGScreenSpaceShadows s_shadows;
	return s_shadows;
}

ORGScreenSpaceShadows::~ORGScreenSpaceShadows() = default;

void ORGScreenSpaceShadows::Setup(const D3D11_TEXTURE2D_DESC& a_desc)
{
	impl.reset();
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass)) {
		logger::warn("[ORG] Screen Space Shadows stays on D3D11: the render graph or its Prepass anchor is not available");
		return;
	}
	try {
		auto state = std::make_shared<ShadowState>();
		state->program = LoadProgram(host->Device(), globals::features::screenSpaceShadows.GetScaledSampleCount());
		// The D3D11 sampler's: point, border white (beyond the screen is far depth, no shadow).
		state->sampler = ShaderBindings::CreateSampler({ .minFilter = rhi::Filter::Nearest,
			.magFilter = rhi::Filter::Nearest,
			.mipFilter = rhi::MipFilter::Nearest,
			.addressU = rhi::AddressMode::Border,
			.addressV = rhi::AddressMode::Border,
			.addressW = rhi::AddressMode::Border,
			.borderPreset = rhi::BorderPreset::OpaqueWhite });
		// Written in full every frame, but the engine's draws read it through D3D11: exported, so never aliased.
		state->output = std::make_unique<GraphResources::Texture2D>(a_desc, kTextureName,
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient, .d3d11 = GraphResources::Export::SRV });
		state->latch = std::make_shared<org::LatchBlock>("cs.sss.latch", static_cast<std::uint32_t>(sizeof(RaymarchLatchData)), host->FrameSlots());
		state->depth = CurrentDepth();
		if (!state->program || !state->sampler || !*state->output || !state->output->SRV() || !state->depth) {
			logger::warn("[ORG] Screen Space Shadows stays on D3D11: its program, sampler, texture or depth input could not be made");
			return;
		}

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.passes.push_back({ .name = "cs.sss.clear",
			.anchor = FrameAnchors::Anchor::Prepass,
			.order = 100,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [state]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ClearPass>(state->output->Graph()); } });
		client.passes.push_back({ .name = "cs.sss.raymarch",
			.anchor = FrameAnchors::Anchor::Prepass,
			.order = 101,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [state]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<RaymarchPass>(*state); } });
		client.registerResources = [state](org::RenderGraph& a_graph) { state->output->Register(a_graph); };
		client.commit = [state](const FrameAnchors::Point&) {
			auto& feature = globals::features::screenSpaceShadows;
			RaymarchLatchData data{};
			if (feature.ShouldDraw()) {
				const auto dispatches = feature.BuildDispatches();
				for (std::uint32_t i = 0; i < dispatches.count; ++i) {
					data.constants[i] = dispatches.constants[i];
					std::memcpy(data.groups[i], dispatches.groups[i], sizeof(dispatches.groups[i]));
				}
			}
			state->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, data);
		};
		client.onFailed = [] {
			logger::error("[ORG] Screen Space Shadows returns to D3D11: the render graph failed");
			Get().impl.reset();
		};
		if (!FrameAnchors::Register(std::move(client)))
			return;
		impl = std::make_unique<Impl>();
		impl->state = std::move(state);
		logger::info("[ORG] Screen Space Shadows runs on the render graph ({}x{}, {} samples, depth from {})", a_desc.Width, a_desc.Height,
			impl->state->program->sampleCount, DepthLabel());
	} catch (const std::exception& e) {
		logger::error("[ORG] Screen Space Shadows setup failed, staying on D3D11: {}", e.what());
		FrameAnchors::Unregister(kClientId);
		impl.reset();
	}
}

ID3D11ShaderResourceView* ORGScreenSpaceShadows::Prepass()
{
	if (!impl || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& state = *impl->state;
	bool changed = false;
	// The sample count follows the render size (as GetComputeRaymarch's recompile does).
	if (const auto sampleCount = globals::features::screenSpaceShadows.GetScaledSampleCount(); sampleCount != state.program->sampleCount) {
		auto program = LoadProgram(RenderGraphRuntime::Get().Host()->Device(), sampleCount);
		if (!program) {
			logger::error("[ORG] Screen Space Shadows returns to D3D11: the {}-sample program could not be made", sampleCount);
			FrameAnchors::Unregister(kClientId);
			impl.reset();
			return nullptr;
		}
		state.program = std::move(program);
		changed = true;
	}
	// Terrain Blending on or off, or a new engine depth texture.
	if (auto depth = CurrentDepth(); depth != state.depth) {
		if (!depth) {
			FrameAnchors::Unregister(kClientId);
			impl.reset();
			return nullptr;
		}
		state.depth = std::move(depth);
		logger::info("[ORG] Screen Space Shadows: depth now from {}", DepthLabel());
		changed = true;
	}
	if (changed)
		FrameAnchors::Refresh(kClientId);
	return state.output->SRV();
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Screen Space Shadows always draws with D3D11.
#	include "ORGScreenSpaceShadows.h"

struct ORGScreenSpaceShadows::Impl
{};

bool ORGScreenSpaceShadows::Requested() { return false; }

ORGScreenSpaceShadows& ORGScreenSpaceShadows::Get()
{
	static ORGScreenSpaceShadows s_shadows;
	return s_shadows;
}

ORGScreenSpaceShadows::~ORGScreenSpaceShadows() = default;
void ORGScreenSpaceShadows::Setup(const D3D11_TEXTURE2D_DESC&) {}
ID3D11ShaderResourceView* ORGScreenSpaceShadows::Prepass() { return nullptr; }

#endif
