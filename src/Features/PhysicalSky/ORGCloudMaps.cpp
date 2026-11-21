// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/PhysicalSky/ORGCloudMaps.h"
#include "RenderGraph/Engine/Native/Native.h"

ORGCloudMaps::ORGCloudMaps() = default;
ORGCloudMaps::~ORGCloudMaps() = default;

#include "Features/PhysicalSky.h"
#include "Features/PhysicalSky/ORGCloudInputs.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/D3D11Wrapper.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "State.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <ORGModuleServices/Passes/GenerateMipsPass.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>

#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace
{
	using Bind = RenderGraph::AnchorPasses::Bind;
	using Program = ORGCloudMaps::Program;

	constexpr const char* kClientId = "cs.physical-sky.cloud-maps";
	// Below the cloud renderer (cs.physical-sky.clouds, 160). Every step reads or writes only its own maps before it.
	constexpr std::int32_t kClientOrder = 150;
	constexpr std::int32_t kRendererOrder = 160;

	struct ProgramDesc
	{
		const char* source;
		const wchar_t* entry;
		std::uint32_t words;  // the push block, as the shader's resource list gives it
	};

	// In Program order. The words are the shaders' push blocks (check-dual-target-shaders.py reports them).
	constexpr std::array<ProgramDesc, static_cast<std::size_t>(Program::Count)> kPrograms{ {
		{ "PhysicalSky/NdfNoise.cs.hlsl", L"main", 3 },
		{ "PhysicalSky/NdfGenerate.cs.hlsl", L"main", 13 },
		{ "PhysicalSky/MidCloudGenerate.cs.hlsl", L"main", 10 },
		{ "PhysicalSky/CirrusGenerate.cs.hlsl", L"generateWeather", 10 },
		{ "PhysicalSky/LocalNdf.cs.hlsl", L"main", 10 },
		{ "PhysicalSky/StormNdf.cs.hlsl", L"main", 11 },
		{ "PhysicalSky/CloudNoiseGenerate.cs.hlsl", L"generateShape", 7 },
		{ "PhysicalSky/CloudNoiseGenerate.cs.hlsl", L"generateShapeMip", 7 },
		{ "PhysicalSky/CloudNoiseGenerate.cs.hlsl", L"generateAdjustment", 7 },
		{ "PhysicalSky/CloudNoiseGenerate.cs.hlsl", L"generateAdjustmentMip", 7 },
		{ "PhysicalSky/CloudCrossSection.cs.hlsl", L"main", 9 },
	} };

	/** @brief What the D3D11 side names a resource by: the object a tripwire handle or DXVK's wraps. */
	const void* KeyOf(ID3D11Resource* a_resource)
	{
		return a_resource ? static_cast<const void*>(RenderGraph::Engine::Tripwire::Unwrap(a_resource)) : nullptr;
	}

	ULONG RefCount(IUnknown* a_object)
	{
		a_object->AddRef();
		return a_object->Release();
	}

	/** @brief A generated map's twin, or a file texture's copy in guest mode. */
	struct Twin
	{
		std::shared_ptr<GraphResources::Resource> resource;
		ID3D11Resource* d3d11 = nullptr;
		// The D3D11 references the twin itself holds. A manager's wrapper adds one; when it is gone the twin is unused.
		ULONG baseline = 0;
		bool copy = false;  // a file texture's copy, owned by its import
		std::uint32_t width = 0, height = 0, mips = 1;
		std::shared_ptr<const void> wrapper;  // the manager's D3D11 wrapper's registration (GraphResources::D3D11Wrapper)
	};

	/** @brief A file texture as the graph reads it. */
	struct Import
	{
		winrt::com_ptr<ID3D11ShaderResourceView> srv;  // kept, so its address is never reused while cached
		FrameAnchors::Texture texture;
		std::shared_ptr<Twin> copy;  // guest mode, for arrays and volumes
		bool array = false;
	};

	/** @brief One step as it is registered. Two equal descs are the same pass. */
	struct StepDesc
	{
		std::string name;
		std::int32_t order = 0;
		bool mips = false;
		Program program = Program::Count;
		std::vector<Bind> bindings;  // the latch entry holds offset 0 until the registration places it
		std::uint32_t latchBytes = 0;
		std::shared_ptr<Twin> mipsTexture;
		std::vector<std::shared_ptr<Twin>> twins;  // what the step declares, for its liveness
		std::vector<std::shared_ptr<Import>> imports;
		std::uint64_t serial = 0;  // grows with each change of a step, so a registration holds this one or a newer one
	};
	using StepPtr = std::shared_ptr<const StepDesc>;

	bool SameBind(const Bind& a_left, const Bind& a_right)
	{
		return a_left.kind == a_right.kind && a_left.resource == a_right.resource && a_left.value == a_right.value && a_left.mip == a_right.mip &&
		       a_left.id == a_right.id && a_left.variant == a_right.variant && a_left.slot == a_right.slot;
	}

	bool SameStep(const StepDesc& a_left, const StepDesc& a_right)
	{
		return a_left.name == a_right.name && a_left.order == a_right.order && a_left.mips == a_right.mips && a_left.program == a_right.program &&
		       a_left.latchBytes == a_right.latchBytes && a_left.mipsTexture == a_right.mipsTexture &&
		       std::ranges::equal(a_left.bindings, a_right.bindings, SameBind);
	}

	/** @brief One registration of the client: its steps' gate entries and latch regions. */
	struct Registration
	{
		struct Placed
		{
			StepPtr desc;  // the step as this registration holds it
			std::uint32_t entry = 0;  // dispatches only
			std::uint32_t offset = 0;
			std::uint32_t bytes = 0;
			std::uint32_t width = 0, height = 0, mips = 0;  // mips only
		};
		std::unordered_map<std::string, Placed> steps;
		std::shared_ptr<RenderGraph::RunGate> gate;
		std::shared_ptr<org::LatchBlock> latch;
	};

	/**
	 * @brief The client's key: every step the generators hold and the twins the client registers (the map set).
	 * A change of either is a new set, observed as a variable, so the next revision registers the client again.
	 */
	struct StepSet
	{
		std::vector<StepPtr> steps;  // in their order inside the client
		std::vector<std::shared_ptr<Twin>> twins;
		std::unordered_set<const Twin*> twinSet;
	};
	using StepSetKey = std::shared_ptr<const StepSet>;

	/** @brief One step's work waiting for a commit. */
	struct PendingStep
	{
		StepPtr step;
		std::vector<std::byte> constants;
		std::array<std::uint32_t, 3> groups{ 1, 1, 1 };
	};
}

