#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "TruePBR/ORGGlintsNoise.h"

#include <cstring>

namespace
{
	constexpr const char* kEnvSwitch = "CS_ORG_GLINTS";
}

bool ORGGlintsNoise::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA(kEnvSwitch, value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

ORGGlintsNoise& ORGGlintsNoise::Get()
{
	static ORGGlintsNoise s_noise;
	return s_noise;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>

#	include <atomic>
#	include <utility>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.true-pbr.glints";
	constexpr const char* kNoiseName = "TruePBR::GlintsNoise";
	constexpr const char* kShader = "Common/Glints/noisegen.cs.hlsl";
	// TruePBR::SetupGlintsTexture's size, and noisegen.cs.hlsl's group size.
	constexpr UINT kNoiseSize = 128;
	constexpr std::uint32_t kGroupSize = 32;
	// TruePBR binds the noise here for the engine's Lighting pixel shaders (Glints2023.hlsli's Glint2023NoiseMap).
	constexpr UINT kNoiseRegister = 20;

	// noisegen.cs.hlsl's push block (NOISEGEN_RESOURCES), in list order.
	struct NoiseGenBlock
	{
		ShaderBindings::Handle noise;
	};
}

struct ORGGlintsNoise::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	std::unique_ptr<GraphResources::Texture2D> noise;

	enum class Fill : std::uint8_t
	{
		Needed,     // the fill pass is in the graph; not enough epochs with it have been committed yet
		Submitted,  // enough epochs with it were committed: the next Update drops the pass
		Done,       // the graph was built again without it
	} fill = Fill::Needed;

	// Set when an epoch prepares the fill pass. The epoch run right after registering is built without it, so only an epoch
	// that prepared it counts; the fill is deterministic, so it runs in a few to be sure one reached the GPU.
	std::atomic<bool> prepared = false;
	std::uint32_t filledEpochs = 0;
	static constexpr std::uint32_t kFillEpochs = 3;
};

namespace
{
	using Impl = ORGGlintsNoise::Impl;

	struct FillBindings
	{
		org::DeclaredViewToken noise;
	};

	struct FillFrame
	{
		std::shared_ptr<const ComputeProgram> program;
	};

	/** @brief Fills the noise, one thread a texel. It is in the graph only until a few epochs prepared it. */
	class NoiseFillPass final : public org::TypedRenderGraphPass<NoiseFillPass, FillFrame, FillBindings>
	{
	public:
		// At the build, on the render thread: the texture as it is now (GraphResources' rule).
		explicit NoiseFillPass(const std::shared_ptr<Impl>& a_impl) :
			impl(a_impl), program(a_impl->program), noise(a_impl->noise->Graph()) {}

		FillBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.UnorderedAccess(noise).View() };
		}

		// The fill depends only on what the pass was made with.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get())); }

		FillFrame Prepare(const FillBindings&, const org::PassPrepareContext&) const
		{
			impl->prepared.store(true, std::memory_order_release);
			return { program };
		}

		static void Record(const FillBindings& a_bindings, const FillFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program)
				return;
			const NoiseGenBlock block{ a_recording.Index(a_bindings.noise) };
			constexpr std::uint32_t groups = (kNoiseSize + kGroupSize - 1) / kGroupSize;
			a_frame.program->Dispatch(a_recording, ShaderBindings::Data(block), groups, groups, 1);
		}

	private:
		std::shared_ptr<Impl> impl;
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<org::Resource> noise;
	};

	/** @brief Unbinds t20 before the export goes: its texture is freed once D3D11 is past the commands issued until then. */
	void UnbindNoise()
	{
		ID3D11ShaderResourceView* none = nullptr;
		globals::d3d::context->PSSetShaderResources(kNoiseRegister, 1, &none);
	}
}

void ORGGlintsNoise::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] TruePBR glints stay on D3D11: the EarlyPrepass anchor could not be reserved");
}

bool ORGGlintsNoise::Active() const
{
	return impl && !failed && RenderGraphRuntime::Get().IsActive();
}

bool ORGGlintsNoise::Filled() const
{
	return Active() && impl->fill != Impl::Fill::Needed;
}

ID3D11ShaderResourceView* ORGGlintsNoise::NoiseSRV() const
{
	return Filled() ? impl->noise->SRV() : nullptr;
}

