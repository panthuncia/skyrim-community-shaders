#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/PhysicalSky/ORGPhysicalSky.h"
#include "RenderGraph/Engine/Native/Native.h"

bool ORGPhysicalSky::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

ORGPhysicalSky& ORGPhysicalSky::Get()
{
	static ORGPhysicalSky s_physicalSky;
	return s_physicalSky;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Buffer.h"
#	include "Features/PhysicalSky.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>

#	include <algorithm>
#	include <optional>
#	include <span>
#	include <string>
#	include <utility>

namespace
{
	constexpr const char* kLutsClientId = "cs.physical-sky.luts";
	constexpr const char* kLutShader = "PhysicalSky/LutGen.cs.hlsl";
	constexpr const char* kDiskPhaseShader = "PhysicalSky/DiskPhaseLut.cs.hlsl";

	using Lut = ORGPhysicalSky::Lut;
	constexpr auto kLutCount = static_cast<std::size_t>(Lut::Count);
	// Gate entries: the four LutGen dispatches in LUTGEN order, then the disk phase LUT.
	constexpr std::uint32_t kLutGenEntries = 4;
	constexpr std::uint32_t kDiskPhaseEntry = 4;
	// The disk phase LUT's 12 floats (DiskPhaseFrame), at the latch's start.
	constexpr std::uint32_t kDiskPhaseBytes = 12 * sizeof(float);
	// LutGen's push block: the shared handles, three SRVs, the output and three samplers.
	constexpr std::uint32_t kLutGenWords = 4 + 3 + 1 + 3;
	constexpr std::uint32_t kDiskPhaseWords = 1 + 2;

	constexpr std::string_view IdOf(Lut a_lut)
	{
		switch (a_lut) {
		case Lut::Transmittance:
			return ORGPhysicalSky::kTransmittanceLutId;
		case Lut::Multiscatter:
			return ORGPhysicalSky::kMultiscatterLutId;
		case Lut::SkyView:
			return ORGPhysicalSky::kSkyViewLutId;
		case Lut::AerialPerspective:
			return ORGPhysicalSky::kApLutId;
		case Lut::DiskPhase:
			return ORGPhysicalSky::kDiskPhaseLutId;
		default:
			return {};
		}
	}

	/** @brief The 2D twins by Lut, null for the 3D AP LUT or a D3D11 texture. */
	std::array<std::unique_ptr<GraphResources::Texture2D>, kLutCount>& Twins2D()
	{
		static std::array<std::unique_ptr<GraphResources::Texture2D>, kLutCount> s_twins;
		return s_twins;
	}

	std::unique_ptr<GraphResources::Texture3D>& ApTwin()
	{
		static std::unique_ptr<GraphResources::Texture3D> s_twin;
		return s_twin;
	}

	const GraphResources::Resource* TwinOf(Lut a_lut)
	{
		if (a_lut == Lut::AerialPerspective)
			return ApTwin().get();
		return Twins2D()[static_cast<std::size_t>(a_lut)].get();
	}

	// D3D11 writes them only on frames the graph does not, read them always, and the next frame's reflection faces read them.
	GraphResources::Options LutOptions(Lut a_lut)
	{
		return { .lifetime = a_lut == Lut::DiskPhase ? GraphResources::Lifetime::Persistent : GraphResources::Lifetime::History,
			.d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV };
	}

	/** @brief LutGen's group counts, as PhysicalSky::GenerateLuts dispatches them. */
	std::array<std::uint32_t, 3> LutGroups(Lut a_lut)
	{
		switch (a_lut) {
		case Lut::Transmittance:
			return { (PhysicalSky::kTrLutW + 7u) >> 3, (PhysicalSky::kTrLutH + 7u) >> 3, 1 };
		case Lut::Multiscatter:
			// Each group integrates sixteen directions for four LUT texels.
			return { (PhysicalSky::kMsLutW + 1u) >> 1, (PhysicalSky::kMsLutH + 1u) >> 1, 1 };
		case Lut::SkyView:
			return { (PhysicalSky::kSvLutW + 7u) >> 3, (PhysicalSky::kSvLutH + 7u) >> 3, 1 };
		case Lut::AerialPerspective:
			// Two rays per group; all depth intervals are evaluated in parallel.
			return { (PhysicalSky::kApLutW + 1u) >> 1, PhysicalSky::kApLutH, 1 };
		default:
			return { 1, 1, 1 };
		}
	}

	// A template, so a 2D twin takes Publish's 2D overload.
	template <class Twin>
	bool PublishTwin(const Twin& a_twin, Lut a_lut)
	{
		if (PublishedResources::Publish(IdOf(a_lut), a_twin))
			return true;
		logger::error("[ORG] Physical Sky: {} could not be published; it is a D3D11 texture", IdOf(a_lut));
		return false;
	}

	/** @brief The D3D11 sampler's description, for the graph's twin of it. */
	org::OwnedDescriptorBinding SamplerOf(ID3D11SamplerState* a_sampler)
	{
		if (!a_sampler)
			return {};
		D3D11_SAMPLER_DESC desc{};
		a_sampler->GetDesc(&desc);
		return ShaderBindings::CreateSamplerFromD3D11(desc);
	}
}

struct ORGPhysicalSky::Impl
{
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::shared_ptr<org::LatchBlock> latch;
	std::array<std::shared_ptr<const ComputeProgram>, kLutGenEntries> lutPrograms;
	std::shared_ptr<const ComputeProgram> diskPhaseProgram;
	// SampTr, SampSv and SampNoise, kept as long as the programs.
	std::array<org::OwnedDescriptorBinding, 3> samplers;

	// Render thread. Set at the feature's EarlyPrepass, taken by the next commit.
	bool luts = false;
	std::optional<std::array<float, 12>> diskPhase;

	void Commit()
	{
		if (std::exchange(luts, false))
			for (std::uint32_t i = 0; i < kLutGenEntries; ++i) {
				const auto groups = LutGroups(static_cast<Lut>(i));
				gate->OpenDispatch(i, groups[0], groups[1], groups[2]);
			}
		if (diskPhase) {
			latch->Write(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, std::as_bytes(std::span(*diskPhase)));
			gate->OpenDispatch(kDiskPhaseEntry, (PhysicalSky::kDiskPhaseLutSize + 63u) / 64u, 3, 1);
			diskPhase.reset();
		}
	}
};

ORGPhysicalSky::~ORGPhysicalSky() = default;

void ORGPhysicalSky::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::error("[ORG] Physical Sky: the EarlyPrepass anchor could not be reserved; the LUTs stay on D3D11");
}

