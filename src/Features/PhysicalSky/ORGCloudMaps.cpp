#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/PhysicalSky/ORGCloudMaps.h"
#include "RenderGraph/Engine/Native/Native.h"

bool ORGCloudMaps::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

ORGCloudMaps& ORGCloudMaps::Get()
{
	static ORGCloudMaps s_maps;
	return s_maps;
}

ORGCloudMaps::~ORGCloudMaps() = default;

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "Features/PhysicalSky.h"
#	include "Features/PhysicalSky/ORGCloudInputs.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GenerateMips.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>

#	include <algorithm>
#	include <map>
#	include <unordered_map>
#	include <unordered_set>

namespace
{
	using Bind = RenderGraph::AnchorPasses::Bind;
	using Program = ORGCloudMaps::Program;

	constexpr const char* kClientId = "cs.physical-sky.cloud-maps";
	// Below the cloud renderer (cs.physical-sky.clouds, 160). Every step reads or writes only its own maps before it.
	constexpr std::int32_t kClientOrder = 150;
	constexpr std::int32_t kRendererOrder = 160;
	// A step or a file import nobody asked for in this many frames is dropped at the next registration.
	constexpr std::uint64_t kStaleFrames = 600;

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

	std::uint64_t Frame()
	{
		return globals::state ? globals::state->frameCount : 0;
	}

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
	};

	/** @brief A file texture as the graph reads it. */
	struct Import
	{
		winrt::com_ptr<ID3D11ShaderResourceView> srv;  // kept, so its address is never reused while cached
		FrameAnchors::Texture texture;
		std::shared_ptr<Twin> copy;  // guest mode, for arrays and volumes
		bool array = false;
		std::uint64_t used = 0;
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
		std::uint64_t used = 0;
	};

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
			std::uint32_t entry = 0;  // dispatches only
			std::uint32_t offset = 0;
			std::uint32_t bytes = 0;
			std::uint32_t width = 0, height = 0, mips = 0;  // mips only
		};
		std::unordered_map<std::string, Placed> steps;
		std::shared_ptr<RenderGraph::RunGate> gate;
		std::shared_ptr<org::LatchBlock> latch;
	};
}

struct ORGCloudMaps::Impl
{
	std::array<std::shared_ptr<const ComputeProgram>, static_cast<std::size_t>(Program::Count)> programs;
	std::shared_ptr<const GraphPasses::GenerateMipsProgram> mips;
	std::array<org::OwnedDescriptorBinding, static_cast<std::size_t>(Sampler::Count)> samplers;
	std::uint32_t nullIndex = DCLF::GpuTextures::kInvalid;
	std::shared_ptr<const void> nullOwner;

	// The steps every registration holds, by name.
	std::map<std::string, StepDesc> steps;
	bool dirty = false;

	/** @brief One registration as EndFrame built it: the client's key. */
	struct Built
	{
		std::shared_ptr<Registration> registration;
		FrameAnchors::ClientDesc client;
		std::unordered_set<const Twin*> twins;  // what its graphs register
	};
	// The client is declared on the sky's and the clouds' toggles. Its key is the newest registration built, which is not a
	// variable: a new one invalidates the key. held is the one the adopted revision holds.
	RenderGraph::Frame::DeclaredClient client;
	std::shared_ptr<const Built> built;
	std::shared_ptr<const Built> held;

	// This frame's view of the schedule, and the work waiting for a commit.
	std::uint64_t frame = UINT64_MAX;
	bool scheduled = false;
	std::uint64_t pendingFrame = 0;
	Work pending;
	std::string refusal;  // why work was refused, logged when it changes
	bool failed = false;

	void Refuse(std::string_view a_reason)
	{
		if (refusal != a_reason) {
			logger::info("[ORG] Physical Sky cloud maps: work waits ({})", a_reason);
			refusal = a_reason;
		}
	}