struct ORGCloudMaps::Impl
{
	std::array<RenderGraph::Programs::Fixed<ComputeProgram>, static_cast<std::size_t>(Program::Count)> programs;
	RenderGraph::Programs::Fixed<ComputeProgram> mips;
	std::array<org::OwnedDescriptorBinding, static_cast<std::size_t>(Sampler::Count)> samplers;
	DCLF::GpuTextures::Binding null;

	// The steps the next step set holds, by name. A changed step gets a new desc with a newer serial.
	std::map<std::string, StepPtr> steps;
	std::uint64_t stepSerial = 0;
	bool dirty = false;

	// The client is declared on the sky's and the clouds' toggles and on the step set.
	RenderGraph::Frame::Observed<StepSetKey> stepSet{ "PhysicalSky.CloudMaps.Steps" };
	RenderGraph::Frame::DeclaredClient client;

	/** @brief One update's work waiting for a commit. */
	struct Queued
	{
		std::vector<PendingStep> steps;
		bool sequential = false;
	};
	// The work waiting for a commit, by manager, oldest first. Work whose steps a registration does not hold yet waits for the
	// one that does. Each commit runs at most one update per manager, so one update's steps never mix with another's in a run.
	std::map<std::string, std::deque<Queued>> queues;

	bool Pending() const
	{
		return std::ranges::any_of(queues, [](const auto& a_entry) { return !a_entry.second.empty(); });
	}

	/**
	 * @brief Queues a_work behind a_owner's earlier updates. A plain update replaces the plain update queued last when it has
	 * every step of that one, since every step rewrites its whole output.
	 */
	void Push(const std::string& a_owner, Queued a_work)
	{
		auto& queue = queues[a_owner];
		const auto covers = [&](const Queued& a_older) {
			return std::ranges::all_of(a_older.steps, [&](const PendingStep& a_old) {
				return std::ranges::any_of(a_work.steps, [&](const PendingStep& a_new) { return a_new.step->name == a_old.step->name; });
			});
		};
		while (!a_work.sequential && !queue.empty() && !queue.back().sequential && covers(queue.back()))
			queue.pop_back();
		queue.push_back(std::move(a_work));
	}

	/** @brief Opens a pending step in this run. */
	template <class RunRef>
	static void Run(const Registration& a_registration, const PendingStep& a_pending, const RunRef& a_run)
	{
		const auto& placed = a_registration.steps.at(a_pending.step->name);
		if (a_pending.step->mips) {
			const auto groups = org::services::PlanGenerateMipsGroups(placed.width, placed.height, placed.mips, 1);
			a_registration.latch->Write(a_run, placed.offset, std::as_bytes(std::span(groups)).first((std::min)(groups.size() * sizeof(org::services::GenerateMipsGroups), std::size_t(placed.bytes))));
			return;
		}
		if (!a_pending.constants.empty())
			a_registration.latch->Write(a_run, placed.offset, std::span(a_pending.constants).first((std::min)(a_pending.constants.size(), std::size_t(placed.bytes))));
		a_registration.gate->OpenDispatch(placed.entry, a_pending.groups[0], a_pending.groups[1], a_pending.groups[2]);
	}