Texture2D* ORGPhysicalSky::MakeLut2D(Lut a_lut, const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (a_lut == Lut::AerialPerspective || a_lut >= Lut::Count || !Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& twin = Twins2D()[static_cast<std::size_t>(a_lut)];
	if (twin)
		return nullptr;
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = 0;
	desc.CPUAccessFlags = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	auto texture = std::make_unique<GraphResources::Texture2D>(desc, std::string(IdOf(a_lut)), LutOptions(a_lut));
	if (!*texture || !texture->D3D11() || !texture->SRV() || !texture->UAV()) {
		logger::error("[ORG] Physical Sky: {} could not be made on the render graph; it is a D3D11 texture", IdOf(a_lut));
		return nullptr;
	}
	if (!PublishTwin(*texture, a_lut))
		return nullptr;
	// The twin keeps the image, the wrapper keeps the D3D11 texture and its views.
	auto* d3d11 = texture->D3D11();
	d3d11->AddRef();
	auto* wrapper = new Texture2D(d3d11, IdOf(a_lut).data());
	wrapper->srv.copy_from(texture->SRV());
	wrapper->uav.copy_from(texture->UAV());
	twin = std::move(texture);
	return wrapper;
}

Texture3D* ORGPhysicalSky::MakeLut3D(Lut a_lut, const D3D11_TEXTURE3D_DESC& a_desc)
{
	if (a_lut != Lut::AerialPerspective || !Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& twin = ApTwin();
	if (twin)
		return nullptr;
	D3D11_TEXTURE3D_DESC desc = a_desc;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = 0;
	desc.CPUAccessFlags = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	auto texture = std::make_unique<GraphResources::Texture3D>(desc, std::string(IdOf(a_lut)), LutOptions(a_lut));
	if (!*texture || !texture->D3D11() || !texture->SRV() || !texture->UAV()) {
		logger::error("[ORG] Physical Sky: {} could not be made on the render graph; it is a D3D11 texture", IdOf(a_lut));
		return nullptr;
	}
	if (!PublishTwin(*texture, a_lut))
		return nullptr;
	auto* d3d11 = texture->D3D11();
	d3d11->AddRef();
	auto* wrapper = new Texture3D(d3d11);
	wrapper->srv.copy_from(texture->SRV());
	wrapper->uav.copy_from(texture->UAV());
	twin = std::move(texture);
	return wrapper;
}

bool ORGPhysicalSky::Setup(const PhysicalSky& a_feature)
{
	impl.reset();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!runtime.IsActive() || !host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass)) {
		logger::error("[ORG] Physical Sky: the render graph or its EarlyPrepass anchor is not available; the LUTs stay on D3D11");
		return false;
	}
	for (std::size_t i = 0; i < kLutCount; ++i)
		if (!TwinOf(static_cast<Lut>(i))) {
			logger::error("[ORG] Physical Sky: {} is a D3D11 texture; the LUTs stay on D3D11", IdOf(static_cast<Lut>(i)));
			return false;
		}

	auto next = std::make_shared<Impl>();
	for (std::uint32_t i = 0; i < kLutGenEntries; ++i) {
		const auto value = std::to_wstring(i);
		next->lutPrograms[i] = ComputeProgram::Load(host->Device(), { .source = kLutShader, .constantWords = kLutGenWords, .defines = { { L"LUTGEN", value } } });
	}
	next->diskPhaseProgram = ComputeProgram::Load(host->Device(), { .source = kDiskPhaseShader, .constantWords = kDiskPhaseWords });
	next->samplers = { SamplerOf(a_feature.sampTr.get()), SamplerOf(a_feature.sampSv.get()), SamplerOf(a_feature.sampNoise.get()) };
	next->gate = RenderGraph::RunGate::Create("cs.physical-sky.luts.gate", kLutGenEntries + 1);
	next->latch = std::make_shared<org::LatchBlock>("cs.physical-sky.luts.latch", kDiskPhaseBytes);
	const bool programs = std::ranges::all_of(next->lutPrograms, [](const auto& a_program) { return a_program != nullptr; }) && next->diskPhaseProgram;
	const bool samplers = std::ranges::all_of(next->samplers, [](const auto& a_sampler) { return static_cast<bool>(a_sampler); });
	if (!programs || !samplers || !next->gate) {
		logger::error("[ORG] Physical Sky: the LUT programs, samplers or run gate could not be made; the LUTs stay on D3D11");
		return false;
	}

	namespace AnchorPasses = RenderGraph::AnchorPasses;
	using Bind = AnchorPasses::Bind;
	const auto& tr = *TwinOf(Lut::Transmittance);
	const auto& ms = *TwinOf(Lut::Multiscatter);
	const auto& diskPhase = *TwinOf(Lut::DiskPhase);
	const auto samplerBinds = [&next] {
		return std::array{ Bind::Handle(next->samplers[0].Index()), Bind::Handle(next->samplers[1].Index()), Bind::Handle(next->samplers[2].Index()) };
	}();
	// Before IBL's and other EarlyPrepass clients, which read nothing of these.
	AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kLutsClientId, -20, next->latch, next->gate);
	builder.Add(AnchorPasses::Dispatch{ .name = "disk-phase",
		.program = next->diskPhaseProgram,
		.bindings = { Bind::Uav(diskPhase), Bind::Latch(0) },
		.gate = kDiskPhaseEntry });
	constexpr std::array<const char*, kLutGenEntries> names{ "transmittance", "multiscatter", "sky-view", "aerial-perspective" };
	for (std::uint32_t i = 0; i < kLutGenEntries; ++i) {
		const auto lut = static_cast<Lut>(i);
		// The transmittance LUT reads none, the multiscatter LUT only the transmittance one.
		const bool readsTr = lut != Lut::Transmittance;
		const bool readsMs = lut == Lut::SkyView || lut == Lut::AerialPerspective;
		std::vector<Bind> bindings{ Bind::Shared(), readsTr ? Bind::Srv(tr) : Bind::Unused(), readsMs ? Bind::Srv(ms) : Bind::Unused(),
			readsMs ? Bind::Srv(diskPhase) : Bind::Unused(), Bind::Uav(*TwinOf(lut)) };
		bindings.insert(bindings.end(), samplerBinds.begin(), samplerBinds.end());
		builder.Add(AnchorPasses::Dispatch{ .name = names[i], .program = next->lutPrograms[i], .bindings = std::move(bindings), .gate = i });
	}
	if (builder.Failed()) {
		logger::error("[ORG] Physical Sky: {}; the LUTs stay on D3D11", builder.Failure());
		return false;
	}

	FrameAnchors::ClientDesc client{};
	client.id = kLutsClientId;
	client.sharedConstants = builder.ReadsSharedConstants();
	client.passes = builder.Take();
	client.gates = { next->gate };
	client.registerResources = [](org::RenderGraph& a_graph) {
		for (std::size_t i = 0; i < kLutCount; ++i)
			TwinOf(static_cast<Lut>(i))->Register(a_graph);
	};
	// Every run closes the gate first, so a frame without the feature dispatches nothing.
	client.active = [state = next] { return state->luts || state->diskPhase.has_value(); };
	client.commit = [state = next](const FrameAnchors::Point&) { state->Commit(); };
	impl = next;
	if (!FrameAnchors::Register(std::move(client))) {
		impl.reset();
		logger::error("[ORG] Physical Sky: the LUT client could not be registered; the LUTs stay on D3D11");
		return false;
	}
	logger::info("[ORG] Physical Sky: the LUTs run on the render graph at the EarlyPrepass anchor ({})", kLutsClientId);
	return true;
}

