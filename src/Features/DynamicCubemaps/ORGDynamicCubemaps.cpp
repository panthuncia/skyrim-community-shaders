// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "ORGDynamicCubemaps.h"

#include <winrt/base.h>

#include "Features/DynamicCubemaps.h"
#include "Globals.h"
#include "RenderGraph/Switches.h"

#include "Deferred.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include <ORGModuleServices/Passes/GenerateMipsPass.h>
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/OwnedDescriptorBinding.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/GloballyIndexedResource.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace
{
	constexpr const char* kClientId = "cs.dyncubemaps";
	constexpr const char* kDetectShader = "DynamicCubemaps/DetectCaptureLightingCS.hlsl";
	constexpr const char* kUpdateShader = "DynamicCubemaps/UpdateCubemapCS.hlsl";
	constexpr const char* kInferShader = "DynamicCubemaps/InferCubemapCS.hlsl";
	constexpr const char* kIrradianceShader = "DynamicCubemaps/SpecularIrradianceCS.hlsl";
	constexpr const char* kEncodeShader = "DynamicCubemaps/BC6HEncodeCS.hlsl";
	constexpr const char* kPublishShader = "DynamicCubemaps/PublishCubemapCS.hlsl";
	constexpr const char* kDefaultCubemapName = "DynamicCubemaps::DefaultCubemap";

	// The D3D11 path's names for the same textures.
	constexpr std::array<std::string_view, 2> kEnvNames{ ORGDynamicCubemaps::kEnvId, ORGDynamicCubemaps::kEnvReflectionsId };
	constexpr std::array<std::string_view, 2> kBC6HNames{ ORGDynamicCubemaps::kEnvBC6HId, ORGDynamicCubemaps::kEnvReflectionsBC6HId };

	// As the D3D11 path's MIPLEVELS: 256 x 256 down to 1 x 1.
	constexpr std::uint32_t kMips = 9;
	constexpr std::uint32_t kFaces = 6;
	// After Subsurface Scattering's passes (199-202) at DeferredPasses.
	constexpr std::int32_t kOrder = 300;

	using ShaderBindings::Handle;
	using Groups = org::services::GenerateMipsGroups;

	// The environment's, the reflections', and the reflections' with the sky hidden.
	enum Variant : std::uint32_t
	{
		kBase,
		kReflections,
		kFake,
		kVariants,
	};

	// Push blocks in each shader's list order.
	struct CaptureBlock  // 52 bytes
	{
		SharedConstants::Handles shared;
		Handle dynamicCubemap, raw, position, lightingState, depth, color, sampler;
		ShaderBindings::Latch updateData;
	};
	struct InferBlock  // 36 bytes
	{
		SharedConstants::Handles shared;
		Handle envCapture, reflections, defaultCubemap, inferred, sampler;
	};
	struct IrradianceBlock  // 36 bytes, PUSH struct on a 16-byte boundary
	{
		SharedConstants::Handles shared;
		Handle input, output, sampler, pad;
		float roughness;
	};
	struct EncodeBlock  // 32 bytes, PUSH struct on a 16-byte boundary
	{
		Handle source, output, pad[2];
		std::uint32_t blocksX, blocksY, mip, pad2;
	};
	struct PublishBlock  // 8 bytes
	{
		Handle source, destination;
	};
	static_assert(ShaderBindings::Words<CaptureBlock>() == 13 && ShaderBindings::Words<InferBlock>() == 9 &&
				  ShaderBindings::Words<IrradianceBlock>() == 9 && ShaderBindings::Words<EncodeBlock>() == 8 && ShaderBindings::Words<PublishBlock>() == 2);

	/** @brief The capture constants, then the group counts of every indirect dispatch. The commit writes this frame's step and leaves the rest zero. */
	struct FrameLatch
	{
		DynamicCubemaps::UpdateCubemapCB capture;
		Groups detect[2];
		Groups update[kVariants];
		Groups captureMips[2][kMips - 1];
		Groups infer[kVariants];
		Groups inferredMips[kMips - 1];
		Groups irradiance[kMips - 1];  // mips 1 to 8
		Groups encode[2][kMips];
		Groups publish[2][kMips];
	};
	static_assert(sizeof(DynamicCubemaps::UpdateCubemapCB) == 32 && sizeof(Groups) == 16 && offsetof(FrameLatch, detect) == 32);

	constexpr std::uint32_t GroupsOffset(std::size_t a_fieldOffset, std::uint32_t a_index) { return static_cast<std::uint32_t>(a_fieldOffset) + a_index * sizeof(Groups); }

	/** @brief Owns its indirect dispatch signature. */
	using Program = RenderGraph::Programs::Fixed<ComputeProgram>;

	struct Programs
	{
		Program detect;
		std::array<Program, kVariants> update, infer;
		Program irradiance, encode, publish;
		RenderGraph::Programs::Fixed<ComputeProgram> mips;
	};

	/** @brief ComputeProgram adds the defines every D3D11 build gets, so both paths build one permutation. */
	template <class Block>
	Program QueueProgram(const char* a_source, const wchar_t* a_define = nullptr)
	{
		ComputeProgram::Desc desc{ .source = a_source, .constantWords = ShaderBindings::Words<Block>() };
		if (a_define)
			desc.defines.push_back({ a_define, L"" });
		return RenderGraph::Programs::Queue("DynamicCubemaps", std::move(desc));
	}

	/** @brief As DynamicCubemaps' GetComputeShader* compile them, plus OMS's GenerateMips for arrays and cubes. Queued at setup. */
	std::shared_ptr<const Programs> QueuePrograms()
	{
		auto programs = std::make_shared<Programs>();
		programs->detect = QueueProgram<CaptureBlock>(kDetectShader);
		programs->update[kBase] = QueueProgram<CaptureBlock>(kUpdateShader);
		programs->update[kReflections] = QueueProgram<CaptureBlock>(kUpdateShader, L"REFLECTIONS");
		programs->update[kFake] = QueueProgram<CaptureBlock>(kUpdateShader, L"FAKEREFLECTIONS");
		programs->infer[kBase] = QueueProgram<InferBlock>(kInferShader);
		programs->infer[kReflections] = QueueProgram<InferBlock>(kInferShader, L"REFLECTIONS");
		programs->infer[kFake] = QueueProgram<InferBlock>(kInferShader, L"FAKEREFLECTIONS");
		programs->irradiance = QueueProgram<IrradianceBlock>(kIrradianceShader);
		programs->encode = QueueProgram<EncodeBlock>(kEncodeShader);
		programs->publish = QueueProgram<PublishBlock>(kPublishShader);
		programs->mips = RenderGraph::Programs::Queue("DynamicCubemaps", ComputeProgram::GenerateMips(true));
		return programs;
	}

	// The default view (UINT32_MAX) or a variant, at a mip.
	struct ViewDesc
	{
		std::uint32_t variant = UINT32_MAX;
		std::uint32_t mip = 0;
	};

	constexpr std::uint32_t kUavArray = static_cast<std::uint32_t>(org::UAVViewType::Texture2DArrayFull);
	constexpr std::uint32_t kSrvArray = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull);

	struct Use
	{
		FrameAnchors::Texture resource;  // an import's slot, or a twin's resource
		bool write = false;
		std::vector<ViewDesc> views{ ViewDesc{} };
	};

	struct Word
	{
		enum class Kind : std::uint8_t
		{
			Literal,
			View,    // value is the view's index, in declaration order
			Shared,  // four words
			Latch,   // two words, value is the byte offset in the region
		};
		Kind kind = Kind::Literal;
		std::uint32_t value = 0;
	};

	Word Literal(std::uint32_t a_value) { return { Word::Kind::Literal, a_value }; }
	Word View(std::uint32_t a_index) { return { Word::Kind::View, a_index }; }
	Word Shared() { return { Word::Kind::Shared, 0 }; }
	Word LatchAt(std::uint32_t a_offset) { return { Word::Kind::Latch, a_offset }; }

	struct Dispatch
	{
		const Program* program = nullptr;
		std::vector<Word> block;
		std::uint32_t groups = 0;
		bool barrier = false;
	};

	/** @brief Fixed from the build on, so its recording is the same every frame. */
	struct Plan
	{
		std::shared_ptr<const Programs> programs;  // keeps the dispatches' programs
		std::shared_ptr<const org::LatchBlock> latch;
		std::vector<Use> uses;
		std::vector<Dispatch> dispatches;
		bool shared = false;
	};

	struct GatedBindings
	{
		SharedConstants::Bindings shared;
		std::vector<org::DeclaredViewToken> views;
	};

	struct GatedFrame
	{
		std::shared_ptr<const Plan> plan;
	};

	/** @brief Group counts come from the latch. The commit zeroes those not needed this frame, so the recording stays the same. */
	class GatedComputePass final : public org::TypedRenderGraphPass<GatedComputePass, GatedFrame, GatedBindings>
	{
	public:
		explicit GatedComputePass(std::shared_ptr<const Plan> a_plan) :
			plan(std::move(a_plan)) {}

		GatedBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			GatedBindings bindings;
			if (plan->shared)
				bindings.shared = SharedConstants::Declare(a_builder);
			for (const auto& use : plan->uses) {
				if (use.write) {
					std::vector<org::UavView> views;
					for (const auto& view : use.views)
						views.push_back({ view.variant, view.mip, 0 });
					const auto declared = a_builder.UnorderedAccess(use.resource, std::span<const org::UavView>(views));
					bindings.views.insert(bindings.views.end(), declared.views.begin(), declared.views.end());
				} else {
					std::vector<org::SrvView> views;
					for (const auto& view : use.views)
						views.push_back({ view.variant, view.mip, 0 });
					const auto declared = a_builder.ShaderResource(use.resource, std::span<const org::SrvView>(views));
					bindings.views.insert(bindings.views.end(), declared.views.begin(), declared.views.end());
				}
			}
			return bindings;
		}

		// Everything the recording depends on is fixed with the plan. The frame's work is in the latch.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(plan.get())); }

		GatedFrame Prepare(const GatedBindings&, const org::PassPrepareContext&) const { return { plan }; }

		static void Record(const GatedBindings& a_bindings, const GatedFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto* current = a_frame.plan.get();
			if (!current || !current->latch)
				return;
			const org::RunRef run = a_recording.Run();
			const auto shared = current->shared ? SharedConstants::Index(a_recording, a_bindings.shared) : SharedConstants::Handles{};
			std::array<std::uint32_t, ShaderBindings::kMaxPushWords> words{};
			for (const auto& dispatch : current->dispatches) {
				std::uint32_t count = 0;
				const auto push = [&](std::uint32_t a_word) {
					if (count < words.size())
						words[count] = a_word;
					++count;
				};
				for (const auto& word : dispatch.block) {
					switch (word.kind) {
					case Word::Kind::Literal:
						push(word.value);
						break;
					case Word::Kind::View:
						push(a_recording.Index(a_bindings.views[word.value]));
						break;
					case Word::Kind::Shared:
						push(shared.frameBuffer);
						push(shared.sharedData);
						push(shared.featureData);
						push(shared.permutation);
						break;
					case Word::Kind::Latch:
						{
							const auto latch = ShaderBindings::LatchOf(*current->latch, run, word.value);
							push(latch.handle);
							push(latch.offset);
							break;
						}
					}
				}
				const auto program = dispatch.program->Get();
				const auto& compute = *program;
				// A block that does not match the program's records nothing. This is a programming error.
				if (count != compute.constantWords)
					continue;
				if (dispatch.barrier)
					a_recording.UavBarrier();
				compute.DispatchIndirect(a_recording, std::span<const std::uint32_t>(words.data(), count), *current->latch, dispatch.groups);
			}
		}

	private:
		std::shared_ptr<const Plan> plan;
	};

	struct CopyRegion
	{
		std::uint32_t source = 0, sourceMip = 0, sourceSlice = 0;
		std::uint32_t target = 0, targetMip = 0, targetSlice = 0;
	};

	struct CopyPlan
	{
		std::vector<std::shared_ptr<org::Resource>> sources, targets;
		std::vector<CopyRegion> regions;
	};

	struct CopyBindings
	{
		std::vector<org::ResourceBindingToken> sources, targets;
	};

	// A commit opens a copy's entry while it has a new source to take.
	constexpr std::uint32_t kFilteredMipZeroEntry = 0;
	constexpr std::uint32_t kBC6HEntry = 1;  // + the cubemap
	constexpr std::uint32_t kCopyEntries = 3;
	constexpr std::uint32_t kGateEntries = kCopyEntries;

	// Set by a commit when a pass writes the copy's source, cleared when the copy records.
	using CopyDirty = std::array<std::atomic<bool>, kCopyEntries>;

	struct CopyFrame
	{
		std::shared_ptr<const CopyPlan> plan;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t entry = 0;
		std::shared_ptr<CopyDirty> dirty;
	};

	/** @brief Copies in the runs after their source changed. Only for copies whose result stays the same between the frames that need them. */
	class CopyPass final : public org::TypedRenderGraphPass<CopyPass, CopyFrame, CopyBindings>
	{
	public:
		CopyPass(std::shared_ptr<const CopyPlan> a_plan, std::shared_ptr<const RenderGraph::RunGate> a_gate, std::uint32_t a_entry,
			std::shared_ptr<CopyDirty> a_dirty) :
			plan(std::move(a_plan)), gate(std::move(a_gate)), entry(a_entry), dirty(std::move(a_dirty)) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			CopyBindings bindings;
			for (const auto& source : plan->sources)
				bindings.sources.push_back(a_builder.CopySource(source));
			for (const auto& target : plan->targets)
				bindings.targets.push_back(a_builder.CopyDestination(target));
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(plan.get())); }

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { plan, gate, entry, dirty }; }

		// A kept late batch keeps its list while SkipsAtExecution's answer holds.
		static constexpr bool kRecordKeyedBySkip = true;

		// A run that left the gate entry closed records only barriers.
		static bool SkipsAtExecution(const CopyBindings&, const CopyFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return a_frame.gate && !a_frame.gate->IsOpen(a_recording.Run(), a_frame.entry);
		}

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.plan)
				return;
			auto& commands = a_recording.Commands();
			// The whole source subresource. For the encoder that is its texels, which are the BC6H blocks.
			for (const auto& region : a_frame.plan->regions)
				commands.CopyTextureRegion(
					{ .texture = a_recording.Resolve(a_bindings.targets[region.target]).GetHandle(), .mip = region.targetMip, .arraySlice = region.targetSlice },
					{ .texture = a_recording.Resolve(a_bindings.sources[region.source]).GetHandle(), .mip = region.sourceMip, .arraySlice = region.sourceSlice });
			if (a_frame.dirty)
				(*a_frame.dirty)[a_frame.entry].store(false, std::memory_order_release);
		}

	private:
		std::shared_ptr<const CopyPlan> plan;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t entry = 0;
		std::shared_ptr<CopyDirty> dirty;
	};

	struct Imports
	{
		FrameAnchors::Texture depth, main, reflections, defaultCubemap;

		bool operator==(const Imports&) const = default;
		explicit operator bool() const { return depth && main && reflections && defaultCubemap; }
	};

	/** @brief What a registration is made for: the reflection cubemap's edge, and the imports the last update found. */
	struct ClientKey
	{
		std::uint32_t size = 0;
		Imports imports;

		bool operator==(const ClientKey&) const = default;
	};


	/** @brief a_main is the capture's colour source: the forward pass's main target, as upstream's D3D11 capture reads. */
	Imports Import(std::uint32_t a_main)
	{
		Imports imports;
		imports.depth = FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY));
		imports.main = FrameAnchors::EngineTexture(FrameAnchors::Colour(a_main));
		imports.reflections = FrameAnchors::EngineTexture(FrameAnchors::Cubemap(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS));
		if (auto* view = globals::features::dynamicCubemaps.defaultCubemap) {
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			if (auto texture = resource.try_as<ID3D11Texture2D>())
				imports.defaultCubemap = FrameAnchors::ImportTexture(fmt::format("cs.d3d11.{}", kDefaultCubemapName), { .texture = texture.get(), .srv = view, .immutable = true });
		}
		return imports;
	}

	D3D11_TEXTURE2D_DESC TextureDesc(UINT a_size, UINT a_mips, DXGI_FORMAT a_format, UINT a_bindFlags, bool a_cube)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_size;
		desc.Height = a_size;
		desc.MipLevels = a_mips;
		desc.ArraySize = kFaces;
		desc.Format = a_format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = a_bindFlags;
		desc.MiscFlags = a_cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;
		return desc;
	}
}