	/** @brief At the commit of a run of a_registration. Each manager's oldest update runs once the registration holds all its steps. */
	void Commit(const Registration& a_registration)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		const auto run = host->CurrentRun();
		const auto holds = [&](const PendingStep& a_pending) {
			const auto found = a_registration.steps.find(a_pending.step->name);
			// A newer desc of the step serves too: its work runs after this one.
			return found != a_registration.steps.end() && found->second.desc->serial >= a_pending.step->serial;
		};
		for (auto& [owner, queue] : queues) {
			if (queue.empty() || !std::ranges::all_of(queue.front().steps, holds))
				continue;
			for (const auto& pending : queue.front().steps)
				Run(a_registration, pending, run);
			queue.pop_front();
		}
	}
};

namespace
{
	/** @brief The generated maps' twins and the file copies, by the D3D11 object. Render thread. */
	std::unordered_map<const void*, std::shared_ptr<Twin>>& Twins()
	{
		static std::unordered_map<const void*, std::shared_ptr<Twin>> s_twins;
		return s_twins;
	}

	// Set when a twin is made, so the next EndFrame registers the client with it.
	bool s_twinsChanged = false;
	// The twins the latest registration registers. A graph built from now on has them.
	std::unordered_set<const Twin*> s_registeredTwins;
	// The twins the cloud renderer read through Gather last, which the client registers though no step of its own uses them.
	std::unordered_map<const Twin*, std::shared_ptr<Twin>> s_inputTwins;
	// The file imports Gather read last. They stay while it reads them, though no step uses them.
	std::unordered_set<const Import*> s_inputImports;

	std::unordered_map<const ID3D11ShaderResourceView*, std::shared_ptr<Import>>& Imports()
	{
		static std::unordered_map<const ID3D11ShaderResourceView*, std::shared_ptr<Import>> s_imports;
		return s_imports;
	}

	std::shared_ptr<Twin> TwinOf(ID3D11Resource* a_resource)
	{
		const auto found = Twins().find(KeyOf(a_resource));
		return found != Twins().end() ? found->second : nullptr;
	}

	std::shared_ptr<Twin> TwinOf(ID3D11ShaderResourceView* a_srv)
	{
		if (!a_srv)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		a_srv->GetResource(resource.put());
		return TwinOf(resource.get());
	}

	/** @brief A name no live twin has: the graph registers resources by name. */
	std::string UniqueName(const char* a_name)
	{
		std::string base = a_name ? a_name : "PhysicalSky::CloudMap";
		std::string name = base;
		for (std::uint32_t i = 1; std::ranges::any_of(Twins(), [&](const auto& a_entry) { return a_entry.second->resource->Name() == name; }); ++i)
			name = std::format("{}#{}", base, i);
		return name;
	}

	/** @brief Drops twins no manager holds any more. Their graph resources live on in older graphs until those are gone. */
	void PruneTwins()
	{
		std::erase_if(Twins(), [](const auto& a_entry) {
			return !a_entry.second->copy && a_entry.second->baseline && RefCount(a_entry.second->d3d11) <= a_entry.second->baseline;
		});
	}

	std::shared_ptr<Twin> Remember(std::shared_ptr<GraphResources::Resource> a_resource, ID3D11Resource* a_d3d11, bool a_copy, std::uint32_t a_width,
		std::uint32_t a_height, std::uint32_t a_mips)
	{
		auto twin = std::make_shared<Twin>();
		twin->resource = std::move(a_resource);
		twin->d3d11 = a_d3d11;
		twin->baseline = RefCount(a_d3d11);
		twin->copy = a_copy;
		twin->width = a_width;
		twin->height = a_height;
		twin->mips = a_mips;
		Twins()[KeyOf(a_d3d11)] = twin;
		s_twinsChanged = true;
		return twin;
	}

	UINT FullChain(UINT a_width, UINT a_height, UINT a_depth = 1)
	{
		return static_cast<UINT>(std::bit_width((std::max)({ a_width, a_height, a_depth, 1u })));
	}