void ORGGlintsNoise::Fail()
{
	failed = true;
	FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kNoiseName);
	if (impl)
		UnbindNoise();
	impl.reset();
}

bool ORGGlintsNoise::Setup()
{
	auto* host = RenderGraphRuntime::Get().Host();
	auto next = std::make_shared<Impl>();
	next->program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<NoiseGenBlock>() });
	if (!next->program) {
		logger::error("[ORG] TruePBR glints: {} could not be compiled; back to D3D11", kShader);
		Fail();
		return false;
	}

	// TruePBR::SetupGlintsTexture's texture.
	const D3D11_TEXTURE2D_DESC desc{ .Width = kNoiseSize,
		.Height = kNoiseSize,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS };
	// Engine draws read it at t20 until they are ported. It is filled once, so it lives for good.
	next->noise = std::make_unique<GraphResources::Texture2D>(desc, kNoiseName,
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!*next->noise || !next->noise->SRV()) {
		logger::error("[ORG] TruePBR glints: the {}x{} noise texture could not be created; back to D3D11", kNoiseSize, kNoiseSize);
		Fail();
		return false;
	}

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	// Made only until a few epochs prepared it (Update drops it).
	client.passes.push_back({ .name = std::string(kClientId) + ".noise-fill",
		.anchor = FrameAnchors::Anchor::EarlyPrepass,
		.order = 0,
		.kind = FrameAnchors::PassKind::Compute,
		.make = [next]() -> std::shared_ptr<org::RenderPass> {
			if (next->fill != Impl::Fill::Needed)
				return nullptr;
			return std::make_shared<NoiseFillPass>(next);
		} });
	client.registerResources = [next](org::RenderGraph& a_graph) { next->noise->Register(a_graph); };
	client.commit = [next](const FrameAnchors::Point&) {
		if (next->fill == Impl::Fill::Needed && next->prepared.exchange(false, std::memory_order_acq_rel) &&
			++next->filledEpochs >= Impl::kFillEpochs)
			next->fill = Impl::Fill::Submitted;
	};
	// Active only for the fill.
	client.active = [next] { return next->fill == Impl::Fill::Needed; };
	client.onFailed = [] {
		logger::error("[ORG] TruePBR glints: the render graph failed; back to D3D11");
		PublishedResources::Withdraw(kNoiseName);
		UnbindNoise();
		auto& self = Get();
		self.failed = true;
		self.impl.reset();
	};
	if (!FrameAnchors::Register(std::move(client))) {
		Fail();
		return false;
	}
	// Drawcall Limit Fix's colour passes read it by name, ordered after the fill (Integrations/TruePBR).
	if (!PublishedResources::Publish(*next->noise)) {
		logger::error("[ORG] TruePBR glints: the noise texture could not be published; back to D3D11");
		Fail();
		return false;
	}
	impl = std::move(next);
	logger::info("[ORG] TruePBR glints: the {}x{} noise is filled on the render graph at EarlyPrepass", kNoiseSize, kNoiseSize);
	return true;
}

bool ORGGlintsNoise::Update()
{
	if (!Requested() || failed)
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.IsActive() || !runtime.Host() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass))
		return false;
	if (!impl && !Setup())
		return false;
	// An epoch with the fill was submitted: the graph is built again without the pass.
	if (impl->fill == Impl::Fill::Submitted) {
		impl->fill = Impl::Fill::Done;
		FrameAnchors::Refresh(kClientId);
		logger::info("[ORG] TruePBR glints: the noise was filled; the fill pass is dropped");
	}
	return true;
}

ORGGlintsNoise::~ORGGlintsNoise() = default;

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the glints noise is always made on D3D11.
struct ORGGlintsNoise::Impl
{};

void ORGGlintsNoise::Reserve() {}
bool ORGGlintsNoise::Update() { return false; }
bool ORGGlintsNoise::Active() const { return false; }
bool ORGGlintsNoise::Filled() const { return false; }
ID3D11ShaderResourceView* ORGGlintsNoise::NoiseSRV() const { return nullptr; }
void ORGGlintsNoise::Fail() { failed = true; }
bool ORGGlintsNoise::Setup() { return false; }
ORGGlintsNoise::~ORGGlintsNoise() = default;

#endif