struct ORGDynamicCubemaps::Impl
{
	UINT size = 0;  // mip 0, the engine's reflections cubemap's size
	// Built at setup. A shader reload swaps their pipelines (ComputeProgram's live slot).
	std::shared_ptr<const Programs> programs;
	org::OwnedDescriptorBinding sampler;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> copyGate;  // opened while their source is new
	// The filtered mip copy runs once before the first inference. The BC6H copies wait for their first encode.
	std::shared_ptr<CopyDirty> copyDirty = [] {
		auto dirty = std::make_shared<CopyDirty>();
		for (std::uint32_t entry = 0; entry < kCopyEntries; ++entry)
			(*dirty)[entry].store(entry == kFilteredMipZeroEntry);
		return dirty;
	}();
	// The capture's colour source, as the last update found it: Deferred's forward target, which the engine binds. Not a variable
	// yet (B2's interim). The key reads it and imports it, so no import is made mid-frame. kMAIN until the first update.
	RenderGraph::Frame::Observed<std::uint32_t> captureMain{ "DynamicCubemaps.CaptureMain" };

	/** @brief The capture's colour source a revision carries. */
	std::uint32_t CaptureMain(const RenderGraph::Frame::Inputs& a_in) const
	{
		const auto* main = captureMain.Get(a_in);
		return main ? *main : static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN);
	}
	RenderGraph::Frame::DeclaredClient client;

	// Graph-only and read in later frames. The filtered one is a plain array, read through array views.
	std::array<std::unique_ptr<GraphResources::Texture2D>, 2> capture, raw, position;
	std::unique_ptr<GraphResources::StructuredBuffer> lightingState;
	std::unique_ptr<GraphResources::Texture2D> inferred, filtered;
	// Kept for the copy that follows every frame.
	std::array<std::array<std::unique_ptr<GraphResources::Texture2D>, kMips>, 2> scratch;
	// Exported, so their D3D11 wrap zeroes them when they are made: black until the first publish, like the D3D11 path's.
	std::array<std::unique_ptr<GraphResources::Texture2D>, 2> env, bc6h;

	// A cubemap counts as valid from the run of its first encode, whose BC6H copy follows the encode in that run.
	std::array<bool, 2> encoded{};

	/** @brief The registration for a new key, when a revision starts. A nested class, so it reads a_self's impl. */
	static bool MakeClient(ORGDynamicCubemaps* a_self, const ClientKey& a_key, FrameAnchors::ClientDesc& a_client);

	bool Complete() const
	{
		const auto made = [](const auto& a_resource) { return a_resource && static_cast<bool>(*a_resource); };
		for (std::uint32_t i = 0; i < 2; ++i) {
			if (!made(capture[i]) || !made(raw[i]) || !made(position[i]) || !made(env[i]) || !made(bc6h[i]) || !env[i]->SRV() || !bc6h[i]->SRV())
				return false;
			if (!std::ranges::all_of(scratch[i], made))
				return false;
		}
		return made(lightingState) && made(inferred) && made(filtered);
	}

	void Register(org::RenderGraph& a_graph) const
	{
		for (std::uint32_t i = 0; i < 2; ++i) {
			capture[i]->Register(a_graph);
			raw[i]->Register(a_graph);
			position[i]->Register(a_graph);
			env[i]->Register(a_graph);
			bc6h[i]->Register(a_graph);
			for (const auto& texture : scratch[i])
				texture->Register(a_graph);
		}
		lightingState->Register(a_graph);
		inferred->Register(a_graph);
		filtered->Register(a_graph);
	}

	/** @brief Each cubemap runs only on its capture frames. */
	std::shared_ptr<const Plan> CapturePlan(const Imports& a_imports) const
	{
		auto plan = std::make_shared<Plan>();
		plan->programs = programs;
		plan->latch = latch;
		plan->shared = true;
		for (std::uint32_t i = 0; i < 2; ++i) {
			plan->uses.push_back({ capture[i]->Graph(), true, { { kUavArray, 0 } } });
			plan->uses.push_back({ raw[i]->Graph(), true, { { kUavArray, 0 } } });
			plan->uses.push_back({ position[i]->Graph(), true, { { kUavArray, 0 } } });
		}
		plan->uses.push_back({ lightingState->Graph(), true });
		plan->uses.push_back({ a_imports.depth, false });
		plan->uses.push_back({ a_imports.main, false });
		const auto block = [&](std::uint32_t a_cubemap) {
			return std::vector<Word>{ Shared(), View(a_cubemap * 3), View(a_cubemap * 3 + 1), View(a_cubemap * 3 + 2), View(6), View(7), View(8),
				Literal(sampler.Index()), LatchAt(offsetof(FrameLatch, capture)) };
		};
		// Detection writes the lighting state the capture reads, and the capture writes what the next detection reads.
		plan->dispatches.push_back({ &programs->detect, block(0), GroupsOffset(offsetof(FrameLatch, detect), 0), false });
		plan->dispatches.push_back({ &programs->update[kBase], block(0), GroupsOffset(offsetof(FrameLatch, update), kBase), true });
		plan->dispatches.push_back({ &programs->detect, block(1), GroupsOffset(offsetof(FrameLatch, detect), 1), true });
		plan->dispatches.push_back({ &programs->update[kReflections], block(1), GroupsOffset(offsetof(FrameLatch, update), kReflections), true });
		plan->dispatches.push_back({ &programs->update[kFake], block(1), GroupsOffset(offsetof(FrameLatch, update), kFake), true });
		return plan;
	}

	std::shared_ptr<const Plan> InferPlan(const Imports& a_imports) const
	{
		auto plan = std::make_shared<Plan>();
		plan->programs = programs;
		plan->latch = latch;
		plan->shared = true;
		// Cube views of every mip.
		plan->uses.push_back({ capture[0]->Graph(), false });
		plan->uses.push_back({ capture[1]->Graph(), false });
		plan->uses.push_back({ a_imports.reflections, false });
		plan->uses.push_back({ a_imports.defaultCubemap, false });
		plan->uses.push_back({ inferred->Graph(), true, { { kUavArray, 0 } } });
		for (std::uint32_t variant = 0; variant < kVariants; ++variant) {
			const std::vector<Word> block{ Shared(), View(variant == kBase ? 0u : 1u), View(2), View(3), View(4), Literal(sampler.Index()) };
			plan->dispatches.push_back({ &programs->infer[variant], block, GroupsOffset(offsetof(FrameLatch, infer), variant), variant != kBase });
		}
		return plan;
	}

	/** @brief Mips 1 to 8, spread over frames by the state machine. */
	std::shared_ptr<const Plan> IrradiancePlan() const
	{
		auto plan = std::make_shared<Plan>();
		plan->programs = programs;
		plan->latch = latch;
		plan->shared = true;
		plan->uses.push_back({ inferred->Graph(), false });
		Use output{ filtered->Graph(), true, {} };
		for (std::uint32_t mip = 1; mip < kMips; ++mip)
			output.views.push_back({ kUavArray, mip });
		plan->uses.push_back(std::move(output));
		// As the D3D11 path, mip / (mips - 1).
		const float deltaRoughness = 1.0f / static_cast<float>(kMips - 1);
		for (std::uint32_t mip = 1; mip < kMips; ++mip) {
			const float roughness = static_cast<float>(mip) * deltaRoughness;
			const std::vector<Word> block{ Shared(), View(0), View(mip), Literal(sampler.Index()), Literal(0), Literal(std::bit_cast<std::uint32_t>(roughness)) };
			plan->dispatches.push_back({ &programs->irradiance, block, GroupsOffset(offsetof(FrameLatch, irradiance), mip - 1), false });
		}
		return plan;
	}

	std::shared_ptr<const Plan> EncodePlan() const
	{
		auto plan = std::make_shared<Plan>();
		plan->programs = programs;
		plan->latch = latch;
		// The D3D11 path's Texture2DArray view. Load takes the mip.
		plan->uses.push_back({ filtered->Graph(), false, { { kSrvArray, 0 } } });
		for (std::uint32_t i = 0; i < 2; ++i)
			for (const auto& texture : scratch[i])
				plan->uses.push_back({ texture->Graph(), true, { { kUavArray, 0 } } });
		for (std::uint32_t i = 0; i < 2; ++i)
			for (std::uint32_t mip = 0; mip < kMips; ++mip) {
				const std::uint32_t blocks = (std::max)((std::max)(size >> mip, 1u) / 4, 1u);
				const std::vector<Word> block{ View(0), View(1 + i * kMips + mip), Literal(0), Literal(0), Literal(blocks), Literal(blocks), Literal(mip), Literal(0) };
				plan->dispatches.push_back({ &programs->encode, block, GroupsOffset(offsetof(FrameLatch, encode), i * kMips + mip), false });
			}
		return plan;
	}

	/** @brief The D3D11 path's CopyResource. */
	std::shared_ptr<const Plan> PublishPlan() const
	{
		auto plan = std::make_shared<Plan>();
		plan->programs = programs;
		plan->latch = latch;
		Use source{ filtered->Graph(), false, {} };
		for (std::uint32_t mip = 0; mip < kMips; ++mip)
			source.views.push_back({ kSrvArray, mip });
		plan->uses.push_back(std::move(source));
		for (std::uint32_t i = 0; i < 2; ++i) {
			Use target{ env[i]->Graph(), true, {} };
			for (std::uint32_t mip = 0; mip < kMips; ++mip)
				target.views.push_back({ kUavArray, mip });
			plan->uses.push_back(std::move(target));
		}
		for (std::uint32_t i = 0; i < 2; ++i)
			for (std::uint32_t mip = 0; mip < kMips; ++mip)
				plan->dispatches.push_back({ &programs->publish, { View(mip), View(kMips + i * kMips + mip) },
					GroupsOffset(offsetof(FrameLatch, publish), i * kMips + mip), false });
		return plan;
	}

	/** @brief After an inference. The result is the same between inference frames. */
	std::shared_ptr<const CopyPlan> FilteredMipZeroPlan() const
	{
		auto plan = std::make_shared<CopyPlan>();
		plan->sources.push_back(inferred->Graph());
		plan->targets.push_back(filtered->Graph());
		for (std::uint32_t face = 0; face < kFaces; ++face)
			plan->regions.push_back({ 0, 0, face, 0, 0, face });
		return plan;
	}

	/** @brief After a publish. The result is the same between publish frames. */
	std::shared_ptr<const CopyPlan> BC6HPlan(std::uint32_t a_cubemap) const
	{
		auto plan = std::make_shared<CopyPlan>();
		for (const auto& texture : scratch[a_cubemap])
			plan->sources.push_back(texture->Graph());
		plan->targets.push_back(bc6h[a_cubemap]->Graph());
		for (std::uint32_t mip = 0; mip < kMips; ++mip)
			for (std::uint32_t face = 0; face < kFaces; ++face)
				plan->regions.push_back({ mip, 0, face, 0, mip, face });
		return plan;
	}

	/** @brief Runs the state machine and writes this frame's group counts and capture constants. */
	void Commit()
	{
		auto& feature = globals::features::dynamicCubemaps;
		FrameLatch data{};
		feature.BeginUpdate();
		const auto step = feature.NextUpdateStep();
		const std::uint32_t cubemap = step.reflections ? 1 : 0;
		// The D3D11 path's ceil(size / 8) groups on each face.
		const std::uint32_t faceGroups = (size + 7) / 8;
		if (step.capture) {
			(*copyDirty)[kFilteredMipZeroEntry].store(true, std::memory_order_release);
			const Variant variant = !step.reflections ? kBase : (feature.fakeReflections ? kFake : kReflections);
			data.capture = feature.NextCaptureConstants(step.reflections);
			data.detect[cubemap] = { 1, 1, 1, 0 };
			data.update[variant] = { faceGroups, faceGroups, kFaces, 0 };
			data.infer[variant] = { faceGroups, faceGroups, kFaces, 0 };
			const auto mips = org::services::PlanGenerateMipsGroups(size, size, kMips, kFaces);
			std::ranges::copy(mips, std::begin(data.captureMips[cubemap]));
			std::ranges::copy(mips, std::begin(data.inferredMips));
		}
		for (std::uint32_t mip = step.irradianceStart; mip < step.irradianceEnd; ++mip) {
			const std::uint32_t groups = (std::max)(((size >> mip) + 7) / 8, 1u);
			data.irradiance[mip - 1] = { groups, groups, kFaces, 0 };
		}
		if (step.publish) {
			for (std::uint32_t mip = 0; mip < kMips; ++mip) {
				const std::uint32_t blocks = (std::max)((std::max)(size >> mip, 1u) / 4, 1u);
				const std::uint32_t encodeGroups = (std::max)((blocks + 7) / 8, 1u);
				const std::uint32_t publishGroups = ((std::max)(size >> mip, 1u) + 7) / 8;
				data.encode[cubemap][mip] = { encodeGroups, encodeGroups, kFaces, 0 };
				data.publish[cubemap][mip] = { publishGroups, publishGroups, kFaces, 0 };
			}
			encoded[cubemap] = true;
			(*copyDirty)[kBC6HEntry + cubemap].store(true, std::memory_order_release);
		}
		// The BC6H copy is in the graph from the start and runs after this run's encode.
		for (std::uint32_t i = 0; i < 2; ++i)
			if (encoded[i])
				feature.cubemapValid[i] = true;
		latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, data);
		if (copyGate)
			for (std::uint32_t entry = 0; entry < kCopyEntries; ++entry)
				if ((*copyDirty)[entry].load(std::memory_order_acquire))
					copyGate->OpenDispatch(entry, 1);
	}
};