	/** @brief A file texture as a graph handle, imported once. Empty, logged, when it cannot be. */
	std::shared_ptr<Import> ImportOf(ID3D11ShaderResourceView* a_srv)
	{
		auto& imports = Imports();
		if (const auto found = imports.find(a_srv); found != imports.end())
			return found->second;
		winrt::com_ptr<ID3D11Resource> resource;
		a_srv->GetResource(resource.put());
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		a_srv->GetDesc(&view);
		auto entry = std::make_shared<Import>();
		entry->srv.copy_from(a_srv);
		const auto name = std::format("PhysicalSky::CloudFile{}", imports.size());
		const bool plain2D = view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
		if (plain2D) {
			auto texture = resource.try_as<ID3D11Texture2D>();
			entry->texture = texture ? FrameAnchors::ImportTexture("cs.d3d11." + name, { .texture = texture.get(), .srv = a_srv, .immutable = true }) : FrameAnchors::Texture{};
		} else if (RenderGraph::Engine::Native::Active()) {
			// Native mode: the file texture is exempt, so the graph reads it as it is.
			entry->array = view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			if (auto exempt = RenderGraph::Engine::Native::ImmutableTextureOf(resource.get(), view.Format))
				entry->texture = FrameAnchors::Texture(std::move(exempt));
		} else if (auto texture3D = resource.try_as<ID3D11Texture3D>()) {
			// Guest mode imports 2D textures only, so a volume becomes a twin filled once by a D3D11 copy.
			D3D11_TEXTURE3D_DESC desc{};
			texture3D->GetDesc(&desc);
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = desc.CPUAccessFlags = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			auto twin = std::make_shared<GraphResources::Texture3D>(desc, name, GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
			if (*twin && twin->D3D11()) {
				globals::d3d::context->CopyResource(twin->D3D11(), texture3D.get());
				auto* d3d11 = twin->D3D11();
				entry->copy = Remember(std::move(twin), d3d11, true, desc.Width, desc.Height, desc.MipLevels);
				entry->texture = RenderGraph::AnchorPasses::TextureOf(*entry->copy->resource);
			}
		} else if (auto texture2D = resource.try_as<ID3D11Texture2D>()) {
			// The same for a texture array, such as a voxel cloud's slices.
			D3D11_TEXTURE2D_DESC desc{};
			texture2D->GetDesc(&desc);
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = desc.CPUAccessFlags = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			entry->array = true;
			auto twin = std::make_shared<GraphResources::Texture2D>(desc, name,
				GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV, .array = true });
			if (*twin && twin->D3D11()) {
				globals::d3d::context->CopyResource(twin->D3D11(), texture2D.get());
				auto* d3d11 = twin->D3D11();
				entry->copy = Remember(std::move(twin), d3d11, true, desc.Width, desc.Height, desc.MipLevels);
				entry->texture = RenderGraph::AnchorPasses::TextureOf(*entry->copy->resource);
			}
		}
		if (!entry->texture) {
			logger::error("[ORG] Physical Sky cloud maps: the file texture {} could not be put on the render graph", name);
			return nullptr;
		}
		imports[a_srv] = entry;
		return entry;
	}

	/** @brief Drops the imports no step holds and Gather did not read last, with their copies. */
	void PruneImports(const std::map<std::string, StepPtr>& a_steps)
	{
		std::unordered_set<const Import*> held = s_inputImports;
		for (const auto& [name, step] : a_steps)
			for (const auto& import : step->imports)
				held.insert(import.get());
		std::erase_if(Imports(), [&](const auto& a_entry) {
			if (held.contains(a_entry.second.get()))
				return false;
			if (a_entry.second->copy)
				Twins().erase(KeyOf(a_entry.second->copy->d3d11));
			return true;
		});
	}

	/** @brief A view as a graph handle: a twin's, or a file texture's import. Null for null. a_ok is false when it has none. */
	FrameAnchors::Texture HandleOf(ID3D11ShaderResourceView* a_srv, bool& a_ok)
	{
		if (!a_srv)
			return {};
		if (const auto twin = TwinOf(a_srv)) {
			// A twin the latest registration does not register is in no graph yet: the next registers it.
			s_inputTwins.emplace(twin.get(), twin);
			if (!s_registeredTwins.contains(twin.get())) {
				a_ok = false;
				s_twinsChanged = true;
			}
			return RenderGraph::AnchorPasses::TextureOf(*twin->resource);
		}
		if (const auto import = ImportOf(a_srv)) {
			s_inputImports.insert(import.get());
			if (import->copy) {
				s_inputTwins.emplace(import->copy.get(), import->copy);
				if (!s_registeredTwins.contains(import->copy.get())) {
					a_ok = false;
					s_twinsChanged = true;
				}
			}
			return import->texture;
		}
		a_ok = false;
		return {};
	}
}

