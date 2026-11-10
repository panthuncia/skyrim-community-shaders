#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "ORGDynamicCubemaps.h"

#include "Features/DynamicCubemaps.h"
#include "Globals.h"

#include <cstring>

namespace
{
	bool EnvironmentFlag(const char* a_name)
	{
		char value[8]{};
		return GetEnvironmentVariableA(a_name, value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}
}

ORGDynamicCubemaps& ORGDynamicCubemaps::Get()
{
	static ORGDynamicCubemaps s_instance;
	return s_instance;
}

bool ORGDynamicCubemaps::Requested()
{
	static const bool requested = EnvironmentFlag("CS_ORG_DYNAMIC_CUBEMAPS");
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GenerateMips.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/OwnedDescriptorBinding.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/GloballyIndexedResource.h>

#	include <algorithm>
#	include <array>
#	include <bit>
#	include <cstddef>
#	include <optional>
#	include <span>
#	include <vector>

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

	// The published outputs: the D3D11 path's names for the same textures.
	constexpr std::array<std::string_view, 2> kEnvNames{ ORGDynamicCubemaps::kEnvId, ORGDynamicCubemaps::kEnvReflectionsId };
	constexpr std::array<std::string_view, 2> kBC6HNames{ ORGDynamicCubemaps::kEnvBC6HId, ORGDynamicCubemaps::kEnvReflectionsBC6HId };

	// As the D3D11 path (DynamicCubemaps.cpp's MIPLEVELS): every cubemap's mips, the engine's 256 x 256 down to 1 x 1.
	constexpr std::uint32_t kMips = 9;
	constexpr std::uint32_t kFaces = 6;
	// After Subsurface Scattering's passes (199-202) at DeferredPasses.
	constexpr std::int32_t kOrder = 300;
	// Frames AfterUpdate waits for the inputs before the cubemaps return to D3D11.
	constexpr std::uint32_t kRegisterAttempts = 600;

	using ShaderBindings::Handle;
	using Groups = GraphPasses::DispatchGroups;

	// The capture and inference programs: the environment's, the reflections', and the reflections' with the sky hidden.
	enum Variant : std::uint32_t
	{
		kBase,
		kReflections,
		kFake,
		kVariants,
	};

	// The push blocks, in their shaders' list order (check-dual-target-shaders reports each size).
	struct CaptureBlock  // CaptureCommon.hlsli: 52 bytes
	{
		SharedConstants::Handles shared;
		Handle dynamicCubemap, raw, position, lightingState, depth, color, sampler;
		ShaderBindings::Latch updateData;
	};
	struct InferBlock  // InferCubemapCS: 36 bytes
	{
		SharedConstants::Handles shared;
		Handle envCapture, reflections, defaultCubemap, inferred, sampler;
	};
	struct IrradianceBlock  // SpecularIrradianceCS: 36 bytes, the PUSH struct on a 16-byte boundary
	{
		SharedConstants::Handles shared;
		Handle input, output, sampler, pad;
		float roughness;
	};
	struct EncodeBlock  // BC6HEncodeCS: 32 bytes, the PUSH struct on a 16-byte boundary
	{
		Handle source, output, pad[2];
		std::uint32_t blocksX, blocksY, mip, pad2;
	};
	struct PublishBlock  // PublishCubemapCS: 8 bytes
	{
		Handle source, destination;
	};
	static_assert(ShaderBindings::Words<CaptureBlock>() == 13 && ShaderBindings::Words<InferBlock>() == 9 &&
				  ShaderBindings::Words<IrradianceBlock>() == 9 && ShaderBindings::Words<EncodeBlock>() == 8 && ShaderBindings::Words<PublishBlock>() == 2);

	/**
	 * @brief One execution's region of the latch block: the capture constants CaptureCommon.hlsli reads (its LATCH), then the group
	 * counts of every indirect dispatch the recording holds. The commit writes the counts of this frame's step and leaves the rest zero.
	 */
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

	/** @brief A program, with the indirect dispatch signature made against its layout. */
	struct Program
	{
		std::shared_ptr<const ComputeProgram> compute;
		rhi::CommandSignaturePtr signature;

		explicit operator bool() const { return compute && signature; }
	};

	struct Programs
	{
		Program detect;
		std::array<Program, kVariants> update, infer;
		Program irradiance, encode, publish;
		std::shared_ptr<const GraphPasses::GenerateMipsProgram> mips;

		explicit operator bool() const
		{
			return detect && std::ranges::all_of(update, [](const Program& p) { return static_cast<bool>(p); }) &&
			       std::ranges::all_of(infer, [](const Program& p) { return static_cast<bool>(p); }) && irradiance && encode && publish && mips;
		}
	};

	/** @brief The defines a_own plus those Util::CompileShader gives every D3D11 build, so both paths build one permutation. */
	std::vector<std::pair<std::wstring, std::wstring>> ProgramDefines(std::vector<std::pair<std::string, std::string>> a_own)
	{
		std::vector<std::string_view> names;
		for (const auto& define : a_own)
			names.push_back(define.first);
		for (auto& define : Util::GetCommonShaderDefines(names))
			a_own.push_back(std::move(define));
		for (const char* name : { "COMPUTESHADER", "WINPC", "DX11" })
			a_own.emplace_back(name, "");
		// Defines are ASCII.
		std::vector<std::pair<std::wstring, std::wstring>> wide;
		for (const auto& [name, value] : a_own)
			wide.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
		return wide;
	}

	template <class Block>
	Program LoadProgram(rhi::Device a_device, const char* a_source, std::vector<std::pair<std::string, std::string>> a_defines)
	{
		Program program;
		program.compute = ComputeProgram::Load(a_device,
			{ .source = a_source, .constantWords = ShaderBindings::Words<Block>(), .defines = ProgramDefines(std::move(a_defines)) });
		if (!program.compute)
			return {};
		rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
		if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 },
				program.compute->layout->GetHandle(), program.signature) != rhi::Result::Ok)
			return {};
		return program;
	}

	/** @brief Every program, as DynamicCubemaps' GetComputeShader* compile them, and ORG's GenerateMips for arrays and cubes. */
	std::shared_ptr<const Programs> LoadPrograms(rhi::Device a_device)
	{
		auto programs = std::make_shared<Programs>();
		programs->detect = LoadProgram<CaptureBlock>(a_device, kDetectShader, {});
		programs->update[kBase] = LoadProgram<CaptureBlock>(a_device, kUpdateShader, {});
		programs->update[kReflections] = LoadProgram<CaptureBlock>(a_device, kUpdateShader, { { "REFLECTIONS", "" } });
		programs->update[kFake] = LoadProgram<CaptureBlock>(a_device, kUpdateShader, { { "FAKEREFLECTIONS", "" } });
		programs->infer[kBase] = LoadProgram<InferBlock>(a_device, kInferShader, {});
		programs->infer[kReflections] = LoadProgram<InferBlock>(a_device, kInferShader, { { "REFLECTIONS", "" } });
		programs->infer[kFake] = LoadProgram<InferBlock>(a_device, kInferShader, { { "FAKEREFLECTIONS", "" } });
		programs->irradiance = LoadProgram<IrradianceBlock>(a_device, kIrradianceShader, {});
		programs->encode = LoadProgram<EncodeBlock>(a_device, kEncodeShader, {});
		programs->publish = LoadProgram<PublishBlock>(a_device, kPublishShader, {});
		programs->mips = GraphPasses::LoadGenerateMips(a_device, true);
		if (!*programs)
			return nullptr;
		return programs;
	}

	// A view a pass declares: the default one (UINT32_MAX) or a variant, at a mip.
	struct ViewDesc
	{
		std::uint32_t variant = UINT32_MAX;
		std::uint32_t mip = 0;
	};

	constexpr std::uint32_t kUavArray = static_cast<std::uint32_t>(org::UAVViewType::Texture2DArrayFull);
	constexpr std::uint32_t kSrvArray = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull);

	/** @brief One resource a gated pass declares, with its views in order. */
	struct Use
	{
		std::shared_ptr<org::Resource> resource;
		bool write = false;
		std::vector<ViewDesc> views{ ViewDesc{} };
	};

	/** @brief A word of a push block: a constant, a declared view's handle, the four shared handles, or a latch entry. */
	struct Word
	{
		enum class Kind : std::uint8_t
		{
			Literal,
			View,    // value: the view's index, in the order the uses declare them
			Shared,  // four words
			Latch,   // two words; value: the byte offset in the execution's region
		};
		Kind kind = Kind::Literal;
		std::uint32_t value = 0;
	};

	Word Literal(std::uint32_t a_value) { return { Word::Kind::Literal, a_value }; }
	Word View(std::uint32_t a_index) { return { Word::Kind::View, a_index }; }
	Word Shared() { return { Word::Kind::Shared, 0 }; }
	Word LatchAt(std::uint32_t a_offset) { return { Word::Kind::Latch, a_offset }; }

	/** @brief One indirect dispatch: its program, its push block, where its group counts are in the region, and a barrier before it. */
	struct Dispatch
	{
		const Program* program = nullptr;
		std::vector<Word> block;
		std::uint32_t groups = 0;
		bool barrier = false;
	};

	/** @brief What a gated pass declares and records: fixed from the build on, so its recording is the same every frame. */
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

	/**
	 * @brief Compute dispatches whose group counts the execution's latch region holds: the commit sets those of this frame's work and
	 * zeros the rest, so a dispatch runs only on the frames that need it while the recording stays the same.
	 */
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

		// Everything the recording depends on is fixed with the plan; the frame's work is in the latch.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(plan.get())); }

		GatedFrame Prepare(const GatedBindings&, const org::PassPrepareContext&) const { return { plan }; }

		static void Record(const GatedBindings& a_bindings, const GatedFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto* current = a_frame.plan.get();
			if (!current || !current->latch)
				return;
			const std::uint32_t slot = a_recording.FrameSlot();
			auto& commands = a_recording.Commands();
			const auto latchBuffer = current->latch->Resource()->GetAPIResource().GetHandle();
			const std::uint64_t region = current->latch->Offset(slot);
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
							const auto latch = ShaderBindings::LatchOf(*current->latch, slot, word.value);
							push(latch.handle);
							push(latch.offset);
							break;
						}
					}
				}
				const auto& compute = *dispatch.program->compute;
				// A block that does not match the program's records nothing (a programming error, caught in the first test).
				if (count != compute.constantWords)
					continue;
				if (dispatch.barrier)
					a_recording.UavBarrier();
				commands.BindLayout(compute.layout->GetHandle());
				commands.BindPipeline(compute.Pipeline());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, count, words.data());
				commands.ExecuteIndirect(dispatch.program->signature->GetHandle(), latchBuffer, region + dispatch.groups, {}, 0, 1);
			}
		}

	private:
		std::shared_ptr<const Plan> plan;
	};

	/** @brief One subresource copy: source and target by index into the copy plan's lists. */
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

	struct CopyFrame
	{
		std::shared_ptr<const CopyPlan> plan;
	};

	/**
	 * @brief Whole subresource copies, every execution. Only copies whose result does not change between the frames that need them:
	 * the inferred cubemap's mip 0 into the filtered one, and each encoder's output into its BC6H cubemap.
	 */
	class CopyPass final : public org::TypedRenderGraphPass<CopyPass, CopyFrame, CopyBindings>
	{
	public:
		explicit CopyPass(std::shared_ptr<const CopyPlan> a_plan) :
			plan(std::move(a_plan)) {}

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

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { plan }; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.plan)
				return;
			auto& commands = a_recording.Commands();
			// The extent is the source subresource's whole size (in the encoder's case its texels, which are the BC6H blocks).
			for (const auto& region : a_frame.plan->regions)
				commands.CopyTextureRegion(
					{ .texture = a_recording.Resolve(a_bindings.targets[region.target]).GetHandle(), .mip = region.targetMip, .arraySlice = region.targetSlice },
					{ .texture = a_recording.Resolve(a_bindings.sources[region.source]).GetHandle(), .mip = region.sourceMip, .arraySlice = region.sourceSlice });
		}

	private:
		std::shared_ptr<const CopyPlan> plan;
	};

	/** @brief The engine's and the feature's textures the passes read, imported (FrameAnchors). */
	struct Imports
	{
		std::shared_ptr<org::ExternalTextureResource> depth, main, reflections, defaultCubemap;

		bool operator==(const Imports&) const = default;
		explicit operator bool() const { return depth && main && reflections && defaultCubemap; }
	};

	Imports Import()
	{
		Imports imports;
		imports.depth = FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
		imports.main = FrameAnchors::EngineTarget(RE::RENDER_TARGETS::kMAIN);
		imports.reflections = FrameAnchors::EngineCubemap(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS);
		if (auto* view = globals::features::dynamicCubemaps.defaultCubemap) {
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			if (auto texture = resource.try_as<ID3D11Texture2D>())
				imports.defaultCubemap = FrameAnchors::D3D11Texture(kDefaultCubemapName, texture.get(), view, nullptr);
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

	/**
	 * @brief Zeros every subresource of an exported texture through D3D11, once, before the graph first uses it: what the D3D11 path's
	 * clears give its readers before the first publish (DXVK's new images are zero). R11G11B10 texels or BC6H blocks.
	 */
	void ZeroTexture(ID3D11Texture2D* a_texture)
	{
		D3D11_TEXTURE2D_DESC desc{};
		a_texture->GetDesc(&desc);
		const bool blocks = desc.Format == DXGI_FORMAT_BC6H_UF16;
		const UINT bytes = blocks ? 16u : 4u;
		std::vector<std::uint8_t> zeros;
		for (UINT mip = 0; mip < desc.MipLevels; ++mip) {
			const UINT width = (std::max)(desc.Width >> mip, 1u);
			const UINT height = (std::max)(desc.Height >> mip, 1u);
			const UINT columns = blocks ? (width + 3) / 4 : width;
			const UINT rows = blocks ? (height + 3) / 4 : height;
			zeros.assign(static_cast<std::size_t>(columns) * bytes * rows, 0);
			for (UINT slice = 0; slice < desc.ArraySize; ++slice)
				globals::d3d::context->UpdateSubresource(a_texture, D3D11CalcSubresource(mip, slice, desc.MipLevels), nullptr, zeros.data(), columns * bytes, 0);
		}
	}
}

struct ORGDynamicCubemaps::Impl
{
	UINT size = 0;  // the cubemaps' mip 0, the engine's reflections cubemap's
	std::shared_ptr<const Programs> programs;
	bool programsDirty = false;
	org::OwnedDescriptorBinding sampler;
	std::shared_ptr<org::LatchBlock> latch;
	Imports imports;

	// Graph-only, read in later frames (History). The capture textures per cubemap; the filtered one is a plain array, which the
	// encoder and the publish read through array views.
	std::array<std::unique_ptr<GraphResources::Texture2D>, 2> capture, raw, position;
	std::unique_ptr<GraphResources::StructuredBuffer> lightingState;
	std::unique_ptr<GraphResources::Texture2D> inferred, filtered;
	// The encoder's output for each BC6H mip, a 6-slice array of uint4 blocks, kept for the copy that follows every frame.
	std::array<std::array<std::unique_ptr<GraphResources::Texture2D>, kMips>, 2> scratch;
	// Exported and published.
	std::array<std::unique_ptr<GraphResources::Texture2D>, 2> env, bc6h;

	// Each cubemap encoded since the programs were built; the BC6H copy in the graph (from the frame after its first encode).
	std::array<bool, 2> encoded{};
	std::array<bool, 2> copying{};
	std::function<void()> onFailed;

	// The anchor client, registered at the first AfterUpdate with every input (Setup runs before the engine targets exist).
	bool registered = false;
	std::uint32_t registerAttempts = 0;
	bool waitLogged = false;

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

	/** @brief Detection and capture for both cubemaps: each runs only on its capture frames. */
	std::shared_ptr<const Plan> CapturePlan() const
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
		plan->uses.push_back({ imports.depth, false });
		plan->uses.push_back({ imports.main, false });
		const auto block = [&](std::uint32_t a_cubemap) {
			return std::vector<Word>{ Shared(), View(a_cubemap * 3), View(a_cubemap * 3 + 1), View(a_cubemap * 3 + 2), View(6), View(7), View(8),
				Literal(sampler.Index()), LatchAt(offsetof(FrameLatch, capture)) };
		};
		// Detection writes the lighting state the capture reads; the capture writes what the next detection reads.
		plan->dispatches.push_back({ &programs->detect, block(0), GroupsOffset(offsetof(FrameLatch, detect), 0), false });
		plan->dispatches.push_back({ &programs->update[kBase], block(0), GroupsOffset(offsetof(FrameLatch, update), kBase), true });
		plan->dispatches.push_back({ &programs->detect, block(1), GroupsOffset(offsetof(FrameLatch, detect), 1), true });
		plan->dispatches.push_back({ &programs->update[kReflections], block(1), GroupsOffset(offsetof(FrameLatch, update), kReflections), true });
		plan->dispatches.push_back({ &programs->update[kFake], block(1), GroupsOffset(offsetof(FrameLatch, update), kFake), true });
		return plan;
	}

	/** @brief Inference into the shared inferred cubemap, from either capture. */
	std::shared_ptr<const Plan> InferPlan() const
	{
		auto plan = std::make_shared<Plan>();
		plan->programs = programs;
		plan->latch = latch;
		plan->shared = true;
		// The captures' and the cubemaps' default views: cube views of every mip.
		plan->uses.push_back({ capture[0]->Graph(), false });
		plan->uses.push_back({ capture[1]->Graph(), false });
		plan->uses.push_back({ imports.reflections, false });
		plan->uses.push_back({ imports.defaultCubemap, false });
		plan->uses.push_back({ inferred->Graph(), true, { { kUavArray, 0 } } });
		for (std::uint32_t variant = 0; variant < kVariants; ++variant) {
			const std::vector<Word> block{ Shared(), View(variant == kBase ? 0u : 1u), View(2), View(3), View(4), Literal(sampler.Index()) };
			plan->dispatches.push_back({ &programs->infer[variant], block, GroupsOffset(offsetof(FrameLatch, infer), variant), variant != kBase });
		}
		return plan;
	}

	/** @brief The pre-filtered specular mips 1 to 8 of the filtered cubemap, from the inferred one; the state machine spreads them. */
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
		// As the D3D11 path: mip / (mips - 1).
		const float deltaRoughness = 1.0f / static_cast<float>(kMips - 1);
		for (std::uint32_t mip = 1; mip < kMips; ++mip) {
			const float roughness = static_cast<float>(mip) * deltaRoughness;
			const std::vector<Word> block{ Shared(), View(0), View(mip), Literal(sampler.Index()), Literal(0), Literal(std::bit_cast<std::uint32_t>(roughness)) };
			plan->dispatches.push_back({ &programs->irradiance, block, GroupsOffset(offsetof(FrameLatch, irradiance), mip - 1), false });
		}
		return plan;
	}

	/** @brief BC6H encoding of every mip of the filtered cubemap, into the encoder outputs of the cubemap being published. */
	std::shared_ptr<const Plan> EncodePlan() const
	{
		auto plan = std::make_shared<Plan>();
		plan->programs = programs;
		plan->latch = latch;
		// Every mip and face: the D3D11 path's Texture2DArray view (Load takes the mip).
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

	/** @brief The filtered cubemap, every mip, into the environment cubemap being published (the D3D11 path's CopyResource). */
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

	/** @brief The inferred cubemap's mip 0 into the filtered one's, every frame: the same between the inference frames. */
	std::shared_ptr<const CopyPlan> FilteredMipZeroPlan() const
	{
		auto plan = std::make_shared<CopyPlan>();
		plan->sources.push_back(inferred->Graph());
		plan->targets.push_back(filtered->Graph());
		for (std::uint32_t face = 0; face < kFaces; ++face)
			plan->regions.push_back({ 0, 0, face, 0, 0, face });
		return plan;
	}

	/** @brief A cubemap's encoder outputs into its BC6H cubemap, every frame: the same between its publish frames. */
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

	/** @brief In the epoch: the state machine's step for this frame, as the group counts of its work and the capture constants. */
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
			const Variant variant = !step.reflections ? kBase : (feature.fakeReflections ? kFake : kReflections);
			data.capture = feature.NextCaptureConstants(step.reflections);
			data.detect[cubemap] = { 1, 1, 1, 0 };
			data.update[variant] = { faceGroups, faceGroups, kFaces, 0 };
			data.infer[variant] = { faceGroups, faceGroups, kFaces, 0 };
			const auto mips = GraphPasses::GenerateMipsGroups(size, size, kMips, kFaces);
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
		}
		// The BC6H cubemap is valid once its copy runs: on its first publish only from the next frame (AfterUpdate adds the copy).
		for (std::uint32_t i = 0; i < 2; ++i)
			if (encoded[i] && copying[i])
				feature.cubemapValid[i] = true;
		latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, data);
	}
};