ORGDynamicCubemaps::ORGDynamicCubemaps() = default;
ORGDynamicCubemaps::~ORGDynamicCubemaps() = default;

bool ORGDynamicCubemaps::Setup()
{
	// Once a session. The cubemaps are published, so they are never torn down.
	if (!BeginSetup())
		return IsActive();
	try {
		auto state = std::make_unique<Impl>();
		auto& cubemap = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
		if (!cubemap.texture)
			Fatal("the engine has no reflections cubemap");
		D3D11_TEXTURE2D_DESC engine{};
		cubemap.texture->GetDesc(&engine);
		state->size = engine.Width;
		if (engine.Width != engine.Height || engine.Width != (1u << (kMips - 1)))
			Fatal(fmt::format("the engine's reflections cubemap is {}x{}, not the {} mips the passes are made for", engine.Width, engine.Height, kMips));

		state->programs = QueuePrograms();
		// The D3D11 path's compute sampler, linear and wrapped.
		state->sampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearWrap);

		const UINT size = state->size;
		constexpr UINT kReadWrite = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		const GraphResources::Options history{ .lifetime = GraphResources::Lifetime::History };
		// Readers still use D3D11: the composite, IBL, the engine's draws and DCLF's reflection faces.
		const GraphResources::Options exportedOptions{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV };
		constexpr std::array<const char*, 2> kCaptureNames{ "DynamicCubemaps::EnvCapture", "DynamicCubemaps::EnvCaptureReflections" };
		constexpr std::array<const char*, 2> kRawNames{ "DynamicCubemaps::EnvCaptureRaw", "DynamicCubemaps::EnvCaptureRawReflections" };
		constexpr std::array<const char*, 2> kPositionNames{ "DynamicCubemaps::EnvCapturePosition", "DynamicCubemaps::EnvCapturePositionReflections" };
		constexpr std::array<const char*, 2> kScratchNames{ "DynamicCubemaps::BC6HScratch", "DynamicCubemaps::BC6HScratchReflections" };
		for (std::uint32_t i = 0; i < 2; ++i) {
			state->capture[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_R16G16B16A16_FLOAT, kReadWrite, true), kCaptureNames[i], history);
			// Only mip 0, since nothing reads the rest of the D3D11 path's chain.
			state->raw[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, kReadWrite, false), kRawNames[i], history);
			state->position[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, kReadWrite, false), kPositionNames[i], history);
			state->env[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_R11G11B10_FLOAT, kReadWrite, true),
				std::string(kEnvNames[i]), exportedOptions);
			state->bc6h[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_BC6H_UF16, D3D11_BIND_SHADER_RESOURCE, true),
				std::string(kBC6HNames[i]), exportedOptions);
			for (std::uint32_t mip = 0; mip < kMips; ++mip) {
				// One uint4 block per 4x4 texels. Mips under 4x4 still take one block.
				const UINT blocks = (std::max)((std::max)(size >> mip, 1u) / 4, 1u);
				state->scratch[i][mip] = std::make_unique<GraphResources::Texture2D>(TextureDesc(blocks, 1, DXGI_FORMAT_R32G32B32A32_UINT, kReadWrite, false),
					fmt::format("{} mip{}", kScratchNames[i], mip), history);
			}
		}
		state->inferred = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_R11G11B10_FLOAT, kReadWrite, true), "DynamicCubemaps::EnvInferred", history);
		state->filtered = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_R11G11B10_FLOAT, kReadWrite, false), "DynamicCubemaps::EnvFiltered", history);
		// Needs no first clear, since each cubemap's first capture resets and writes every field the shaders read.
		D3D11_BUFFER_DESC stateDesc{};
		stateDesc.ByteWidth = sizeof(DynamicCubemaps::CaptureLightingState);
		stateDesc.Usage = D3D11_USAGE_DEFAULT;
		stateDesc.BindFlags = kReadWrite;
		stateDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		stateDesc.StructureByteStride = sizeof(DynamicCubemaps::CaptureLightingState);
		state->lightingState = std::make_unique<GraphResources::StructuredBuffer>(stateDesc, 1, "DynamicCubemaps::CaptureLightingState", history);
		state->latch = std::make_shared<org::LatchBlock>("cs.dyncubemaps.latch", static_cast<std::uint32_t>(sizeof(FrameLatch)));
		if (!state->sampler || !state->Complete())
			Fatal("its sampler or textures could not be made");
		// The engine made its targets just before CS's setup, and the feature loaded its default cubemap.
		// Deferred has not chosen the forward targets yet; the first update moves the import to its main target.
		if (const auto imports = Import(RE::RENDER_TARGETS::kMAIN); !imports)
			Fatal(fmt::format("its inputs cannot be imported (depth {}, main {}, reflections {}, default cubemap {})", imports.depth != nullptr,
				imports.main != nullptr, imports.reflections != nullptr, imports.defaultCubemap != nullptr));
		state->copyGate = RenderGraph::RunGate::Create("cs.dyncubemaps.copies", kGateEntries);
		impl = std::move(state);
		// Declared on the reflection cubemap. Its first registration is at the first revision after this; the anchors then hold
		// it until the shared constants exist.
		auto* self = this;
		impl->client = RenderGraph::Frame::DeclareClient<ClientKey>(
			kClientId, RenderGraph::Frame::DependsOn(RenderGraph::Frame::Vars::ReflectionCubemap(), impl->captureMain.Var()),
			[self](const RenderGraph::Frame::Inputs& a_in) -> std::optional<ClientKey> {
				const auto& current = self->impl;
				if (!current)
					return std::nullopt;
				// Zero until EngineTargets raised it: the cubemap the textures were made for.
				const auto cubemap = a_in.Get(RenderGraph::Frame::Vars::ReflectionCubemap());
				// Imported here, when the revision starts, so a new texture joins the revision.
				const auto main = current->CaptureMain(a_in);
				auto imports = Import(main);
				if (!imports)
					RenderGraph::Fatal(kName, fmt::format("its inputs cannot be imported (main target {})", main));
				return ClientKey{ cubemap.size ? cubemap.size : current->size, std::move(imports) };
			},
			[self](const ClientKey& a_key, FrameAnchors::ClientDesc& a_client) { return Impl::MakeClient(self, a_key, a_client); });
		for (std::uint32_t i = 0; i < 2; ++i)
			if (!PublishedResources::Publish(*impl->env[i]) || !PublishedResources::Publish(*impl->bc6h[i]))
				Fatal("the cubemaps could not be published");
		logger::info("[ORG] Dynamic Cubemaps: the cubemaps update on the render graph at DeferredPasses ({}x{}, {} mips); the client follows the frame revisions",
			size, size, kMips);
		return true;
	} catch (const std::exception& e) {
		Fatal(fmt::format("setup threw: {}", e.what()));
	}
}