eastl::unique_ptr<Texture2D> ORGCloudMaps::MakeTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, bool a_array)
{
	if (!On())
		return eastl::make_unique<Texture2D>(a_desc, a_name);
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = desc.CPUAccessFlags = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	if (!desc.MipLevels)
		desc.MipLevels = FullChain(desc.Width, desc.Height);
	const auto name = UniqueName(a_name);
	auto twin = std::make_shared<GraphResources::Texture2D>(desc, name,
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV, .array = a_array });
	if (!twin->D3D11())
		Fatal(fmt::format("{} could not be made on the render graph", name));
	auto* d3d11 = twin->D3D11();
	auto remembered = Remember(twin, d3d11, false, desc.Width, desc.Height, desc.MipLevels);
	// The wrapper takes its own reference. The twin keeps the image.
	auto wrapper = GraphResources::MakeWrapper(*twin, name.c_str());
	// A device whose references do not count the wrapper's (native mode's may not) would make the twin look unused at once.
	// Such a twin is never pruned.
	if (RefCount(d3d11) <= remembered->baseline)
		remembered->baseline = 0;
	auto* object = wrapper.Get();
	remembered->wrapper = std::make_shared<GraphResources::D3D11Wrapper<::Texture2D>>(std::move(wrapper));
	return eastl::unique_ptr<Texture2D>(object);
}

eastl::unique_ptr<Texture3D> ORGCloudMaps::MakeTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, const char* a_name)
{
	if (!On())
		return eastl::make_unique<Texture3D>(a_desc, a_name);
	D3D11_TEXTURE3D_DESC desc = a_desc;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = desc.CPUAccessFlags = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	if (!desc.MipLevels)
		desc.MipLevels = FullChain(desc.Width, desc.Height, desc.Depth);
	const auto name = UniqueName(a_name);
	auto twin = std::make_shared<GraphResources::Texture3D>(desc, name,
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV });
	if (!twin->D3D11())
		Fatal(fmt::format("{} could not be made on the render graph", name));
	auto* d3d11 = twin->D3D11();
	auto remembered = Remember(twin, d3d11, false, desc.Width, desc.Height, desc.MipLevels);
	auto wrapper = GraphResources::MakeWrapper(*twin, name.c_str());
	// A device whose references do not count the wrapper's (native mode's may not) would make the twin look unused at once.
	// Such a twin is never pruned.
	if (RefCount(d3d11) <= remembered->baseline)
		remembered->baseline = 0;
	auto* object = wrapper.Get();
	remembered->wrapper = std::make_shared<GraphResources::D3D11Wrapper<::Texture3D>>(std::move(wrapper));
	return eastl::unique_ptr<Texture3D>(object);
}