ORGDynamicCubemaps::~ORGDynamicCubemaps() = default;

void ORGDynamicCubemaps::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::DeferredPasses))
		logger::warn("[ORG] Dynamic Cubemaps: the DeferredPasses anchor could not be reserved; the cubemaps stay on D3D11");
}

bool ORGDynamicCubemaps::Setup(std::function<void()> a_onFailed)
{
	impl.reset();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses)) {
		logger::warn("[ORG] Dynamic Cubemaps: the render graph or its DeferredPasses anchor is not available; the cubemaps stay on D3D11");
		return false;
	}
	try {
		auto state = std::make_unique<Impl>();
		auto& cubemap = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
		if (!cubemap.texture) {
			logger::warn("[ORG] Dynamic Cubemaps: no engine reflections cubemap; the cubemaps stay on D3D11");
			return false;
		}
		D3D11_TEXTURE2D_DESC engine{};
		cubemap.texture->GetDesc(&engine);
		state->size = engine.Width;
		if (engine.Width != engine.Height || engine.Width != (1u << (kMips - 1))) {
			logger::warn("[ORG] Dynamic Cubemaps: the engine's reflections cubemap is {}x{}, not the {} mips the passes are made for; the cubemaps stay on D3D11",
				engine.Width, engine.Height, kMips);
			return false;
		}

		state->programs = LoadPrograms(host->Device());
		// The D3D11 path's compute sampler: linear, wrapped, every mip.
		state->sampler = ShaderBindings::CreateSampler({ .addressU = rhi::AddressMode::Wrap, .addressV = rhi::AddressMode::Wrap, .addressW = rhi::AddressMode::Wrap });

		const UINT size = state->size;
		constexpr UINT kReadWrite = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		const GraphResources::Options history{ .lifetime = GraphResources::Lifetime::History };
		// Read through D3D11 (the composite, IBL, the engine's draws, Drawcall Limit Fix's reflection faces) until those readers move.
		const GraphResources::Options exportedOptions{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV };
		constexpr std::array<const char*, 2> kCaptureNames{ "DynamicCubemaps::EnvCapture", "DynamicCubemaps::EnvCaptureReflections" };
		constexpr std::array<const char*, 2> kRawNames{ "DynamicCubemaps::EnvCaptureRaw", "DynamicCubemaps::EnvCaptureRawReflections" };
		constexpr std::array<const char*, 2> kPositionNames{ "DynamicCubemaps::EnvCapturePosition", "DynamicCubemaps::EnvCapturePositionReflections" };
		constexpr std::array<const char*, 2> kScratchNames{ "DynamicCubemaps::BC6HScratch", "DynamicCubemaps::BC6HScratchReflections" };
		for (std::uint32_t i = 0; i < 2; ++i) {
			state->capture[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_R16G16B16A16_FLOAT, kReadWrite, true), kCaptureNames[i], history);
			// The history and positions only at mip 0 (the D3D11 path gives them the whole chain, which nothing reads).
			state->raw[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, kReadWrite, false), kRawNames[i], history);
			state->position[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, kReadWrite, false), kPositionNames[i], history);
			state->env[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_R11G11B10_FLOAT, kReadWrite, true),
				std::string(kEnvNames[i]), exportedOptions);
			state->bc6h[i] = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_BC6H_UF16, D3D11_BIND_SHADER_RESOURCE, true),
				std::string(kBC6HNames[i]), exportedOptions);
			for (std::uint32_t mip = 0; mip < kMips; ++mip) {
				// One uint4 block per 4x4 texels; mips under 4x4 still take one block.
				const UINT blocks = (std::max)((std::max)(size >> mip, 1u) / 4, 1u);
				state->scratch[i][mip] = std::make_unique<GraphResources::Texture2D>(TextureDesc(blocks, 1, DXGI_FORMAT_R32G32B32A32_UINT, kReadWrite, false),
					fmt::format("{} mip{}", kScratchNames[i], mip), history);
			}
		}
		state->inferred = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_R11G11B10_FLOAT, kReadWrite, true), "DynamicCubemaps::EnvInferred", history);
		state->filtered = std::make_unique<GraphResources::Texture2D>(TextureDesc(size, kMips, DXGI_FORMAT_R11G11B10_FLOAT, kReadWrite, false), "DynamicCubemaps::EnvFiltered", history);
		// The capture's lighting state. Its first contents need no clear (the D3D11 path's): the first capture of each cubemap resets
		// (resetCapture starts true), and a reset writes every field the shaders read afterwards.
		D3D11_BUFFER_DESC stateDesc{};
		stateDesc.ByteWidth = sizeof(DynamicCubemaps::CaptureLightingState);
		stateDesc.Usage = D3D11_USAGE_DEFAULT;
		stateDesc.BindFlags = kReadWrite;
		stateDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		stateDesc.StructureByteStride = sizeof(DynamicCubemaps::CaptureLightingState);
		state->lightingState = std::make_unique<GraphResources::StructuredBuffer>(stateDesc, 1, "DynamicCubemaps::CaptureLightingState", history);
		state->latch = std::make_shared<org::LatchBlock>("cs.dyncubemaps.latch", static_cast<std::uint32_t>(sizeof(FrameLatch)), host->FrameSlots());
		if (!state->programs || !state->sampler || !state->Complete()) {
			logger::warn("[ORG] Dynamic Cubemaps: its programs, sampler or textures could not be made; the cubemaps stay on D3D11");
			return false;
		}
		// Black until the first publish, as the D3D11 path's cubemaps are.
		for (std::uint32_t i = 0; i < 2; ++i) {
			ZeroTexture(state->env[i]->D3D11());
			ZeroTexture(state->bc6h[i]->D3D11());
		}
		state->onFailed = std::move(a_onFailed);
		impl = std::move(state);
		for (std::uint32_t i = 0; i < 2; ++i)
			if (!PublishedResources::Publish(*impl->env[i]) || !PublishedResources::Publish(*impl->bc6h[i])) {
				logger::warn("[ORG] Dynamic Cubemaps: the cubemaps could not be published; they stay on D3D11");
				Withdraw();
				impl.reset();
				return false;
			}
		// The client registers at the first AfterUpdate that has the engine targets and the shared constants.
		logger::info("[ORG] Dynamic Cubemaps: the cubemaps update on the render graph ({}x{}, {} mips)", size, size, kMips);
		return true;
	} catch (const std::exception& e) {
		logger::error("[ORG] Dynamic Cubemaps setup failed, staying on D3D11: {}", e.what());
		if (impl)
			Withdraw();
		impl.reset();
		return false;
	}
}