bool ORGPhysicalSky::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

bool ORGPhysicalSky::GenerateLuts(PhysicalSky& a_feature)
{
	// Until the schedule holds the client the D3D11 dispatches write the exports.
	if (!IsActive() || !FrameAnchors::Scheduled(kLutsClientId, FrameAnchors::Anchor::EarlyPrepass))
		return false;
	impl->luts = true;
	// As UpdateDiskPhaseLut: only when the constants change.
	const auto parameters = a_feature.DiskPhaseParameters();
	if (!a_feature.diskPhaseValid || parameters != a_feature.diskPhaseParameters) {
		impl->diskPhase = parameters;
		a_feature.diskPhaseParameters = parameters;
		a_feature.diskPhaseValid = true;
	}
	return true;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph the LUTs are always D3D11's.
struct ORGPhysicalSky::Impl
{};

ORGPhysicalSky::~ORGPhysicalSky() = default;
void ORGPhysicalSky::Reserve() {}
Texture2D* ORGPhysicalSky::MakeLut2D(Lut, const D3D11_TEXTURE2D_DESC&) { return nullptr; }
Texture3D* ORGPhysicalSky::MakeLut3D(Lut, const D3D11_TEXTURE3D_DESC&) { return nullptr; }
bool ORGPhysicalSky::Setup(const PhysicalSky&) { return false; }
bool ORGPhysicalSky::IsActive() const { return false; }
bool ORGPhysicalSky::GenerateLuts(PhysicalSky&) { return false; }

#endif