void ORGCloudMaps::Setup(const PhysicalSky& a_feature)
{
	// Once a session.
	if (!BeginSetup())
		return;
	auto next = std::make_shared<Impl>();
	for (std::size_t i = 0; i < kPrograms.size(); ++i) {
		const auto& program = kPrograms[i];
		next->programs[i] = RenderGraph::Programs::Queue("CloudMaps", ComputeProgram::Desc{ .source = program.source, .entry = program.entry, .constantWords = program.words });
	}
	next->mips = RenderGraph::Programs::Queue("CloudMaps", ComputeProgram::GenerateMips(false));
	const auto samplerOf = [](ID3D11SamplerState* a_sampler) {
		D3D11_SAMPLER_DESC desc{};
		if (a_sampler)
			a_sampler->GetDesc(&desc);
		return a_sampler ? ShaderBindings::CreateSamplerFromD3D11(desc) : org::OwnedDescriptorBinding{};
	};
	next->samplers = { ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearWrap), ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearClamp), samplerOf(a_feature.sampNoise.get()),
		samplerOf(a_feature.sampTr.get()) };
	next->null = DCLF::GpuTextures::Get().NullBinding();
	if (!next->null)
		Fatal("the null descriptor could not be made");
	if (!std::ranges::all_of(next->samplers, [](const auto& a_sampler) { return static_cast<bool>(a_sampler); }))
		Fatal("the samplers could not be made");
	using namespace RenderGraph::Frame;
	// The registration of a step set, made when a revision starts.
	const auto makeClient = [](const std::shared_ptr<Impl>& a_state, const StepSet& a_set, FrameAnchors::ClientDesc& a_client) {
		auto registration = std::make_shared<Registration>();
		std::uint32_t entries = 0, bytes = 0;
		for (const auto& step : a_set.steps) {
			Registration::Placed placed{ .desc = step, .offset = bytes, .bytes = step->latchBytes };
			if (step->mips) {
				placed.width = step->mipsTexture->width;
				placed.height = step->mipsTexture->height;
				placed.mips = step->mipsTexture->mips;
			} else
				placed.entry = entries++;
			bytes += (step->latchBytes + 15u) & ~15u;
			registration->steps[step->name] = placed;
		}
		registration->gate = RenderGraph::RunGate::Create(std::format("{}.gate", kClientId), (std::max)(entries, 1u));
		registration->latch = std::make_shared<org::LatchBlock>(std::format("{}.latch", kClientId), (std::max)(bytes, 16u));

		namespace AnchorPasses = RenderGraph::AnchorPasses;
		// Every step stays below the cloud renderer's order.
		const auto base = (std::min)(kClientOrder, kRendererOrder - 1 - static_cast<std::int32_t>(a_set.steps.size()));
		AnchorPasses::Builder builder(FrameAnchors::Anchor::Prepass, kClientId, base, registration->latch, registration->gate);
		for (const auto& step : a_set.steps) {
			const auto& placed = registration->steps[step->name];
			if (step->mips) {
				builder.Add(step->name, FrameAnchors::PassKind::Compute,
					[program = org::services::GenerateMipsProgram{ a_state->mips.Get(), false }, texture = step->mipsTexture->resource->Graph(), latch = registration->latch, offset = placed.offset]() {
						return org::services::MakeGenerateMipsPass(program, texture, { latch, offset });
					});
			} else {
				auto bindings = step->bindings;
				for (auto& bind : bindings)
					if (bind.kind == Bind::Kind::Latch)
						bind.value = placed.offset;
				builder.Add(AnchorPasses::Dispatch{ .name = step->name,
					.program = a_state->programs[static_cast<std::size_t>(step->program)].Get(),
					.bindings = std::move(bindings),
					.gate = placed.entry });
			}
		}
		if (builder.Failed())
			Fatal(builder.Failure());
		std::vector<std::shared_ptr<GraphResources::Resource>> resources;
		for (const auto& twin : a_set.twins)
			resources.push_back(twin->resource);

		a_client = {};
		a_client.id = kClientId;
		a_client.passes = builder.Take();
		a_client.gates = { registration->gate };
		a_client.registerResources = [resources = std::move(resources)](org::RenderGraph& a_graph) {
			for (const auto& resource : resources)
				resource->Register(a_graph);
		};
		const std::weak_ptr<Impl> weak = a_state;
		a_client.active = [weak] {
			const auto state = weak.lock();
			return state && state->Pending();
		};
		a_client.commit = [weak, registration](const FrameAnchors::Point&) {
			if (const auto state = weak.lock())
				state->Commit(*registration);
		};
		logger::info("[ORG] Physical Sky cloud maps: {} registered with {} steps ({} gated dispatches, {} latch bytes)", kClientId, a_set.steps.size(), entries,
			bytes);
	};
	const auto& settings = a_feature.settings;
	const std::weak_ptr<Impl> weak = next;
	next->client = DeclareClient<StepSetKey>(
		kClientId, DependsOn(settings.enabled, settings.enableVolumetricClouds, next->stepSet.Var()),
		[weak](const Inputs& a_in) -> std::optional<StepSetKey> {
			const auto state = weak.lock();
			const auto& sky = globals::features::physicalSky.settings;
			if (!state || !a_in.Get(sky.enabled) || !a_in.Get(sky.enableVolumetricClouds))
				return std::nullopt;
			const auto* set = state->stepSet.Get(a_in);
			if (!set || !*set || (*set)->steps.empty())
				return std::nullopt;
			return *set;
		},
		[weak, makeClient](const StepSetKey& a_set, FrameAnchors::ClientDesc& a_client) {
			const auto state = weak.lock();
			if (!state)
				return false;
			makeClient(state, *a_set, a_client);
			// A graph built from now on registers these.
			s_registeredTwins = a_set->twinSet;
			return true;
		});
	impl = std::move(next);
	logger::info("[ORG] Physical Sky cloud maps: the generators run on the render graph ({}, Prepass)", kClientId);
}