bool ORGDynamicCubemaps::RegisterClient()
{
	auto* self = this;
	const auto gated = [self](std::shared_ptr<const Plan> (Impl::*a_plan)() const) {
		return [self, a_plan]() -> std::shared_ptr<org::RenderPass> {
			const auto& current = self->impl;
			return current ? std::make_shared<GatedComputePass>(((*current).*a_plan)()) : nullptr;
		};
	};
	const auto mips = [self](bool a_inferred, std::uint32_t a_cubemap) {
		return [self, a_inferred, a_cubemap]() -> std::shared_ptr<org::RenderPass> {
			const auto& current = self->impl;
			if (!current)
				return nullptr;
			const auto offset = a_inferred ? GroupsOffset(offsetof(FrameLatch, inferredMips), 0) : GroupsOffset(offsetof(FrameLatch, captureMips), a_cubemap * (kMips - 1));
			return GraphPasses::MakeGenerateMips(current->programs->mips, (a_inferred ? current->inferred : current->capture[a_cubemap])->Graph(),
				{ current->latch, offset });
		};
	};
	const auto bc6hCopy = [self](std::uint32_t a_cubemap) {
		return [self, a_cubemap]() -> std::shared_ptr<org::RenderPass> {
			const auto& current = self->impl;
			// From the frame after the cubemap's first encode: before it, its encoder outputs hold nothing.
			return current && current->copying[a_cubemap] ? std::make_shared<CopyPass>(current->BC6HPlan(a_cubemap)) : nullptr;
		};
	};
	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.sharedConstants = true;
	const auto pass = [&client](const char* a_name, std::int32_t a_order, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
		client.passes.push_back({ .name = a_name, .anchor = FrameAnchors::Anchor::DeferredPasses, .order = kOrder + a_order, .kind = a_kind, .make = std::move(a_make) });
	};
	using Kind = FrameAnchors::PassKind;
	pass("cs.dyncubemaps.capture", 0, Kind::Compute, gated(&Impl::CapturePlan));
	pass("cs.dyncubemaps.capture-mips", 1, Kind::Compute, mips(false, 0));
	pass("cs.dyncubemaps.capture-mips-reflections", 2, Kind::Compute, mips(false, 1));
	pass("cs.dyncubemaps.infer", 3, Kind::Compute, gated(&Impl::InferPlan));
	pass("cs.dyncubemaps.filtered-mip0", 4, Kind::Copy, [self]() -> std::shared_ptr<org::RenderPass> {
		return self->impl ? std::make_shared<CopyPass>(self->impl->FilteredMipZeroPlan()) : nullptr;
	});
	pass("cs.dyncubemaps.inferred-mips", 5, Kind::Compute, mips(true, 0));
	pass("cs.dyncubemaps.irradiance", 6, Kind::Compute, gated(&Impl::IrradiancePlan));
	pass("cs.dyncubemaps.encode", 7, Kind::Compute, gated(&Impl::EncodePlan));
	pass("cs.dyncubemaps.bc6h", 8, Kind::Copy, bc6hCopy(0));
	pass("cs.dyncubemaps.bc6h-reflections", 9, Kind::Copy, bc6hCopy(1));
	pass("cs.dyncubemaps.publish", 10, Kind::Compute, gated(&Impl::PublishPlan));
	client.registerResources = [self](org::RenderGraph& a_graph) {
		if (self->impl)
			self->impl->Register(a_graph);
	};
	client.commit = [self](const FrameAnchors::Point&) {
		if (self->impl)
			self->impl->Commit();
	};
	client.onFailed = [self] { self->Fail("the render graph failed", false); };
	return FrameAnchors::Register(std::move(client));
}