bool ORGDynamicCubemaps::Impl::MakeClient(ORGDynamicCubemaps* a_self, const ClientKey& a_key, FrameAnchors::ClientDesc& a_client)
{
	auto* self = a_self;
	const auto& made = self->impl;
	if (!made)
		return false;
	// The passes are made for a cubemap of kMips mips. The engine makes its targets once a session (Q4), so another size is an error.
	if (a_key.size != made->size)
		Fatal(fmt::format("the engine's reflection cubemap is now {0}x{0}; the passes are made for {1}x{1}", a_key.size, made->size));
	const auto gated = [self](std::shared_ptr<const Plan> (Impl::*a_plan)() const) {
		return [self, a_plan]() -> std::shared_ptr<org::RenderPass> {
			const auto& current = self->impl;
			return current ? std::make_shared<GatedComputePass>(((*current).*a_plan)()) : nullptr;
		};
	};
	const auto withImports = [self, imports = a_key.imports](std::shared_ptr<const Plan> (Impl::*a_plan)(const Imports&) const) {
		return [self, a_plan, imports]() -> std::shared_ptr<org::RenderPass> {
			const auto& current = self->impl;
			return current ? std::make_shared<GatedComputePass>(((*current).*a_plan)(imports)) : nullptr;
		};
	};
	const auto mips = [self](bool a_inferred, std::uint32_t a_cubemap) {
		return [self, a_inferred, a_cubemap]() -> std::shared_ptr<org::RenderPass> {
			const auto& current = self->impl;
			if (!current)
				return nullptr;
			const auto offset = a_inferred ? GroupsOffset(offsetof(FrameLatch, inferredMips), 0) : GroupsOffset(offsetof(FrameLatch, captureMips), a_cubemap * (kMips - 1));
			return org::services::MakeGenerateMipsPass({ current->programs->mips.Get(), true }, (a_inferred ? current->inferred : current->capture[a_cubemap])->Graph(),
				{ current->latch, offset });
		};
	};
	// In the graph from the start. Its entry opens only after an encode wrote the scratch.
	const auto bc6hCopy = [self](std::uint32_t a_cubemap) {
		return [self, a_cubemap]() -> std::shared_ptr<org::RenderPass> {
			const auto& current = self->impl;
			return current ? std::make_shared<CopyPass>(current->BC6HPlan(a_cubemap), current->copyGate, kBC6HEntry + a_cubemap, current->copyDirty) : nullptr;
		};
	};
	a_client.id = kClientId;
	a_client.sharedConstants = true;
	a_client.gates = { made->copyGate };
	// Gated so the anchor can run in a frame phase. Dispatch counts are zero in runs the commit did not write, and copies skip at execution.
	const auto pass = [&a_client](const char* a_name, std::int32_t a_order, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
		a_client.passes.push_back({ .name = a_name,
			.anchor = FrameAnchors::Anchor::DeferredPasses,
			.order = kOrder + a_order,
			.kind = a_kind,
			.make = std::move(a_make),
			.atExecution = a_kind == FrameAnchors::PassKind::Copy });
	};
	using Kind = FrameAnchors::PassKind;
	pass("cs.dyncubemaps.capture", 0, Kind::Compute, withImports(&Impl::CapturePlan));
	pass("cs.dyncubemaps.capture-mips", 1, Kind::Compute, mips(false, 0));
	pass("cs.dyncubemaps.capture-mips-reflections", 2, Kind::Compute, mips(false, 1));
	pass("cs.dyncubemaps.infer", 3, Kind::Compute, withImports(&Impl::InferPlan));
	pass("cs.dyncubemaps.filtered-mip0", 4, Kind::Copy, [self]() -> std::shared_ptr<org::RenderPass> {
		return self->impl ? std::make_shared<CopyPass>(self->impl->FilteredMipZeroPlan(), self->impl->copyGate, kFilteredMipZeroEntry, self->impl->copyDirty) : nullptr;
	});
	pass("cs.dyncubemaps.inferred-mips", 5, Kind::Compute, mips(true, 0));
	pass("cs.dyncubemaps.irradiance", 6, Kind::Compute, gated(&Impl::IrradiancePlan));
	pass("cs.dyncubemaps.encode", 7, Kind::Compute, gated(&Impl::EncodePlan));
	pass("cs.dyncubemaps.bc6h", 8, Kind::Copy, bc6hCopy(0));
	pass("cs.dyncubemaps.bc6h-reflections", 9, Kind::Copy, bc6hCopy(1));
	pass("cs.dyncubemaps.publish", 10, Kind::Compute, gated(&Impl::PublishPlan));
	a_client.registerResources = [self](org::RenderGraph& a_graph) {
		if (self->impl)
			self->impl->Register(a_graph);
	};
	// No active callback, so the commit runs every run.
	a_client.commit = [self](const FrameAnchors::Point&) {
		if (self->impl)
			self->impl->Commit();
	};
	logger::info("[ORG] Dynamic Cubemaps: a client for the next revision ({0}x{0}, main target {1})", a_key.size, a_key.imports.main.Id());
	return true;
}


ORGDynamicCubemaps::Outputs ORGDynamicCubemaps::Exported() const
{
	if (!impl)
		return {};
	return { impl->env[0]->D3D11(), impl->env[0]->SRV(), impl->env[1]->D3D11(), impl->env[1]->SRV(), impl->bc6h[0]->D3D11(), impl->bc6h[0]->SRV(),
		impl->bc6h[1]->D3D11(), impl->bc6h[1]->SRV() };
}

bool ORGDynamicCubemaps::AfterUpdate()
{
	if (!impl)
		return false;
	// The first update moves the capture's colour source to Deferred's forward target. The next revision carries it and makes
	// the client for it. Old imports stay alive with the builds that hold them.
	impl->captureMain.Observe(static_cast<std::uint32_t>(globals::deferred->forwardRenderTargets[0]));
	return true;
}

void ORGDynamicCubemaps::ClearShaderCache()
{
	// The programs reload with every live pipeline (RenderGraphRuntime::RequestShaderReload). No cubemap counts as valid until
	// its next encode.
	if (impl)
		impl->encoded = {};
}