void ORGCloudMaps::Submit(Work a_work)
{
	if (!IsActive())
		return;
	auto& state = *impl;
	// The steps the work needs, as they would be registered.
	std::vector<StepDesc> needed;
	std::vector<PendingStep> work;
	std::string missing;
	const auto srvBind = [&](ID3D11ShaderResourceView* a_srv, StepDesc& a_step) {
		if (!a_srv)
			return Bind::Handle(state.null.index);
		if (const auto twin = TwinOf(a_srv)) {
			a_step.twins.push_back(twin);
			return Bind::Srv(*twin->resource);
		}
		if (const auto import = ImportOf(a_srv)) {
			a_step.imports.push_back(import);
			return import->array ? Bind::SrvArray(import->texture) : Bind::Srv(import->texture);
		}
		missing = std::format("step {} reads a file texture that is not on the render graph", a_step.name);
		return Bind::Handle(state.null.index);
	};
	const auto twinBind = [&](ID3D11Resource* a_texture, StepDesc& a_step, auto&& a_make) {
		const auto twin = TwinOf(a_texture);
		if (!twin) {
			missing = std::format("step {} writes or reads a texture that is not a cloud map twin", a_step.name);
			return Bind::Handle(state.null.index);
		}
		a_step.twins.push_back(twin);
		return a_make(*twin->resource);
	};
	for (auto& dispatch : a_work.dispatches) {
		StepDesc step{ .name = dispatch.step, .order = dispatch.order, .program = dispatch.program, .latchBytes = static_cast<std::uint32_t>(dispatch.constants.size()) };
		for (const auto& input : dispatch.inputs) {
			switch (input.kind) {
			case Input::Kind::Srv:
				if (input.texture)
					step.bindings.push_back(twinBind(input.texture, step, [&](const GraphResources::Resource& a_twin) { return Bind::Srv(a_twin, input.mip); }));
				else
					step.bindings.push_back(srvBind(input.srv, step));
				break;
			case Input::Kind::Uav:
				step.bindings.push_back(twinBind(input.texture, step, [&](const GraphResources::Resource& a_twin) { return Bind::Uav(a_twin, input.mip); }));
				break;
			case Input::Kind::UavArray:
				step.bindings.push_back(twinBind(input.texture, step,
					[&](const GraphResources::Resource& a_twin) { return Bind::UavArray(RenderGraph::AnchorPasses::TextureOf(a_twin)); }));
				break;
			case Input::Kind::Sampler:
				step.bindings.push_back(Bind::Handle(state.samplers[input.value].Index()));
				break;
			case Input::Kind::Latch:
				step.bindings.push_back(Bind::Latch(0));
				break;
			case Input::Kind::Word:
				step.bindings.push_back(Bind::Handle(input.value));
				break;
			}
		}
		needed.push_back(std::move(step));
		PendingStep pending;
		pending.constants = std::move(dispatch.constants);
		pending.groups = dispatch.groups;
		work.push_back(std::move(pending));
	}
	for (const auto& mips : a_work.mips) {
		StepDesc step{ .name = mips.step, .order = mips.order, .mips = true };
		step.mipsTexture = TwinOf(mips.texture);
		if (!step.mipsTexture) {
			missing = std::format("step {} makes the mips of a texture that is not a cloud map twin", mips.step);
			break;
		}
		step.twins.push_back(step.mipsTexture);
		step.latchBytes = (std::max)(step.mipsTexture->mips, 2u) * static_cast<std::uint32_t>(sizeof(org::services::GenerateMipsGroups));
		needed.push_back(std::move(step));
		work.push_back({});
	}
	// A manager's texture that is not a twin, or a file texture that cannot be imported, is a defect.
	if (!missing.empty())
		Fatal(missing);
	// New or changed steps make a new step set at EndFrame. Their work waits for the registration that holds them.
	for (std::size_t i = 0; i < needed.size(); ++i) {
		auto& step = needed[i];
		auto found = state.steps.find(step.name);
		if (found == state.steps.end() || !SameStep(*found->second, step)) {
			step.serial = ++state.stepSerial;
			std::string name = step.name;
			found = state.steps.insert_or_assign(std::move(name), std::make_shared<const StepDesc>(std::move(step))).first;
			state.dirty = true;
		}
		work[i].step = found->second;
	}
	if (a_work.owner.empty())
		Fatal("a manager's work names no owner");
	state.Push(a_work.owner, Impl::Queued{ .steps = std::move(work), .sequential = a_work.sequential });
}