	void Commit(const Registration& a_registration)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		const auto run = host->CurrentRun();
		for (const auto& dispatch : pending.dispatches) {
			const auto found = a_registration.steps.find(dispatch.step);
			if (found == a_registration.steps.end())
				continue;
			const auto& placed = found->second;
			if (!dispatch.constants.empty())
				a_registration.latch->Write(run, placed.offset, std::span(dispatch.constants).first((std::min)(dispatch.constants.size(), std::size_t(placed.bytes))));
			a_registration.gate->OpenDispatch(placed.entry, dispatch.groups[0], dispatch.groups[1], dispatch.groups[2]);
		}
		for (const auto& chain : pending.mips) {
			const auto found = a_registration.steps.find(chain.step);
			if (found == a_registration.steps.end())
				continue;
			const auto& placed = found->second;
			const auto groups = GraphPasses::GenerateMipsGroups(placed.width, placed.height, placed.mips, 1);
			a_registration.latch->Write(run, placed.offset, std::as_bytes(std::span(groups)).first((std::min)(groups.size() * sizeof(GraphPasses::DispatchGroups), std::size_t(placed.bytes))));
		}
		pending = {};
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
		if (const auto found = imports.find(a_srv); found != imports.end()) {
			found->second->used = Frame();
			return found->second;
		}
		winrt::com_ptr<ID3D11Resource> resource;
		a_srv->GetResource(resource.put());
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		a_srv->GetDesc(&view);
		auto entry = std::make_shared<Import>();
		entry->srv.copy_from(a_srv);
		entry->used = Frame();
		const auto name = std::format("PhysicalSky::CloudFile{}", imports.size());
		const bool plain2D = view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
		if (plain2D) {
			auto texture = resource.try_as<ID3D11Texture2D>();
			entry->texture = texture ? FrameAnchors::ImmutableTexture(name, texture.get(), a_srv) : FrameAnchors::Texture{};
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

	/** @brief Drops imports nobody read lately, with their copies. */
	void PruneImports()
	{
		const auto now = Frame();
		std::erase_if(Imports(), [&](const auto& a_entry) {
			if (now - a_entry.second->used <= kStaleFrames)
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
	if (!Requested() || !RenderGraphRuntime::Get().IsActive())
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
	if (!*twin || !twin->D3D11()) {
		logger::error("[ORG] Physical Sky cloud maps: {} could not be made on the render graph; the clouds cannot render", name);
		return eastl::make_unique<Texture2D>(a_desc, a_name);
	}
	auto* d3d11 = twin->D3D11();
	auto remembered = Remember(twin, d3d11, false, desc.Width, desc.Height, desc.MipLevels);
	// The wrapper takes its own reference. The twin keeps the image.
	d3d11->AddRef();
	// A device whose references do not count the wrapper's (native mode's may not) would make the twin look unused at once.
	// Such a twin is never pruned.
	if (RefCount(d3d11) <= remembered->baseline)
		remembered->baseline = 0;
	auto wrapper = eastl::make_unique<Texture2D>(d3d11, name.c_str());
	wrapper->srv.copy_from(twin->SRV());
	wrapper->uav.copy_from(twin->UAV());
	return wrapper;
}

eastl::unique_ptr<Texture3D> ORGCloudMaps::MakeTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, const char* a_name)
{
	if (!Requested() || !RenderGraphRuntime::Get().IsActive())
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
	if (!*twin || !twin->D3D11()) {
		logger::error("[ORG] Physical Sky cloud maps: {} could not be made on the render graph; the clouds cannot render", name);
		return eastl::make_unique<Texture3D>(a_desc, a_name);
	}
	auto* d3d11 = twin->D3D11();
	auto remembered = Remember(twin, d3d11, false, desc.Width, desc.Height, desc.MipLevels);
	d3d11->AddRef();
	// A device whose references do not count the wrapper's (native mode's may not) would make the twin look unused at once.
	// Such a twin is never pruned.
	if (RefCount(d3d11) <= remembered->baseline)
		remembered->baseline = 0;
	auto wrapper = eastl::make_unique<Texture3D>(d3d11);
	wrapper->srv.copy_from(twin->SRV());
	wrapper->uav.copy_from(twin->UAV());
	return wrapper;
}

bool ORGCloudMaps::Setup(const PhysicalSky& a_feature)
{
	impl.reset();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!runtime.IsActive() || !host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass)) {
		logger::error("[ORG] Physical Sky cloud maps: the render graph or its Prepass anchor is not available; the cloud maps are not generated");
		return false;
	}
	auto next = std::make_shared<Impl>();
	for (std::size_t i = 0; i < kPrograms.size(); ++i) {
		const auto& program = kPrograms[i];
		next->programs[i] = ComputeProgram::Load(host->Device(), { .source = program.source, .entry = program.entry, .constantWords = program.words });
		if (!next->programs[i]) {
			logger::error("[ORG] Physical Sky cloud maps: {} ({}) did not build; the cloud maps are not generated", program.source,
				winrt::to_string(program.entry));
			return false;
		}
	}
	next->mips = GraphPasses::LoadGenerateMips(host->Device(), false);
	D3D11_SAMPLER_DESC wrap{ .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
		.MaxAnisotropy = 1,
		.ComparisonFunc = D3D11_COMPARISON_NEVER,
		.MaxLOD = D3D11_FLOAT32_MAX };
	auto clamp = wrap;
	clamp.AddressU = clamp.AddressV = clamp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	const auto samplerOf = [](ID3D11SamplerState* a_sampler) {
		D3D11_SAMPLER_DESC desc{};
		if (a_sampler)
			a_sampler->GetDesc(&desc);
		return a_sampler ? ShaderBindings::CreateSamplerFromD3D11(desc) : org::OwnedDescriptorBinding{};
	};
	next->samplers = { ShaderBindings::CreateSamplerFromD3D11(wrap), ShaderBindings::CreateSamplerFromD3D11(clamp), samplerOf(a_feature.sampNoise.get()),
		samplerOf(a_feature.sampTr.get()) };
	auto null = DCLF::GpuTextures::Get().NullBinding();
	next->nullIndex = null.index;
	next->nullOwner = std::move(null.owner);
	const bool samplers = std::ranges::all_of(next->samplers, [](const auto& a_sampler) { return static_cast<bool>(a_sampler); });
	if (!next->mips || !samplers || next->nullIndex == DCLF::GpuTextures::kInvalid) {
		logger::error("[ORG] Physical Sky cloud maps: the mip program, the samplers or the null descriptor could not be made; the cloud maps are not generated");
		return false;
	}
	using namespace RenderGraph::Frame;
	using BuiltKey = std::shared_ptr<const Impl::Built>;
	const auto& settings = a_feature.settings;
	const std::weak_ptr<Impl> weak = next;
	next->client = DeclareClient<BuiltKey>(
		kClientId, DependsOn(settings.enabled, settings.enableVolumetricClouds),
		[weak](const Inputs& a_in) -> std::optional<BuiltKey> {
			const auto state = weak.lock();
			const auto& sky = globals::features::physicalSky.settings;
			if (!state || !state->built || !a_in.Get(sky.enabled) || !a_in.Get(sky.enableVolumetricClouds))
				return std::nullopt;
			return state->built;
		},
		[](const BuiltKey& a_built, FrameAnchors::ClientDesc& a_client) {
			a_client = a_built->client;
			// A graph built from now on registers these.
			s_registeredTwins = a_built->twins;
			return true;
		},
		[weak](const BuiltKey& a_built) {
			if (const auto state = weak.lock())
				state->held = a_built;
		});
	if (!next->client) {
		logger::error("[ORG] Physical Sky cloud maps: the client could not be declared; the cloud maps are not generated");
		return false;
	}
	impl = std::move(next);
	logger::info("[ORG] Physical Sky cloud maps: the generators run on the render graph ({}, Prepass)", kClientId);
	return true;
}

bool ORGCloudMaps::IsActive() const
{
	return impl && !impl->failed && RenderGraphRuntime::Get().IsActive();
}

void ORGCloudMaps::BeginFrame()
{
	if (!impl || impl->frame == Frame())
		return;
	impl->frame = Frame();
	// Asked once a frame, so every manager's work this frame meets the same registration: the newest one, once adopted.
	if (!impl->client.Registered())
		impl->held.reset();
	impl->scheduled = impl->built && impl->held == impl->built;
}

bool ORGCloudMaps::Submit(Work a_work)
{
	if (!IsActive())
		return false;
	BeginFrame();
	auto& state = *impl;
	// The steps the work needs, as they would be registered.
	std::vector<StepDesc> needed;
	bool ok = true;
	const auto srvBind = [&](ID3D11ShaderResourceView* a_srv, StepDesc& a_step) {
		if (!a_srv)
			return Bind::Handle(state.nullIndex);
		if (const auto twin = TwinOf(a_srv)) {
			a_step.twins.push_back(twin);
			return Bind::Srv(*twin->resource);
		}
		if (const auto import = ImportOf(a_srv)) {
			a_step.imports.push_back(import);
			return import->array ? Bind::SrvArray(import->texture) : Bind::Srv(import->texture);
		}
		ok = false;
		return Bind::Handle(state.nullIndex);
	};
	const auto twinBind = [&](ID3D11Resource* a_texture, StepDesc& a_step, auto&& a_make) {
		const auto twin = TwinOf(a_texture);
		if (!twin) {
			ok = false;
			return Bind::Handle(state.nullIndex);
		}
		a_step.twins.push_back(twin);
		return a_make(*twin->resource);
	};
	for (const auto& dispatch : a_work.dispatches) {
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
	}
	for (const auto& mips : a_work.mips) {
		StepDesc step{ .name = mips.step, .order = mips.order, .mips = true };
		step.mipsTexture = TwinOf(mips.texture);
		if (!step.mipsTexture) {
			ok = false;
			break;
		}
		step.twins.push_back(step.mipsTexture);
		step.latchBytes = (std::max)(step.mipsTexture->mips, 2u) * static_cast<std::uint32_t>(sizeof(GraphPasses::DispatchGroups));
		needed.push_back(std::move(step));
	}
	if (!ok) {
		// A manager's texture is not a twin, or a file texture cannot be imported. Logged where it happened.
		state.Refuse("a map or file texture is not on the render graph");
		return false;
	}
	// New or changed steps register the client again with the revision after EndFrame. The work waits until it is adopted.
	bool known = true;
	for (auto& step : needed) {
		step.used = Frame();
		auto found = state.steps.find(step.name);
		if (found == state.steps.end() || !SameStep(found->second, step)) {
			state.steps[step.name] = std::move(step);
			state.dirty = true;
			known = false;
		} else
			found->second.used = Frame();
	}
	if (!known) {
		state.Refuse("new steps register the client");
		return false;
	}
	if (!state.scheduled) {
		state.Refuse("the adopted revision does not hold the newest registration yet");
		return false;
	}
	if ((!state.pending.dispatches.empty() || !state.pending.mips.empty()) && state.pendingFrame != Frame()) {
		state.Refuse("an earlier frame's work is not committed yet");
		return false;
	}
	state.pendingFrame = Frame();
	std::ranges::move(a_work.dispatches, std::back_inserter(state.pending.dispatches));
	std::ranges::move(a_work.mips, std::back_inserter(state.pending.mips));
	state.refusal.clear();
	return true;
}

void ORGCloudMaps::EndFrame()
{
	if (!IsActive() || (!impl->dirty && !s_twinsChanged))
		return;
	auto& state = *impl;
	state.dirty = false;
	s_twinsChanged = false;
	PruneTwins();
	PruneImports();
	const auto now = Frame();
	// Steps whose maps are gone, or that nobody asked for lately, are dropped.
	std::erase_if(state.steps, [&](const auto& a_entry) {
		const auto& step = a_entry.second;
		const bool deadTwin = std::ranges::any_of(step.twins, [](const auto& a_twin) { return !Twins().contains(KeyOf(a_twin->d3d11)); });
		const bool deadImport = std::ranges::any_of(step.imports, [](const auto& a_import) { return !Imports().contains(a_import->srv.get()); });
		return deadTwin || deadImport || now - step.used > kStaleFrames;
	});
	if (state.steps.empty())
		return;

	std::vector<const StepDesc*> ordered;
	for (const auto& [name, step] : state.steps)
		ordered.push_back(&step);
	std::ranges::stable_sort(ordered, [](const StepDesc* a_left, const StepDesc* a_right) { return a_left->order < a_right->order; });

	auto registration = std::make_shared<Registration>();
	std::uint32_t entries = 0, bytes = 0;
	for (const auto* step : ordered) {
		Registration::Placed placed{ .offset = bytes, .bytes = step->latchBytes };
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
	if (!registration->gate) {
		logger::error("[ORG] Physical Sky cloud maps: the run gate could not be made; the cloud maps are not generated");
		state.failed = true;
		return;
	}

	namespace AnchorPasses = RenderGraph::AnchorPasses;
	// Every step stays below the cloud renderer's order.
	const auto base = (std::min)(kClientOrder, kRendererOrder - 1 - static_cast<std::int32_t>(ordered.size()));
	AnchorPasses::Builder builder(FrameAnchors::Anchor::Prepass, kClientId, base, registration->latch, registration->gate);
	for (const auto* step : ordered) {
		const auto& placed = registration->steps[step->name];
		if (step->mips) {
			builder.Add(step->name, FrameAnchors::PassKind::Compute,
				[program = state.mips, texture = step->mipsTexture->resource->Graph(), latch = registration->latch, offset = placed.offset]() {
					return GraphPasses::MakeGenerateMips(program, texture, { latch, offset });
				});
		} else {
			auto bindings = step->bindings;
			for (auto& bind : bindings)
				if (bind.kind == Bind::Kind::Latch)
					bind.value = placed.offset;
			builder.Add(AnchorPasses::Dispatch{ .name = step->name,
				.program = state.programs[static_cast<std::size_t>(step->program)],
				.bindings = std::move(bindings),
				.gate = placed.entry });
		}
	}
	if (builder.Failed()) {
		logger::error("[ORG] Physical Sky cloud maps: {}; the cloud maps are not generated", builder.Failure());
		state.failed = true;
		return;
	}
	// The twins the steps use, and those only the cloud renderer reads (the voxel lighting, file copies, the current noise).
	// A twin nothing uses stays out: an idle one of a ping-pong pair would enter the graph with no state to start from.
	std::vector<std::shared_ptr<GraphResources::Resource>> resources;
	std::unordered_set<const Twin*> registeredTwins;
	const auto add = [&](const std::shared_ptr<Twin>& a_twin) {
		if (a_twin && Twins().contains(KeyOf(a_twin->d3d11)) && registeredTwins.insert(a_twin.get()).second)
			resources.push_back(a_twin->resource);
	};
	for (const auto* step : ordered) {
		for (const auto& twin : step->twins)
			add(twin);
		add(step->mipsTexture);
	}
	for (const auto& [_, twin] : s_inputTwins)
		add(twin);

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes = builder.Take();
	client.gates = { registration->gate };
	client.registerResources = [resources = std::move(resources)](org::RenderGraph& a_graph) {
		for (const auto& resource : resources)
			resource->Register(a_graph);
	};
	client.active = [self = impl] { return !self->pending.dispatches.empty() || !self->pending.mips.empty(); };
	client.commit = [self = impl, registration](const FrameAnchors::Point&) { self->Commit(*registration); };
	// The next revision registers it (the client's key).
	auto next = std::make_shared<Impl::Built>();
	next->registration = registration;
	next->client = std::move(client);
	next->twins = std::move(registeredTwins);
	state.built = std::move(next);
	state.client.Invalidate();
	logger::info("[ORG] Physical Sky cloud maps: {} built with {} steps ({} gated dispatches, {} latch bytes) for the next revision", kClientId, ordered.size(), entries,
		bytes);
}

bool ORGCloudInputs::Gather(const PhysicalSky& a_feature, Inputs& a_out)
{
	a_out = {};
	static std::string s_logged;
	s_inputTwins.clear();
	const auto fail = [](const char* a_reason) {
		if (s_logged != a_reason) {
			logger::warn("[ORG] Physical Sky clouds: no clouds this frame ({})", a_reason);
			s_logged = a_reason;
		}
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
	s_logged.clear();
	return true;
}

#else  // !CS_HAS_RENDER_GRAPH

struct ORGCloudMaps::Impl
{};

eastl::unique_ptr<Texture2D> ORGCloudMaps::MakeTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, bool) { return eastl::make_unique<Texture2D>(a_desc, a_name); }
eastl::unique_ptr<Texture3D> ORGCloudMaps::MakeTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, const char* a_name) { return eastl::make_unique<Texture3D>(a_desc, a_name); }
bool ORGCloudMaps::Setup(const PhysicalSky&) { return false; }
bool ORGCloudMaps::IsActive() const { return false; }
bool ORGCloudMaps::Submit(Work) { return false; }
void ORGCloudMaps::BeginFrame() {}
void ORGCloudMaps::EndFrame() {}

#endif