void ORGDynamicCubemaps::Withdraw()
{
	for (std::uint32_t i = 0; i < 2; ++i) {
		PublishedResources::Withdraw(kEnvNames[i]);
		PublishedResources::Withdraw(kBC6HNames[i]);
	}
}

void ORGDynamicCubemaps::Fail(const char* a_reason, bool a_unregister)
{
	logger::error("[ORG] Dynamic Cubemaps returns to D3D11: {}", a_reason);
	if (a_unregister)
		FrameAnchors::Unregister(kClientId);
	Withdraw();
	auto failed = std::move(impl);
	// The feature drops its views of the exported textures first: they go when the graph's resources are released below.
	if (failed && failed->onFailed)
		failed->onFailed();
}

bool ORGDynamicCubemaps::IsActive() const
{
	return impl != nullptr;
}

ORGDynamicCubemaps::Outputs ORGDynamicCubemaps::Exported() const
{
	if (!impl)
		return {};
	return { impl->env[0]->D3D11(), impl->env[0]->SRV(), impl->env[1]->D3D11(), impl->env[1]->SRV(), impl->bc6h[0]->D3D11(), impl->bc6h[0]->SRV(),
		impl->bc6h[1]->D3D11(), impl->bc6h[1]->SRV() };
}

void ORGDynamicCubemaps::AfterUpdate()
{
	if (!impl)
		return;
	if (!RenderGraphRuntime::Get().IsActive()) {
		Fail("the render graph is no longer active", true);
		return;
	}
	auto& state = *impl;
	if (!state.registered) {
		// Once the engine targets are there and CS has uploaded its shared constants: the first frames in the world. The anchor runs
		// the client from the next frame; the readers see the black cubemaps until the first publish.
		state.imports = Import();
		if (state.imports && RegisterClient()) {
			state.registered = true;
			logger::info("[ORG] Dynamic Cubemaps: registered on the DeferredPasses anchor");
		} else if (++state.registerAttempts >= kRegisterAttempts) {
			Fail("its inputs or its anchor client were not ready in time", true);
		} else if (!std::exchange(state.waitLogged, true)) {
			logger::info("[ORG] Dynamic Cubemaps: waiting for the engine targets and the shared constants (depth {}, main {}, reflections {}, default cubemap {})",
				state.imports.depth != nullptr, state.imports.main != nullptr, state.imports.reflections != nullptr, state.imports.defaultCubemap != nullptr);
		}
		return;
	}
	bool refresh = false;
	if (state.programsDirty) {
		auto programs = LoadPrograms(RenderGraphRuntime::Get().Host()->Device());
		if (!programs) {
			Fail("its programs could not be built again", true);
			return;
		}
		state.programs = std::move(programs);
		state.programsDirty = false;
		refresh = true;
	}
	// A resolution change or a new engine cubemap: imported again (the old imports stay alive with the builds that hold them).
	if (auto imports = Import(); imports && imports != state.imports) {
		state.imports = std::move(imports);
		refresh = true;
	}
	for (std::uint32_t i = 0; i < 2; ++i)
		if (state.encoded[i] && !state.copying[i]) {
			state.copying[i] = true;
			refresh = true;
		}
	if (refresh)
		FrameAnchors::Refresh(kClientId);
}

void ORGDynamicCubemaps::ClearShaderCache()
{
	if (!impl)
		return;
	impl->programsDirty = true;
	impl->encoded = {};
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the cubemaps always update with D3D11.
struct ORGDynamicCubemaps::Impl
{};

ORGDynamicCubemaps::~ORGDynamicCubemaps() = default;
void ORGDynamicCubemaps::Reserve() {}
bool ORGDynamicCubemaps::Setup(std::function<void()>) { return false; }
bool ORGDynamicCubemaps::IsActive() const { return false; }
ORGDynamicCubemaps::Outputs ORGDynamicCubemaps::Exported() const { return {}; }
void ORGDynamicCubemaps::AfterUpdate() {}
void ORGDynamicCubemaps::ClearShaderCache() {}
void ORGDynamicCubemaps::Withdraw() {}
void ORGDynamicCubemaps::Fail(const char*, bool) {}
bool ORGDynamicCubemaps::RegisterClient() { return false; }

#endif