void ORGCloudMaps::EndFrame()
{
	if (!IsActive() || (!impl->dirty && !s_twinsChanged))
		return;
	auto& state = *impl;
	state.dirty = false;
	s_twinsChanged = false;
	PruneTwins();
	// Steps whose maps are gone are dropped, then the imports nothing holds.
	std::erase_if(state.steps, [](const auto& a_entry) {
		return std::ranges::any_of(a_entry.second->twins, [](const auto& a_twin) { return !Twins().contains(KeyOf(a_twin->d3d11)); });
	});
	PruneImports(state.steps);
	// Work for a dropped step writes maps that are gone, and no registration would hold it.
	for (auto& [owner, queue] : state.queues)
		std::erase_if(queue, [&](const Impl::Queued& a_queued) {
			return std::ranges::any_of(a_queued.steps, [&](const PendingStep& a_pending) { return !state.steps.contains(a_pending.step->name); });
		});

	auto next = std::make_shared<StepSet>();
	for (const auto& [name, step] : state.steps)
		next->steps.push_back(step);
	std::ranges::stable_sort(next->steps, [](const StepPtr& a_left, const StepPtr& a_right) { return a_left->order < a_right->order; });
	// The twins the steps use, and those only the cloud renderer reads (the voxel lighting, file copies, the current noise).
	// A twin nothing uses stays out: an idle one of a ping-pong pair would enter the graph with no state to start from.
	const auto add = [&](const std::shared_ptr<Twin>& a_twin) {
		if (a_twin && Twins().contains(KeyOf(a_twin->d3d11)) && next->twinSet.insert(a_twin.get()).second)
			next->twins.push_back(a_twin);
	};
	for (const auto& step : next->steps) {
		for (const auto& twin : step->twins)
			add(twin);
		add(step->mipsTexture);
	}
	for (const auto& [_, twin] : s_inputTwins)
		add(twin);
	logger::info("[ORG] Physical Sky cloud maps: a step set of {} steps for the next revision", next->steps.size());
	// The next revision registers it (the client's key).
	state.stepSet.Observe(StepSetKey(std::move(next)));
}

bool ORGCloudInputs::Gather(const PhysicalSky& a_feature, Inputs& a_out)
{
	a_out = {};
	static Util::ReasonLog s_reasons("[ORG] Physical Sky clouds: no clouds this frame", spdlog::level::warn);
	s_inputTwins.clear();
	s_inputImports.clear();
	const auto fail = [](const char* a_reason) {
		s_reasons.Report(a_reason);
		return false;
	};
	auto& maps = ORGCloudMaps::Get();
	if (!maps.IsActive())
		return fail("the cloud maps are not on the render graph");
	bool ok = true;
	auto& feature = const_cast<PhysicalSky&>(a_feature);  // GetNdf only queries the loaded textures
	const auto& settings = a_feature.settings;
	a_out.shapeNoise = HandleOf(a_feature.baseShapeNoiseSrv.get(), ok);
	a_out.profileLut = HandleOf(a_feature.cloudProfileLutSrv.get(), ok);
	a_out.adjustmentLut = HandleOf(a_feature.cloudAdjustmentLutSrv.get(), ok);
	const auto ndf = feature.ndfManager.GetNdf(settings.cloudMap, feature.ndfTexManager);
	if (!ndf)
		return fail("the cloud map is not generated yet");
	a_out.ndfHeight = HandleOf(ndf.height, ok);
	a_out.ndfModeling = HandleOf(ndf.modeling, ok);
	const auto& noise = a_feature.ndfManager.GetNoiseInputs();
	for (std::size_t i = 0; i < a_out.middleNoise.size(); ++i)
		a_out.middleNoise[i] = HandleOf(noise[i + 1], ok);
	if (settings.cloudLayer.middle.enabled)
		a_out.middleWeather = HandleOf(a_feature.midCloudMapManager.Weather(), ok);
	if (settings.cloudLayer.cirrus.enabled) {
		const auto cirrus = a_feature.cirrusMapManager.GetTextures();
		if (cirrus) {
			a_out.cirrusWeather = HandleOf(cirrus.weather, ok);
			a_out.cirrusPatterns = HandleOf(cirrus.patterns, ok);
		}
	}
	const auto local = a_feature.localNdfManager.GetTextures();
	a_out.localHeight = HandleOf(local.height, ok);
	a_out.localModeling = HandleOf(local.modeling, ok);
	a_out.localMaximum = HandleOf(a_feature.localNdfManager.GetMaximum(), ok);
	a_out.stormHeight = HandleOf(a_feature.cloudStormManager.Height(), ok);
	a_out.stormModeling = HandleOf(a_feature.cloudStormManager.Modeling(), ok);
	const auto& voxel = a_feature.voxelCloudManager;
	if (voxel.Count()) {
		const auto& bindings = voxel.Bindings();
		for (std::uint32_t i = 0; i < VoxelCloudManager::kAssetSlots; ++i) {
			a_out.voxelModeling[i] = HandleOf(bindings[i], ok);
			a_out.voxelDistance[i] = HandleOf(bindings[VoxelCloudManager::kAssetSlots + i], ok);
		}
		a_out.voxelNoise = HandleOf(bindings[VoxelCloudManager::kAssetSlots * 2], ok);
		a_out.voxelLighting = HandleOf(voxel.Lighting(), ok);
	}
	// As the renderer binds t76: the NDF clouds' NVDF noise replaces the voxel binding.
	if (settings.cloudNoise.useVoxelNoise)
		a_out.voxelNoise = HandleOf(voxel.LoadedNoise(), ok);
	if (!ok)
		return fail("a map or file texture is not on the render graph yet");
	return true;
}

